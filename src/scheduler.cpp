#include "scheduler.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "alert.h"
#include "backup.h"
#include "log.h"
#include "retention.h"

namespace lwsbk {

namespace {

int g_sig_pipe[2] = {-1, -1};
volatile sig_atomic_t g_shutdown = 0;

void signal_handler(int) {
  g_shutdown = 1;
  unsigned char b = 1;
  // best-effort wakeup; the pipe is non-blocking
  [[maybe_unused]] ssize_t n = write(g_sig_pipe[1], &b, 1);
}

}  // namespace

void apply_timezone(const std::string& tz) {
  if (tz == "local") return;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

time_t next_run_epoch(const std::string& hhmm, time_t now) {
  int hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  int mm = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');

  struct tm tmv;
  localtime_r(&now, &tmv);
  tmv.tm_hour = hh;
  tmv.tm_min = mm;
  tmv.tm_sec = 0;
  tmv.tm_isdst = -1;
  time_t t = mktime(&tmv);
  if (t == static_cast<time_t>(-1))
    throw std::runtime_error("cannot compute next run time");
  if (t <= now) {
    localtime_r(&now, &tmv);
    tmv.tm_mday += 1;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    t = mktime(&tmv);
    if (t == static_cast<time_t>(-1))
      throw std::runtime_error("cannot compute next run time");
  }
  return t;
}

void Shutdown::install() {
  if (g_sig_pipe[0] >= 0) return;
  if (pipe2(g_sig_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
  struct sigaction sa {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

bool Shutdown::requested() { return g_shutdown != 0; }

int Shutdown::fd() { return g_sig_pipe[0]; }

int run_daemon(const Config& cfg, const SecretKey& key) {
  Shutdown::install();

  std::filesystem::create_directories(cfg.destination_dir);
  cleanup_partials(cfg.destination_dir);

  LWSBK_INFO("daemon started: daily backup at %s (%s), retention %d",
             cfg.schedule_time.c_str(), cfg.timezone.c_str(),
             cfg.retention_days);

  while (!Shutdown::requested()) {
    time_t next = next_run_epoch(cfg.schedule_time, time(nullptr));
    {
      struct tm tmv;
      localtime_r(&next, &tmv);
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tmv);
      LWSBK_INFO("next backup at %s", buf);
    }

    // Sleep in bounded slices so wall-clock jumps (NTP, suspend) are picked
    // up within a minute, waking early on the signal pipe.
    bool fire = false;
    while (!Shutdown::requested()) {
      time_t now = time(nullptr);
      if (now >= next) {
        fire = true;
        break;
      }
      long remaining_ms = static_cast<long>(next - now) * 1000L;
      int timeout = remaining_ms > 60000L ? 60000 : static_cast<int>(remaining_ms);
      struct pollfd pfd {Shutdown::fd(), POLLIN, 0};
      poll(&pfd, 1, timeout);
    }
    if (!fire) break;

    try {
      run_backup_cycle(cfg, key, [] { return Shutdown::requested(); });
    } catch (const std::exception& e) {
      if (Shutdown::requested()) {
        LWSBK_WARN("backup aborted by shutdown request: %s", e.what());
        break;
      }
      LWSBK_ERROR("backup cycle failed: %s", e.what());
      fire_failure_webhook(cfg.on_failure_webhook, "backup", e.what());
      // stay alive; next scheduled run may succeed
    }
  }

  LWSBK_INFO("daemon shutting down cleanly");
  return 0;
}

}  // namespace lwsbk
