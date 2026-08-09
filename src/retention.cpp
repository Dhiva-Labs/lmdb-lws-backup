#include "retention.h"

#include <ctime>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "log.h"

namespace fs = std::filesystem;

namespace lwsbk {

std::string backup_filename_for_date(const std::string& yyyymmdd) {
  return std::string(kBackupPrefix) + yyyymmdd + kBackupSuffix;
}

std::string manifest_filename_for_date(const std::string& yyyymmdd) {
  return std::string(kBackupPrefix) + yyyymmdd + kManifestSuffix;
}

std::optional<std::string> parse_backup_date(const std::string& filename) {
  const size_t plen = std::strlen(kBackupPrefix);
  const size_t slen = std::strlen(kBackupSuffix);
  if (filename.size() != plen + 8 + slen) return std::nullopt;
  if (filename.compare(0, plen, kBackupPrefix) != 0) return std::nullopt;
  if (filename.compare(plen + 8, slen, kBackupSuffix) != 0) return std::nullopt;
  std::string date = filename.substr(plen, 8);
  for (char c : date)
    if (c < '0' || c > '9') return std::nullopt;
  return date;
}

std::string yyyymmdd_minus_days(const std::string& yyyymmdd, int days) {
  if (yyyymmdd.size() != 8)
    throw std::invalid_argument("bad date string: " + yyyymmdd);
  struct tm tm {};
  tm.tm_year = std::stoi(yyyymmdd.substr(0, 4)) - 1900;
  tm.tm_mon = std::stoi(yyyymmdd.substr(4, 2)) - 1;
  tm.tm_mday = std::stoi(yyyymmdd.substr(6, 2)) - days;
  tm.tm_hour = 12;  // midday avoids any DST edge in the normalization
  time_t t = timegm(&tm);
  struct tm out;
  gmtime_r(&t, &out);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d", &out);
  return buf;
}

std::vector<BackupEntry> scan_backups(const std::string& dir) {
  std::vector<BackupEntry> out;
  for (const auto& de : fs::directory_iterator(dir)) {
    if (!de.is_regular_file()) continue;
    auto date = parse_backup_date(de.path().filename().string());
    if (!date) continue;
    BackupEntry e;
    e.enc_path = de.path().string();
    e.date = *date;
    fs::path manifest = de.path().parent_path() / manifest_filename_for_date(*date);
    if (fs::exists(manifest)) e.manifest_path = manifest.string();
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(),
            [](const BackupEntry& a, const BackupEntry& b) { return a.date < b.date; });
  return out;
}

std::vector<BackupEntry> plan_deletions(const std::vector<BackupEntry>& sorted_asc,
                                        int retention_days,
                                        const std::string& today_yyyymmdd) {
  std::vector<BackupEntry> doomed;
  if (retention_days < 1) return doomed;  // defensive: never "retain zero"
  const size_t n = sorted_asc.size();
  const size_t keep = static_cast<size_t>(retention_days);
  if (n <= keep) return doomed;

  // Oldest date still inside the retention window (inclusive of today).
  const std::string cutoff =
      yyyymmdd_minus_days(today_yyyymmdd, retention_days - 1);

  // Only the first n-keep entries are even candidates; the newest `keep`
  // are always protected regardless of age.
  for (size_t i = 0; i < n - keep; ++i) {
    if (sorted_asc[i].date < cutoff) doomed.push_back(sorted_asc[i]);
  }
  return doomed;
}

int apply_retention(const std::string& dir, int retention_days,
                    const std::string& today_yyyymmdd) {
  auto backups = scan_backups(dir);
  auto doomed = plan_deletions(backups, retention_days, today_yyyymmdd);
  int removed = 0;
  for (const BackupEntry& e : doomed) {
    std::error_code ec;
    if (fs::remove(e.enc_path, ec) && !ec) {
      ++removed;
      LWSBK_INFO("retention: deleted %s", e.enc_path.c_str());
    } else if (ec) {
      LWSBK_WARN("retention: could not delete %s: %s", e.enc_path.c_str(),
                 ec.message().c_str());
      continue;  // keep the manifest so the pair stays consistent
    }
    if (e.manifest_path) {
      fs::remove(*e.manifest_path, ec);
      if (ec)
        LWSBK_WARN("retention: could not delete %s: %s",
                   e.manifest_path->c_str(), ec.message().c_str());
    }
  }
  return removed;
}

int cleanup_partials(const std::string& dir) {
  int removed = 0;
  for (const auto& de : fs::directory_iterator(dir)) {
    if (!de.is_regular_file()) continue;
    const std::string name = de.path().filename().string();
    const size_t slen = std::strlen(kPartialSuffix);
    if (name.size() > slen &&
        name.compare(name.size() - slen, slen, kPartialSuffix) == 0) {
      std::error_code ec;
      if (fs::remove(de.path(), ec) && !ec) {
        ++removed;
        LWSBK_WARN("removed stale partial file from an interrupted run: %s",
                   de.path().c_str());
      }
    }
  }
  return removed;
}

int cleanup_orphan_manifests(const std::string& dir) {
  int removed = 0;
  const size_t plen = std::strlen(kBackupPrefix);
  const size_t slen = std::strlen(kManifestSuffix);
  for (const auto& de : fs::directory_iterator(dir)) {
    if (!de.is_regular_file()) continue;
    const std::string name = de.path().filename().string();
    if (name.size() != plen + 8 + slen) continue;
    if (name.compare(0, plen, kBackupPrefix) != 0) continue;
    if (name.compare(plen + 8, slen, kManifestSuffix) != 0) continue;
    const std::string date = name.substr(plen, 8);
    fs::path enc = de.path().parent_path() / backup_filename_for_date(date);
    if (!fs::exists(enc)) {
      std::error_code ec;
      if (fs::remove(de.path(), ec) && !ec) {
        ++removed;
        LWSBK_INFO("removed orphan manifest %s", de.path().c_str());
      }
    }
  }
  return removed;
}

}  // namespace lwsbk
