#pragma once

#include <cstddef>
#include <cstdint>

// Byte-layout mirror of the monero-lws LMDB schema, verified against
// upstream src/db/data.h (struct account) and src/db/storage.cpp (table
// definitions). monero-lws stores rows as raw in-memory struct bytes
// (host-endian, x86-64/aarch64 little-endian in practice), with explicit
// reserved fields so the layout has no hidden padding.
//
// Only the accounts table is ever parsed (for the backup manifest); every
// other table is treated as opaque bytes — the backup itself is a raw
// mdb_env_copy2 snapshot and is schema-version agnostic.

namespace lwsbk {

inline constexpr const char kAccountsTable[] = "accounts_v1_by_status,id";

// mirror of lws::db::account (144 bytes)
struct LwsAccount {
  uint32_t id;                     // enum class account_id : uint32_t
  uint32_t access;                 // enum class account_time : uint32_t
  unsigned char view_public[32];   // account_address
  unsigned char spend_public[32];
  unsigned char view_key[32];      // plaintext secret — never log or copy out
  uint64_t scan_height;            // enum class block_id : uint64_t
  uint64_t start_height;
  uint32_t creation;
  uint8_t flags;
  char reserved[3];
  uint32_t lookahead_major;        // address_index
  uint32_t lookahead_minor;
  uint64_t lookahead_fail;
};

static_assert(sizeof(LwsAccount) == 144, "account layout drifted");
static_assert(offsetof(LwsAccount, view_key) == 72, "account layout drifted");
static_assert(offsetof(LwsAccount, scan_height) == 104, "account layout drifted");
static_assert(offsetof(LwsAccount, lookahead_fail) == 136, "account layout drifted");

inline constexpr size_t kAccountRowSize = sizeof(LwsAccount);
inline constexpr size_t kScanHeightOffset = offsetof(LwsAccount, scan_height);

// Current table set (upstream storage.cpp); used by tests to build a
// faithful synthetic environment. The tool itself never assumes this list —
// verification enumerates whatever named DBs actually exist.
inline constexpr const char* kLwsTables[] = {
    "blocks_by_id",
    "pow_by_id",
    "accounts_v1_by_status,id",
    "accounts_by_address",
    "accounts_by_height,id",
    "outputs_v2_by_account_id,block_id,tx_hash,output_id",
    "spends_v1_by_account_id,block_id,tx_hash,image",
    "key_images_by_output_id,image",
    "requests_v1_by_type,address",
    "webhooks_by_account_id,payment_id",
    "webhook_events_by_account_id,type,block_id,tx_hash,output_id,payment_id,event_id",
    "subaddress_ranges_by_account_id,major_index",
    "subaddress_indexes_by_account_id,public_key",
};

}  // namespace lwsbk
