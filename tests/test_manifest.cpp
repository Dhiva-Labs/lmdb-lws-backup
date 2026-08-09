#include "manifest.h"

#include <sys/stat.h>

#include <cstring>

#include "hot_copy.h"
#include "lws_schema.h"
#include "test_util.h"

using namespace lwsbk;

namespace {

TempDir* g_tmp = nullptr;

LwsAccount make_account(uint32_t id, uint64_t scan_height) {
  LwsAccount a{};
  a.id = id;
  a.access = 1700000000;
  std::memset(a.view_public, 0xA0 + (id & 0xf), sizeof(a.view_public));
  std::memset(a.spend_public, 0xB0 + (id & 0xf), sizeof(a.spend_public));
  std::memset(a.view_key, 0xC0 + (id & 0xf), sizeof(a.view_key));
  a.scan_height = scan_height;
  a.start_height = scan_height > 1000 ? scan_height - 1000 : 0;
  a.creation = 1690000000;
  return a;
}

void put_account(MDB_env* env, uint8_t status, const LwsAccount& a) {
  MDB_txn* txn = nullptr;
  mdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(txn, kAccountsTable, MDB_CREATE | MDB_DUPSORT, &dbi),
            "dbi_open accounts");
  MDB_val k{sizeof(status), &status};
  MDB_val v{sizeof(a), const_cast<LwsAccount*>(&a)};
  mdb_check(mdb_put(txn, dbi, &k, &v, 0), "put account");
  mdb_check(mdb_txn_commit(txn), "commit");
}

void put_raw(MDB_env* env, const char* table, const std::string& k,
             const std::string& v, unsigned flags) {
  MDB_txn* txn = nullptr;
  mdb_check(mdb_txn_begin(env, nullptr, 0, &txn), "txn_begin");
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(txn, table, MDB_CREATE | flags, &dbi), "dbi_open");
  MDB_val mk{k.size(), const_cast<char*>(k.data())};
  MDB_val mv{v.size(), const_cast<char*>(v.data())};
  mdb_check(mdb_put(txn, dbi, &mk, &mv, 0), "put");
  mdb_check(mdb_txn_commit(txn), "commit");
}

void test_walk_accounts() {
  std::filesystem::create_directory(g_tmp->sub("env1"));
  Env env = Env::open_readwrite(g_tmp->sub("env1"), 32, 64u << 20, false);

  put_account(env.get(), 0, make_account(1, 3181022));
  put_account(env.get(), 0, make_account(2, 3181090));
  put_account(env.get(), 1, make_account(3, 3181055));
  put_raw(env.get(), "blocks_by_id", "k1", "v1", 0);
  put_raw(env.get(), "blocks_by_id", "k2", "v2", 0);

  ReadTxn txn(env.get());
  AccountsSummary s = walk_accounts(txn.get());
  CHECK(s.account_count == 3);
  CHECK(s.scan_height_min && *s.scan_height_min == 3181022);
  CHECK(s.scan_height_max && *s.scan_height_max == 3181090);

  auto names = list_named_dbs(txn.get());
  CHECK(names.size() == 2);

  CHECK(walk_all_tables(txn.get()) == 5);
}

void test_empty_accounts_table() {
  std::filesystem::create_directory(g_tmp->sub("env2"));
  Env env = Env::open_readwrite(g_tmp->sub("env2"), 32, 16u << 20, false);
  // Create the table but leave it empty.
  MDB_txn* txn = nullptr;
  mdb_check(mdb_txn_begin(env.get(), nullptr, 0, &txn), "txn");
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(txn, kAccountsTable, MDB_CREATE | MDB_DUPSORT, &dbi),
            "dbi");
  mdb_check(mdb_txn_commit(txn), "commit");

  ReadTxn ro(env.get());
  AccountsSummary s = walk_accounts(ro.get());
  CHECK(s.account_count == 0);
  CHECK(!s.scan_height_min && !s.scan_height_max);
}

void test_missing_accounts_table_throws() {
  std::filesystem::create_directory(g_tmp->sub("env3"));
  Env env = Env::open_readwrite(g_tmp->sub("env3"), 32, 16u << 20, false);
  put_raw(env.get(), "blocks_by_id", "k", "v", 0);
  ReadTxn ro(env.get());
  CHECK_THROWS(walk_accounts(ro.get()));
}

void test_malformed_account_row_throws() {
  std::filesystem::create_directory(g_tmp->sub("env4"));
  Env env = Env::open_readwrite(g_tmp->sub("env4"), 32, 16u << 20, false);
  put_raw(env.get(), kAccountsTable, std::string(1, '\0'),
          "way-too-short-for-an-account", MDB_DUPSORT);
  ReadTxn ro(env.get());
  CHECK_THROWS(walk_accounts(ro.get()));
}

void test_manifest_json_round_trip() {
  Manifest m;
  m.timestamp = "2026-08-10T03:00:00Z";
  m.account_count = 4213;
  m.scan_height_min = 3181022;
  m.scan_height_max = 3181090;
  m.sha256_of_encrypted_file = "abc123def456";
  m.encrypted_size_bytes = 987654321;
  m.tool_version = "0.1.0";

  Manifest r = manifest_from_json(manifest_to_json(m));
  CHECK(r.timestamp == m.timestamp);
  CHECK(r.account_count == m.account_count);
  CHECK(r.scan_height_min == m.scan_height_min);
  CHECK(r.scan_height_max == m.scan_height_max);
  CHECK(r.sha256_of_encrypted_file == m.sha256_of_encrypted_file);
  CHECK(r.encrypted_size_bytes == m.encrypted_size_bytes);
  CHECK(r.tool_version == m.tool_version);

  // Null heights (empty database) survive the round trip.
  Manifest e;
  e.timestamp = "2026-08-10T03:00:00Z";
  e.sha256_of_encrypted_file = "00";
  Manifest er = manifest_from_json(manifest_to_json(e));
  CHECK(!er.scan_height_min && !er.scan_height_max);
  CHECK(er.account_count == 0);

  CHECK_THROWS(manifest_from_json("not json at all"));
  CHECK_THROWS(manifest_from_json("{\"timestamp\": \"x\"}"));  // missing sha
}

void test_manifest_file_io() {
  Manifest m;
  m.timestamp = rfc3339_utc_now();
  m.account_count = 7;
  m.sha256_of_encrypted_file = "deadbeef";
  std::string path = g_tmp->sub("m.json");
  write_manifest_file(path, m);

  struct stat st;
  CHECK(stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);

  Manifest r = read_manifest_file(path);
  CHECK(r.account_count == 7);
  CHECK(r.timestamp == m.timestamp);
}

}  // namespace

int main() {
  TempDir tmp("manifest");
  g_tmp = &tmp;

  RUN(test_walk_accounts);
  RUN(test_empty_accounts_table);
  RUN(test_missing_accounts_table_throws);
  RUN(test_malformed_account_row_throws);
  RUN(test_manifest_json_round_trip);
  RUN(test_manifest_file_io);

  return test_exit();
}
