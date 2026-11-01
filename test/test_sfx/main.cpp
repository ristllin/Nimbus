#include <unity.h>

#include <cstring>

#include "nimbus/sfx_map.h"
#include "nimbus/speaker_memory.h"

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

// ---- the speaker's memory gate --------------------------------------------------------

// Lumi's numbers: a resting device plays its cues; the moment a cue's open failed on (11 KB
// internal free: 7.4 KB of RTC that cannot serve DMA and 3.4 KB of main SRAM) is refused, and
// so is every point short of the buffers plus the radio's reserve.
static void test_a_cue_opens_only_with_the_buffers_and_the_reserve_free() {
  const uint32_t floor = kSpeakerDmaNeedBytes + kCueDmaReserve;
  TEST_ASSERT_TRUE(speakerMemoryOk(SoundKind::Cue, 21876, 9716));     // Lumi at rest
  TEST_ASSERT_FALSE(speakerMemoryOk(SoundKind::Cue, 10880 - 7452, 900));   // the failure itself
  TEST_ASSERT_TRUE(speakerMemoryOk(SoundKind::Cue, floor, 4096));
  TEST_ASSERT_FALSE(speakerMemoryOk(SoundKind::Cue, floor - 1, 4096));
  TEST_ASSERT_FALSE(speakerMemoryOk(SoundKind::Cue, floor, kSpeakerDmaMinLargest - 1));   // one buffer cannot fit
  TEST_ASSERT_TRUE(speakerMemoryOk(SoundKind::Cue, floor, kSpeakerDmaMinLargest));
}

// What the owner asked for keeps a smaller reserve, never a smaller need, for every free size.
static void test_an_asked_sound_never_needs_more_than_a_cue() {
  for (uint32_t f = 0; f < 32768; f += 64) {
    if (speakerMemoryOk(SoundKind::Cue, f, 2048)) TEST_ASSERT_TRUE(speakerMemoryOk(SoundKind::Asked, f, 2048));
    if (f < kSpeakerDmaNeedBytes) TEST_ASSERT_FALSE(speakerMemoryOk(SoundKind::Asked, f, 2048));
  }
  TEST_ASSERT_TRUE(speakerMemoryOk(SoundKind::Asked, kSpeakerDmaNeedBytes + kAskedDmaReserve, 2048));
  TEST_ASSERT_FALSE(speakerMemoryOk(SoundKind::Asked, kSpeakerDmaNeedBytes + kAskedDmaReserve - 1, 2048));
}

static constexpr uint32_t kAmple = 32768;

// A failed open holds the cues back, doubling to the cap: the attempts per hour are bounded
// whatever the memory does. Count them (absence of thrash, not a final value).
static void test_failed_opens_back_off_to_a_bounded_rate() {
  CueGate g;
  uint32_t now = 1000, tries = 0;
  for (; now < 3600u * 1000u; now += 1000) {     // a cue every second for an hour, every open fails
    if (!g.allow(SoundKind::Cue, kAmple, 4096, now)) continue;
    ++tries;
    g.finished(false, true, now);
  }
  // 1, 2, 4, 8, 16, 16, 16 minutes of hold: 7 or 8 tries in the hour, not 3600.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(8, tries);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(6, tries);
  TEST_ASSERT_EQUAL_UINT32(CueGate::kHoldMaxMs, g.holdMs());
}

static void test_a_played_cue_ends_the_hold() {
  CueGate g;
  TEST_ASSERT_TRUE(g.allow(SoundKind::Cue, kAmple, 4096, 1000));
  g.finished(false, true, 1000);
  TEST_ASSERT_EQUAL_UINT32(CueGate::kHoldFirstMs, g.holdMs());
  TEST_ASSERT_FALSE(g.allow(SoundKind::Cue, kAmple, 4096, 1000 + CueGate::kHoldFirstMs - 1));
  TEST_ASSERT_TRUE(g.allow(SoundKind::Cue, kAmple, 4096, 1000 + CueGate::kHoldFirstMs));
  g.finished(true, false, 1000 + CueGate::kHoldFirstMs);
  TEST_ASSERT_EQUAL_UINT32(0, g.holdMs());
  TEST_ASSERT_TRUE(g.allow(SoundKind::Cue, kAmple, 4096, 1000 + CueGate::kHoldFirstMs + 1));
}

// A clip that is not there is not a memory failure: it starts no hold and ends none.
static void test_a_missing_clip_starts_no_hold() {
  CueGate g;
  g.finished(false, false, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, g.holdMs());
  g.finished(false, true, 2000);
  g.finished(false, false, 3000);
  TEST_ASSERT_EQUAL_UINT32(CueGate::kHoldFirstMs, g.holdMs());
}

// The hold survives millis() wrapping past zero.
static void test_the_hold_survives_the_millis_wrap() {
  CueGate g;
  const uint32_t start = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(g.allow(SoundKind::Cue, kAmple, 4096, start));
  g.finished(false, true, start);
  TEST_ASSERT_FALSE(g.allow(SoundKind::Cue, kAmple, 4096, start + 1000));   // past the wrap, still held
  TEST_ASSERT_TRUE(g.allow(SoundKind::Cue, kAmple, 4096, start + CueGate::kHoldFirstMs));
}

// The owner's own sound ignores the cue hold; memory still decides it, and neither counts.
static void test_an_asked_sound_ignores_the_hold() {
  CueGate g;
  g.finished(false, true, 1000);
  TEST_ASSERT_TRUE(g.allow(SoundKind::Asked, kAmple, 4096, 1001));
  TEST_ASSERT_FALSE(g.allow(SoundKind::Asked, 100, 4096, 1002));
  TEST_ASSERT_EQUAL_UINT32(0, g.skipped());
}

// Skipped cues say so, at most once per period, with the count since the last line.
static void test_skips_log_one_line_per_period() {
  CueGate g;
  TEST_ASSERT_FALSE(g.logDue(1));   // nothing skipped yet
  uint32_t lines = 0;
  for (uint32_t now = 1000; now < 3600u * 1000u; now += 1000) {
    TEST_ASSERT_FALSE(g.allow(SoundKind::Cue, 100, 100, now));
    if (g.logDue(now)) {
      ++lines;
      g.clearSkipped();
    }
  }
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(7, lines);   // 3600 skipped cues, a handful of lines
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(6, lines);
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
  RUN_TEST(test_a_cue_opens_only_with_the_buffers_and_the_reserve_free);
  RUN_TEST(test_an_asked_sound_never_needs_more_than_a_cue);
  RUN_TEST(test_failed_opens_back_off_to_a_bounded_rate);
  RUN_TEST(test_a_played_cue_ends_the_hold);
  RUN_TEST(test_a_missing_clip_starts_no_hold);
  RUN_TEST(test_the_hold_survives_the_millis_wrap);
  RUN_TEST(test_an_asked_sound_ignores_the_hold);
  RUN_TEST(test_skips_log_one_line_per_period);
  UNITY_END();
  return 0;
}
