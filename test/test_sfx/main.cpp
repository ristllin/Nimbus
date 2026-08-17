#include <unity.h>

#include <cstring>

#include "nimbus/sfx_map.h"

using namespace nimbus::sfx;

void setUp() {}
void tearDown() {}

// Every event has a slug, and slugs round-trip through parseSlug (the console
// SFX <slug> command depends on this).
static void test_slugs_roundtrip() {
  for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++) {
    const char* s = slug((Ev)i);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(strlen(s) > 0);
    Ev back;
    TEST_ASSERT_TRUE_MESSAGE(parseSlug(s, back), s);
    TEST_ASSERT_EQUAL_INT((int)i, (int)back);
  }
  Ev e;
  TEST_ASSERT_FALSE(parseSlug("bogus_slug", e));
  TEST_ASSERT_FALSE(parseSlug("", e));
  TEST_ASSERT_FALSE(parseSlug(nullptr, e));
  TEST_ASSERT_NULL(slug(Ev::COUNT));
}

// Level "none" is silent for EVERY event in BOTH modes - the hard mute.
static void test_none_is_silent_everywhere() {
  for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++) {
    TEST_ASSERT_FALSE(shouldPlay((Ev)i, kLevelNone, true));
    TEST_ASSERT_FALSE(shouldPlay((Ev)i, kLevelNone, false));
  }
}

// The level ladder is monotonic: if an event plays at level L it plays at L+1.
static void test_ladder_monotonic() {
  for (int mode = 0; mode <= 1; mode++) {
    for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++) {
      for (uint8_t lvl = kLevelLight; lvl < kLevelHeavy; lvl++) {
        if (shouldPlay((Ev)i, lvl, mode))
          TEST_ASSERT_TRUE(shouldPlay((Ev)i, lvl + 1, mode));
      }
    }
  }
}

// Orchestrator heavy voices EVERYTHING; light voices exactly the critical set.
static void test_orchestrator_tiers() {
  for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++)
    TEST_ASSERT_TRUE(shouldPlay((Ev)i, kLevelHeavy, true));
  TEST_ASSERT_TRUE(shouldPlay(Ev::NeedsYou,   kLevelLight, true));
  TEST_ASSERT_TRUE(shouldPlay(Ev::Error,      kLevelLight, true));
  TEST_ASSERT_TRUE(shouldPlay(Ev::LowBattery, kLevelLight, true));
  TEST_ASSERT_TRUE(shouldPlay(Ev::Boot,       kLevelLight, true));
  TEST_ASSERT_FALSE(shouldPlay(Ev::WifiUp,     kLevelLight, true));
  TEST_ASSERT_FALSE(shouldPlay(Ev::AgentSpawn, kLevelLight, true));
  TEST_ASSERT_FALSE(shouldPlay(Ev::TurnStart,  kLevelMedium, true));  // heavy-only
}

// Notifier is significantly lighter EVEN AT HEAVY: per-job churn (spawn/turn/
// voice/etc.) is never voiced - the broker floods JobState during coding.
static void test_notifier_much_lighter() {
  TEST_ASSERT_FALSE(shouldPlay(Ev::AgentSpawn,  kLevelHeavy, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::TurnStart,   kLevelHeavy, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::ReplySent,   kLevelHeavy, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::VoiceListen, kLevelHeavy, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::MemSaved,    kLevelHeavy, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::SyncDone,    kLevelHeavy, false));
  // notifier light = attention-critical only
  TEST_ASSERT_TRUE(shouldPlay(Ev::NeedsYou,   kLevelLight, false));
  TEST_ASSERT_TRUE(shouldPlay(Ev::Error,      kLevelLight, false));
  TEST_ASSERT_TRUE(shouldPlay(Ev::LowBattery, kLevelLight, false));
  TEST_ASSERT_FALSE(shouldPlay(Ev::Boot,      kLevelLight, false));  // medium+
  // notifier heavy adds connectivity + done, nothing more
  TEST_ASSERT_TRUE(shouldPlay(Ev::WifiUp,    kLevelHeavy, false));
  TEST_ASSERT_TRUE(shouldPlay(Ev::BleDown,   kLevelHeavy, false));
  TEST_ASSERT_TRUE(shouldPlay(Ev::AgentDone, kLevelHeavy, false));
  // count check: notifier heavy voices strictly fewer events than orch heavy
  int notif = 0, orch = 0;
  for (unsigned i = 0; i < (unsigned)Ev::COUNT; i++) {
    if (shouldPlay((Ev)i, kLevelHeavy, false)) notif++;
    if (shouldPlay((Ev)i, kLevelHeavy, true)) orch++;
  }
  TEST_ASSERT_TRUE(notif * 2 <= orch);  // "significantly lighter"
}

// The rate gate: global gap collapses bursts; per-event cooldown collapses
// repeats; distinct events pass once the global gap elapses.
static void test_rate_gate() {
  RateGate g(300, 2000);
  TEST_ASSERT_TRUE(g.allow(Ev::AgentDone, 1000));
  TEST_ASSERT_FALSE(g.allow(Ev::Error, 1100));       // inside global gap
  TEST_ASSERT_TRUE(g.allow(Ev::Error, 1400));        // gap elapsed, new event
  TEST_ASSERT_FALSE(g.allow(Ev::AgentDone, 1800));   // event cooldown (1000+2000)
  TEST_ASSERT_FALSE(g.allow(Ev::AgentDone, 2900));   // still cooling
  TEST_ASSERT_TRUE(g.allow(Ev::AgentDone, 3100));    // cooled + gap ok
  g.reset();
  TEST_ASSERT_TRUE(g.allow(Ev::AgentDone, 3200));    // reset clears history
}

// First-ever event plays even at nowMs=0 (no false "seen" state at boot).
static void test_rate_gate_boot_edge() {
  RateGate g;
  TEST_ASSERT_TRUE(g.allow(Ev::Boot, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_slugs_roundtrip);
  RUN_TEST(test_none_is_silent_everywhere);
  RUN_TEST(test_ladder_monotonic);
  RUN_TEST(test_orchestrator_tiers);
  RUN_TEST(test_notifier_much_lighter);
  RUN_TEST(test_rate_gate);
  RUN_TEST(test_rate_gate_boot_edge);
  UNITY_END();
  return 0;
}
