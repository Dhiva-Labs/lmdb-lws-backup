#include "retention.h"

#include <fstream>

#include "test_util.h"

using namespace lwsbk;

namespace {

TempDir* g_tmp = nullptr;

BackupEntry entry(const std::string& date) {
  BackupEntry e;
  e.date = date;
  e.enc_path = "/backups/" + backup_filename_for_date(date);
  return e;
}

// N consecutive days ending at `last` (inclusive), ascending.
std::vector<BackupEntry> consecutive(const std::string& last, int n) {
  std::vector<BackupEntry> v;
  for (int i = n - 1; i >= 0; --i)
    v.push_back(entry(yyyymmdd_minus_days(last, i)));
  return v;
}

void touch(const std::string& path) {
  std::ofstream f(path);
  f << "x";
}

void test_filename_parsing() {
  CHECK(backup_filename_for_date("20260810") ==
        "lws-backup-20260810.lmdbbak.enc");
  CHECK(parse_backup_date("lws-backup-20260810.lmdbbak.enc") == "20260810");
  CHECK(!parse_backup_date("lws-backup-20260810.lmdbbak.enc.partial"));
  CHECK(!parse_backup_date("lws-backup-2026081.lmdbbak.enc"));
  CHECK(!parse_backup_date("lws-backup-2026081x.lmdbbak.enc"));
  CHECK(!parse_backup_date("other-20260810.lmdbbak.enc"));
  CHECK(!parse_backup_date("lws-backup-20260810.manifest.json"));
}

void test_date_arithmetic() {
  CHECK(yyyymmdd_minus_days("20260810", 0) == "20260810");
  CHECK(yyyymmdd_minus_days("20260810", 1) == "20260809");
  CHECK(yyyymmdd_minus_days("20260810", 10) == "20260731");  // month boundary
  CHECK(yyyymmdd_minus_days("20260101", 1) == "20251231");   // year boundary
  CHECK(yyyymmdd_minus_days("20260301", 1) == "20260228");   // non-leap
  CHECK(yyyymmdd_minus_days("20240301", 1) == "20240229");   // leap year
}

void test_steady_state_35_days() {
  // 35 consecutive dailies, retention 30 → the 5 oldest go.
  auto files = consecutive("20260810", 35);
  auto doomed = plan_deletions(files, 30, "20260810");
  CHECK(doomed.size() == 5);
  CHECK(doomed.front().date == "20260707");  // oldest first
  CHECK(doomed.back().date == "20260711");
  // Newest kept must be the cutoff date.
  CHECK(files[5].date == "20260712");
}

void test_exactly_at_retention() {
  auto files = consecutive("20260810", 30);
  CHECK(plan_deletions(files, 30, "20260810").empty());
}

void test_downtime_never_mass_deletes() {
  // 30 backups, then the tool was down for 60 days. All are age-expired,
  // but the newest-30 protection keeps every one of them.
  auto files = consecutive("20260610", 30);
  auto doomed = plan_deletions(files, 30, "20260810");
  CHECK(doomed.empty());

  // One new backup arrives after the outage: only the single oldest may go.
  files.push_back(entry("20260810"));
  doomed = plan_deletions(files, 30, "20260810");
  CHECK(doomed.size() == 1);
  CHECK(doomed[0].date == "20260512");
}

void test_retention_shrink() {
  // Operator lowers retention 30 → 7: everything older than 7 days AND
  // outside the newest 7 is deleted.
  auto files = consecutive("20260810", 30);
  auto doomed = plan_deletions(files, 7, "20260810");
  CHECK(doomed.size() == 23);
  CHECK(doomed.back().date == "20260803");
}

void test_same_day_rerun_stable() {
  // Same-day rerun overwrites today's file; plan must not delete anything
  // extra when run twice with identical input.
  auto files = consecutive("20260810", 31);
  auto first = plan_deletions(files, 30, "20260810");
  CHECK(first.size() == 1);
  files.erase(files.begin());
  CHECK(plan_deletions(files, 30, "20260810").empty());
}

void test_defensive_bounds() {
  auto files = consecutive("20260810", 10);
  CHECK(plan_deletions(files, 0, "20260810").empty());
  CHECK(plan_deletions({}, 30, "20260810").empty());
}

void test_apply_retention_filesystem() {
  std::string dir = g_tmp->sub("dest");
  std::filesystem::create_directory(dir);
  // 33 consecutive dailies with manifests.
  for (int i = 0; i < 33; ++i) {
    std::string d = yyyymmdd_minus_days("20260810", i);
    touch(dir + "/" + backup_filename_for_date(d));
    touch(dir + "/" + manifest_filename_for_date(d));
  }
  touch(dir + "/unrelated.txt");
  touch(dir + "/lws-backup-20260810.lmdbbak.enc.partial");

  int removed = apply_retention(dir, 30, "20260810");
  CHECK(removed == 3);
  auto left = scan_backups(dir);
  CHECK(left.size() == 30);
  CHECK(left.front().date == "20260712");
  CHECK(left.back().date == "20260810");
  for (const auto& e : left) CHECK(e.manifest_path.has_value());
  CHECK(std::filesystem::exists(dir + "/unrelated.txt"));

  CHECK(cleanup_partials(dir) == 1);
  CHECK(!std::filesystem::exists(dir +
                                 "/lws-backup-20260810.lmdbbak.enc.partial"));

  // Orphan manifest cleanup.
  touch(dir + "/" + manifest_filename_for_date("20200101"));
  CHECK(cleanup_orphan_manifests(dir) == 1);
  CHECK(scan_backups(dir).size() == 30);
}

}  // namespace

int main() {
  TempDir tmp("retention");
  g_tmp = &tmp;

  RUN(test_filename_parsing);
  RUN(test_date_arithmetic);
  RUN(test_steady_state_35_days);
  RUN(test_exactly_at_retention);
  RUN(test_downtime_never_mass_deletes);
  RUN(test_retention_shrink);
  RUN(test_same_day_rerun_stable);
  RUN(test_defensive_bounds);
  RUN(test_apply_retention_filesystem);

  return test_exit();
}
