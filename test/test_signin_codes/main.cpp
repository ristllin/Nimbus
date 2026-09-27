// Host tests for nimbus::SigninCodes - the single-use, TTL-bounded sign-in code
// table behind CUM-45 (access token out of URLs). Pins the security-critical
// mechanics: single use, expiry (wraparound-safe), capacity reuse, and a
// constant-work redeem that rejects wrong/empty/over-long codes.
#include <unity.h>

#include <cstdio>

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

// --- CUM-453: the slot pick is expiry-aware. A displayed 10-minute code must never
// be evicted by the Sign-in QR's 2-minute scan codes, which re-mint every ~90 s.

// The incident shape: a hand-entry code on screen while the scan path keeps
// minting. Round-robin overwrote it after eight scan mints; now it outlives them.
static void test_display_code_survives_scan_churn() {
  const uint32_t t0 = 1000;
  codes->mint("SHOWN0001", t0, SigninCodes::DISPLAY_TTL_MS);
  char c[12];
  // 20 scan mints ~90 s apart plus a burst of 3 link codes each time: far more
  // mints than CAP inside the display code's own 10-minute life.
  uint32_t t = t0;
  for (int i = 0; t < t0 + SigninCodes::DISPLAY_TTL_MS - 90000; i++) {
    t += 30000;
    for (int k = 0; k < 3; k++) {
      std::snprintf(c, sizeof c, "S%02d%d", i, k);
      codes->mint(c, t);
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(codes->isRedeemable("SHOWN0001", t),
                           "scan-code churn evicted the displayed code");
  TEST_ASSERT_TRUE(codes->redeem("SHOWN0001", t));
}

// With every slot holding a live code, the one closest to expiry goes - never the
// long-lived displayed code - and an exact tie falls back to the oldest mint.
static void test_full_table_evicts_soonest_expiring() {
  codes->mint("DISP0001", 0, SigninCodes::DISPLAY_TTL_MS);       // expires at 600 s
  char c[8];
  for (int i = 0; i < (int)SigninCodes::CAP - 1; i++) {
    std::snprintf(c, sizeof c, "Q%d", i);
    codes->mint(c, uint32_t(1000 * (i + 1)));                    // expire at 121..127 s
  }
  TEST_ASSERT_EQUAL_UINT32(SigninCodes::CAP, codes->liveCount(8000));
  codes->mint("QNEW", 8000);
  TEST_ASSERT_FALSE(codes->isRedeemable("Q0", 8000));            // soonest to expire: evicted
  TEST_ASSERT_TRUE(codes->isRedeemable("DISP0001", 8000));       // the long code survives
  TEST_ASSERT_TRUE(codes->isRedeemable("Q1", 8000));
  TEST_ASSERT_TRUE(codes->isRedeemable("QNEW", 8000));
}

// A used or expired slot is reused before any redeemable code is touched.
static void test_used_and_expired_slots_reused_first() {
  char c[8];
  for (int i = 0; i < (int)SigninCodes::CAP; i++) {
    std::snprintf(c, sizeof c, "U%d", i);
    codes->mint(c, 0, SigninCodes::DISPLAY_TTL_MS);
  }
  TEST_ASSERT_TRUE(codes->redeem("U5", 10));                      // frees slot 5 (used)
  codes->mint("NEWA", 20, SigninCodes::DISPLAY_TTL_MS);
  for (int i = 0; i < (int)SigninCodes::CAP; i++) {
    if (i == 5) continue;
    std::snprintf(c, sizeof c, "U%d", i);
    TEST_ASSERT_TRUE_MESSAGE(codes->isRedeemable(c, 20), "a live code was evicted over a used slot");
  }
  TEST_ASSERT_TRUE(codes->isRedeemable("NEWA", 20));
}

// The class rule over an arbitrary mint/redeem sequence: while any slot holds
// nothing redeemable, a mint never evicts a redeemable code.
static void test_mint_never_evicts_live_while_a_slot_is_free() {
  uint32_t rng = 0x453u, t = 0;
  auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
  char minted[64][12];
  int count = 0;
  for (int step = 0; step < 400; step++) {
    t += next() % 40000;                                           // 0-40 s between events
    size_t liveBefore = codes->liveCount(t);
    bool liveNow[64];
    for (int i = 0; i < count; i++) liveNow[i] = codes->isRedeemable(minted[i], t);
    if (next() % 5 == 0 && count > 0) {                              // sometimes redeem one
      codes->redeem(minted[next() % uint32_t(count)], t);
      continue;
    }
    char c[12];
    std::snprintf(c, sizeof c, "R%05d", step);
    const uint32_t ttl = (next() % 3 == 0) ? SigninCodes::DISPLAY_TTL_MS
                                           : SigninCodes::DEFAULT_TTL_MS;
    codes->mint(c, t, ttl);
    if (liveBefore < SigninCodes::CAP) {
      for (int i = 0; i < count; i++)
        TEST_ASSERT_TRUE_MESSAGE(!liveNow[i] || codes->isRedeemable(minted[i], t),
                                 "a redeemable code was evicted while a slot was free");
    }
    std::snprintf(minted[count % 64], sizeof minted[0], "%s", c);
    if (count < 64) count++;
  }
}

// isRedeemable mirrors redeem() without consuming: wrong, used, expired, and
// malformed codes all read false, and asking twice changes nothing.
static void test_is_redeemable_is_read_only() {
  codes->mint("PEEK0001", 1000);
  TEST_ASSERT_TRUE(codes->isRedeemable("PEEK0001", 1000));
  TEST_ASSERT_TRUE(codes->isRedeemable("PEEK0001", 1000));        // no side effect
  TEST_ASSERT_FALSE(codes->isRedeemable("PEEK0002", 1000));
  TEST_ASSERT_FALSE(codes->isRedeemable("PEEK000", 1000));
  TEST_ASSERT_FALSE(codes->isRedeemable("", 1000));
  TEST_ASSERT_FALSE(codes->isRedeemable(nullptr, 1000));
  TEST_ASSERT_FALSE(codes->isRedeemable("PEEK0001", 1000 + SigninCodes::DEFAULT_TTL_MS));
  TEST_ASSERT_TRUE(codes->redeem("PEEK0001", 2000));              // peeking consumed nothing
  TEST_ASSERT_FALSE(codes->isRedeemable("PEEK0001", 2000));       // used -> no longer shown
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
  RUN_TEST(test_display_code_survives_scan_churn);
  RUN_TEST(test_full_table_evicts_soonest_expiring);
  RUN_TEST(test_used_and_expired_slots_reused_first);
  RUN_TEST(test_mint_never_evicts_live_while_a_slot_is_free);
  RUN_TEST(test_is_redeemable_is_read_only);
  return UNITY_END();
}
