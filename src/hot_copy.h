#pragma once

#include <lmdb.h>

#include <stdexcept>
#include <string>
#include <utility>

// RAII wrappers around the LMDB C API plus the hot-copy primitive.
//
// Safety contract with the live monero-lws process:
//   - the source env is only ever opened MDB_RDONLY,
//   - mdb_env_set_mapsize is never called on it,
//   - every transaction is guaranteed to be aborted on every exit path
//     (a leaked read transaction permanently pins a reader slot and stops
//     the writer from reclaiming free pages),
//   - mdb_reader_check() runs before each backup to clear slots left by
//     crashed prior runs.

namespace lwsbk {

class LmdbError : public std::runtime_error {
 public:
  LmdbError(int rc, const std::string& what)
      : std::runtime_error(what + ": " + mdb_strerror(rc)), rc_(rc) {}
  int rc() const noexcept { return rc_; }

 private:
  int rc_;
};

inline void mdb_check(int rc, const char* what) {
  if (rc != MDB_SUCCESS) throw LmdbError(rc, what);
}

// Move-only owner of an MDB_env.
class Env {
 public:
  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  Env(Env&& o) noexcept : env_(std::exchange(o.env_, nullptr)) {}
  Env& operator=(Env&& o) noexcept {
    if (this != &o) {
      reset();
      env_ = std::exchange(o.env_, nullptr);
    }
    return *this;
  }
  ~Env() { reset(); }

  // Opens an existing environment read-only, auto-detecting whether `path`
  // is a subdirectory env (data.mdb inside) or a single-file env
  // (MDB_NOSUBDIR). Never resizes the map. `no_lock` must only be used for
  // private scratch copies that no other process can possibly have open.
  static Env open_readonly(const std::string& path, unsigned max_dbs,
                           bool no_lock = false);

  // Opens or creates a writable environment (tests and --restore targets).
  static Env open_readwrite(const std::string& path, unsigned max_dbs,
                            size_t map_size, bool no_subdir);

  MDB_env* get() const noexcept { return env_; }
  explicit operator bool() const noexcept { return env_ != nullptr; }
  void reset() noexcept {
    if (env_) {
      mdb_env_close(env_);
      env_ = nullptr;
    }
  }

 private:
  MDB_env* env_ = nullptr;
};

// Move-only read-only transaction; aborted on destruction.
class ReadTxn {
 public:
  explicit ReadTxn(MDB_env* env) {
    mdb_check(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn_),
              "mdb_txn_begin(RDONLY)");
  }
  ReadTxn(const ReadTxn&) = delete;
  ReadTxn& operator=(const ReadTxn&) = delete;
  ReadTxn(ReadTxn&& o) noexcept : txn_(std::exchange(o.txn_, nullptr)) {}
  ~ReadTxn() { abort(); }

  MDB_txn* get() const noexcept { return txn_; }
  void abort() noexcept {
    if (txn_) {
      mdb_txn_abort(txn_);
      txn_ = nullptr;
    }
  }

 private:
  MDB_txn* txn_ = nullptr;
};

// Move-only cursor; closed on destruction.
class Cursor {
 public:
  Cursor(MDB_txn* txn, MDB_dbi dbi) {
    mdb_check(mdb_cursor_open(txn, dbi, &cur_), "mdb_cursor_open");
  }
  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;
  Cursor(Cursor&& o) noexcept : cur_(std::exchange(o.cur_, nullptr)) {}
  ~Cursor() {
    if (cur_) mdb_cursor_close(cur_);
  }
  MDB_cursor* get() const noexcept { return cur_; }

 private:
  MDB_cursor* cur_ = nullptr;
};

// Clears reader-table slots owned by dead processes. Returns slots cleared.
int reader_check(MDB_env* env);

// Live reader-slot count (used by tests to prove we never leak slots).
int reader_count(MDB_env* env);

// Streams a compacting snapshot of `env` to `fd` via
// mdb_env_copyfd2(MDB_CP_COMPACT). Blocks until the copy is complete or
// fails; throws LmdbError on failure. The fd may be a pipe — LMDB performs
// plain blocking write()s.
void copy_env_to_fd(MDB_env* env, int fd);

}  // namespace lwsbk
