#include <signal.h>

#include <cstdio>
#include <exception>

#include "cli.h"
#include "log.h"

int main(int argc, char** argv) {
  // Broken pipes (LMDB copy stream, webhook curl) surface as write() errors
  // we handle; the default SIGPIPE would kill us mid-backup.
  signal(SIGPIPE, SIG_IGN);
  try {
    return lwsbk::run_cli(argc, argv);
  } catch (const std::exception& e) {
    LWSBK_ERROR("fatal: %s", e.what());
    std::fprintf(stderr, "lmdb-lws-backup: %s\n", e.what());
    return 1;
  }
}
