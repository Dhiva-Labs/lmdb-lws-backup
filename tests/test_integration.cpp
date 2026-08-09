// End-to-end test: drives the real lmdb-lws-backup binary (argv[1]) against
// a synthetic monero-lws-shaped LMDB environment while a writer thread
// mutates it, mimicking the live scanner. Verifies:
//   - repeated --once runs succeed and produce verifiable backups
//   - the reader table stays flat (no leaked slots) across runs
//   - manifests carry correct account counts / scan heights
//   - no plaintext or .partial files are left in the destination
//   - retention prunes pre-seeded old backups correctly
//   - --verify detects a corrupted file
//   - --restore produces a walkable plain LMDB env
//   - --daemon shuts down cleanly on SIGTERM

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "crypto.h"
#include "hot_copy.h"
#include "lws_schema.h"
#include "manifest.h"
#include "retention.h"
#include "test_util.h"

using namespace lwsbk;

namespace {

constexpr int kNumAccounts = 50;
constexpr uint64_t kBaseHeight = 3181000;
const char kKeyB64[] = "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=";

TempDir* g_tmp = nullptr;
std::string g_binary;
std::string g_config_path;
std::string g_src_dir;
std::string g_dest_dir;

Env g_src;
std::vector<LwsAccount> g_accounts;  // shadow copies for dupsort updates
std::atomic<bool> g_writer_stop{false};
std::atomic<uint64_t> g_writer_iterations{0};

// ---- synthetic monero-lws environment ------------------------------------

void put_dup(MDB_txn* txn, const char* table, const void* key, size_t klen,
             const void* val, size_t vlen) {
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(txn, table, MDB_CREATE | MDB_DUPSORT, &dbi), table);
  MDB_val k{klen, const_cast<void*>(key)};
  MDB_val v{vlen, const_cast<void*>(val)};
  mdb_check(mdb_put(txn, dbi, &k, &v, 0), table);
}

void build_synthetic_env() {
  std::filesystem::create_directory(g_src_dir);
  g_src = Env::open_readwrite(g_src_dir, 32, 256u << 20, false);

  MDB_txn* txn = nullptr;
  mdb_check(mdb_txn_begin(g_src.get(), nullptr, 0, &txn), "txn");

  // Every current monero-lws table exists, even if empty.
  for (const char* name : kLwsTables) {
    MDB_dbi dbi;
    mdb_check(mdb_dbi_open(txn, name, MDB_CREATE | MDB_DUPSORT, &dbi), name);
  }

  for (int i = 0; i < kNumAccounts; ++i) {
    LwsAccount a{};
    a.id = static_cast<uint32_t>(i + 1);
    a.access = 1700000000 + i;
    std::memset(a.view_public, i + 1, sizeof(a.view_public));
    std::memset(a.spend_public, i + 2, sizeof(a.spend_public));
    std::memset(a.view_key, i + 3, sizeof(a.view_key));
    a.scan_height = kBaseHeight + static_cast<uint64_t>(i);
    a.start_height = 3000000;
    a.creation = 1690000000;
    uint8_t status = static_cast<uint8_t>(i % 3);  // spread across statuses
    put_dup(txn, kAccountsTable, &status, 1, &a, sizeof(a));
    g_accounts.push_back(a);

    // A few plausible rows in the big tables.
    uint32_t account_id = a.id;
    unsigned char blob[120];
    std::memset(blob, i, sizeof(blob));
    put_dup(txn, "outputs_v2_by_account_id,block_id,tx_hash,output_id",
            &account_id, sizeof(account_id), blob, sizeof(blob));
    put_dup(txn, "spends_v1_by_account_id,block_id,tx_hash,image", &account_id,
            sizeof(account_id), blob, 96);
  }
  uint64_t h = kBaseHeight;
  unsigned char hash[40] = {9};
  unsigned key0 = 0;
  put_dup(txn, "blocks_by_id", &key0, sizeof(key0), hash, sizeof(hash));
  (void)h;
  mdb_check(mdb_txn_commit(txn), "commit");
}

