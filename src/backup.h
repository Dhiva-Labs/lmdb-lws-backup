#pragma once

#include <functional>
#include <string>

#include "config.h"
#include "crypto.h"
#include "manifest.h"

namespace lwsbk {

struct BackupOutcome {
  std::string enc_path;
  std::string manifest_path;
  Manifest manifest;
  double duration_seconds = 0;
};

// One full backup cycle: reader_check → hot compacting copy piped straight
// into the encrypted stream (plaintext never touches persistent storage) →
// fsync → decrypt-to-RAM verify (full table walk, plus the accounts walk
// under the monero-lws profile) → manifest → atomic rename into place →
// retention. Throws on any failure; on failure no .enc without .partial
// suffix is ever left behind and the previous day's backups are untouched.
BackupOutcome run_backup_cycle(const Config& cfg, const SecretKey& key,
                               const std::function<bool()>& abort_requested);

struct VerifyResult {
  std::string profile;       // resolved: "monero-lws" or "generic"
  AccountsSummary accounts;  // meaningful only when profile == "monero-lws"
  uint64_t table_count = 0;
  uint64_t total_rows = 0;
  uint64_t plaintext_bytes = 0;
  std::string sha256_hex;  // of the encrypted file, recomputed
};

// Decrypts enc_path into anonymous RAM (memfd → /dev/shm fallback → the
// configured scratch dir), opens it as an LMDB env and walks every named
// table and every plain main-DB entry. The profile is resolved from
// cfg.profile: "auto" becomes monero-lws when the accounts table exists,
// generic otherwise; monero-lws (resolved or forced) also runs the strict
// accounts walk, and a forced monero-lws without an accounts table fails.
// Throws if anything fails to open or walk. Used both by the in-cycle
// verify step and the --verify command.
VerifyResult verify_backup_file(const Config& cfg, const std::string& enc_path,
                                const SecretKey& key,
                                const std::function<bool()>& abort_requested);

// Decrypts a backup to <to_path>/data.mdb (plain LMDB subdir env layout),
// then sanity-opens it with the same profile-resolved walk as
// verify_backup_file. Refuses to overwrite an existing data.mdb.
VerifyResult restore_backup(const Config& cfg, const std::string& enc_path,
                            const std::string& to_path, const SecretKey& key);

// "YYYYMMDD" in the configured timezone (used for backup filenames).
std::string current_date_string(const Config& cfg);

}  // namespace lwsbk
