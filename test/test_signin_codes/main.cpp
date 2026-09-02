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

// --- CUM-295: the hand-entry ("Show code") DISPLAY path has a longer TTL, and the
// scan/QR path keeps the short one. These test the CLASS rule "a displayed code must
// outlive a 2-minute read window, a scanned code need not" - not one instance.

// Display-path TTL honored: a code minted with DISPLAY_TTL_MS is still redeemable
// long past the 2-minute scan window, and dies exactly at its own (10 min) TTL.
static void test_display_ttl_outlives_scan_window() {
  const uint32_t t0 = 1000;
  codes->mint("DISPLAY01", t0, SigninCodes::DISPLAY_TTL_MS);   // 10-minute code
  // Past the old 2-minute wall (where the incident failed) it is STILL valid.
  TEST_ASSERT_EQUAL_UINT32(1, codes->liveCount(t0 + 121000));
  codes->clear();
  codes->mint("DISPLAY02", t0, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_TRUE(codes->redeem("DISPLAY02", t0 + 121000));   // 2 min + 1 s: ok
  codes->clear();
  codes->mint("DISPLAY03", t0, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_TRUE(codes->redeem("DISPLAY03",
                                 t0 + SigninCodes::DISPLAY_TTL_MS - 1));  // just inside
  codes->clear();
  codes->mint("DISPLAY04", t0, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_FALSE(codes->redeem("DISPLAY04",
                                  t0 + SigninCodes::DISPLAY_TTL_MS));     // exactly at TTL: dead
}

// QR/scan-path TTL unchanged: a default-minted code still dies at 2 minutes even
// while a display code minted at the same instant lives on in the same table.
static void test_scan_ttl_unchanged_beside_display() {
  const uint32_t t0 = 5000;
  codes->mint("SCAN0000", t0);                                 // default (2 min)
  codes->mint("DISP0000", t0, SigninCodes::DISPLAY_TTL_MS);    // display (10 min)
  const uint32_t at = t0 + SigninCodes::DEFAULT_TTL_MS;        // exactly 2 min later
  TEST_ASSERT_FALSE(codes->redeem("SCAN0000", at));            // scan code expired
  TEST_ASSERT_TRUE(codes->redeem("DISP0000", at));             // display code alive
  TEST_ASSERT_EQUAL_UINT32(120000u, SigninCodes::DEFAULT_TTL_MS);
  TEST_ASSERT_EQUAL_UINT32(600000u, SigninCodes::DISPLAY_TTL_MS);
}

// Used display code dies: single-use holds regardless of TTL length.
static void test_display_code_single_use() {
  codes->mint("ONESHOT0", 1000, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_TRUE(codes->redeem("ONESHOT0", 2000));           // first use
  TEST_ASSERT_FALSE(codes->redeem("ONESHOT0", 2001));          // dead after use
}

// --- SigninDisplayCode: the on-screen lifecycle behind the honest countdown. ---
using nimbus::SigninDisplayCode;

static void test_display_lifecycle_expiry_and_remint() {
  SigninDisplayCode d;
  TEST_ASSERT_TRUE(d.expired(0));            // unset reads as expired -> caller mints
  d.set(1000, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_FALSE(d.expired(1000 + 121000));                       // alive past 2 min
  TEST_ASSERT_FALSE(d.expired(1000 + SigninCodes::DISPLAY_TTL_MS - 1));
  TEST_ASSERT_TRUE(d.expired(1000 + SigninCodes::DISPLAY_TTL_MS));   // expires at its TTL
  // Re-mint while shown: set() again gives a fresh full window.
  d.set(1000 + SigninCodes::DISPLAY_TTL_MS, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_FALSE(d.expired(1000 + SigninCodes::DISPLAY_TTL_MS + 1000));
}

static void test_display_countdown_secs_left() {
  SigninDisplayCode d;
  TEST_ASSERT_EQUAL_UINT32(0, d.secsLeft(0));       // unset -> 0
  d.set(1000, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_EQUAL_UINT32(600, d.secsLeft(1000));            // full 10:00 at mint (ceil)
  TEST_ASSERT_EQUAL_UINT32(599, d.secsLeft(1000 + 1000));     // 9:59 after 1 s
  TEST_ASSERT_EQUAL_UINT32(1, d.secsLeft(1000 + SigninCodes::DISPLAY_TTL_MS - 1));  // 0:01
  TEST_ASSERT_EQUAL_UINT32(0, d.secsLeft(1000 + SigninCodes::DISPLAY_TTL_MS));      // 0:00 at expiry
}

static void test_display_countdown_wraparound() {
  SigninDisplayCode d;
  const uint32_t near = 0xFFFFFF00u;             // expiry wraps past UINT32 max
  d.set(near, SigninCodes::DISPLAY_TTL_MS);
  TEST_ASSERT_FALSE(d.expired(near + 1000));
  TEST_ASSERT_EQUAL_UINT32(599, d.secsLeft(near + 1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mint_then_redeem_once);
  RUN_TEST(test_wrong_code_rejected);
  RUN_TEST(test_expiry);
  RUN_TEST(test_empty_and_overlong_ignored);
  RUN_TEST(test_capacity_reuse_evicts_oldest);
  RUN_TEST(test_expiry_wraparound);
  RUN_TEST(test_display_ttl_outlives_scan_window);
  RUN_TEST(test_scan_ttl_unchanged_beside_display);
  RUN_TEST(test_display_code_single_use);
  RUN_TEST(test_display_lifecycle_expiry_and_remint);
  RUN_TEST(test_display_countdown_secs_left);
  RUN_TEST(test_display_countdown_wraparound);
  return UNITY_END();
}
