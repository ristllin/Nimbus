#include <unity.h>

#include <cstring>
#include <string>

#include "nimbus/action_feedback.h"
#include "nimbus/sfx_map.h"

using namespace nimbus::action;

void setUp() {}
void tearDown() {}

// The whole point of the helper: the cue is a function of the OUTCOME only, and
// the three outcomes are DISTINGUISHABLE in BOTH channels. If Ok and Failed ever
// share a sound or a ring colour the owner is back to "can't tell it worked".
static void test_outcomes_are_distinct_in_both_channels() {
  const Cues ok = cuesFor(Outcome::Ok);
  const Cues ack = cuesFor(Outcome::Acknowledged);
  const Cues fail = cuesFor(Outcome::Failed);

  // Distinct ring cue per outcome.
  TEST_ASSERT_EQUAL_INT((int)RingCue::Success, (int)ok.ring);
  TEST_ASSERT_EQUAL_INT((int)RingCue::Ack, (int)ack.ring);
  TEST_ASSERT_EQUAL_INT((int)RingCue::Failure, (int)fail.ring);
  TEST_ASSERT_NOT_EQUAL((int)ok.ring, (int)fail.ring);
  TEST_ASSERT_NOT_EQUAL((int)ok.ring, (int)ack.ring);
  TEST_ASSERT_NOT_EQUAL((int)ack.ring, (int)fail.ring);

  // Distinct sound per outcome.
  TEST_ASSERT_NOT_EQUAL((int)ok.sfx, (int)fail.sfx);
  TEST_ASSERT_NOT_EQUAL((int)ok.sfx, (int)ack.sfx);
  TEST_ASSERT_NOT_EQUAL((int)ack.sfx, (int)fail.sfx);
}

// Failure MUST voice the Error tone and paint the red ring - this is the exact
// gap the owner reported ("I've never seen an error after a button press").
static void test_failure_is_the_error_tone_and_red() {
  const Cues fail = cuesFor(Outcome::Failed);
  TEST_ASSERT_EQUAL_INT((int)nimbus::sfx::Ev::Error, (int)fail.sfx);
  TEST_ASSERT_EQUAL_INT((int)RingCue::Failure, (int)fail.ring);
}

// The reused tones are real, embedded-safe events (each maps to a known slug, so
// the device can resolve a clip - all three ship an embedded clip too).
static void test_cue_sounds_have_valid_slugs() {
  for (Outcome o : {Outcome::Ok, Outcome::Acknowledged, Outcome::Failed}) {
    const Cues c = cuesFor(o);
    const char* s = nimbus::sfx::slug(c.sfx);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(std::strlen(s) > 0);
  }
  TEST_ASSERT_EQUAL_STRING("agent_done", nimbus::sfx::slug(cuesFor(Outcome::Ok).sfx));
  TEST_ASSERT_EQUAL_STRING("agent_spawn",
                           nimbus::sfx::slug(cuesFor(Outcome::Acknowledged).sfx));
  TEST_ASSERT_EQUAL_STRING("error", nimbus::sfx::slug(cuesFor(Outcome::Failed).sfx));
}

// The screen-line budget guard: printable ASCII, single line, <= 48 chars.
static void test_screen_line_ok_rejects_bad_lines() {
  TEST_ASSERT_TRUE(screenLineOk("SD card mounted"));
  TEST_ASSERT_TRUE(screenLineOk("No SD card found. Reseat it and rescan."));

  TEST_ASSERT_FALSE(screenLineOk(nullptr));
  TEST_ASSERT_FALSE(screenLineOk(""));
  TEST_ASSERT_FALSE(screenLineOk("line one\nline two"));  // newline
  TEST_ASSERT_FALSE(screenLineOk("has\ttab"));            // control char
  TEST_ASSERT_FALSE(screenLineOk("caf\xc3\xa9"));         // non-ASCII (UTF-8 e-acute)
  TEST_ASSERT_FALSE(screenLineOk("dash \xe2\x80\x94 here"));  // em dash bytes

  // Boundary: exactly 48 passes, 49 fails.
  const std::string at = std::string(kMaxActionLineLen, 'x');
  const std::string over = std::string(kMaxActionLineLen + 1, 'x');
  TEST_ASSERT_TRUE(screenLineOk(at.c_str()));
  TEST_ASSERT_FALSE(screenLineOk(over.c_str()));
}

