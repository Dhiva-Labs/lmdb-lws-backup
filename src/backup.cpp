#include "backup.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "hot_copy.h"
#include "log.h"
#include "retention.h"
#include "version.h"

namespace fs = std::filesystem;

namespace lwsbk {

namespace {

constexpr unsigned kMaxDbs = 128;

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

struct FdGuard {
  int fd = -1;
  explicit FdGuard(int f) : fd(f) {}
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  ~FdGuard() { reset(); }
  void reset() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  int release() {
    int f = fd;
    fd = -1;
    return f;
  }
};

// RAM-backed (or operator-overridden) scratch file for decrypt-for-verify.
// Always unlinked/anonymous: a crash can never leave plaintext behind, and
// nothing else in the namespace can open it.
int make_scratch_fd(const Config& cfg) {
  if (!cfg.verify_scratch_dir.empty()) {
    std::string tmpl = cfg.verify_scratch_dir + "/lwsbk-verify-XXXXXX";
    int fd = mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0)
      throw_errno("cannot create verify scratch file in " +
                  cfg.verify_scratch_dir);
    fchmod(fd, 0600);
    ::unlink(tmpl.c_str());
    LWSBK_DEBUG("verify scratch: file in %s (operator override)",
                cfg.verify_scratch_dir.c_str());
    return fd;
  }

  int fd = memfd_create("lwsbk-verify", MFD_CLOEXEC);
  if (fd >= 0) {
    LWSBK_DEBUG("verify scratch: memfd (anonymous RAM)");
    return fd;
  }

  std::string tmpl = "/dev/shm/lwsbk-verify-XXXXXX";
  fd = mkostemp(tmpl.data(), O_CLOEXEC);
  if (fd >= 0) {
    fchmod(fd, 0600);
    ::unlink(tmpl.c_str());
    LWSBK_DEBUG("verify scratch: unlinked file on /dev/shm");
    return fd;
  }
  throw std::runtime_error(
      "no RAM-backed scratch available (memfd_create and /dev/shm both "
      "failed); set [verify] scratch_dir to a directory you accept transient "
      "plaintext on");
}

VerifyResult walk_scratch_env(int scratch_fd) {
  VerifyResult res;
  std::string path = "/proc/self/fd/" + std::to_string(scratch_fd);
  // no_lock: the scratch inode is private to this process, and a "-lock"
  // sibling path cannot exist under /proc.
  Env env = Env::open_readonly(path, kMaxDbs, /*no_lock=*/true);
  ReadTxn txn(env.get());
  res.accounts = walk_accounts(txn.get());
  res.total_rows = walk_all_tables(txn.get());
  return res;
}

void fsync_dir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw_errno("cannot open destination dir " + dir);
  if (fsync(fd) != 0) {
    int e = errno;
    ::close(fd);
    errno = e;
    throw_errno("fsync of destination dir " + dir);
  }
  ::close(fd);
}

}  // namespace

std::string current_date_string(const Config& cfg) {
  time_t now = time(nullptr);
  struct tm tmv;
  if (cfg.timezone == "UTC")
    gmtime_r(&now, &tmv);
  else
    localtime_r(&now, &tmv);  // caller has applied TZ via apply_timezone()
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
  return buf;
}

VerifyResult verify_backup_file(const Config& cfg, const std::string& enc_path,
                                const SecretKey& key,
                                const std::function<bool()>& abort_requested) {
  int in = ::open(enc_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) throw_errno("cannot open backup file " + enc_path);
  FdGuard in_guard(in);
  FdGuard scratch(make_scratch_fd(cfg));

  StreamResult dr = decrypt_fd_stream(in, scratch.fd, key, abort_requested);
  in_guard.reset();

  VerifyResult res = walk_scratch_env(scratch.fd);
  res.plaintext_bytes = dr.plaintext_bytes;
  res.sha256_hex = to_hex(dr.sha256, sizeof(dr.sha256));
  return res;
}

VerifyResult restore_backup(const Config& cfg, const std::string& enc_path,
                            const std::string& to_path, const SecretKey& key) {
  fs::create_directories(to_path);
  fs::permissions(to_path, fs::perms::owner_all,
                  fs::perm_options::replace);
  fs::path data = fs::path(to_path) / "data.mdb";
  if (fs::exists(data))
    throw std::runtime_error("refusing to overwrite existing " + data.string());

  int in = ::open(enc_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) throw_errno("cannot open backup file " + enc_path);
  FdGuard in_guard(in);
  int out = ::open(data.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out < 0) throw_errno("cannot create " + data.string());
  FdGuard out_guard(out);

  try {
    decrypt_fd_stream(in, out, key, nullptr);
    if (fsync(out) != 0) throw_errno("fsync " + data.string());
  } catch (...) {
    out_guard.reset();
    ::unlink(data.c_str());
    throw;
  }
  out_guard.reset();
  in_guard.reset();

  // Sanity-open the restored environment and summarize it.
  VerifyResult res;
  Env env = Env::open_readonly(to_path, kMaxDbs);
  ReadTxn txn(env.get());
  res.accounts = walk_accounts(txn.get());
  res.total_rows = walk_all_tables(txn.get());
  res.sha256_hex = sha256_file_hex(enc_path);
  return res;
}

