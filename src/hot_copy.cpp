#include "hot_copy.h"

#include <sys/stat.h>

#include <cctype>
#include <cstring>
#include <filesystem>

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

std::string resolve_env_path(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return path;
  const std::filesystem::path p(path);
  if (p.filename().string() != "data.mdb") return path;
  std::string dir = p.parent_path().string();
  if (dir.empty()) dir = ".";
  struct stat lock_st;
  const bool subdir_lock = ::stat((dir + "/lock.mdb").c_str(), &lock_st) == 0;
  const bool file_lock = ::stat((path + "-lock").c_str(), &lock_st) == 0;
  if (subdir_lock && file_lock) {
    // Traces of both lock protocols: a subdirectory-env writer would use
    // lock.mdb, a single-file writer of this data.mdb would use
    // data.mdb-lock. Guessing wrong registers our reader in a lock file the
    // live writer never reads, and the snapshot can be torn mid-copy.
    // Refuse and make the operator resolve the ambiguity.
    throw std::runtime_error(
        "ambiguous LMDB layout: both " + dir + "/lock.mdb and " + path +
        "-lock exist, so it is unclear whether the live writer opened the "
        "directory environment or the data.mdb file directly; delete the "
        "stale lock file (the one no running process holds open), or set "
        "db_path to the directory if this is a subdirectory environment");
  }
  if (subdir_lock) return dir;
  return path;
}

Env Env::open_readonly(const std::string& path, unsigned max_dbs,
                       bool no_lock) {
  const std::string env_path = resolve_env_path(path);
  if (env_path != path)
    LWSBK_DEBUG("%s is the data file of a subdirectory env; opening %s",
                path.c_str(), env_path.c_str());
  struct stat st = stat_or_throw(env_path);
  // MDB_NOTLS: reader slots bind to transaction objects instead of threads,
  // so RAII abort releases the slot immediately and mdb_env_copyfd2 can run
  // its own read transaction while we hold one. Purely local bookkeeping —
  // does not change anything for other processes using the same database.
  unsigned flags = MDB_RDONLY | MDB_NOTLS;
  if (S_ISREG(st.st_mode)) {
    flags |= MDB_NOSUBDIR;
  } else if (!S_ISDIR(st.st_mode)) {
    throw std::runtime_error(
        "database path is neither a file nor a directory: " + env_path);
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
  mdb_check(mdb_env_open(raw, env_path.c_str(), flags, 0600), "mdb_env_open");
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
