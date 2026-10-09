#pragma once

#include <lmdb.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Backup manifest: a plaintext JSON sidecar holding only counts, heights,
// sizes, and the ciphertext hash — never key material of any kind.

namespace lwsbk {

struct AccountsSummary {
  uint64_t account_count = 0;
  std::optional<uint64_t> scan_height_min;
  std::optional<uint64_t> scan_height_max;
};

struct TableWalk {
  uint64_t table_count = 0;   // named sub-DBs successfully opened
  uint64_t named_rows = 0;    // rows across all named sub-DBs
  uint64_t main_db_rows = 0;  // plain data entries in the unnamed main DB
  bool has_lws_accounts = false;  // "accounts_v1_by_status,id" present
  uint64_t total_rows() const { return named_rows + main_db_rows; }
};

struct Manifest {
  std::string timestamp;  // RFC3339 UTC
  std::string profile;    // "monero-lws" | "generic"; "" in pre-0.2 files
  // Account fields are not serialized when profile == "generic".
  uint64_t account_count = 0;
  std::optional<uint64_t> scan_height_min;
  std::optional<uint64_t> scan_height_max;
  std::optional<uint64_t> table_count;
  std::optional<uint64_t> total_rows;
  std::string sha256_of_encrypted_file;  // hex
  uint64_t encrypted_size_bytes = 0;
  std::string tool_version;
};

// Walks "accounts_v1_by_status,id". Throws if the table is missing, a row
// size doesn't match the verified 144-byte layout (schema drift), or the
// cursor walk fails (e.g. MDB_CORRUPTED).
AccountsSummary walk_accounts(MDB_txn* txn);

// Names of all named databases, discovered by walking the unnamed main DB —
// schema-blind, works across monero-lws table-version migrations.
std::vector<std::string> list_named_dbs(MDB_txn* txn);

// Opens every named DB and walks every row; returns total rows visited.
// Throws on the first open or cursor failure.
uint64_t walk_all_tables(MDB_txn* txn);

// Schema-blind walk of any LMDB environment: every main-DB key that opens as
// a named sub-DB is a table and is walked in full; any other key is a plain
// main-DB row. Throws on cursor failure (e.g. MDB_CORRUPTED) and when the
// env was opened with too few max_dbs to hold every table (MDB_DBS_FULL).
TableWalk walk_tables_generic(MDB_txn* txn);

std::string rfc3339_utc_now();

std::string manifest_to_json(const Manifest& m);
// Strict flat-object parser for files this tool wrote (any version). Throws
// on garbage or when timestamp / sha256_of_encrypted_file is missing.
Manifest manifest_from_json(const std::string& json);

void write_manifest_file(const std::string& path, const Manifest& m);  // 0600
Manifest read_manifest_file(const std::string& path);

}  // namespace lwsbk
