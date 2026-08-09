#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Streaming authenticated encryption for backup artifacts.
//
// File format (.lmdbbak.enc):
//   [0..8)    magic "LWSBKUP1"
//   [8..12)   u32 LE format version (1)
//   [12..16)  u32 LE plaintext chunk size
//   [16..40)  crypto_secretstream_xchacha20poly1305 header
//   [40..)    ciphertext blocks of chunk_size+ABYTES, final block shorter,
//             tagged TAG_FINAL
// The 16-byte prefix is bound as additional data of the first block, so any
// header tampering fails authentication. A file whose stream lacks a FINAL
// tag is treated as truncated and always fails decryption.

namespace lwsbk {

inline constexpr size_t kKeyBytes = 32;
inline constexpr uint32_t kDefaultChunkSize = 1u << 20;  // 1 MiB
inline constexpr uint32_t kMaxChunkSize = 64u << 20;

// 32-byte key in sodium_malloc()ed (locked, guarded) memory, zeroed on free.
class SecretKey {
 public:
  SecretKey();
  ~SecretKey();
  SecretKey(const SecretKey&) = delete;
  SecretKey& operator=(const SecretKey&) = delete;
  SecretKey(SecretKey&& o) noexcept;
  SecretKey& operator=(SecretKey&& o) noexcept;

  unsigned char* data() noexcept { return bytes_; }
  const unsigned char* data() const noexcept { return bytes_; }

  // All loaders throw std::runtime_error with a message that never includes
  // key bytes. Base64 input must decode to exactly 32 bytes.
  static SecretKey from_base64(const std::string& b64);
  static SecretKey from_env(const std::string& var);
  // Key file must be 0600 or stricter and contain either exactly 32 raw
  // bytes or a base64 string.
  static SecretKey from_file(const std::string& path);

 private:
  unsigned char* bytes_ = nullptr;
};

struct StreamResult {
  uint64_t plaintext_bytes = 0;
  uint64_t ciphertext_bytes = 0;  // total bytes of the encrypted file
  unsigned char sha256[32] = {};  // over the complete encrypted file
};

// Reads plaintext from in_fd to EOF, writes the framed encrypted stream to
// out_fd. `producer_ok` is consulted when EOF is reached, BEFORE the final
// chunk is sealed: if it returns false (e.g. the LMDB copy thread failed),
// this throws and the output ends without a FINAL tag, guaranteeing it can
// never decrypt as a complete backup. `abort_requested` is polled between
// chunks; returning true aborts with an exception.
StreamResult encrypt_fd_stream(int in_fd, int out_fd, const SecretKey& key,
                               uint32_t chunk_size,
                               const std::function<bool()>& producer_ok,
                               const std::function<bool()>& abort_requested);

// Reads the framed encrypted stream from in_fd, writes plaintext to out_fd.
// Throws on authentication failure, truncation, or trailing garbage.
// sha256 in the result is over the ciphertext consumed (for manifest
// cross-checks).
StreamResult decrypt_fd_stream(int in_fd, int out_fd, const SecretKey& key,
                               const std::function<bool()>& abort_requested);

// sha256 of an existing file, hex-encoded (used by --verify).
std::string sha256_file_hex(const std::string& path);

std::string to_hex(const unsigned char* data, size_t len);

}  // namespace lwsbk
