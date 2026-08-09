#include "log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace lwsbk {

namespace {

std::mutex g_mutex;
int g_fd = -1;
LogLevel g_level = LogLevel::info;

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO";
    case LogLevel::warn: return "WARN";
    case LogLevel::error: return "ERROR";
  }
  return "?";
}

// yyyy-mm-ddThh:mm:ss.mmmZ
void format_timestamp(char* buf, size_t len) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm_utc;
  gmtime_r(&tv.tv_sec, &tm_utc);
  size_t n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm_utc);
  snprintf(buf + n, len - n, ".%03ldZ", static_cast<long>(tv.tv_usec / 1000));
}

void write_all(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // logging must never throw or loop forever
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
}

}  // namespace

LogLevel log_level_from_string(const std::string& s) {
  if (s == "debug") return LogLevel::debug;
  if (s == "info") return LogLevel::info;
  if (s == "warn") return LogLevel::warn;
  if (s == "error") return LogLevel::error;
  throw std::invalid_argument("unknown log level: '" + s +
                              "' (expected debug|info|warn|error)");
}

void Logger::set_level(LogLevel level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_level = level;
}

LogLevel Logger::level() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_level;
}

void Logger::init_file(const std::string& path) {
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw std::runtime_error("cannot open log file " + path + ": " +
                             std::strerror(errno));
  }
  // The file may predate us with looser permissions; tighten them.
  if (fchmod(fd, 0600) != 0) {
    ::close(fd);
    throw std::runtime_error("cannot chmod log file " + path + ": " +
                             std::strerror(errno));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd >= 0) ::close(g_fd);
  g_fd = fd;
}

void Logger::close() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_fd >= 0) {
    ::close(g_fd);
    g_fd = -1;
  }
}

void Logger::log(LogLevel level, const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (level < g_level) return;

  char ts[40];
  format_timestamp(ts, sizeof(ts));

  char msg[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char line[4200];
  int n = snprintf(line, sizeof(line), "%s %-5s %s\n", ts, level_name(level), msg);
  if (n < 0) return;
  size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n)
                                                     : sizeof(line) - 1;

  if (g_fd >= 0) {
    write_all(g_fd, line, len);
    if (level >= LogLevel::warn) write_all(STDERR_FILENO, line, len);
  } else {
    write_all(STDERR_FILENO, line, len);
  }
}

}  // namespace lwsbk
