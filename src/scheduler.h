#pragma once

#include <ctime>
#include <string>

#include "config.h"
#include "crypto.h"

namespace lwsbk {

// "UTC" → TZ=UTC, "local" → leave process TZ untouched, anything else is
// treated as an IANA zone name and exported as TZ. Call once at startup,
// before any date formatting.
void apply_timezone(const std::string& tz);

// Epoch of the next strictly-future occurrence of HH:MM in the process
// timezone (DST-aware via mktime).
time_t next_run_epoch(const std::string& hhmm, time_t now);

// Self-pipe SIGINT/SIGTERM handling. install() is idempotent.
class Shutdown {
 public:
  static void install();
  static bool requested();
  static int fd();  // poll()able; readable once a signal has arrived
};

// --daemon: sleep until schedule_time, run a backup cycle, repeat. A cycle
// failure is logged and alerted but does not exit the daemon. Returns the
// process exit code.
int run_daemon(const Config& cfg, const SecretKey& key);

}  // namespace lwsbk
