#include "config.h"

#include <fstream>

#include "scheduler.h"
#include "test_util.h"

using namespace lwsbk;

namespace {

TempDir* g_tmp = nullptr;

std::string write_toml(const char* name, const std::string& content) {
  std::string path = g_tmp->sub(name);
  std::ofstream f(path);
  f << content;
  return path;
}

void test_full_config() {
  std::string path = write_toml("full.toml", R"(
[source]
db_path = "/srv/lws/db"
profile = "generic"
max_named_dbs = 512

[backup]
destination_dir = "/srv/backups"
filename_prefix = "my-app.db_v2"
retention_days = 14
schedule_time = "02:30"
timezone = "Europe/Berlin"
chunk_size = 65536

[encryption]
key_env_var = "MY_KEY"

[verify]
scratch_dir = "/mnt/scratch"

[logging]
log_path = "/var/log/x.log"
level = "debug"

[alerting]
on_failure_webhook = "https://example.invalid/hook"
)");
  Config cfg = load_config(path);
  CHECK(cfg.db_path == "/srv/lws/db");
  CHECK(cfg.profile == "generic");
  CHECK(cfg.max_named_dbs == 512);
  CHECK(cfg.filename_prefix == "my-app.db_v2");
  CHECK(cfg.destination_dir == "/srv/backups");
  CHECK(cfg.retention_days == 14);
  CHECK(cfg.schedule_time == "02:30");
  CHECK(cfg.timezone == "Europe/Berlin");
  CHECK(cfg.chunk_size == 65536);
  CHECK(cfg.key_env_var == "MY_KEY");
  CHECK(cfg.key_file.empty());
  CHECK(cfg.verify_scratch_dir == "/mnt/scratch");
  CHECK(cfg.log_path == "/var/log/x.log");
  CHECK(cfg.log_level == "debug");
  CHECK(cfg.on_failure_webhook == "https://example.invalid/hook");
  validate_config(cfg);
}

void test_defaults_and_minimal() {
  std::string path = write_toml("min.toml", R"(
[encryption]
key_env_var = "K"
)");
  Config cfg = load_config(path);
  CHECK(cfg.db_path == "/home/monero-lws/.bitmonero/light_wallet_server");
  CHECK(cfg.profile == "auto");
  CHECK(cfg.max_named_dbs == 128);
  CHECK(cfg.filename_prefix == "lws-backup");
  CHECK(cfg.destination_dir == "/var/backups/monero-lws");
  CHECK(cfg.retention_days == 30);
  CHECK(cfg.schedule_time == "03:00");
  CHECK(cfg.timezone == "UTC");
  CHECK(cfg.log_level == "info");
  CHECK(cfg.on_failure_webhook.empty());
  validate_config(cfg);
}

void test_validation_failures() {
  // No key source at all.
  Config c1 = load_config(write_toml("nokey.toml", "[source]\n"));
  CHECK_THROWS(validate_config(c1));

  // Both key sources.
  Config c2 = load_config(write_toml("bothkeys.toml", R"(
[encryption]
key_env_var = "K"
key_file = "/etc/key"
)"));
  CHECK_THROWS(validate_config(c2));

  // Bad schedule times.
  Config c3 = load_config(write_toml("ok.toml",
                                     "[encryption]\nkey_env_var = \"K\"\n"));
  c3.schedule_time = "25:00";
  CHECK_THROWS(validate_config(c3));
  c3.schedule_time = "12:60";
  CHECK_THROWS(validate_config(c3));
  c3.schedule_time = "3:00";
  CHECK_THROWS(validate_config(c3));
  c3.schedule_time = "03:00";
  validate_config(c3);

  // Retention bounds.
  c3.retention_days = 0;
  CHECK_THROWS(validate_config(c3));
  c3.retention_days = 30;

  // Log level.
  c3.log_level = "verbose";
  CHECK_THROWS(validate_config(c3));
  c3.log_level = "info";

  // Parse errors and bad types throw at load.
  CHECK_THROWS(load_config(write_toml("broken.toml", "[source\n")));
  CHECK_THROWS(load_config(
      write_toml("badtype.toml", "[backup]\nretention_days = \"thirty\"\n")));
  CHECK_THROWS(load_config(g_tmp->sub("nonexistent.toml")));
}

void test_profile_and_prefix_validation() {
  Config cfg = load_config(write_toml("prof.toml",
                                      "[encryption]\nkey_env_var = \"K\"\n"));

  for (const char* ok : {"auto", "monero-lws", "generic"}) {
    cfg.profile = ok;
    validate_config(cfg);
  }
  for (const char* bad : {"bogus", "", "Generic", "monero_lws"}) {
    cfg.profile = bad;
    CHECK_THROWS(validate_config(cfg));
  }
  cfg.profile = "auto";

  for (const char* ok : {"lws-backup", "a", "my.db_v2-x", "9lives", "_x",
                         "UPPER-lower"}) {
    cfg.filename_prefix = ok;
    validate_config(cfg);
  }
  cfg.filename_prefix = std::string(100, 'a');
  validate_config(cfg);

  for (const std::string& bad :
       {std::string("a/b"), std::string("../x"), std::string(".hidden"),
        std::string("-flag"), std::string(), std::string(101, 'a'),
        std::string("has space"), std::string("caf\xc3\xa9"),
        std::string("a\0b", 3)}) {
    cfg.filename_prefix = bad;
    CHECK_THROWS(validate_config(cfg));
  }
  cfg.filename_prefix = "lws-backup";
  validate_config(cfg);

  cfg.max_named_dbs = 0;
  CHECK_THROWS(validate_config(cfg));
  cfg.max_named_dbs = 4097;
  CHECK_THROWS(validate_config(cfg));
  cfg.max_named_dbs = 4096;
  validate_config(cfg);

  // max_named_dbs is range-checked at load, like chunk_size.
  CHECK_THROWS(load_config(
      write_toml("nd0.toml", "[source]\nmax_named_dbs = 0\n")));
  CHECK_THROWS(load_config(
      write_toml("nd5000.toml", "[source]\nmax_named_dbs = 5000\n")));
  CHECK_THROWS(load_config(
      write_toml("ndneg.toml", "[source]\nmax_named_dbs = -1\n")));
  CHECK_THROWS(load_config(
      write_toml("ndstr.toml", "[source]\nmax_named_dbs = \"many\"\n")));
  Config lo =
      load_config(write_toml("nd1.toml", "[source]\nmax_named_dbs = 1\n"));
  CHECK(lo.max_named_dbs == 1);
  Config hi = load_config(
      write_toml("nd4096.toml", "[source]\nmax_named_dbs = 4096\n"));
  CHECK(hi.max_named_dbs == 4096);

  // Bad types for the new string keys throw at load.
  CHECK_THROWS(
      load_config(write_toml("profint.toml", "[source]\nprofile = 3\n")));
  CHECK_THROWS(load_config(
      write_toml("pfxint.toml", "[backup]\nfilename_prefix = 3\n")));
}

void test_next_run_epoch() {
  apply_timezone("UTC");
  // 2026-08-10 12:00:00 UTC
  struct tm tmv {};
  tmv.tm_year = 126;
  tmv.tm_mon = 7;
  tmv.tm_mday = 10;
  tmv.tm_hour = 12;
  time_t now = timegm(&tmv);

  // 15:30 today is still ahead.
  time_t next = next_run_epoch("15:30", now);
  CHECK(next - now == (3 * 3600 + 30 * 60));

  // 03:00 already passed → tomorrow.
  next = next_run_epoch("03:00", now);
  CHECK(next - now == (15 * 3600));

  // Exactly now → tomorrow, never immediate re-fire.
  next = next_run_epoch("12:00", now);
  CHECK(next - now == 24 * 3600);
}

}  // namespace

int main() {
  TempDir tmp("config");
  g_tmp = &tmp;

  RUN(test_full_config);
  RUN(test_defaults_and_minimal);
  RUN(test_validation_failures);
  RUN(test_profile_and_prefix_validation);
  RUN(test_next_run_epoch);

  return test_exit();
}
