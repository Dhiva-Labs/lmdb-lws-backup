#include "crypto.h"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <vector>

#include "test_util.h"

using namespace lwsbk;

namespace {

constexpr uint32_t kChunk = 4096;
TempDir* g_tmp = nullptr;

SecretKey test_key() {
  // Fixed all-0x42 key, base64 of 32 bytes.
  return SecretKey::from_base64("QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=");
}

std::vector<unsigned char> pattern(size_t n) {
  std::vector<unsigned char> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<unsigned char>((i * 2654435761u) >> 7);
  return v;
}

void write_file(const std::string& path, const std::vector<unsigned char>& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
  CHECK(f.good());
}

std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
}

StreamResult encrypt_file(const std::string& in, const std::string& out,
                          const SecretKey& key,
                          const std::function<bool()>& producer_ok = nullptr) {
  int ifd = ::open(in.c_str(), O_RDONLY | O_CLOEXEC);
  int ofd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  CHECK(ifd >= 0 && ofd >= 0);
  StreamResult r;
  try {
    r = encrypt_fd_stream(ifd, ofd, key, kChunk, producer_ok, nullptr);
  } catch (...) {
    ::close(ifd);
    ::close(ofd);
    throw;
  }
  ::close(ifd);
  ::close(ofd);
  return r;
}

StreamResult decrypt_file(const std::string& in, const std::string& out,
                          const SecretKey& key) {
  int ifd = ::open(in.c_str(), O_RDONLY | O_CLOEXEC);
  int ofd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  CHECK(ifd >= 0 && ofd >= 0);
  StreamResult r;
  try {
    r = decrypt_fd_stream(ifd, ofd, key, nullptr);
  } catch (...) {
    ::close(ifd);
    ::close(ofd);
    throw;
  }
  ::close(ifd);
  ::close(ofd);
  return r;
}

void test_round_trip_sizes() {
  SecretKey key = test_key();
  for (size_t size : {size_t{0}, size_t{1}, size_t{kChunk},
                      size_t{kChunk} * 3 + 7}) {
    auto data = pattern(size);
    std::string plain = g_tmp->sub("plain.bin");
    std::string enc = g_tmp->sub("enc.bin");
    std::string dec = g_tmp->sub("dec.bin");
    write_file(plain, data);

    StreamResult er = encrypt_file(plain, enc, key);
    CHECK_MSG(er.plaintext_bytes == size, "size %zu", size);
    CHECK(sha256_file_hex(enc) == to_hex(er.sha256, 32));

    StreamResult dr = decrypt_file(enc, dec, key);
    CHECK(dr.plaintext_bytes == size);
    CHECK(to_hex(dr.sha256, 32) == to_hex(er.sha256, 32));
    CHECK_MSG(read_file(dec) == data, "round trip mismatch at size %zu", size);
  }
}

void test_wrong_key_fails() {
  SecretKey key = test_key();
  auto data = pattern(kChunk * 2 + 100);
  write_file(g_tmp->sub("p2.bin"), data);
  encrypt_file(g_tmp->sub("p2.bin"), g_tmp->sub("e2.bin"), key);

  SecretKey other =
      SecretKey::from_base64("Q0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0NDQ0M=");
  CHECK_THROWS(decrypt_file(g_tmp->sub("e2.bin"), g_tmp->sub("d2.bin"), other));
}

void test_tampering_fails() {
  SecretKey key = test_key();
  auto data = pattern(kChunk + 50);
  write_file(g_tmp->sub("p3.bin"), data);
  encrypt_file(g_tmp->sub("p3.bin"), g_tmp->sub("e3.bin"), key);
  auto enc = read_file(g_tmp->sub("e3.bin"));

  // Flip a ciphertext byte.
  auto t1 = enc;
  t1[60] ^= 0x01;
  write_file(g_tmp->sub("t1.bin"), t1);
  CHECK_THROWS(decrypt_file(g_tmp->sub("t1.bin"), g_tmp->sub("o.bin"), key));

  // Corrupt the magic.
  auto t2 = enc;
  t2[0] ^= 0xff;
  write_file(g_tmp->sub("t2.bin"), t2);
  CHECK_THROWS(decrypt_file(g_tmp->sub("t2.bin"), g_tmp->sub("o.bin"), key));

  // Tamper with the chunk-size field (bound as AD of the first block).
  auto t3 = enc;
  t3[12] ^= 0x01;
  write_file(g_tmp->sub("t3.bin"), t3);
  CHECK_THROWS(decrypt_file(g_tmp->sub("t3.bin"), g_tmp->sub("o.bin"), key));
}

