#include <unity.h>

#include "nimbus/audio_cue.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

// CUM-296: the silent-play cue. Tested here rather than on hardware because the whole
// bug is a pure UX-honesty decision - "is this volume too low to hear a track, and if
// so what do we say" - with no speaker in the loop. A silent-play regression (the cue
// never fires, or fires with the wrong copy) would be invisible in a quick bench look:
// playback "works", it just makes no sound, which is exactly the trap.

// The floor is a real gate: below it the cue must fire, at/above it must not. Asserted
// as a boundary so a future re-tune of kAudibleFloorPct still has a test on both sides.
static void test_floor_boundary() {
  TEST_ASSERT_TRUE(volumeInaudible(0));
  TEST_ASSERT_TRUE(volumeInaudible(5));                    // the owner's live repro value
  TEST_ASSERT_TRUE(volumeInaudible(kAudibleFloorPct - 1));
  TEST_ASSERT_FALSE(volumeInaudible(kAudibleFloorPct));    // boundary is INCLUSIVE-audible
  TEST_ASSERT_FALSE(volumeInaudible(85));                  // clearly audible in the ticket
  TEST_ASSERT_FALSE(volumeInaudible(100));
}

// The floor must be a low value: it is a "near silent" guard, not a "quiet" one. If it
// crept up toward normal listening levels the cue would nag on every ordinary play.
static void test_floor_is_low() {
  TEST_ASSERT_TRUE_MESSAGE(kAudibleFloorPct >= 5, "floor too low to catch the silent-play trap");
  TEST_ASSERT_TRUE_MESSAGE(kAudibleFloorPct <= 20, "floor too high - would nag at normal volume");
}

// Below the floor: a non-empty, honest cue that names the actual level and the next step.
static void test_cue_present_and_names_the_level() {
  const std::string cue = lowVolumeCue(5);
  TEST_ASSERT_FALSE_MESSAGE(cue.empty(), "a below-floor play must surface a cue");
  TEST_ASSERT_TRUE_MESSAGE(cue.find("5%") != std::string::npos, "cue must name the current level");
  TEST_ASSERT_TRUE_MESSAGE(cue.find("Sound") != std::string::npos, "cue must name the next step");
}

// At/above the floor: no cue at all (never nag when the track is audible).
static void test_no_cue_when_audible() {
  TEST_ASSERT_TRUE(lowVolumeCue(kAudibleFloorPct).empty());
  TEST_ASSERT_TRUE(lowVolumeCue(50).empty());
  TEST_ASSERT_TRUE(lowVolumeCue(100).empty());
}

// Copy discipline (project rule): the cue is user-facing and reads aloud over Telegram,
// so it must be ASCII with no em dash and no space-hyphen-space. Asserted over EVERY
// below-floor value so no single percentage produces off-style copy.
static void test_cue_copy_is_clean_ascii() {
  for (uint8_t v = 0; v < kAudibleFloorPct; ++v) {
    const std::string cue = lowVolumeCue(v);
    TEST_ASSERT_FALSE(cue.empty());
    for (unsigned char c : cue)
      TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7f, "cue must be printable ASCII (no em dash / UTF-8)");
    TEST_ASSERT_TRUE_MESSAGE(cue.find(" - ") == std::string::npos, "no space-hyphen-space in copy");
    // No JSON-breaking characters: the cue is spliced verbatim into the media.play JSON
    // result, so a stray quote or backslash would corrupt it.
    TEST_ASSERT_TRUE_MESSAGE(cue.find('"') == std::string::npos, "cue must not contain a double quote");
    TEST_ASSERT_TRUE_MESSAGE(cue.find('\\') == std::string::npos, "cue must not contain a backslash");
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_floor_boundary);
  RUN_TEST(test_floor_is_low);
  RUN_TEST(test_cue_present_and_names_the_level);
  RUN_TEST(test_no_cue_when_audible);
  RUN_TEST(test_cue_copy_is_clean_ascii);
  return UNITY_END();
}
