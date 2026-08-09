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
// fsync → decrypt-to-RAM verify (accounts walk + full table walk) → manifest
// → atomic rename into place → retention. Throws on any failure; on failure
// no .enc without .partial suffix is ever left behind and the previous
// day's backups are untouched.
BackupOutcome run_backup_cycle(const Config& cfg, const SecretKey& key,
                               const std::function<bool()>& abort_requested);

struct VerifyResult {
  AccountsSummary accounts;
  uint64_t total_rows = 0;
  uint64_t plaintext_bytes = 0;
  std::string sha256_hex;  // of the encrypted file, recomputed
};

// Decrypts enc_path into anonymous RAM (memfd → /dev/shm fallback → the
// configured scratch dir), opens it as an LMDB env, walks the accounts
// table and every other named table. Throws if anything fails to open or
// walk. Used both by the in-cycle verify step and the --verify command.
VerifyResult verify_backup_file(const Config& cfg, const std::string& enc_path,
                                const SecretKey& key,
                                const std::function<bool()>& abort_requested);

// Decrypts a backup to <to_path>/data.mdb (plain LMDB subdir env layout),
// then sanity-opens it. Refuses to overwrite an existing data.mdb.
VerifyResult restore_backup(const Config& cfg, const std::string& enc_path,
                            const std::string& to_path, const SecretKey& key);

// "YYYYMMDD" in the configured timezone (used for backup filenames).
std::string current_date_string(const Config& cfg);

}  // namespace lwsbk
