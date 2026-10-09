#include "manifest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "hot_copy.h"
#include "lws_schema.h"

namespace lwsbk {

AccountsSummary walk_accounts(MDB_txn* txn) {
  MDB_dbi dbi;
  int rc = mdb_dbi_open(txn, kAccountsTable, 0, &dbi);
  if (rc != MDB_SUCCESS)
    throw LmdbError(rc,
                    std::string("cannot open table '") + kAccountsTable +
                        "' (is this a monero-lws database?)");

  Cursor cur(txn, dbi);
  AccountsSummary s;
  MDB_val k{}, v{};
  rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
  while (rc == MDB_SUCCESS) {
    if (v.mv_size != kAccountRowSize)
      throw std::runtime_error(
          "account row has size " + std::to_string(v.mv_size) + ", expected " +
          std::to_string(kAccountRowSize) +
          " — monero-lws schema drift, refusing to produce a manifest");
    uint64_t h = 0;
    std::memcpy(&h, static_cast<const char*>(v.mv_data) + kScanHeightOffset,
                sizeof(h));
    ++s.account_count;
    if (!s.scan_height_min || h < *s.scan_height_min) s.scan_height_min = h;
    if (!s.scan_height_max || h > *s.scan_height_max) s.scan_height_max = h;
    rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
  }
  if (rc != MDB_NOTFOUND)
    throw LmdbError(rc, "cursor walk of accounts table failed");
  return s;
}

std::vector<std::string> list_named_dbs(MDB_txn* txn) {
  MDB_dbi main_dbi;
  mdb_check(mdb_dbi_open(txn, nullptr, 0, &main_dbi), "open main DB");
  Cursor cur(txn, main_dbi);
  std::vector<std::string> names;
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
  while (rc == MDB_SUCCESS) {
    names.emplace_back(static_cast<const char*>(k.mv_data), k.mv_size);
    rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
  }
  if (rc != MDB_NOTFOUND) throw LmdbError(rc, "cursor walk of main DB failed");
  return names;
}

namespace {

uint64_t count_rows(MDB_txn* txn, MDB_dbi dbi, const std::string& name) {
  Cursor cur(txn, dbi);
  uint64_t rows = 0;
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
  while (rc == MDB_SUCCESS) {
    ++rows;
    rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
  }
  if (rc != MDB_NOTFOUND)
    throw LmdbError(rc, "cursor walk of table '" + name + "' failed");
  return rows;
}

}  // namespace

uint64_t walk_all_tables(MDB_txn* txn) {
  uint64_t total = 0;
  for (const std::string& name : list_named_dbs(txn)) {
    MDB_dbi dbi;
    int rc = mdb_dbi_open(txn, name.c_str(), 0, &dbi);
    if (rc != MDB_SUCCESS)
      throw LmdbError(rc, "cannot open table '" + name + "'");
    total += count_rows(txn, dbi, name);
  }
  return total;
}

TableWalk walk_tables_generic(MDB_txn* txn) {
  MDB_dbi main_dbi;
  mdb_check(mdb_dbi_open(txn, nullptr, 0, &main_dbi), "open main DB");
  Cursor cur(txn, main_dbi);
  TableWalk w;
  MDB_val k{}, v{};
  int rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
  while (rc == MDB_SUCCESS) {
    std::string name(static_cast<const char*>(k.mv_data), k.mv_size);
    MDB_dbi dbi = 0;
    // Sub-DB names are C strings (mdb_dbi_open uses strlen), so a key with an
    // embedded NUL is plain data and must not be looked up truncated.
    int orc = name.find('\0') != std::string::npos
                  ? MDB_INCOMPATIBLE
                  : mdb_dbi_open(txn, name.c_str(), 0, &dbi);
    if (orc == MDB_SUCCESS) {
      ++w.table_count;
      w.named_rows += count_rows(txn, dbi, name);
      if (name == kAccountsTable) w.has_lws_accounts = true;
    } else if (orc == MDB_INCOMPATIBLE || orc == MDB_NOTFOUND) {
      ++w.main_db_rows;
    } else if (orc == MDB_DBS_FULL) {
      throw LmdbError(orc, "too many named tables (limit reached at '" + name +
                               "') — raise [source] max_named_dbs");
    } else {
      throw LmdbError(orc, "cannot open table '" + name + "'");
    }
    rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
  }
  if (rc != MDB_NOTFOUND) throw LmdbError(rc, "cursor walk of main DB failed");
  return w;
}

std::string rfc3339_utc_now() {
  time_t now = time(nullptr);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buf;
}

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Minimal strict parser for the flat JSON objects this tool writes:
// string, unsigned-integer, and null values only.
struct FlatJson {
  std::map<std::string, std::string> strings;
  std::map<std::string, uint64_t> numbers;
};

FlatJson parse_flat_json(const std::string& text) {
  FlatJson out;
  size_t i = 0;
  auto ws = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
  auto fail = [&](const char* why) -> std::runtime_error {
    return std::runtime_error("manifest JSON parse error at offset " +
                              std::to_string(i) + ": " + why);
  };
  auto parse_string = [&]() -> std::string {
    if (text[i] != '"') throw fail("expected string");
    ++i;
    std::string s;
    while (i < text.size() && text[i] != '"') {
      if (text[i] == '\\') {
        ++i;
        if (i >= text.size()) throw fail("bad escape");
        switch (text[i]) {
          case '"': s += '"'; break;
          case '\\': s += '\\'; break;
          case 'n': s += '\n'; break;
          case 'r': s += '\r'; break;
          case 't': s += '\t'; break;
          case 'u': {
            if (i + 4 >= text.size()) throw fail("bad \\u escape");
            s += static_cast<char>(
                std::stoul(text.substr(i + 1, 4), nullptr, 16));
            i += 4;
            break;
          }
          default: throw fail("unsupported escape");
        }
      } else {
        s += text[i];
      }
      ++i;
    }
    if (i >= text.size()) throw fail("unterminated string");
    ++i;  // closing quote
    return s;
  };

  ws();
  if (i >= text.size() || text[i] != '{') throw fail("expected '{'");
  ++i;
  ws();
  if (i < text.size() && text[i] == '}') return out;
  for (;;) {
    ws();
    std::string key = parse_string();
    ws();
    if (i >= text.size() || text[i] != ':') throw fail("expected ':'");
    ++i;
    ws();
    if (i >= text.size()) throw fail("truncated");
    if (text[i] == '"') {
      out.strings[key] = parse_string();
    } else if (text.compare(i, 4, "null") == 0) {
      i += 4;
    } else if (std::isdigit(static_cast<unsigned char>(text[i]))) {
      size_t start = i;
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
      out.numbers[key] = std::stoull(text.substr(start, i - start));
    } else {
      throw fail("unsupported value type");
    }
    ws();
    if (i >= text.size()) throw fail("truncated");
    if (text[i] == ',') {
      ++i;
      continue;
    }
    if (text[i] == '}') break;
    throw fail("expected ',' or '}'");
  }
  return out;
}

}  // namespace

