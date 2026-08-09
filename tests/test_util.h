#pragma once

// Minimal dependency-free test harness: each test file is one executable;
// CHECK failures are counted and reported, main() exits non-zero if any.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

inline int t_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,        \
                   __LINE__, #cond);                                       \
      ++t_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_MSG(cond, ...)                                               \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s — ", __FILE__,       \
                   __LINE__, #cond);                                       \
      std::fprintf(stderr, __VA_ARGS__);                                   \
      std::fprintf(stderr, "\n");                                          \
      ++t_failures;                                                        \
    }                                                                      \
  } while (0)

#define CHECK_THROWS(expr)                                                 \
  do {                                                                     \
    bool t_threw = false;                                                  \
    try {                                                                  \
      (void)(expr);                                                        \
    } catch (const std::exception&) {                                      \
      t_threw = true;                                                      \
    }                                                                      \
    CHECK_MSG(t_threw, "expected %s to throw", #expr);                     \
  } while (0)

#define RUN(fn)                                                            \
  do {                                                                     \
    std::fprintf(stderr, "== %s\n", #fn);                                  \
    fn();                                                                  \
  } while (0)

// Self-deleting temporary directory under $TMPDIR.
struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const char* tag) {
    std::string tmpl =
        (std::filesystem::temp_directory_path() /
         (std::string("lwsbk-") + tag + "-XXXXXX"))
            .string();
    char* buf = tmpl.data();
    if (!mkdtemp(buf)) {
      std::perror("mkdtemp");
      std::abort();
    }
    path = buf;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
  std::string sub(const char* name) const { return (path / name).string(); }
};

inline int test_exit() {
  if (t_failures) {
    std::fprintf(stderr, "FAILED: %d check(s)\n", t_failures);
    return 1;
  }
  std::fprintf(stderr, "OK\n");
  return 0;
}
