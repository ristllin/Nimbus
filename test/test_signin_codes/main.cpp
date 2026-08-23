// Host tests for nimbus::SigninCodes - the single-use, TTL-bounded sign-in code
// table behind CUM-45 (access token out of URLs). Pins the security-critical
// mechanics: single use, expiry (wraparound-safe), capacity reuse, and a
// constant-work redeem that rejects wrong/empty/over-long codes.
#include <unity.h>

#include "nimbus/signin_codes.h"

using nimbus::SigninCodes;

static SigninCodes* codes = nullptr;

void setUp() { codes = new SigninCodes(120000); }        // 2 min TTL
void tearDown() { delete codes; codes = nullptr; }

static void test_mint_then_redeem_once() {
  TEST_ASSERT_TRUE(codes->mint("ABCD1234", 1000));
  TEST_ASSERT_EQUAL_UINT32(1, codes->liveCount(1000));
  TEST_ASSERT_TRUE(codes->redeem("ABCD1234", 1500));      // first use works
  TEST_ASSERT_FALSE(codes->redeem("ABCD1234", 1600));     // single use: second fails
  TEST_ASSERT_EQUAL_UINT32(0, codes->liveCount(1600));
}

static void test_wrong_code_rejected() {
  codes->mint("RIGHTCODE", 0);
  TEST_ASSERT_FALSE(codes->redeem("WRONGCODE", 10));
  TEST_ASSERT_FALSE(codes->redeem("RIGHT", 10));          // prefix, different length
  TEST_ASSERT_TRUE(codes->redeem("RIGHTCODE", 10));       // the real one still valid
}

static void test_expiry() {
  codes->mint("EXPIRES00", 1000);
  TEST_ASSERT_FALSE(codes->redeem("EXPIRES00", 1000 + 120000));   // exactly at TTL: expired
  codes->mint("EXPIRES01", 2000);
  TEST_ASSERT_TRUE(codes->redeem("EXPIRES01", 2000 + 119999));    // just inside TTL: ok
}

static void test_empty_and_overlong_ignored() {
  TEST_ASSERT_FALSE(codes->mint("", 0));
  TEST_ASSERT_FALSE(codes->mint("0123456789ABCDEF", 0));  // 16 chars >= MAXLEN
  TEST_ASSERT_FALSE(codes->redeem("", 0));
  TEST_ASSERT_EQUAL_UINT32(0, codes->liveCount(0));
}

static void test_capacity_reuse_evicts_oldest() {
  for (int i = 0; i < (int)SigninCodes::CAP; i++) {
    char c[8]; c[0] = 'C'; c[1] = char('0' + i); c[2] = '\0';
    codes->mint(c, 100);
  }
  TEST_ASSERT_EQUAL_UINT32(SigninCodes::CAP, codes->liveCount(100));
  codes->mint("CNEW", 100);                               // one more -> evicts C0
  TEST_ASSERT_FALSE(codes->redeem("C0", 100));            // oldest gone
  TEST_ASSERT_TRUE(codes->redeem("CNEW", 100));           // newest present
}

static void test_expiry_wraparound() {
  // now near UINT32 max, expiry wraps past 0: still handled by signed diff.
  const uint32_t near = 0xFFFFFF00u;
  codes->mint("WRAP", near);
  TEST_ASSERT_TRUE(codes->redeem("WRAP", near + 1000));   // within TTL across the wrap
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mint_then_redeem_once);
  RUN_TEST(test_wrong_code_rejected);
  RUN_TEST(test_expiry);
  RUN_TEST(test_empty_and_overlong_ignored);
  RUN_TEST(test_capacity_reuse_evicts_oldest);
  RUN_TEST(test_expiry_wraparound);
  return UNITY_END();
}