std::string manifest_to_json(const Manifest& m) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"timestamp\": \"" << json_escape(m.timestamp) << "\",\n";
  os << "  \"profile\": \"" << json_escape(m.profile) << "\",\n";
  if (m.profile != "generic") {
    os << "  \"account_count\": " << m.account_count << ",\n";
    os << "  \"scan_height_min\": ";
    if (m.scan_height_min) os << *m.scan_height_min; else os << "null";
    os << ",\n  \"scan_height_max\": ";
    if (m.scan_height_max) os << *m.scan_height_max; else os << "null";
    os << ",\n";
  }
  if (m.table_count) os << "  \"table_count\": " << *m.table_count << ",\n";
  if (m.total_rows) os << "  \"total_rows\": " << *m.total_rows << ",\n";
  os << "  \"sha256_of_encrypted_file\": \""
     << json_escape(m.sha256_of_encrypted_file) << "\",\n";
  os << "  \"encrypted_size_bytes\": " << m.encrypted_size_bytes << ",\n";
  os << "  \"tool_version\": \"" << json_escape(m.tool_version) << "\"\n";
  os << "}\n";
  return os.str();
}

Manifest manifest_from_json(const std::string& json) {
  FlatJson f = parse_flat_json(json);
  Manifest m;
  auto need_str = [&](const char* key) -> std::string {
    auto it = f.strings.find(key);
    if (it == f.strings.end())
      throw std::runtime_error(std::string("manifest missing field: ") + key);
    return it->second;
  };
  auto opt_str = [&](const char* key) -> std::string {
    auto it = f.strings.find(key);
    return it == f.strings.end() ? std::string() : it->second;
  };
  auto opt_num = [&](const char* key) -> std::optional<uint64_t> {
    auto it = f.numbers.find(key);
    if (it == f.numbers.end()) return std::nullopt;
    return it->second;
  };
  m.timestamp = need_str("timestamp");
  m.profile = opt_str("profile");
  m.account_count = opt_num("account_count").value_or(0);
  m.scan_height_min = opt_num("scan_height_min");
  m.scan_height_max = opt_num("scan_height_max");
  m.table_count = opt_num("table_count");
  m.total_rows = opt_num("total_rows");
  m.sha256_of_encrypted_file = need_str("sha256_of_encrypted_file");
  m.encrypted_size_bytes = opt_num("encrypted_size_bytes").value_or(0);
  m.tool_version = opt_str("tool_version");
  return m;
}

void write_manifest_file(const std::string& path, const Manifest& m) {
  std::string json = manifest_to_json(m);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot write manifest " + path + ": " +
                             std::strerror(errno));
  const char* p = json.data();
  size_t left = json.size();
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      throw std::runtime_error("cannot write manifest " + path + ": " +
                               std::strerror(e));
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  if (fsync(fd) != 0 || ::close(fd) != 0)
    throw std::runtime_error("cannot flush manifest " + path);
}

Manifest read_manifest_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot read manifest " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return manifest_from_json(ss.str());
}

}  // namespace lwsbk
