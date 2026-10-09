#include "config.h"

#include <cctype>
#include <set>
#include <stdexcept>

#include <toml.hpp>

#include "log.h"

namespace lwsbk {

namespace {

void warn_unknown_keys(const toml::table& tbl, const std::string& section,
                       const std::set<std::string>& known) {
  for (const auto& [key, val] : tbl) {
    (void)val;
    std::string k(key.str());
    if (!known.count(k))
      LWSBK_WARN("config: unknown key '%s' in [%s] — ignored", k.c_str(),
                 section.empty() ? "(top level)" : section.c_str());
  }
}

template <typename T>
void read_value(const toml::table* tbl, const char* key, T& out,
                const std::string& section) {
  if (!tbl) return;
  if (const toml::node* n = tbl->get(key)) {
    if (auto v = n->value<T>()) {
      out = *v;
    } else {
      throw std::runtime_error("config: [" + section + "] " + key +
                               " has the wrong type");
    }
  }
}

constexpr unsigned kMaxNamedDbsLimit = 4096;
constexpr std::size_t kMaxPrefixLen = 100;

bool is_prefix_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

void validate_filename_prefix(const std::string& p) {
  if (p.empty())
    throw std::runtime_error("config: filename_prefix must not be empty");
  if (p.size() > kMaxPrefixLen)
    throw std::runtime_error("config: filename_prefix must be at most " +
                             std::to_string(kMaxPrefixLen) + " characters");
  if (p[0] == '.' || p[0] == '-')
    throw std::runtime_error(
        "config: filename_prefix must not start with '.' or '-', got '" + p +
        "'");
  for (char c : p)
    if (!is_prefix_char(c))
      throw std::runtime_error(
          "config: filename_prefix may contain only letters, digits, '.', '_' "
          "and '-', got '" +
          p + "'");
}

}  // namespace

Config load_config(const std::string& path) {
  Config cfg;
  toml::table root;
  try {
    root = toml::parse_file(path);
  } catch (const toml::parse_error& e) {
    std::ostringstream os;
    os << "cannot parse config " << path << ": " << e.description()
       << " (line " << e.source().begin.line << ")";
    throw std::runtime_error(os.str());
  }

  warn_unknown_keys(root, "",
                    {"source", "backup", "encryption", "verify", "logging",
                     "alerting"});

  const toml::table* source = root["source"].as_table();
  if (source)
    warn_unknown_keys(*source, "source",
                      {"db_path", "profile", "max_named_dbs"});
  read_value<std::string>(source, "db_path", cfg.db_path, "source");
  read_value<std::string>(source, "profile", cfg.profile, "source");
  int64_t named_dbs = cfg.max_named_dbs;
  read_value<int64_t>(source, "max_named_dbs", named_dbs, "source");
  if (named_dbs < 1 || named_dbs > static_cast<int64_t>(kMaxNamedDbsLimit))
    throw std::runtime_error("config: [source] max_named_dbs must be in 1.." +
                             std::to_string(kMaxNamedDbsLimit));
  cfg.max_named_dbs = static_cast<unsigned>(named_dbs);

  const toml::table* backup = root["backup"].as_table();
  if (backup)
    warn_unknown_keys(*backup, "backup",
                      {"destination_dir", "retention_days", "schedule_time",
                       "timezone", "chunk_size", "filename_prefix"});
  read_value<std::string>(backup, "destination_dir", cfg.destination_dir,
                          "backup");
  read_value<std::string>(backup, "filename_prefix", cfg.filename_prefix,
                          "backup");
  int64_t retention = cfg.retention_days;
  read_value<int64_t>(backup, "retention_days", retention, "backup");
  cfg.retention_days = static_cast<int>(retention);
  read_value<std::string>(backup, "schedule_time", cfg.schedule_time, "backup");
  read_value<std::string>(backup, "timezone", cfg.timezone, "backup");
  int64_t chunk = cfg.chunk_size;
  read_value<int64_t>(backup, "chunk_size", chunk, "backup");
  if (chunk <= 0 || chunk > static_cast<int64_t>(kMaxChunkSize))
    throw std::runtime_error("config: [backup] chunk_size out of range");
  cfg.chunk_size = static_cast<uint32_t>(chunk);

  const toml::table* enc = root["encryption"].as_table();
  if (enc) warn_unknown_keys(*enc, "encryption", {"key_env_var", "key_file"});
  read_value<std::string>(enc, "key_env_var", cfg.key_env_var, "encryption");
  read_value<std::string>(enc, "key_file", cfg.key_file, "encryption");

  const toml::table* verify = root["verify"].as_table();
  if (verify) warn_unknown_keys(*verify, "verify", {"scratch_dir"});
  read_value<std::string>(verify, "scratch_dir", cfg.verify_scratch_dir,
                          "verify");

  const toml::table* logging = root["logging"].as_table();
  if (logging) warn_unknown_keys(*logging, "logging", {"log_path", "level"});
  read_value<std::string>(logging, "log_path", cfg.log_path, "logging");
  read_value<std::string>(logging, "level", cfg.log_level, "logging");

  const toml::table* alerting = root["alerting"].as_table();
  if (alerting)
    warn_unknown_keys(*alerting, "alerting", {"on_failure_webhook"});
  read_value<std::string>(alerting, "on_failure_webhook",
                          cfg.on_failure_webhook, "alerting");

  return cfg;
}

void validate_config(const Config& cfg) {
  if (cfg.db_path.empty())
    throw std::runtime_error("config: source db_path must not be empty");
  if (cfg.profile != "auto" && cfg.profile != "monero-lws" &&
      cfg.profile != "generic")
    throw std::runtime_error(
        "config: profile must be one of auto, monero-lws, generic — got '" +
        cfg.profile + "'");
  if (cfg.max_named_dbs < 1 || cfg.max_named_dbs > kMaxNamedDbsLimit)
    throw std::runtime_error("config: max_named_dbs must be in 1.." +
                             std::to_string(kMaxNamedDbsLimit));
  validate_filename_prefix(cfg.filename_prefix);
  if (cfg.destination_dir.empty())
    throw std::runtime_error("config: backup destination_dir must not be empty");
  if (cfg.retention_days < 1 || cfg.retention_days > 3650)
    throw std::runtime_error("config: retention_days must be in 1..3650");

  const std::string& t = cfg.schedule_time;
  bool ok = t.size() == 5 && std::isdigit(static_cast<unsigned char>(t[0])) &&
            std::isdigit(static_cast<unsigned char>(t[1])) && t[2] == ':' &&
            std::isdigit(static_cast<unsigned char>(t[3])) &&
            std::isdigit(static_cast<unsigned char>(t[4]));
  if (ok) {
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[3] - '0') * 10 + (t[4] - '0');
    ok = hh < 24 && mm < 60;
  }
  if (!ok)
    throw std::runtime_error("config: schedule_time must be HH:MM, got '" + t +
                             "'");

  if (cfg.timezone.empty())
    throw std::runtime_error("config: timezone must not be empty");

  const bool has_env = !cfg.key_env_var.empty();
  const bool has_file = !cfg.key_file.empty();
  if (!has_env && !has_file)
    throw std::runtime_error(
        "config: no encryption key source — set [encryption] key_env_var or "
        "key_file; refusing to run without encryption");
  if (has_env && has_file)
    throw std::runtime_error(
        "config: both key_env_var and key_file are set — choose exactly one");

  log_level_from_string(cfg.log_level);  // throws on bad value
}

SecretKey load_key(const Config& cfg) {
  if (!cfg.key_env_var.empty()) return SecretKey::from_env(cfg.key_env_var);
  return SecretKey::from_file(cfg.key_file);
}

}  // namespace lwsbk
