#include "cli.h"

#include <getopt.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "alert.h"
#include "backup.h"
#include "config.h"
#include "log.h"
#include "retention.h"
#include "scheduler.h"
#include "version.h"

namespace fs = std::filesystem;

namespace lwsbk {

namespace {

constexpr const char kDefaultConfig[] = "/etc/lmdb-lws-backup/config.toml";

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class Mode { none, once, daemon, verify, list, restore };

struct Options {
  std::string config_path = kDefaultConfig;
  Mode mode = Mode::none;
  std::string verify_file;
  std::string restore_file;
  std::string restore_to;
  std::optional<std::string> db_path;
  std::optional<std::string> profile;
  std::optional<std::string> destination;
  std::optional<int> retention_days;
};

void print_usage(FILE* out) {
  std::fprintf(out,
"lmdb-lws-backup %s — encrypted hot backups for any LMDB database\n"
"\n"
"Usage:\n"
"  lmdb-lws-backup --config <file> --once\n"
"  lmdb-lws-backup --config <file> --daemon\n"
"  lmdb-lws-backup --config <file> --verify <backup-file>\n"
"  lmdb-lws-backup --config <file> --list\n"
"  lmdb-lws-backup --config <file> --restore <backup-file> --to <path>\n"
"\n"
"Modes (exactly one required):\n"
"  --once                 run a single backup cycle and exit (cron/systemd timer)\n"
"  --daemon               internal scheduler; runs daily at [backup] schedule_time\n"
"  --verify <file>        decrypt + integrity-walk an existing backup\n"
"  --list                 list retained backups with manifest summaries\n"
"  --restore <file>       decrypt a backup to a plain LMDB env (needs --to)\n"
"\n"
"Options:\n"
"  --config <file>        config file (default %s)\n"
"  --db-path <path>       override [source] db_path\n"
"  --profile <p>          override [source] profile (auto, monero-lws, generic)\n"
"  --destination <dir>    override [backup] destination_dir\n"
"  --retention-days <n>   override [backup] retention_days\n"
"  --to <path>            restore target directory\n"
"  --help, --version\n",
               kToolVersion, kDefaultConfig);
}

void set_mode(Options& o, Mode m) {
  if (o.mode != Mode::none)
    throw UsageError("exactly one of --once/--daemon/--verify/--list/--restore may be given");
  o.mode = m;
}

Options parse_args(int argc, char** argv) {
  enum {
    kConfig = 1000, kOnce, kDaemon, kVerify, kList, kRestore, kTo,
    kDbPath, kProfile, kDestination, kRetention, kHelp, kVersion
  };
  static const struct option longopts[] = {
      {"config", required_argument, nullptr, kConfig},
      {"once", no_argument, nullptr, kOnce},
      {"daemon", no_argument, nullptr, kDaemon},
      {"verify", required_argument, nullptr, kVerify},
      {"list", no_argument, nullptr, kList},
      {"restore", required_argument, nullptr, kRestore},
      {"to", required_argument, nullptr, kTo},
      {"db-path", required_argument, nullptr, kDbPath},
      {"profile", required_argument, nullptr, kProfile},
      {"destination", required_argument, nullptr, kDestination},
      {"retention-days", required_argument, nullptr, kRetention},
      {"help", no_argument, nullptr, kHelp},
      {"version", no_argument, nullptr, kVersion},
      {nullptr, 0, nullptr, 0}};

  Options o;
  optind = 1;
  int c;
  while ((c = getopt_long(argc, argv, "", longopts, nullptr)) != -1) {
    switch (c) {
      case kConfig: o.config_path = optarg; break;
      case kOnce: set_mode(o, Mode::once); break;
      case kDaemon: set_mode(o, Mode::daemon); break;
      case kVerify:
        set_mode(o, Mode::verify);
        o.verify_file = optarg;
        break;
      case kList: set_mode(o, Mode::list); break;
      case kRestore:
        set_mode(o, Mode::restore);
        o.restore_file = optarg;
        break;
      case kTo: o.restore_to = optarg; break;
      case kDbPath: o.db_path = optarg; break;
      case kProfile: o.profile = optarg; break;
      case kDestination: o.destination = optarg; break;
      case kRetention: {
        char* end = nullptr;
        long v = strtol(optarg, &end, 10);
        if (!end || *end != '\0' || v < 1 || v > 3650)
          throw UsageError("--retention-days must be an integer in 1..3650");
        o.retention_days = static_cast<int>(v);
        break;
      }
      case kHelp:
        print_usage(stdout);
        std::exit(0);
      case kVersion:
        std::printf("lmdb-lws-backup %s\n", kToolVersion);
        std::exit(0);
      default:
        throw UsageError("unknown option (see --help)");
    }
  }
  if (optind < argc)
    throw UsageError(std::string("unexpected argument: ") + argv[optind]);
  if (o.mode == Mode::none)
    throw UsageError("one of --once/--daemon/--verify/--list/--restore is required");
  if (o.mode == Mode::restore && o.restore_to.empty())
    throw UsageError("--restore requires --to <path>");
  if (o.mode != Mode::restore && !o.restore_to.empty())
    throw UsageError("--to only makes sense with --restore");
  return o;
}

std::string human_size(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  if (u == 0)
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  else
    snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  return buf;
}

int age_days(const std::string& yyyymmdd, const std::string& today) {
  struct tm a {}, b {};
  auto parse = [](const std::string& d, struct tm& t) {
    t.tm_year = std::stoi(d.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(d.substr(4, 2)) - 1;
    t.tm_mday = std::stoi(d.substr(6, 2));
    t.tm_hour = 12;
  };
  parse(yyyymmdd, a);
  parse(today, b);
  return static_cast<int>((timegm(&b) - timegm(&a)) / 86400);
}

int cmd_once(const Config& cfg) {
  SecretKey key = load_key(cfg);
  Shutdown::install();  // so Ctrl-C mid-backup aborts cleanly, not mid-write
  try {
    BackupOutcome out = run_backup_cycle(
        cfg, key, [] { return Shutdown::requested(); });
    const Manifest& m = out.manifest;
    const bool lws = m.profile == "monero-lws";
    std::printf("backup ok: %s (%s, %llu %s, %.1fs)\n", out.enc_path.c_str(),
                human_size(m.encrypted_size_bytes).c_str(),
                static_cast<unsigned long long>(
                    lws ? m.account_count : m.total_rows.value_or(0)),
                lws ? "accounts" : "total rows", out.duration_seconds);
    return 0;
  } catch (const std::exception& e) {
    LWSBK_ERROR("backup failed: %s", e.what());
    fire_failure_webhook(cfg.on_failure_webhook, "backup", e.what());
    std::fprintf(stderr, "backup failed: %s\n", e.what());
    return 1;
  }
}

int cmd_verify(const Config& cfg, const std::string& file) {
  SecretKey key = load_key(cfg);
  try {
    VerifyResult vr = verify_backup_file(cfg, file, key, nullptr);
    const bool lws = vr.profile == "monero-lws";
    std::printf("verify ok: %s\n", file.c_str());
    std::printf("  profile:         %s\n", vr.profile.c_str());
    if (lws) {
      std::printf("  accounts:        %llu\n",
                  static_cast<unsigned long long>(vr.accounts.account_count));
      if (vr.accounts.scan_height_min)
        std::printf(
            "  scan height:     %llu..%llu\n",
            static_cast<unsigned long long>(*vr.accounts.scan_height_min),
            static_cast<unsigned long long>(*vr.accounts.scan_height_max));
    }
    std::printf("  tables:          %llu\n",
                static_cast<unsigned long long>(vr.table_count));
    std::printf("  total rows:      %llu\n",
                static_cast<unsigned long long>(vr.total_rows));
    std::printf("  plaintext bytes: %llu\n",
                static_cast<unsigned long long>(vr.plaintext_bytes));
    std::printf("  sha256:          %s\n", vr.sha256_hex.c_str());

    // Cross-check the sidecar manifest when present. It is located by
    // suffix rather than by the configured prefix, so backups taken under
    // an earlier filename_prefix are still checked.
    const std::string name = fs::path(file).filename().string();
    const size_t suffix_len = std::strlen(kBackupSuffix);
    if (name.size() > suffix_len && name.ends_with(kBackupSuffix)) {
      fs::path man =
          fs::path(file).parent_path() /
          (name.substr(0, name.size() - suffix_len) + kManifestSuffix);
      if (fs::exists(man)) {
        Manifest m = read_manifest_file(man.string());
        if (m.sha256_of_encrypted_file != vr.sha256_hex) {
          std::fprintf(stderr,
                       "MISMATCH: manifest sha256 differs from file — the "
                       "encrypted file was modified after backup\n");
          return 1;
        }
        // Accounts are only counted under monero-lws, so a forced generic
        // profile has nothing to compare against an lws manifest.
        if (m.profile != "generic" && lws &&
            m.account_count != vr.accounts.account_count) {
          std::fprintf(stderr,
                       "MISMATCH: manifest account_count %llu != %llu\n",
                       static_cast<unsigned long long>(m.account_count),
                       static_cast<unsigned long long>(
                           vr.accounts.account_count));
          return 1;
        }
        if (m.total_rows && *m.total_rows != vr.total_rows) {
          std::fprintf(stderr, "MISMATCH: manifest total_rows %llu != %llu\n",
                       static_cast<unsigned long long>(*m.total_rows),
                       static_cast<unsigned long long>(vr.total_rows));
          return 1;
        }
        std::printf("  manifest:        matches\n");
      } else {
        std::printf("  manifest:        (none found)\n");
      }
    }
    return 0;
  } catch (const std::exception& e) {
    LWSBK_ERROR("verify failed for %s: %s", file.c_str(), e.what());
    fire_failure_webhook(cfg.on_failure_webhook, "verify", e.what());
    std::fprintf(stderr, "verify FAILED: %s\n", e.what());
    return 1;
  }
}

int cmd_list(const Config& cfg) {
  if (!fs::exists(cfg.destination_dir)) {
    std::printf("no backups (destination %s does not exist)\n",
                cfg.destination_dir.c_str());
    return 0;
  }
  auto backups = scan_backups(cfg.destination_dir, cfg.filename_prefix);
  if (backups.empty()) {
    std::printf("no backups in %s\n", cfg.destination_dir.c_str());
    return 0;
  }
  const std::string today = current_date_string(cfg);
  std::printf("%-10s %5s %10s %10s %-21s %s\n", "DATE", "AGE", "SIZE",
              "ITEMS", "SCAN-HEIGHTS", "FILE");
  for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
    const BackupEntry& e = *it;
    std::error_code ec;
    uint64_t size = fs::file_size(e.enc_path, ec);
    std::string items = "-", heights = "-";
    if (e.manifest_path) {
      try {
        Manifest m = read_manifest_file(*e.manifest_path);
        // Accounts for monero-lws and pre-0.2 manifests, rows for generic.
        if (m.profile != "generic")
          items = std::to_string(m.account_count);
        else if (m.total_rows)
          items = std::to_string(*m.total_rows);
        if (m.scan_height_min)
          heights = std::to_string(*m.scan_height_min) + ".." +
                    std::to_string(*m.scan_height_max);
      } catch (const std::exception&) {
        items = "bad-manifest";
      }
    }
    std::printf("%-10s %4dd %10s %10s %-21s %s\n", e.date.c_str(),
                age_days(e.date, today), human_size(ec ? 0 : size).c_str(),
                items.c_str(), heights.c_str(),
                fs::path(e.enc_path).filename().string().c_str());
  }
  return 0;
}

int cmd_restore(const Config& cfg, const std::string& file,
                const std::string& to) {
  SecretKey key = load_key(cfg);
  VerifyResult vr = restore_backup(cfg, file, to, key);
  std::printf("restored %s -> %s/data.mdb\n", file.c_str(), to.c_str());
  if (vr.profile == "monero-lws") {
    std::printf("  accounts: %llu, total rows: %llu\n",
                static_cast<unsigned long long>(vr.accounts.account_count),
                static_cast<unsigned long long>(vr.total_rows));
    std::printf(
        "note: this is a plain, DECRYPTED LMDB environment containing view "
        "keys.\nStarting monero-lws against it is an operator decision.\n");
  } else {
    std::printf("  tables: %llu, total rows: %llu\n",
                static_cast<unsigned long long>(vr.table_count),
                static_cast<unsigned long long>(vr.total_rows));
    std::printf(
        "note: this is a plain, DECRYPTED LMDB environment — %s contains the "
        "decrypted\ndatabase contents. Protect or delete it accordingly.\n",
        to.c_str());
  }
  return 0;
}

}  // namespace

int run_cli(int argc, char** argv) {
  Options opts;
  try {
    opts = parse_args(argc, argv);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "error: %s\n\n", e.what());
    print_usage(stderr);
    return 2;
  }

  Config cfg;
  try {
    cfg = load_config(opts.config_path);
    if (opts.db_path) cfg.db_path = *opts.db_path;
    if (opts.profile) cfg.profile = *opts.profile;
    if (opts.destination) cfg.destination_dir = *opts.destination;
    if (opts.retention_days) cfg.retention_days = *opts.retention_days;
    validate_config(cfg);
    Logger::set_level(log_level_from_string(cfg.log_level));
    if (!cfg.log_path.empty()) Logger::init_file(cfg.log_path);
    apply_timezone(cfg.timezone);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  }

  switch (opts.mode) {
    case Mode::once: return cmd_once(cfg);
    case Mode::daemon: {
      SecretKey key = load_key(cfg);
      return run_daemon(cfg, key);
    }
    case Mode::verify: return cmd_verify(cfg, opts.verify_file);
    case Mode::list: return cmd_list(cfg);
    case Mode::restore:
      return cmd_restore(cfg, opts.restore_file, opts.restore_to);
    case Mode::none: break;
  }
  return 2;
}

}  // namespace lwsbk
