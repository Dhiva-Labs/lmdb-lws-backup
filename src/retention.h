#pragma once

#include <optional>
#include <string>
#include <vector>

// Retention policy for lws-backup-YYYYMMDD.lmdbbak.enc files.
//
// A backup is deleted only when BOTH hold:
//   1. its filename date is older than retention_days relative to today, and
//   2. it is not among the newest retention_days backups present.
// With daily backups the two conditions coincide. After downtime they
// diverge, and rule 2 guarantees an outage can never cause a mass deletion
// of the only remaining good backups. Deletion is always oldest-first and
// only ever runs after a new backup has been written AND verified.

namespace lwsbk {

struct BackupEntry {
  std::string enc_path;
  std::optional<std::string> manifest_path;
  std::string date;  // "YYYYMMDD" from the filename
};

inline constexpr const char kBackupPrefix[] = "lws-backup-";
inline constexpr const char kBackupSuffix[] = ".lmdbbak.enc";
inline constexpr const char kManifestSuffix[] = ".manifest.json";
inline constexpr const char kPartialSuffix[] = ".partial";

std::string backup_filename_for_date(const std::string& yyyymmdd);
std::string manifest_filename_for_date(const std::string& yyyymmdd);

// "lws-backup-20260810.lmdbbak.enc" → "20260810"; nullopt if not a backup.
std::optional<std::string> parse_backup_date(const std::string& filename);

// Date arithmetic on YYYYMMDD strings (UTC-agnostic, calendar-correct).
std::string yyyymmdd_minus_days(const std::string& yyyymmdd, int days);

// All backups in dir, sorted by date ascending. Manifest sidecars attached.
std::vector<BackupEntry> scan_backups(const std::string& dir);

// Pure planning over an ascending-sorted list; returns entries to delete,
// oldest first.
std::vector<BackupEntry> plan_deletions(const std::vector<BackupEntry>& sorted_asc,
                                        int retention_days,
                                        const std::string& today_yyyymmdd);

// scan + plan + unlink (enc and manifest). Returns number of backups removed.
int apply_retention(const std::string& dir, int retention_days,
                    const std::string& today_yyyymmdd);

// Removes *.partial files left by an interrupted run. Returns count removed.
int cleanup_partials(const std::string& dir);

// Removes lws-backup-*.manifest.json files whose .enc is gone. Returns count.
int cleanup_orphan_manifests(const std::string& dir);

}  // namespace lwsbk
