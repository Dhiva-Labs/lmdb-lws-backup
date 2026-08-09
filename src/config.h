#pragma once

#include <cstdint>
#include <string>

#include "crypto.h"

namespace lwsbk {

struct Config {
  // [source]
  std::string db_path = "/home/monero-lws/.bitmonero/light_wallet_server";
  // [backup]
  std::string destination_dir = "/var/backups/monero-lws";
  int retention_days = 30;
  std::string schedule_time = "03:00";  // --daemon only
  std::string timezone = "UTC";         // "UTC", "local", or an IANA name
  uint32_t chunk_size = kDefaultChunkSize;
  // [encryption] — exactly one source must be configured
  std::string key_env_var;
  std::string key_file;
  // [verify]
  // Empty (default): decrypt-for-verify uses anonymous RAM (memfd, falling
  // back to /dev/shm). A directory here overrides that — only for operators
  // who accept transient plaintext on that filesystem.
  std::string verify_scratch_dir;
  // [logging]
  std::string log_path;  // empty → stderr only
  std::string log_level = "info";
  // [alerting]
  std::string on_failure_webhook;
};

// Parses the TOML file; unknown sections/keys produce warnings, type errors
// throw. Does not validate cross-field constraints — call validate_config
// after CLI overrides are applied.
Config load_config(const std::string& path);

// Throws std::runtime_error on invalid combinations (no key source, bad
// schedule_time, retention < 1, bad log level, ...).
void validate_config(const Config& cfg);

// Resolves the encryption key per config (env var or key file).
SecretKey load_key(const Config& cfg);

}  // namespace lwsbk
