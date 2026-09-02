#include <unity.h>

#include <cctype>
#include <cstring>
#include <string>

#include "nimbus/orch/danger_zone.h"
#include "nimbus/orch/file_store.h"   // FileStore::quotaForCard - quota truth across sizes

using namespace nimbus::orch;
using FS = nimbus::orch::FileStore;

void setUp() {}
void tearDown() {}

// ---- confirm matrix (the type-to-confirm class) ------------------------------
// The whole matrix at once, not one pair: for EVERY (action i, action j), the phrase
// action i requires satisfies action j's gate iff i == j. So each action's own phrase
// fires it, and no action's phrase can ever fire a different (possibly heavier) one.
static void test_confirm_matrix_each_phrase_fires_only_its_own_action() {
  const int n = static_cast<int>(DangerAction::Count);
  TEST_ASSERT_EQUAL_INT(3, n);   // FactoryReset, EraseStorage, FormatCard
  for (int i = 0; i < n; ++i) {
    const char* phrase = confirmPhraseFor(static_cast<DangerAction>(i));
    // Every action has a real, non-empty confirm phrase - a new action that landed
    // without one (or with an out-of-range enum) would trip here.
    TEST_ASSERT_NOT_NULL(phrase);
    TEST_ASSERT_TRUE(phrase[0] != '\0');
    for (int j = 0; j < n; ++j) {
      const char* expected = confirmPhraseFor(static_cast<DangerAction>(j));
      // phrase (what action i needs typed) fires action j's gate iff i == j.
      if (i == j) TEST_ASSERT_TRUE(confirmOk(expected, phrase));
      else        TEST_ASSERT_FALSE(confirmOk(expected, phrase));
    }
  }
}

// A missing / empty / wrong-case / trailing-space / partial phrase is rejected for
// EVERY action - the confirm gate holds across the class, not just for one action.
static void test_confirm_gate_rejects_bad_input_for_every_action() {
  for (int i = 0; i < static_cast<int>(DangerAction::Count); ++i) {
    const char* p = confirmPhraseFor(static_cast<DangerAction>(i));
    TEST_ASSERT_FALSE(confirmOk(p, nullptr));          // missing
    TEST_ASSERT_FALSE(confirmOk(p, ""));               // empty
    TEST_ASSERT_FALSE(confirmOk(p, "yes"));            // unrelated
    // wrong case
    std::string lower = p;
    for (char& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    TEST_ASSERT_FALSE(confirmOk(p, lower.c_str()));
    // trailing space and a partial prefix never satisfy the exact match
    TEST_ASSERT_FALSE(confirmOk(p, (std::string(p) + " ").c_str()));
    TEST_ASSERT_FALSE(confirmOk(p, std::string(p).substr(0, 1).c_str()));
  }
}

// The table is internally consistent: distinct non-empty slugs and phrases, and the
// slug/phrase for each action is stable (frozen wire the /api routes + HIL depend on).
static void test_matrix_table_is_complete_and_distinct() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DangerAction::Count),
                        (int)(sizeof(kDangerActions) / sizeof(kDangerActions[0])));
  for (size_t a = 0; a < sizeof(kDangerActions) / sizeof(kDangerActions[0]); ++a) {
    TEST_ASSERT_TRUE(kDangerActions[a].slug[0] != '\0');
    TEST_ASSERT_TRUE(kDangerActions[a].confirm[0] != '\0');
    for (size_t b = a + 1; b < sizeof(kDangerActions) / sizeof(kDangerActions[0]); ++b) {
      TEST_ASSERT_NOT_EQUAL(0, std::strcmp(kDangerActions[a].slug, kDangerActions[b].slug));
      TEST_ASSERT_NOT_EQUAL(0, std::strcmp(kDangerActions[a].confirm, kDangerActions[b].confirm));
    }
  }
  // Frozen wire pins (the web UI + HIL send these exact strings/slugs).
  TEST_ASSERT_EQUAL_STRING("factory-reset", kDangerActions[0].slug);
  TEST_ASSERT_EQUAL_STRING("FACTORY RESET", kDangerActions[0].confirm);
  TEST_ASSERT_EQUAL_STRING("sdreset", kDangerActions[1].slug);
  TEST_ASSERT_EQUAL_STRING("ERASE STORAGE", kDangerActions[1].confirm);
  TEST_ASSERT_EQUAL_STRING("sdformat", kDangerActions[2].slug);
  TEST_ASSERT_EQUAL_STRING("FORMAT CARD", kDangerActions[2].confirm);
}

