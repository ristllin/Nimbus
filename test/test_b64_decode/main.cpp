#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/util/b64_decode.h"

using nimbus::b64::StreamDecoder;

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> decode_all(const std::string& b64) {
  StreamDecoder d;
  std::vector<uint8_t> out;
  d.feed(b64.data(), b64.size(), [&](uint8_t b) { out.push_back(b); });
  return out;
}

// ---- known vectors (RFC 4648) ----------------------------------------------

static void test_known_vectors() {
  auto h = decode_all("SGVsbG8=");  // "Hello"
  TEST_ASSERT_EQUAL_UINT(5, h.size());
  TEST_ASSERT_EQUAL_STRING("Hello", std::string(h.begin(), h.end()).c_str());

  auto f = decode_all("Zm9vYmFy");  // "foobar"
  TEST_ASSERT_EQUAL_STRING("foobar", std::string(f.begin(), f.end()).c_str());

  TEST_ASSERT_EQUAL_UINT(0, decode_all("").size());
}

// A byte value that is NOT valid base64 output-safe unless the decoder is right:
// 0x00..0xFF round-trips through a base64 the test hard-codes.
static void test_all_byte_values_roundtrip() {
  // base64 of bytes 0x00,0x01,...,0x0F
  auto v = decode_all("AAECAwQFBgcICQoLDA0ODw==");
  TEST_ASSERT_EQUAL_UINT(16, v.size());
  for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_UINT8(i, v[i]);
}

// The whole point of the streaming decoder: chunk boundaries must not matter, and
// interior whitespace/newlines (serial framing) must be skipped.
static void test_chunked_and_framed_equals_whole() {
  const std::string b64 = "AAECAwQFBgcICQoLDA0ODw==";
  auto whole = decode_all(b64);

  // Feed one char at a time, splitting the string arbitrarily, with newlines and
  // spaces sprinkled in - the decoder must ignore framing and match the whole run.
  StreamDecoder d;
  std::vector<uint8_t> pieced;
  auto out = [&](uint8_t b) { pieced.push_back(b); };
  for (size_t i = 0; i < b64.size(); i++) {
    if (i == 3 || i == 7) d.feed('\n', out);   // framing between chunks
    if (i == 5) d.feed(' ', out);
    d.feed(b64[i], out);
  }
  TEST_ASSERT_EQUAL_UINT(whole.size(), pieced.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(whole.data(), pieced.data(), whole.size());
}

// A longer payload fed in uneven blocks reproduces the same bytes (the FSPUT path
// streams a multi-MB file in serial-sized blocks).
static void test_block_boundaries() {
  // 24 bytes 0x10..0x27 -> base64
  const std::string b64 = "EBESExQVFhcYGRobHB0eHyAhIiMkJSYn";
  std::vector<uint8_t> expect;
  for (int i = 0x10; i < 0x10 + 24; i++) expect.push_back((uint8_t)i);

  StreamDecoder d;
  std::vector<uint8_t> got;
  auto out = [&](uint8_t b) { got.push_back(b); };
  // feed in blocks of 5 chars
  for (size_t i = 0; i < b64.size(); i += 5)
    d.feed(b64.data() + i, std::min<size_t>(5, b64.size() - i), out);

  TEST_ASSERT_EQUAL_UINT(expect.size(), got.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect.data(), got.data(), expect.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_known_vectors);
  RUN_TEST(test_all_byte_values_roundtrip);
  RUN_TEST(test_chunked_and_framed_equals_whole);
  RUN_TEST(test_block_boundaries);
  return UNITY_END();
}