void test_truncation_and_trailing_fail() {
  SecretKey key = test_key();
  auto data = pattern(kChunk * 2 + 9);
  write_file(g_tmp->sub("p4.bin"), data);
  encrypt_file(g_tmp->sub("p4.bin"), g_tmp->sub("e4.bin"), key);
  auto enc = read_file(g_tmp->sub("e4.bin"));

  auto cut5 = enc;
  cut5.resize(cut5.size() - 5);
  write_file(g_tmp->sub("c5.bin"), cut5);
  CHECK_THROWS(decrypt_file(g_tmp->sub("c5.bin"), g_tmp->sub("o.bin"), key));

  // Remove the whole final block (9 + ABYTES bytes).
  auto nofinal = enc;
  nofinal.resize(nofinal.size() -
                 (9 + crypto_secretstream_xchacha20poly1305_ABYTES));
  write_file(g_tmp->sub("nf.bin"), nofinal);
  CHECK_THROWS(decrypt_file(g_tmp->sub("nf.bin"), g_tmp->sub("o.bin"), key));

  auto trail = enc;
  trail.push_back(0xaa);
  write_file(g_tmp->sub("tr.bin"), trail);
  CHECK_THROWS(decrypt_file(g_tmp->sub("tr.bin"), g_tmp->sub("o.bin"), key));
}

void test_producer_failure_gate() {
  SecretKey key = test_key();
  auto data = pattern(kChunk * 2);
  write_file(g_tmp->sub("p5.bin"), data);

  CHECK_THROWS(encrypt_file(g_tmp->sub("p5.bin"), g_tmp->sub("e5.bin"), key,
                            [] { return false; }));
  // Whatever was written before the failure must not decrypt as complete.
  CHECK_THROWS(decrypt_file(g_tmp->sub("e5.bin"), g_tmp->sub("o.bin"), key));
}

void test_key_loading() {
  CHECK_THROWS(SecretKey::from_base64("dG9vc2hvcnQ="));  // 8 bytes
  CHECK_THROWS(SecretKey::from_base64("!!!not base64!!!"));
  CHECK_THROWS(SecretKey::from_env("LWSBK_TEST_UNSET_VAR"));

  setenv("LWSBK_TEST_KEY_VAR", "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=",
         1);
  SecretKey k = SecretKey::from_env("LWSBK_TEST_KEY_VAR");
  CHECK(k.data()[0] == 0x42 && k.data()[31] == 0x42);

  // Raw 32-byte key file with 0600.
  std::string kf = g_tmp->sub("key.raw");
  write_file(kf, std::vector<unsigned char>(32, 0x99));
  chmod(kf.c_str(), 0600);
  SecretKey k2 = SecretKey::from_file(kf);
  CHECK(k2.data()[0] == 0x99);

  // Loose permissions must be rejected.
  chmod(kf.c_str(), 0644);
  CHECK_THROWS(SecretKey::from_file(kf));
  chmod(kf.c_str(), 0600);

  // Base64 key file with trailing newline.
  std::string kb = g_tmp->sub("key.b64");
  std::string b64 = "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=\n";
  write_file(kb, std::vector<unsigned char>(b64.begin(), b64.end()));
  chmod(kb.c_str(), 0600);
  SecretKey k3 = SecretKey::from_file(kb);
  CHECK(k3.data()[0] == 0x42);

  // Garbage key file.
  std::string kg = g_tmp->sub("key.bad");
  write_file(kg, std::vector<unsigned char>(10, 0x01));
  chmod(kg.c_str(), 0600);
  CHECK_THROWS(SecretKey::from_file(kg));
}

}  // namespace

int main() {
  TempDir tmp("crypto");
  g_tmp = &tmp;

  RUN(test_round_trip_sizes);
  RUN(test_wrong_key_fails);
  RUN(test_tampering_fails);
  RUN(test_truncation_and_trailing_fail);
  RUN(test_producer_failure_gate);
  RUN(test_key_loading);

  return test_exit();
}
