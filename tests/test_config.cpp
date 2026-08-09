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

[backup]
destination_dir = "/srv/backups"
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
  RUN(test_next_run_epoch);

  return test_exit();
}
