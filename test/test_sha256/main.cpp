#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/util/sha256.h"
#include "nimbus/util/b64url.h"

using nimbus::crypto::Sha256;

void setUp() {}
void tearDown() {}

static std::string hex(const std::string& raw) {
  static const char* d = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out += d[c >> 4];
    out += d[c & 0xf];
  }
  return out;
}

// --- FIPS 180-4 / NIST known-answer vectors, hashed as a CLASS -----------------
struct ShaVec { const char* in; const char* hex; };
static const ShaVec kShaVecs[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"The quick brown fox jumps over the lazy dog",
     "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
};

void test_sha256_vectors() {
  for (const auto& v : kShaVecs) {
    std::string got = hex(Sha256::digest(std::string(v.in)));
    TEST_ASSERT_EQUAL_STRING(v.hex, got.c_str());
  }
}

// Streaming in arbitrary chunks must match the one-shot digest (the device feeds
// bytes as they arrive off the wire).
void test_sha256_streaming_matches() {
  const std::string msg =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  Sha256 one;
  one.update(msg);
  uint8_t da[32];
  one.final(da);
  for (size_t chunk = 1; chunk <= 9; chunk++) {
    Sha256 s;
    for (size_t i = 0; i < msg.size(); i += chunk)
      s.update(reinterpret_cast<const uint8_t*>(msg.data()) + i,
               std::min(chunk, msg.size() - i));
    uint8_t db[32];
    s.final(db);
    TEST_ASSERT_EQUAL_MEMORY(da, db, 32);
  }
}

// --- base64url (RFC 4648 §5, no padding) --------------------------------------
struct B64Vec { const char* in; const char* out; };
static const B64Vec kB64Vecs[] = {
    {"", ""},
    {"f", "Zg"},
    {"fo", "Zm8"},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg"},
    {"fooba", "Zm9vYmE"},
    {"foobar", "Zm9vYmFy"},
};

void test_b64url_vectors() {
  for (const auto& v : kB64Vecs)
    TEST_ASSERT_EQUAL_STRING(v.out, nimbus::b64::urlEncode(std::string(v.in)).c_str());
}

void test_b64url_is_url_safe() {
  // Bytes 0xfb 0xff 0xbf encode to "+/+/" in standard base64; base64url must
  // remap to '-'/'_' and never emit '+', '/', or '='.
  const uint8_t bytes[] = {0xfb, 0xff, 0xbf, 0xfb, 0xff, 0xbf};
  std::string s = nimbus::b64::urlEncode(bytes, sizeof(bytes));
  TEST_ASSERT_EQUAL(std::string::npos, s.find('+'));
  TEST_ASSERT_EQUAL(std::string::npos, s.find('/'));
  TEST_ASSERT_EQUAL(std::string::npos, s.find('='));
  TEST_ASSERT_TRUE(s.find('_') != std::string::npos || s.find('-') != std::string::npos);
}

// --- RFC 7636 Appendix B: the canonical PKCE S256 example ---------------------
// verifier -> SHA-256 -> base64url = the published challenge. This locks the
// exact composition the OAuth flow relies on.
void test_pkce_rfc7636_example() {
  const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  std::string challenge = nimbus::b64::urlEncode(Sha256::digest(verifier));
  TEST_ASSERT_EQUAL_STRING("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", challenge.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_vectors);
  RUN_TEST(test_sha256_streaming_matches);
  RUN_TEST(test_b64url_vectors);
  RUN_TEST(test_b64url_is_url_safe);
  RUN_TEST(test_pkce_rfc7636_example);
  return UNITY_END();
}