// ---- offer matrix (the no-lying-knob class) ----------------------------------
// A control the owner can see but that can only fail is the CUM-290 lying-knob class.
// Full-card Format is offered only when a card is physically present AND the firmware
// has the primitive wired; the always-on actions are offered regardless (they refuse
// loudly with no card, which is honest).
static void test_offer_matrix_no_control_can_only_fail() {
  // Format: needs BOTH the hook and a present card - all four combinations.
  TEST_ASSERT_FALSE(offerFormatCard(false, false));  // no primitive, no card
  TEST_ASSERT_FALSE(offerFormatCard(false, true));   // no primitive - never advertise
  TEST_ASSERT_FALSE(offerFormatCard(true, false));   // primitive but NO card -> hidden
  TEST_ASSERT_TRUE(offerFormatCard(true, true));     // primitive + card -> shown
  // Format's rule is NeedsCard; the wipe actions are Always (refuse loudly on no card).
  TEST_ASSERT_EQUAL_INT((int)OfferRule::NeedsCard, (int)offerRuleFor(DangerAction::FormatCard));
  TEST_ASSERT_EQUAL_INT((int)OfferRule::Always, (int)offerRuleFor(DangerAction::FactoryReset));
  TEST_ASSERT_EQUAL_INT((int)OfferRule::Always, (int)offerRuleFor(DangerAction::EraseStorage));
  // shouldOffer generically: Always ignores card state; NeedsCard follows it.
  TEST_ASSERT_TRUE(shouldOffer(OfferRule::Always, false));
  TEST_ASSERT_TRUE(shouldOffer(OfferRule::Always, true));
  TEST_ASSERT_FALSE(shouldOffer(OfferRule::NeedsCard, false));
  TEST_ASSERT_TRUE(shouldOffer(OfferRule::NeedsCard, true));
}

// ---- quota truth across sizes (host-mockable) --------------------------------
// The artifact-store quota is card capacity minus a fixed reserve, or 0 when the card
// is unsupported (< 1 GB) or smaller than the reserve. Tested as a class over many
// sizes incl. the exact boundaries, never a single point, and never underflowing.
static void test_quota_truth_across_card_sizes() {
  const uint64_t MB = 1024ull * 1024;
  const uint64_t GB = 1024ull * MB;
  const uint64_t reserve = FS::kSdReserveBytes;    // 512 MB
  const uint64_t minCard = FS::kSdMinCardBytes;    // 1 GB
  struct Case { uint64_t card; bool supported; uint64_t quota; };
  const Case cases[] = {
    {0,             false, 0},                 // no card
    {512 * MB,      false, 0},                 // below the 1 GB minimum
    {minCard - 1,   false, 0},                 // just below the minimum
    {minCard,       true,  minCard - reserve}, // exactly the minimum -> 512 MB usable
    {2 * GB,        true,  2 * GB - reserve},
    {16 * GB,       true,  16 * GB - reserve},
    {64 * GB,       true,  64 * GB - reserve},
    {512 * GB,      true,  512 * GB - reserve},
  };
  for (const auto& c : cases) {
    TEST_ASSERT_EQUAL(c.supported, FS::sdCardSupported(c.card));
    TEST_ASSERT_EQUAL_UINT64(c.quota, FS::quotaForCard(c.card));
    // Truth invariants that must hold for ANY size: quota never exceeds the card, and
    // an unsupported card reports exactly 0 (never a silent tiny quota, never underflow).
    TEST_ASSERT_TRUE(FS::quotaForCard(c.card) <= c.card);
    if (!c.supported) TEST_ASSERT_EQUAL_UINT64(0, FS::quotaForCard(c.card));
  }
}

// A store limited to the unsupported-card quota (0) refuses every real save - the
// honest end state when no usable card is present (quota==0 is not a knob that lies).
static void test_zero_quota_refuses_saves() {
  FS::Limits lim;
  lim.maxTotalBytes = FS::quotaForCard(512 * 1024ull * 1024);  // unsupported -> 0
  TEST_ASSERT_EQUAL_UINT64(0, lim.maxTotalBytes);
  FS store(lim);
  std::string err;
  // Any real write would exceed a 0-byte total quota, so it is refused up front.
  TEST_ASSERT_TRUE(store.wouldExceed("p", "f", 1, err));
}

// ---- identity-preservation invariants (unchanged policy, pinned) -------------
// Nothing user-facing survives a factory reset (CUM-230): the keep-list beyond the
// board's physical identity is EMPTY - not even the device name.
static void test_factory_keeps_nothing_user_facing() {
  TEST_ASSERT_EQUAL_INT(0, kFactoryKeepKeyCount);
}

// Factory reset seeds Orchestrator mode so a reset device boots into the setup AP with
// the onboarding wizard reachable (merely clearing the mode key defaults to Notifier).
static void test_factory_seeds_setup_mode() {
  TEST_ASSERT_EQUAL_INT(1, kFactoryResetSeedMode);  // == sys::Mode::Orchestrator
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_confirm_matrix_each_phrase_fires_only_its_own_action);
  RUN_TEST(test_confirm_gate_rejects_bad_input_for_every_action);
  RUN_TEST(test_matrix_table_is_complete_and_distinct);
  RUN_TEST(test_offer_matrix_no_control_can_only_fail);
  RUN_TEST(test_quota_truth_across_card_sizes);
  RUN_TEST(test_zero_quota_refuses_saves);
  RUN_TEST(test_factory_keeps_nothing_user_facing);
  RUN_TEST(test_factory_seeds_setup_mode);
  return UNITY_END();
}