// Test the CLASS, not the instance: EVERY non-null line in the whole catalog fits
// the screen budget and is printable ASCII. A new over-long or non-ASCII action
// line fails here, in CI, not on a customer's panel.
static void test_every_catalog_line_fits_the_budget() {
  for (unsigned i = 0; i < (unsigned)MenuAction::COUNT; ++i) {
    const MenuActionCopy& c = copyFor((MenuAction)i);
    for (const char* line : {c.ok, c.fail, c.ack}) {
      if (line == nullptr) continue;   // nullptr = "the action's own surface shows it"
      TEST_ASSERT_TRUE_MESSAGE(screenLineOk(line), line);
    }
  }
}

// No em dash anywhere in the catalog (the project-wide punctuation ban). Belt and
// suspenders alongside the pre-commit hook: a byte scan across every shipped line.
static void test_catalog_has_no_em_dash() {
  for (unsigned i = 0; i < (unsigned)MenuAction::COUNT; ++i) {
    const MenuActionCopy& c = copyFor((MenuAction)i);
    for (const char* line : {c.ok, c.fail, c.ack}) {
      if (line == nullptr) continue;
      TEST_ASSERT_NULL_MESSAGE(std::strstr(line, "\xe2\x80\x94"), line);
    }
  }
}

// lineFor resolves the outcome to the right catalog field.
static void test_line_for_resolves_by_outcome() {
  const MenuActionCopy& sd = copyFor(MenuAction::RescanSd);
  TEST_ASSERT_EQUAL_STRING(sd.ok, lineFor(sd, Outcome::Ok));
  TEST_ASSERT_EQUAL_STRING(sd.fail, lineFor(sd, Outcome::Failed));
  TEST_ASSERT_EQUAL_PTR(sd.ack, lineFor(sd, Outcome::Acknowledged));  // both null
}

// Catalog shape spot-checks: the fire-and-forget actions are acknowledge-only
// (no faked Ok/Failed), and the knowable ones carry Ok + Failed.
static void test_catalog_shape() {
  // Rescan SD reports a real success and a real failure with a next step.
  const MenuActionCopy& sd = copyFor(MenuAction::RescanSd);
  TEST_ASSERT_NOT_NULL(sd.ok);
  TEST_ASSERT_NOT_NULL(sd.fail);

  // Cloud pairing is genuinely fire-and-forget: acknowledge only, never a fake ok.
  const MenuActionCopy& cp = copyFor(MenuAction::CloudPair);
  TEST_ASSERT_NULL(cp.ok);
  TEST_ASSERT_NULL(cp.fail);
  TEST_ASSERT_NOT_NULL(cp.ack);

  // Publish AP is fire-and-forget too.
  const MenuActionCopy& ap = copyFor(MenuAction::PublishAp);
  TEST_ASSERT_NULL(ap.ok);
  TEST_ASSERT_NOT_NULL(ap.ack);

  // Reset always succeeds locally: an ok line, no failure path.
  const MenuActionCopy& rs = copyFor(MenuAction::Reset);
  TEST_ASSERT_NOT_NULL(rs.ok);
  TEST_ASSERT_NULL(rs.fail);
}

// Out-of-range action is safe: an all-null entry, no words, no crash.
static void test_copy_for_out_of_range_is_null() {
  const MenuActionCopy& c = copyFor(MenuAction::COUNT);
  TEST_ASSERT_NULL(c.ok);
  TEST_ASSERT_NULL(c.fail);
  TEST_ASSERT_NULL(c.ack);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_outcomes_are_distinct_in_both_channels);
  RUN_TEST(test_failure_is_the_error_tone_and_red);
  RUN_TEST(test_cue_sounds_have_valid_slugs);
  RUN_TEST(test_screen_line_ok_rejects_bad_lines);
  RUN_TEST(test_every_catalog_line_fits_the_budget);
  RUN_TEST(test_catalog_has_no_em_dash);
  RUN_TEST(test_line_for_resolves_by_outcome);
  RUN_TEST(test_catalog_shape);
  RUN_TEST(test_copy_for_out_of_range_is_null);
  return UNITY_END();
}
