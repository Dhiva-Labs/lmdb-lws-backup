#include "retention.h"

#include <ctime>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "log.h"

namespace fs = std::filesystem;

namespace lwsbk {

namespace {

// The 8 characters between "<prefix>-" and `suffix` when `name` is exactly
// <prefix>-XXXXXXXX<suffix>; the characters are not checked to be digits.
std::optional<std::string> dated_name_field(const std::string& name,
                                            const std::string& prefix,
                                            const char* suffix) {
  const size_t plen = prefix.size() + 1;  // prefix + the inserted dash
  const size_t slen = std::strlen(suffix);
  if (name.size() != plen + 8 + slen) return std::nullopt;
  if (name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  if (name[prefix.size()] != '-') return std::nullopt;
  if (name.compare(plen + 8, slen, suffix) != 0) return std::nullopt;
  return name.substr(plen, 8);
}

}  // namespace

std::string backup_filename_for_date(const std::string& yyyymmdd,
                                     const std::string& prefix) {
  return prefix + "-" + yyyymmdd + kBackupSuffix;
}

std::string manifest_filename_for_date(const std::string& yyyymmdd,
                                       const std::string& prefix) {
  return prefix + "-" + yyyymmdd + kManifestSuffix;
}

std::optional<std::string> parse_backup_date(const std::string& filename,
                                             const std::string& prefix) {
  auto date = dated_name_field(filename, prefix, kBackupSuffix);
  if (!date) return std::nullopt;
  for (char c : *date)
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

std::vector<BackupEntry> scan_backups(const std::string& dir,
                                      const std::string& prefix) {
  std::vector<BackupEntry> out;
  for (const auto& de : fs::directory_iterator(dir)) {
    if (!de.is_regular_file()) continue;
    auto date = parse_backup_date(de.path().filename().string(), prefix);
    if (!date) continue;
    BackupEntry e;
    e.enc_path = de.path().string();
    e.date = *date;
    fs::path manifest =
        de.path().parent_path() / manifest_filename_for_date(*date, prefix);
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
                    const std::string& today_yyyymmdd,
                    const std::string& prefix) {
  auto backups = scan_backups(dir, prefix);
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

int cleanup_orphan_manifests(const std::string& dir,
                             const std::string& prefix) {
  int removed = 0;
  for (const auto& de : fs::directory_iterator(dir)) {
    if (!de.is_regular_file()) continue;
    const std::string name = de.path().filename().string();
    auto date = dated_name_field(name, prefix, kManifestSuffix);
    if (!date) continue;
    fs::path enc =
        de.path().parent_path() / backup_filename_for_date(*date, prefix);
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
