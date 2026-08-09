#pragma once

namespace lwsbk {

// Full argument parsing + dispatch. Returns the process exit code:
//   0 success, 1 runtime/verify failure, 2 usage or config error.
int run_cli(int argc, char** argv);

}  // namespace lwsbk
