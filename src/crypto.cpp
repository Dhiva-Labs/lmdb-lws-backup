#include "crypto.h"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace lwsbk {

namespace {

constexpr char kMagic[8] = {'L', 'W', 'S', 'B', 'K', 'U', 'P', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kPrefixLen = 16;  // magic + version + chunk_size
constexpr size_t kAbytes = crypto_secretstream_xchacha20poly1305_ABYTES;
constexpr size_t kHeaderBytes = crypto_secretstream_xchacha20poly1305_HEADERBYTES;

void ensure_sodium() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (sodium_init() < 0)
      throw std::runtime_error("sodium_init failed");
  });
}

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

void write_all(int fd, const unsigned char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("write");
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
}

// Reads until `len` bytes or EOF; returns bytes read.
size_t read_full(int fd, unsigned char* data, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::read(fd, data + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("read");
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
  return got;
}

void store_u32le(unsigned char* p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
  p[2] = static_cast<unsigned char>(v >> 16);
  p[3] = static_cast<unsigned char>(v >> 24);
}

uint32_t load_u32le(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Zeroes a secretstream state (contains key material) on scope exit.
struct StateGuard {
  crypto_secretstream_xchacha20poly1305_state st;
  ~StateGuard() { sodium_memzero(&st, sizeof(st)); }
};

// Zeroes a buffer (plaintext) on scope exit.
struct BufGuard {
  std::vector<unsigned char> buf;
  explicit BufGuard(size_t n) : buf(n) {}
  ~BufGuard() { sodium_memzero(buf.data(), buf.size()); }
};

}  // namespace

SecretKey::SecretKey() {
  ensure_sodium();
  bytes_ = static_cast<unsigned char*>(sodium_malloc(kKeyBytes));
  if (!bytes_) throw std::runtime_error("sodium_malloc failed for key storage");
}

SecretKey::~SecretKey() {
  if (bytes_) sodium_free(bytes_);  // sodium_free zeroes before unmapping
}

SecretKey::SecretKey(SecretKey&& o) noexcept : bytes_(o.bytes_) {
  o.bytes_ = nullptr;
}

SecretKey& SecretKey::operator=(SecretKey&& o) noexcept {
  if (this != &o) {
    if (bytes_) sodium_free(bytes_);
    bytes_ = o.bytes_;
    o.bytes_ = nullptr;
  }
  return *this;
}

SecretKey SecretKey::from_base64(const std::string& b64) {
  ensure_sodium();
  SecretKey key;
  unsigned char decoded[kKeyBytes + 8];
  size_t decoded_len = 0;
  if (sodium_base642bin(decoded, sizeof(decoded), b64.data(), b64.size(),
                        " \t\r\n", &decoded_len, nullptr,
                        sodium_base64_VARIANT_ORIGINAL) != 0) {
    throw std::runtime_error("encryption key is not a valid base64 32-byte key");
  }
  if (decoded_len != kKeyBytes) {
    sodium_memzero(decoded, sizeof(decoded));
    throw std::runtime_error(
        "encryption key must decode to exactly 32 bytes, got " +
        std::to_string(decoded_len));
  }
  std::memcpy(key.bytes_, decoded, kKeyBytes);
  sodium_memzero(decoded, sizeof(decoded));
  return key;
}

SecretKey SecretKey::from_env(const std::string& var) {
  const char* val = std::getenv(var.c_str());
  if (!val || !*val)
    throw std::runtime_error("environment variable " + var +
                             " is not set (expected base64-encoded 32-byte key)");
  return from_base64(val);
}

SecretKey SecretKey::from_file(const std::string& path) {
  ensure_sodium();
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw_errno("cannot open key file " + path);
  struct stat st;
  if (fstat(fd, &st) != 0) {
    ::close(fd);
    throw_errno("cannot stat key file " + path);
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    throw std::runtime_error("key file " + path + " is not a regular file");
  }
  if ((st.st_mode & 077) != 0) {
    ::close(fd);
    throw std::runtime_error("key file " + path +
                             " must not be group/world accessible (chmod 600 it)");
  }

  unsigned char raw[256];
  size_t n = 0;
  try {
    n = read_full(fd, raw, sizeof(raw));
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);

  if (n == sizeof(raw)) {
    sodium_memzero(raw, sizeof(raw));
    throw std::runtime_error("key file " + path + " is implausibly large");
  }

  SecretKey key;
  if (n == kKeyBytes) {
    std::memcpy(key.bytes_, raw, kKeyBytes);
    sodium_memzero(raw, sizeof(raw));
    return key;
  }
  std::string b64(reinterpret_cast<char*>(raw), n);
  sodium_memzero(raw, sizeof(raw));
  try {
    key = from_base64(b64);
  } catch (const std::exception&) {
    sodium_memzero(b64.data(), b64.size());
    throw std::runtime_error(
        "key file " + path +
        " must contain exactly 32 raw bytes or a base64-encoded 32-byte key");
  }
  sodium_memzero(b64.data(), b64.size());
  return key;
}

StreamResult encrypt_fd_stream(int in_fd, int out_fd, const SecretKey& key,
                               uint32_t chunk_size,
                               const std::function<bool()>& producer_ok,
                               const std::function<bool()>& abort_requested) {
  ensure_sodium();
  if (chunk_size == 0 || chunk_size > kMaxChunkSize)
    throw std::runtime_error("invalid encryption chunk size");

  StreamResult res;
  crypto_hash_sha256_state sha;
  crypto_hash_sha256_init(&sha);

  auto emit = [&](const unsigned char* data, size_t len) {
    write_all(out_fd, data, len);
    crypto_hash_sha256_update(&sha, data, len);
    res.ciphertext_bytes += len;
  };

  unsigned char prefix[kPrefixLen];
  std::memcpy(prefix, kMagic, sizeof(kMagic));
  store_u32le(prefix + 8, kFormatVersion);
  store_u32le(prefix + 12, chunk_size);

  StateGuard state;
  unsigned char header[kHeaderBytes];
  if (crypto_secretstream_xchacha20poly1305_init_push(&state.st, header,
                                                      key.data()) != 0)
    throw std::runtime_error("secretstream init_push failed");
  emit(prefix, sizeof(prefix));
  emit(header, sizeof(header));

  BufGuard cur(chunk_size), next(chunk_size);
  std::vector<unsigned char> cipher(chunk_size + kAbytes);

  size_t cur_len = read_full(in_fd, cur.buf.data(), chunk_size);
  bool first_chunk = true;
  for (;;) {
    if (abort_requested && abort_requested())
      throw std::runtime_error("encryption aborted by shutdown request");

    // Look ahead one chunk so the last one can carry TAG_FINAL. A short
    // read from read_full means EOF on the source.
    size_t next_len =
        cur_len == chunk_size ? read_full(in_fd, next.buf.data(), chunk_size)
                              : 0;
    const bool is_final = (next_len == 0);

    if (is_final && producer_ok && !producer_ok()) {
      throw std::runtime_error(
          "data producer reported failure; refusing to finalize encrypted "
          "stream");
    }

    const unsigned char* ad = first_chunk ? prefix : nullptr;
    size_t ad_len = first_chunk ? sizeof(prefix) : 0;
    unsigned long long clen = 0;
    unsigned char tag =
        is_final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;
    if (crypto_secretstream_xchacha20poly1305_push(
            &state.st, cipher.data(), &clen, cur.buf.data(), cur_len, ad,
            ad_len, tag) != 0)
      throw std::runtime_error("secretstream push failed");
    emit(cipher.data(), static_cast<size_t>(clen));
    res.plaintext_bytes += cur_len;
    first_chunk = false;

    if (is_final) break;
    std::swap(cur.buf, next.buf);
    cur_len = next_len;
  }

  crypto_hash_sha256_final(&sha, res.sha256);
  return res;
}

StreamResult decrypt_fd_stream(int in_fd, int out_fd, const SecretKey& key,
                               const std::function<bool()>& abort_requested) {
  ensure_sodium();
  StreamResult res;
  crypto_hash_sha256_state sha;
  crypto_hash_sha256_init(&sha);

  auto consume = [&](unsigned char* data, size_t want,
                     const char* what) -> size_t {
    size_t n = read_full(in_fd, data, want);
    crypto_hash_sha256_update(&sha, data, n);
    res.ciphertext_bytes += n;
    if (n < want && what)
      throw std::runtime_error(std::string("encrypted file truncated in ") +
                               what);
    return n;
  };

  unsigned char prefix[kPrefixLen];
  consume(prefix, sizeof(prefix), "file prefix");
  if (std::memcmp(prefix, kMagic, sizeof(kMagic)) != 0)
    throw std::runtime_error("not an lmdb-lws-backup encrypted file (bad magic)");
  if (load_u32le(prefix + 8) != kFormatVersion)
    throw std::runtime_error("unsupported backup format version " +
                             std::to_string(load_u32le(prefix + 8)));
  uint32_t chunk_size = load_u32le(prefix + 12);
  if (chunk_size == 0 || chunk_size > kMaxChunkSize)
    throw std::runtime_error("corrupt header: invalid chunk size");

  unsigned char header[kHeaderBytes];
  consume(header, sizeof(header), "stream header");

  StateGuard state;
  if (crypto_secretstream_xchacha20poly1305_init_pull(&state.st, header,
                                                      key.data()) != 0)
    throw std::runtime_error("invalid stream header (wrong key or corrupt file)");

  const size_t block = chunk_size + kAbytes;
  std::vector<unsigned char> cipher(block);
  BufGuard plain(chunk_size);

  bool first_chunk = true;
  for (;;) {
    if (abort_requested && abort_requested())
      throw std::runtime_error("decryption aborted by shutdown request");

    size_t n = consume(cipher.data(), block, nullptr);
    if (n == 0)
      throw std::runtime_error(
          "encrypted file truncated: stream ended without FINAL tag");
    if (n < kAbytes)
      throw std::runtime_error("encrypted file truncated mid-block");

    const unsigned char* ad = first_chunk ? prefix : nullptr;
    size_t ad_len = first_chunk ? sizeof(prefix) : 0;
    unsigned long long mlen = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(&state.st, plain.buf.data(),
                                                   &mlen, &tag, cipher.data(),
                                                   n, ad, ad_len) != 0)
      throw std::runtime_error(
          "decryption failed: wrong key or corrupted/tampered data");
    first_chunk = false;

    if (out_fd >= 0)
      write_all(out_fd, plain.buf.data(), static_cast<size_t>(mlen));
    res.plaintext_bytes += mlen;

    if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
      unsigned char probe;
      if (read_full(in_fd, &probe, 1) != 0)
        throw std::runtime_error("trailing data after end of encrypted stream");
      break;
    }
    if (n < block)
      throw std::runtime_error(
          "encrypted file truncated: short block without FINAL tag");
  }

  crypto_hash_sha256_final(&sha, res.sha256);
  return res;
}

std::string sha256_file_hex(const std::string& path) {
  ensure_sodium();
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw_errno("cannot open " + path);
  crypto_hash_sha256_state sha;
  crypto_hash_sha256_init(&sha);
  std::vector<unsigned char> buf(1u << 16);
  for (;;) {
    ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      errno = e;
      throw_errno("read " + path);
    }
    if (n == 0) break;
    crypto_hash_sha256_update(&sha, buf.data(), static_cast<size_t>(n));
  }
  ::close(fd);
  unsigned char digest[32];
  crypto_hash_sha256_final(&sha, digest);
  return to_hex(digest, sizeof(digest));
}

std::string to_hex(const unsigned char* data, size_t len) {
  static const char* hexd = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(hexd[data[i] >> 4]);
    out.push_back(hexd[data[i] & 0xf]);
  }
  return out;
}

}  // namespace lwsbk
