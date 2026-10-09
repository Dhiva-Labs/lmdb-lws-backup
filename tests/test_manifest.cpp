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

void test_walk_tables_generic_lws() {
  std::filesystem::create_directory(g_tmp->sub("env5"));
  Env env = Env::open_readwrite(g_tmp->sub("env5"), 32, 16u << 20, false);

  put_account(env.get(), 0, make_account(1, 3181022));
  put_account(env.get(), 0, make_account(2, 3181090));
  put_account(env.get(), 1, make_account(3, 3181055));
  const char* outputs = "outputs_v2_by_account_id,block_id,tx_hash,output_id";
  put_raw(env.get(), outputs, "a1", "o1", MDB_DUPSORT);
  put_raw(env.get(), outputs, "a1", "o2", MDB_DUPSORT);
  put_raw(env.get(), outputs, "a1", "o3", MDB_DUPSORT);
  put_raw(env.get(), outputs, "a2", "o1", MDB_DUPSORT);
  put_raw(env.get(), "spends_v1_by_account_id,block_id,tx_hash,image", "a1",
          "s1", MDB_DUPSORT);
  put_raw(env.get(), "blocks_by_id", "k1", "v1", 0);
  put_raw(env.get(), "blocks_by_id", "k2", "v2", 0);

  ReadTxn txn(env.get());
  TableWalk w = walk_tables_generic(txn.get());
  CHECK(w.table_count == 4);
  CHECK(w.named_rows == 10);  // 3 accounts + 4 outputs + 1 spend + 2 blocks
  CHECK(w.main_db_rows == 0);
  CHECK(w.total_rows() == 10);
  CHECK(w.has_lws_accounts);
  CHECK(walk_all_tables(txn.get()) == w.named_rows);
}

void test_walk_tables_generic_plain_main_db() {
  std::filesystem::create_directory(g_tmp->sub("env6"));
  Env env = Env::open_readwrite(g_tmp->sub("env6"), 32, 16u << 20, false);
  MDB_txn* wtxn = nullptr;
  mdb_check(mdb_txn_begin(env.get(), nullptr, 0, &wtxn), "txn_begin");
  MDB_dbi dbi;
  mdb_check(mdb_dbi_open(wtxn, nullptr, 0, &dbi), "dbi_open main");
  for (const char* key : {"alpha", "beta", "gamma", "delta"}) {
    MDB_val k{std::strlen(key), const_cast<char*>(key)};
    MDB_val v{3, const_cast<char*>("val")};
    mdb_check(mdb_put(wtxn, dbi, &k, &v, 0), "put");
  }
  mdb_check(mdb_txn_commit(wtxn), "commit");

  ReadTxn txn(env.get());
  TableWalk w = walk_tables_generic(txn.get());
  CHECK(w.table_count == 0);
  CHECK(w.named_rows == 0);
  CHECK(w.main_db_rows == 4);
  CHECK(w.total_rows() == 4);
  CHECK(!w.has_lws_accounts);
}

void test_walk_tables_generic_mixed_main_db() {
  std::filesystem::create_directory(g_tmp->sub("env7"));
  Env env = Env::open_readwrite(g_tmp->sub("env7"), 32, 16u << 20, false);
  put_raw(env.get(), "t", "k1", "v1", 0);
  put_raw(env.get(), "t", "k2", "v2", 0);
  put_raw(env.get(), nullptr, "plain", "v", 0);
  // Truncated at the NUL this would name table "t"; it must stay plain data.
  put_raw(env.get(), nullptr, std::string("t\0x", 3), "v", 0);

  ReadTxn txn(env.get());
  TableWalk w = walk_tables_generic(txn.get());
  CHECK(w.table_count == 1);
  CHECK(w.named_rows == 2);
  CHECK(w.main_db_rows == 2);
  CHECK(!w.has_lws_accounts);
}

void test_walk_tables_generic_dupsort_main_db() {
  // A DUPSORT main DB cannot hold named tables; mdb_dbi_open reports
  // MDB_NOTFOUND for every key and every duplicate is a plain row.
  std::filesystem::create_directory(g_tmp->sub("env8"));
  Env env = Env::open_readwrite(g_tmp->sub("env8"), 32, 16u << 20, false);
  put_raw(env.get(), nullptr, "k", "d1", MDB_DUPSORT);
  put_raw(env.get(), nullptr, "k", "d2", MDB_DUPSORT);
  put_raw(env.get(), nullptr, "m", "d1", MDB_DUPSORT);

  ReadTxn txn(env.get());
  TableWalk w = walk_tables_generic(txn.get());
  CHECK(w.table_count == 0);
  CHECK(w.main_db_rows == 3);
}

