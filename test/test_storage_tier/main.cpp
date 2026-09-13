#include <unity.h>

// Pure storage-tier decision (CUM-405): no Arduino, so it is host-tested here while
// memory_subsystem.cpp feeds it the real device inputs. Relative include - the
// native env compiles only test/ (build_src_filter -<*>), so we pull the header
// straight from src/ rather than through a library include path.
#include "../../src/agent/storage_tier.h"

using agent::memory::TierInputs;
using agent::memory::TierDecision;
using agent::memory::decideStorageTier;

void setUp() {}
void tearDown() {}

static TierDecision decide(bool mounted, bool prevSeen, bool cardData) {
  TierInputs in;
  in.mountedSd = mounted;
  in.prevSdSeen = prevSeen;
  in.cardHoldsData = cardData;
  return decideStorageTier(in);
}

// haveSd always mirrors mountedSd; the banner never depends on it directly.
static void test_haveSd_mirrors_mount() {
  TEST_ASSERT_TRUE(decide(true, false, false).haveSd);
  TEST_ASSERT_FALSE(decide(false, true, true).haveSd);
}

// A mounted card NEVER raises the banner - the live store is right there, whatever
// the stale previous-boot flag or a residual probe says.
static void test_mounted_never_banners() {
  TEST_ASSERT_FALSE(decide(true, false, false).sdMissingWithData);
  TEST_ASSERT_FALSE(decide(true, true, false).sdMissingWithData);
  TEST_ASSERT_FALSE(decide(true, false, true).sdMissingWithData);
  TEST_ASSERT_FALSE(decide(true, true, true).sdMissingWithData);
}

// The CUM-405 core case: no card mounted this boot, but a card was present the
// previous boot -> loud banner (this is the flaky-cold-joint-on-a-reboot path).
static void test_prev_seen_banners() {
  TEST_ASSERT_TRUE(decide(false, true, false).sdMissingWithData);
}

// No card mounted, but a non-empty vector blob is still readable off the raw card
// -> loud banner even with no prior flag (e.g. first boot after a flash).
static void test_card_data_banners() {
  TEST_ASSERT_TRUE(decide(false, false, true).sdMissingWithData);
}

// Genuinely card-less (no mount, never seen a card, nothing readable) -> NO banner.
// This is the honest degraded-flash device; nagging it would be the regression.
static void test_truly_cardless_quiet() {
  TEST_ASSERT_FALSE(decide(false, false, false).sdMissingWithData);
}

// Test the invariant, not the instance (AGENTS.md §3): assert the exact rule across
// ALL 8 input combinations. A new evidence source or a flipped condition that breaks
// "banner == not mounted AND (prev seen OR card has data)" fails here.
static void test_full_truth_table() {
  for (int m = 0; m < 2; m++)
    for (int p = 0; p < 2; p++)
      for (int c = 0; c < 2; c++) {
        const bool expectBanner = (m == 0) && (p == 1 || c == 1);
        TierDecision d = decide(m, p, c);
        TEST_ASSERT_EQUAL_MESSAGE(m == 1, d.haveSd, "haveSd must mirror mountedSd");
        TEST_ASSERT_EQUAL_MESSAGE(expectBanner, d.sdMissingWithData,
                                  "banner == !mounted && (prevSeen || cardHoldsData)");
      }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_haveSd_mirrors_mount);
  RUN_TEST(test_mounted_never_banners);
  RUN_TEST(test_prev_seen_banners);
  RUN_TEST(test_card_data_banners);
  RUN_TEST(test_truly_cardless_quiet);
  RUN_TEST(test_full_truth_table);
  return UNITY_END();
}
