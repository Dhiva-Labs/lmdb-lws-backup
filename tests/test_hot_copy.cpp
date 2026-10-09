#include "hot_copy.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

#include "test_util.h"

using namespace lwsbk;

namespace {

constexpr size_t kMapSize = 64u << 20;

void put_kv(MDB_env* env, const char* db, const std::string& k,
            const std::string& v) {
  MDB_txn* txn = nullptr;
  mdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin(rw)");
  MDB_dbi dbi;
  int rc = mdb_dbi_open(txn, db, MDB_CREATE, &dbi);
  if (rc != MDB_SUCCESS) {
    mdb_txn_abort(txn);
    throw LmdbError(rc, "dbi_open");
  }
  MDB_val mk{k.size(), const_cast<char*>(k.data())};
  MDB_val mv{v.size(), const_cast<char*>(v.data())};
  rc = mdb_put(txn, dbi, &mk, &mv, 0);
  if (rc != MDB_SUCCESS) {
    mdb_txn_abort(txn);
    throw LmdbError(rc, "put");
  }
  mdb_check(mdb_txn_commit(txn), "txn_commit");
}

std::string get_kv(MDB_env* env, const char* db, const std::string& k) {
  ReadTxn txn(env);
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(txn.get(), db, 0, &dbi), "dbi_open(ro)");
  MDB_val mk{k.size(), const_cast<char*>(k.data())};
  MDB_val mv{};
  mdb_check(mdb_get(txn.get(), dbi, &mk, &mv), "get");
  return std::string(static_cast<char*>(mv.mv_data), mv.mv_size);
}

TempDir* g_tmp = nullptr;
Env g_src;

void test_create_source_env() {
  std::filesystem::create_directory(g_tmp->sub("src"));
  g_src = Env::open_readwrite(g_tmp->sub("src"), 4, kMapSize, false);
  for (int i = 0; i < 100; ++i)
    put_kv(g_src.get(), "t1", "k" + std::to_string(i), "v" + std::to_string(i));
  CHECK(get_kv(g_src.get(), "t1", "k42") == "v42");
}

void test_raii_releases_reader_slot() {
  CHECK(reader_count(g_src.get()) == 0);
  {
    ReadTxn txn(g_src.get());
    CHECK(reader_count(g_src.get()) == 1);
  }
  CHECK(reader_count(g_src.get()) == 0);

  // Deliberate mid-work exception: the slot must still be released.
  try {
    ReadTxn txn(g_src.get());
    CHECK(reader_count(g_src.get()) == 1);
    throw std::runtime_error("boom");
  } catch (const std::runtime_error&) {
  }
  CHECK(reader_count(g_src.get()) == 0);
}

void test_copy_to_file_and_reopen() {
  std::string dest = g_tmp->sub("copy.mdb");
  int fd = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  {
    ReadTxn pin(g_src.get());  // mirrors the real backup flow
    copy_env_to_fd(g_src.get(), fd);
  }
  CHECK(::close(fd) == 0);
  CHECK(reader_count(g_src.get()) == 0);

  Env copy = Env::open_readonly(dest, 4);  // regular file → MDB_NOSUBDIR
  CHECK(get_kv(copy.get(), "t1", "k42") == "v42");
  CHECK(get_kv(copy.get(), "t1", "k99") == "v99");
}

void test_failed_copy_releases_and_source_survives() {
  int p[2];
  CHECK(::pipe2(p, O_CLOEXEC) == 0);
  CHECK(::close(p[0]) == 0);  // reader gone → LMDB's write() gets EPIPE

  bool threw = false;
  try {
    copy_env_to_fd(g_src.get(), p[1]);
  } catch (const LmdbError&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(::close(p[1]) == 0);
  CHECK(reader_count(g_src.get()) == 0);

  // Source must remain fully usable for writes after a failed copy.
  put_kv(g_src.get(), "t1", "after-fail", "ok");
  CHECK(get_kv(g_src.get(), "t1", "after-fail") == "ok");
}

void test_reader_check_clean() {
  CHECK(reader_check(g_src.get()) == 0);
}

void test_resolve_env_path() {
  namespace fs = std::filesystem;

  const std::string dir = g_tmp->sub("resolve");
  fs::create_directory(dir);
  {
    Env writer = Env::open_readwrite(dir, 4, kMapSize, false);
    put_kv(writer.get(), "t1", "row", "subdir");
  }
  CHECK(resolve_env_path(dir) == dir);

  // data.mdb beside lock.mdb must open the env directory: no private
  // data.mdb-lock may appear, or a live writer would not see our reader.
  const std::string data = dir + "/data.mdb";
  CHECK(fs::exists(dir + "/lock.mdb"));
  CHECK(resolve_env_path(data) == dir);
  {
    Env env = Env::open_readonly(data, 4);
    CHECK(get_kv(env.get(), "t1", "row") == "subdir");
  }
  CHECK(!fs::exists(data + "-lock"));

  // A lone data.mdb (e.g. mdb_copy output) is a single-file env.
  const std::string bare = g_tmp->sub("bare");
  fs::create_directory(bare);
  fs::copy_file(data, bare + "/data.mdb");
  CHECK(resolve_env_path(bare + "/data.mdb") == bare + "/data.mdb");
  {
    Env env = Env::open_readonly(bare + "/data.mdb", 4);
    CHECK(get_kv(env.get(), "t1", "row") == "subdir");
  }

  const std::string single = g_tmp->sub("single.mdb");
  {
    Env writer = Env::open_readwrite(single, 4, kMapSize, true);
    put_kv(writer.get(), "t1", "row", "single-file");
  }
  CHECK(resolve_env_path(single) == single);
  {
    Env env = Env::open_readonly(single, 4);
    CHECK(get_kv(env.get(), "t1", "row") == "single-file");
  }

  const std::string missing = g_tmp->sub("missing/data.mdb");
  CHECK(resolve_env_path(missing) == missing);
  CHECK_THROWS(Env::open_readonly(missing, 4));

  // Traces of BOTH lock protocols: a data.mdb whose directory has lock.mdb
  // AND a sibling data.mdb-lock. The writer could be subdir-mode or
  // single-file-mode; redirecting on a stale lock.mdb would register our
  // reader where a single-file writer never looks (torn snapshot). The
  // resolver must refuse rather than guess.
  const std::string ambi = g_tmp->sub("ambiguous");
  fs::create_directory(ambi);
  fs::copy_file(data, ambi + "/data.mdb");
  { std::ofstream(ambi + "/lock.mdb") << ""; }
  { std::ofstream(ambi + "/data.mdb-lock") << ""; }
  CHECK_THROWS(resolve_env_path(ambi + "/data.mdb"));
  CHECK_THROWS(Env::open_readonly(ambi + "/data.mdb", 4));
}

}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  TempDir tmp("hotcopy");
  g_tmp = &tmp;

  RUN(test_create_source_env);
  RUN(test_raii_releases_reader_slot);
  RUN(test_copy_to_file_and_reopen);
  RUN(test_failed_copy_releases_and_source_survives);
  RUN(test_reader_check_clean);
  RUN(test_resolve_env_path);

  g_src.reset();
  return test_exit();
}