BackupOutcome run_backup_cycle(const Config& cfg, const SecretKey& key,
                               const std::function<bool()>& abort_requested) {
  const auto t0 = std::chrono::steady_clock::now();
  const std::string timestamp = rfc3339_utc_now();

  if (!fs::exists(cfg.destination_dir)) {
    fs::create_directories(cfg.destination_dir);
    fs::permissions(cfg.destination_dir, fs::perms::owner_all,
                    fs::perm_options::replace);
    LWSBK_INFO("created destination dir %s (0700)",
               cfg.destination_dir.c_str());
  }
  cleanup_partials(cfg.destination_dir);
  cleanup_orphan_manifests(cfg.destination_dir);

  Env src = Env::open_readonly(cfg.db_path, kMaxDbs);
  reader_check(src.get());

  const std::string date = current_date_string(cfg);
  const fs::path dir(cfg.destination_dir);
  const std::string enc_final = (dir / backup_filename_for_date(date)).string();
  const std::string enc_partial = enc_final + kPartialSuffix;
  const std::string man_final =
      (dir / manifest_filename_for_date(date)).string();
  const std::string man_partial = man_final + kPartialSuffix;

  LWSBK_INFO("backup starting: %s -> %s", cfg.db_path.c_str(),
             enc_final.c_str());

  int out = ::open(enc_partial.c_str(),
                   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out < 0) throw_errno("cannot create " + enc_partial);
  FdGuard out_guard(out);

  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC) != 0) throw_errno("pipe2");
  FdGuard pipe_r(pfd[0]);

  // The copy thread streams the compacting snapshot into the pipe; the main
  // thread encrypts straight to the destination file. Plaintext exists only
  // in the pipe buffer and our chunk buffers — never on any filesystem.
  std::atomic<bool> copier_ok{false};
  std::string copier_error;
  std::thread copier([&, wfd = pfd[1]] {
    try {
      ReadTxn pin(src.get());  // explicit RAII snapshot pin per spec §6.2
      copy_env_to_fd(src.get(), wfd);
      copier_ok.store(true);
    } catch (const std::exception& e) {
      copier_error = e.what();
    }
    ::close(wfd);
  });

  StreamResult er;
  try {
    er = encrypt_fd_stream(
        pfd[0], out, key, cfg.chunk_size,
        /*producer_ok=*/
        [&] {
          if (copier.joinable()) copier.join();
          return copier_ok.load();
        },
        abort_requested);
    if (fsync(out) != 0) throw_errno("fsync " + enc_partial);
  } catch (...) {
    pipe_r.reset();  // EPIPEs the copy thread out of any blocked write
    if (copier.joinable()) copier.join();
    out_guard.reset();
    ::unlink(enc_partial.c_str());
    if (!copier_error.empty())
      throw std::runtime_error("hot copy failed: " + copier_error);
    throw;
  }
  pipe_r.reset();
  out_guard.reset();

  LWSBK_INFO("hot copy + encrypt done: %llu bytes plaintext -> %llu bytes "
             "encrypted",
             static_cast<unsigned long long>(er.plaintext_bytes),
             static_cast<unsigned long long>(er.ciphertext_bytes));

  // Verify what actually landed on persistent storage.
  VerifyResult vr;
  try {
    vr = verify_backup_file(cfg, enc_partial, key, abort_requested);
    if (vr.sha256_hex != to_hex(er.sha256, sizeof(er.sha256)))
      throw std::runtime_error(
          "sha256 of written file differs from the encrypted stream (torn "
          "write?)");
  } catch (...) {
    ::unlink(enc_partial.c_str());
    throw;
  }

  LWSBK_INFO("verify ok: %llu accounts, scan height %llu..%llu, %llu total "
             "rows across all tables",
             static_cast<unsigned long long>(vr.accounts.account_count),
             static_cast<unsigned long long>(
                 vr.accounts.scan_height_min.value_or(0)),
             static_cast<unsigned long long>(
                 vr.accounts.scan_height_max.value_or(0)),
             static_cast<unsigned long long>(vr.total_rows));

  Manifest m;
  m.timestamp = timestamp;
  m.account_count = vr.accounts.account_count;
  m.scan_height_min = vr.accounts.scan_height_min;
  m.scan_height_max = vr.accounts.scan_height_max;
  m.sha256_of_encrypted_file = vr.sha256_hex;
  m.encrypted_size_bytes = er.ciphertext_bytes;
  m.tool_version = kToolVersion;

  try {
    write_manifest_file(man_partial, m);
    if (::rename(enc_partial.c_str(), enc_final.c_str()) != 0)
      throw_errno("rename " + enc_partial);
    if (::rename(man_partial.c_str(), man_final.c_str()) != 0)
      throw_errno("rename " + man_partial);
    fsync_dir(cfg.destination_dir);
  } catch (...) {
    ::unlink(enc_partial.c_str());
    ::unlink(man_partial.c_str());
    throw;
  }

  // Only now — after the new backup is fully verified and durably in place —
  // is anything old allowed to be deleted.
  int removed = apply_retention(cfg.destination_dir, cfg.retention_days, date);

  BackupOutcome outcome;
  outcome.enc_path = enc_final;
  outcome.manifest_path = man_final;
  outcome.manifest = m;
  outcome.duration_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  LWSBK_INFO("backup complete: %s (%llu bytes, %d old backup(s) pruned, "
             "%.1fs)",
             enc_final.c_str(),
             static_cast<unsigned long long>(m.encrypted_size_bytes), removed,
             outcome.duration_seconds);
  return outcome;
}

}  // namespace lwsbk
