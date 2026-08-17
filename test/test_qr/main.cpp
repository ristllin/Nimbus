#include <unity.h>

#include <string>

#include "nimbus/qr.h"
#include "qr_vectors.h"

using namespace nimbus::qr;

void setUp() {}
void tearDown() {}

// Read bit (x,y) from a row-major, MSB-first, bit=dark packed module map - the
// exact packing gen_qr_vectors.py emits and QrCode uses.
static bool refModule(const uint8_t* modules, int size, int x, int y) {
  const int idx = y * size + x;
  return (modules[idx >> 3] >> (7 - (idx & 7))) & 1;
}

// KNOWN-ANSWER: encode() must reproduce Project Nayuki's reference qrcodegen
// output bit-for-bit (same algorithm, same config: byte mode, ECC M, no ECC
// boost, penalty mask). A match proves the codes are standard + scannable.
static void test_matches_reference_vectors() {
  for (size_t i = 0; i < kQrVectorCount; ++i) {
    const QrVector& v = kQrVectors[i];
    QrCode qr;
    TEST_ASSERT_TRUE_MESSAGE(encode(v.text, qr), v.text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(v.version, qr.version, v.text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(v.size, qr.size, v.text);
    for (int y = 0; y < v.size; ++y)
      for (int x = 0; x < v.size; ++x)
        TEST_ASSERT_EQUAL_MESSAGE(refModule(v.modules, v.size, x, y),
                                  qr.module(x, y), v.text);
  }
}

// Structural invariants (independent of the vectors): the three finder patterns
// (7x7 dark border + light ring + 3x3 core) sit at the corners of every symbol.
static void assertFinder(const QrCode& qr, int ox, int oy) {
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 7; ++x) {
      const bool onBorder = (x == 0 || x == 6 || y == 0 || y == 6);
      const bool inCore = (x >= 2 && x <= 4 && y >= 2 && y <= 4);
      const bool dark = onBorder || inCore;
      TEST_ASSERT_EQUAL(dark, qr.module(ox + x, oy + y));
    }
}

static void test_finder_patterns_present() {
  QrCode qr;
  TEST_ASSERT_TRUE(encode("http://192.168.4.1/", qr));
  assertFinder(qr, 0, 0);                 // top-left
  assertFinder(qr, qr.size - 7, 0);       // top-right
  assertFinder(qr, 0, qr.size - 7);       // bottom-left
}

static void test_deterministic() {
  QrCode a, b;
  TEST_ASSERT_TRUE(encode("http://nimbus.local/", a));
  TEST_ASSERT_TRUE(encode("http://nimbus.local/", b));
  TEST_ASSERT_EQUAL(a.size, b.size);
  for (int y = 0; y < a.size; ++y)
    for (int x = 0; x < a.size; ++x)
      TEST_ASSERT_EQUAL(a.module(x, y), b.module(x, y));
}

static void test_empty_and_overflow_rejected() {
  QrCode qr;
  TEST_ASSERT_FALSE(encode("", qr));       // empty -> false, zeroed
  TEST_ASSERT_EQUAL(0, qr.version);
  // > v6-M byte capacity (106) must fail rather than truncate.
  TEST_ASSERT_FALSE(encode(std::string(200, 'x'), qr));
}

// module() is bounds-safe so renderers can probe the quiet zone unguarded.
static void test_out_of_bounds_is_light() {
  QrCode qr;
  TEST_ASSERT_TRUE(encode("http://192.168.4.1/", qr));
  TEST_ASSERT_FALSE(qr.module(-1, 0));
  TEST_ASSERT_FALSE(qr.module(0, -1));
  TEST_ASSERT_FALSE(qr.module(qr.size, 0));
  TEST_ASSERT_FALSE(qr.module(0, qr.size));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_matches_reference_vectors);
  RUN_TEST(test_finder_patterns_present);
  RUN_TEST(test_deterministic);
  RUN_TEST(test_empty_and_overflow_rejected);
  RUN_TEST(test_out_of_bounds_is_light);
  return UNITY_END();
}