// Continuously bumps scan heights and appends output rows, the way the live
// scanner thread does.
void writer_loop() {
  uint64_t iter = 0;
  while (!g_writer_stop.load()) {
    ++iter;
    size_t idx = iter % g_accounts.size();
    LwsAccount& shadow = g_accounts[idx];
    uint8_t status = static_cast<uint8_t>(idx % 3);

    MDB_txn* txn = nullptr;
    if (mdb_txn_begin(g_src.get(), nullptr, 0, &txn) != MDB_SUCCESS) break;
    MDB_dbi dbi;
    if (mdb_dbi_open(txn, kAccountsTable, MDB_DUPSORT, &dbi) == MDB_SUCCESS) {
      // dupsort update = delete exact old value, insert bumped one
      MDB_val k{1, &status};
      MDB_val v{sizeof(shadow), &shadow};
      mdb_del(txn, dbi, &k, &v);
      LwsAccount bumped = shadow;
      bumped.scan_height += 1;
      MDB_val nv{sizeof(bumped), &bumped};
      if (mdb_put(txn, dbi, &k, &nv, 0) == MDB_SUCCESS) shadow = bumped;

      uint32_t account_id = shadow.id;
      unsigned char blob[120];
      std::memset(blob, static_cast<int>(iter & 0xff), sizeof(blob));
      MDB_dbi odbi;
      if (mdb_dbi_open(txn, "outputs_v2_by_account_id,block_id,tx_hash,output_id",
                       MDB_DUPSORT, &odbi) == MDB_SUCCESS) {
        MDB_val ok{sizeof(account_id), &account_id};
        MDB_val ov{sizeof(blob), blob};
        mdb_put(txn, odbi, &ok, &ov, 0);
      }
      mdb_txn_commit(txn);
      g_writer_iterations.fetch_add(1);
    } else {
      mdb_txn_abort(txn);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// ---- subprocess driver ----------------------------------------------------

int run_tool(const std::vector<std::string>& extra_args,
             std::string* output = nullptr, int timeout_sec = 120) {
  std::string out_path = g_tmp->sub("tool-output.txt");
  pid_t pid = fork();
  if (pid == 0) {
    int ofd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(ofd, STDOUT_FILENO);
    dup2(ofd, STDERR_FILENO);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(g_binary.c_str()));
    std::string cfg_flag = "--config";
    argv.push_back(const_cast<char*>(cfg_flag.c_str()));
    argv.push_back(const_cast<char*>(g_config_path.c_str()));
    for (const auto& a : extra_args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(g_binary.c_str(), argv.data());
    _exit(126);
  }
  int status = 0;
  for (int waited_ms = 0;; waited_ms += 50) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (waited_ms > timeout_sec * 1000) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      std::fprintf(stderr, "tool timed out\n");
      return -1;
    }
    usleep(50 * 1000);
  }
  if (output) {
    std::ifstream f(out_path);
    std::stringstream ss;
    ss << f.rdbuf();
    *output = ss.str();
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

void write_config() {
  g_config_path = g_tmp->sub("config.toml");
  std::ofstream f(g_config_path);
  f << "[source]\ndb_path = \"" << g_src_dir << "\"\n"
    << "[backup]\ndestination_dir = \"" << g_dest_dir << "\"\n"
    << "retention_days = 30\nschedule_time = \"03:00\"\ntimezone = \"UTC\"\n"
    << "chunk_size = 65536\n"
    << "[encryption]\nkey_env_var = \"LWSBK_ITEST_KEY\"\n"
    << "[logging]\nlevel = \"info\"\n";
}

std::string today_utc() {
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
  return buf;
}

// ---- tests ----------------------------------------------------------------

void test_repeated_backups_under_load() {
  const int baseline_readers = reader_count(g_src.get());
  CHECK(baseline_readers == 0);

  for (int run = 0; run < 3; ++run) {
    uint64_t before_iters = g_writer_iterations.load();
    std::string out;
    int rc = run_tool({"--once"}, &out);
    CHECK_MSG(rc == 0, "run %d failed (rc=%d):\n%s", run, rc, out.c_str());
    CHECK_MSG(reader_count(g_src.get()) == 0,
              "leaked reader slot after run %d", run);
    // Writer must have made progress during the backup (it was truly hot).
    if (run > 0) CHECK(g_writer_iterations.load() > before_iters);
  }

  // Exactly one backup for today (same-day rerun overwrote atomically).
  auto backups = scan_backups(g_dest_dir);
  CHECK(backups.size() == 1);
  CHECK(backups[0].date == today_utc());
  CHECK(backups[0].manifest_path.has_value());

  Manifest m = read_manifest_file(*backups[0].manifest_path);
  CHECK_MSG(m.account_count == kNumAccounts, "manifest count %llu",
            static_cast<unsigned long long>(m.account_count));
  CHECK(m.scan_height_min && *m.scan_height_min >= kBaseHeight);
  CHECK(m.scan_height_max &&
        *m.scan_height_max <= kBaseHeight + kNumAccounts +
                                  g_writer_iterations.load() + 1);
  CHECK(!m.sha256_of_encrypted_file.empty());
  CHECK(sha256_file_hex(backups[0].enc_path) == m.sha256_of_encrypted_file);

  // Destination contains only .enc + .manifest.json — nothing plaintext,
  // no leftovers.
  for (const auto& de : std::filesystem::directory_iterator(g_dest_dir)) {
    std::string name = de.path().filename().string();
    bool ok = name.ends_with(".lmdbbak.enc") || name.ends_with(".manifest.json");
    CHECK_MSG(ok, "unexpected file in destination: %s", name.c_str());
  }
}

void test_verify_and_corruption_detection() {
  auto backups = scan_backups(g_dest_dir);
  CHECK(backups.size() == 1);

  std::string out;
  int rc = run_tool({"--verify", backups[0].enc_path}, &out);
  CHECK_MSG(rc == 0, "verify failed:\n%s", out.c_str());
  CHECK(out.find("verify ok") != std::string::npos);
  CHECK(out.find("manifest:        matches") != std::string::npos);

  // Corrupt a copy (different date so it gets its own name).
  std::string bad =
      g_dest_dir + "/" + backup_filename_for_date("20200101");
  std::filesystem::copy_file(backups[0].enc_path, bad);
  {
    std::fstream f(bad, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(1024);
    char b = 0;
    f.read(&b, 1);
    f.seekp(1024);
    b = static_cast<char>(b ^ 0x01);
    f.write(&b, 1);
  }
  rc = run_tool({"--verify", bad}, &out);
  CHECK_MSG(rc != 0, "corrupted file passed verify!\n%s", out.c_str());
  std::filesystem::remove(bad);
}

void test_retention_prunes_preseeded() {
  // Pre-seed 35 fake old backups (raw junk files are fine for retention).
  std::string today = today_utc();
  for (int i = 1; i <= 35; ++i) {
    std::string d = yyyymmdd_minus_days(today, i);
    std::ofstream(g_dest_dir + "/" + backup_filename_for_date(d)) << "junk";
    std::ofstream(g_dest_dir + "/" + manifest_filename_for_date(d)) << "{}";
  }
  CHECK(scan_backups(g_dest_dir).size() == 36);

  std::string out;
  int rc = run_tool({"--once"}, &out);
  CHECK_MSG(rc == 0, "backup failed:\n%s", out.c_str());

  auto after = scan_backups(g_dest_dir);
  CHECK_MSG(after.size() == 30, "expected 30 after retention, got %zu",
            after.size());
  CHECK(after.back().date == today);
  CHECK(after.front().date == yyyymmdd_minus_days(today, 29));
}

void test_restore() {
  auto backups = scan_backups(g_dest_dir);
  std::string newest = backups.back().enc_path;
  std::string to = g_tmp->sub("restored");

  std::string out;
  int rc = run_tool({"--restore", newest, "--to", to}, &out);
  CHECK_MSG(rc == 0, "restore failed:\n%s", out.c_str());
  CHECK(std::filesystem::exists(to + "/data.mdb"));

  Env restored = Env::open_readonly(to, 32);
  ReadTxn txn(restored.get());
  AccountsSummary s = walk_accounts(txn.get());
  CHECK(s.account_count == kNumAccounts);

  // Second restore to the same target must refuse.
  rc = run_tool({"--restore", newest, "--to", to}, &out);
  CHECK_MSG(rc != 0, "restore overwrote an existing target");
}

void test_list() {
  std::string out;
  int rc = run_tool({"--list"}, &out);
  CHECK_MSG(rc == 0, "list failed:\n%s", out.c_str());
  CHECK(out.find(today_utc().substr(0, 4)) != std::string::npos);
  CHECK(out.find(std::to_string(kNumAccounts)) != std::string::npos);
}

void test_daemon_sigterm() {
  pid_t pid = fork();
  if (pid == 0) {
    int ofd = ::open(g_tmp->sub("daemon-out.txt").c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(ofd, STDOUT_FILENO);
    dup2(ofd, STDERR_FILENO);
    execl(g_binary.c_str(), g_binary.c_str(), "--config",
          g_config_path.c_str(), "--daemon", static_cast<char*>(nullptr));
    _exit(126);
  }
  usleep(500 * 1000);  // let it reach the wait loop
  kill(pid, SIGTERM);

  int status = 0;
  for (int waited_ms = 0;; waited_ms += 50) {
    if (waitpid(pid, &status, WNOHANG) == pid) break;
    if (waited_ms > 5000) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      CHECK_MSG(false, "daemon did not exit within 5s of SIGTERM");
      return;
    }
    usleep(50 * 1000);
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  std::ifstream f(g_tmp->sub("daemon-out.txt"));
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK_MSG(ss.str().find("shutting down cleanly") != std::string::npos,
            "daemon output:\n%s", ss.str().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: test_integration <path-to-binary>\n");
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  g_binary = argv[1];

  TempDir tmp("itest");
  g_tmp = &tmp;
  g_src_dir = tmp.sub("lws-db");
  g_dest_dir = tmp.sub("backups");
  setenv("LWSBK_ITEST_KEY", kKeyB64, 1);

  build_synthetic_env();
  write_config();

  std::thread writer(writer_loop);

  RUN(test_repeated_backups_under_load);
  RUN(test_verify_and_corruption_detection);
  RUN(test_retention_prunes_preseeded);
  RUN(test_restore);
  RUN(test_list);
  RUN(test_daemon_sigterm);

  g_writer_stop.store(true);
  writer.join();
  std::fprintf(stderr, "writer made %llu commits during the test\n",
               static_cast<unsigned long long>(g_writer_iterations.load()));
  g_src.reset();

  return test_exit();
}
