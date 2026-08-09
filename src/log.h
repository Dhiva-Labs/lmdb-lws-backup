#pragma once

#include <string>

namespace lwsbk {

enum class LogLevel { debug = 0, info = 1, warn = 2, error = 3 };

// Throws std::invalid_argument on unknown level name.
LogLevel log_level_from_string(const std::string& s);

// Process-wide logger. Writes to stderr until init_file() is called; after
// that, every message at or above the configured level goes to the file
// (0600) and warnings/errors are mirrored to stderr. All methods are
// thread-safe. Messages must never contain key material — callers are
// responsible for redaction.
class Logger {
public:
  static void set_level(LogLevel level);
  static LogLevel level();

  // Opens (appending) the log file with 0600 permissions. Throws
  // std::runtime_error on failure.
  static void init_file(const std::string& path);
  static void close();

  static void log(LogLevel level, const char* fmt, ...)
      __attribute__((format(printf, 2, 3)));
};

}  // namespace lwsbk

#define LWSBK_DEBUG(...) ::lwsbk::Logger::log(::lwsbk::LogLevel::debug, __VA_ARGS__)
#define LWSBK_INFO(...) ::lwsbk::Logger::log(::lwsbk::LogLevel::info, __VA_ARGS__)
#define LWSBK_WARN(...) ::lwsbk::Logger::log(::lwsbk::LogLevel::warn, __VA_ARGS__)
#define LWSBK_ERROR(...) ::lwsbk::Logger::log(::lwsbk::LogLevel::error, __VA_ARGS__)