void test_walk_tables_generic_dbs_full_throws() {
  std::string path = g_tmp->sub("env9");
  std::filesystem::create_directory(path);
  {
    Env env = Env::open_readwrite(path, 32, 16u << 20, false);
    put_raw(env.get(), "t1", "k", "v", 0);
    put_raw(env.get(), "t2", "k", "v", 0);
    put_raw(env.get(), "t3", "k", "v", 0);
  }
  Env env = Env::open_readwrite(path, 2, 16u << 20, false);
  ReadTxn txn(env.get());
  bool threw = false;
  try {
    walk_tables_generic(txn.get());
  } catch (const LmdbError& e) {
    threw = true;
    CHECK(e.rc() == MDB_DBS_FULL);
    CHECK(std::strstr(e.what(), "max_named_dbs") != nullptr);
  }
  CHECK(threw);
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
  CHECK_THROWS(manifest_from_json("{\"sha256_of_encrypted_file\": \"00\"}"));
}

void test_manifest_json_lws_profile() {
  Manifest m;
  m.timestamp = "2026-10-10T03:00:00Z";
  m.profile = "monero-lws";
  m.account_count = 12;
  m.scan_height_min = 3181022;
  m.scan_height_max = 3181090;
  m.table_count = 13;
  m.total_rows = 5000;
  m.sha256_of_encrypted_file = "abc123";
  m.tool_version = "0.2.0";

  std::string json = manifest_to_json(m);
  CHECK(json.find("\"profile\": \"monero-lws\"") != std::string::npos);
  CHECK(json.find("\"account_count\": 12") != std::string::npos);
  Manifest r = manifest_from_json(json);
  CHECK(r.profile == "monero-lws");
  CHECK(r.account_count == 12);
  CHECK(r.scan_height_min == m.scan_height_min);
  CHECK(r.scan_height_max == m.scan_height_max);
  CHECK(r.table_count == m.table_count);
  CHECK(r.total_rows == m.total_rows);
}

void test_manifest_json_generic_profile() {
  Manifest m;
  m.timestamp = "2026-10-10T03:00:00Z";
  m.profile = "generic";
  m.table_count = 7;
  m.total_rows = 123456789;
  m.sha256_of_encrypted_file = "feedface";
  m.encrypted_size_bytes = 4096;
  m.tool_version = "0.2.0";

  std::string json = manifest_to_json(m);
  CHECK(json.find("account_count") == std::string::npos);
  CHECK(json.find("scan_height_min") == std::string::npos);
  CHECK(json.find("scan_height_max") == std::string::npos);
  CHECK(json.find("\"table_count\": 7") != std::string::npos);
  CHECK(json.find("\"total_rows\": 123456789") != std::string::npos);

  Manifest r = manifest_from_json(json);
  CHECK(r.timestamp == m.timestamp);
  CHECK(r.profile == "generic");
  CHECK(r.account_count == 0);
  CHECK(!r.scan_height_min && !r.scan_height_max);
  CHECK(r.table_count && *r.table_count == 7);
  CHECK(r.total_rows && *r.total_rows == 123456789);
  CHECK(r.sha256_of_encrypted_file == m.sha256_of_encrypted_file);
  CHECK(r.encrypted_size_bytes == m.encrypted_size_bytes);
  CHECK(r.tool_version == m.tool_version);
}

void test_manifest_v01_backward_compat() {
  const std::string v01 =
      "{\n"
      "  \"timestamp\": \"2026-08-10T03:00:00Z\",\n"
      "  \"account_count\": 4213,\n"
      "  \"scan_height_min\": 3181022,\n"
      "  \"scan_height_max\": 3181090,\n"
      "  \"sha256_of_encrypted_file\": \"abc123def456\",\n"
      "  \"encrypted_size_bytes\": 987654321,\n"
      "  \"tool_version\": \"0.1.0\"\n"
      "}\n";
  Manifest r = manifest_from_json(v01);
  CHECK(r.timestamp == "2026-08-10T03:00:00Z");
  CHECK(r.profile.empty());
  CHECK(r.account_count == 4213);
  CHECK(r.scan_height_min && *r.scan_height_min == 3181022);
  CHECK(r.scan_height_max && *r.scan_height_max == 3181090);
  CHECK(!r.table_count && !r.total_rows);
  CHECK(r.sha256_of_encrypted_file == "abc123def456");
  CHECK(r.encrypted_size_bytes == 987654321);
  CHECK(r.tool_version == "0.1.0");

  // Empty 0.1 database: null heights.
  Manifest e = manifest_from_json(
      "{\"timestamp\": \"t\", \"account_count\": 0, \"scan_height_min\": null,"
      " \"scan_height_max\": null, \"sha256_of_encrypted_file\": \"00\","
      " \"encrypted_size_bytes\": 1, \"tool_version\": \"0.1.0\"}");
  CHECK(e.profile.empty());
  CHECK(e.account_count == 0);
  CHECK(!e.scan_height_min && !e.scan_height_max);
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
  RUN(test_walk_tables_generic_lws);
  RUN(test_walk_tables_generic_plain_main_db);
  RUN(test_walk_tables_generic_mixed_main_db);
  RUN(test_walk_tables_generic_dupsort_main_db);
  RUN(test_walk_tables_generic_dbs_full_throws);
  RUN(test_manifest_json_round_trip);
  RUN(test_manifest_json_lws_profile);
  RUN(test_manifest_json_generic_profile);
  RUN(test_manifest_v01_backward_compat);
  RUN(test_manifest_file_io);

  return test_exit();
}
