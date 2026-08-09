#include "hot_copy.h"

#include <sys/stat.h>

#include <cctype>
#include <cstring>

#include "log.h"

namespace lwsbk {

namespace {

struct stat stat_or_throw(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    throw std::runtime_error("database path does not exist: " + path + " (" +
                             std::strerror(errno) + ")");
  }
  return st;
}

}  // namespace

Env Env::open_readonly(const std::string& path, unsigned max_dbs,
                       bool no_lock) {
  struct stat st = stat_or_throw(path);
  // MDB_NOTLS: reader slots bind to transaction objects instead of threads,
  // so RAII abort releases the slot immediately and mdb_env_copyfd2 can run
  // its own read transaction while we hold one. Purely local bookkeeping —
  // does not change anything for other processes using the same database.
  unsigned flags = MDB_RDONLY | MDB_NOTLS;
  if (S_ISREG(st.st_mode)) {
    flags |= MDB_NOSUBDIR;
  } else if (!S_ISDIR(st.st_mode)) {
    throw std::runtime_error("database path is neither a file nor a directory: " +
                             path);
  }
  if (no_lock) flags |= MDB_NOLOCK;

  MDB_env* raw = nullptr;
  mdb_check(mdb_env_create(&raw), "mdb_env_create");
  Env env;
  env.env_ = raw;  // owned from here on, including if open below fails
  mdb_check(mdb_env_set_maxdbs(raw, max_dbs), "mdb_env_set_maxdbs");
  // Deliberately no mdb_env_set_mapsize: an existing env is mapped at the
  // size persisted in its meta page, and this tool must never resize the
  // live database.
  mdb_check(mdb_env_open(raw, path.c_str(), flags, 0600), "mdb_env_open");
  return env;
}

Env Env::open_readwrite(const std::string& path, unsigned max_dbs,
                        size_t map_size, bool no_subdir) {
  MDB_env* raw = nullptr;
  mdb_check(mdb_env_create(&raw), "mdb_env_create");
  Env env;
  env.env_ = raw;
  mdb_check(mdb_env_set_maxdbs(raw, max_dbs), "mdb_env_set_maxdbs");
  if (map_size > 0)
    mdb_check(mdb_env_set_mapsize(raw, map_size), "mdb_env_set_mapsize");
  unsigned flags = MDB_NOTLS | (no_subdir ? MDB_NOSUBDIR : 0u);
  mdb_check(mdb_env_open(raw, path.c_str(), flags, 0600), "mdb_env_open");
  return env;
}

int reader_check(MDB_env* env) {
  int dead = 0;
  mdb_check(mdb_reader_check(env, &dead), "mdb_reader_check");
  if (dead > 0)
    LWSBK_WARN("cleared %d stale LMDB reader slot(s) left by a dead process",
               dead);
  return dead;
}

int reader_count(MDB_env* env) {
  int count = 0;
  auto cb = [](const char* msg, void* ctx) -> int {
    // mdb_reader_list emits one callback per line: a header line, then one
    // line per active reader beginning with spaces and a pid. The empty
    // table is reported as "(no active readers)\n".
    const char* p = msg;
    while (*p == ' ' || *p == '\t') ++p;
    if (std::isdigit(static_cast<unsigned char>(*p)))
      ++*static_cast<int*>(ctx);
    return 0;
  };
  int rc = mdb_reader_list(env, cb, &count);
  if (rc < 0) throw LmdbError(rc, "mdb_reader_list");
  return count;
}

void copy_env_to_fd(MDB_env* env, int fd) {
  mdb_check(mdb_env_copyfd2(env, fd, MDB_CP_COMPACT),
            "mdb_env_copyfd2(MDB_CP_COMPACT)");
}

}  // namespace lwsbk
