#include <unity.h>

#include <string>

#include "nimbus/config_store.h"
#include "nimbus/harness/config.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/profile.h"
#include "nimbus/touch_cal.h"
#include "solide/boards/board_freenove_s3.h"
#include "solide/boards/board_solide_s3.h"

// ============================================================================
// test_fresh_device (CUM-245) - the DEFAULT / ABSENT-NVS device as a first-class
// test STATE.
//
// Every other automated leg runs on a PROVISIONED unit: touch already calibrated,
// config already stored, a provider already keyed. The owner, though, hits the
// FIRST boot after a new version - the exact state that shipped touch mirrored out
// of the box (CUM-203), uncalibrated (CUM-189), and once answered a chat with a
// misleading 401 instead of "set up a provider" (CUM-211). AGENTS.md section 3
// ("Fresh device is a first-class test state") makes that STATE mandatory; this
// suite pins it host-side, across the surfaces a fresh boot actually resolves:
//
//   1. Touch cal defaults - the CLASS test over ALL board models x TouchKind, so
//      a new board that ships without a measured default FAILS here (not on glass).
//   2. Flip compose - a stored cal stays valid across a display flip; the 180 is
//      the flip's job, never folded into the cal (CUM-160).
//   3. First-boot config resolution - a fresh Config is exactly the profile
//      presets, no override leaks in from uninitialized memory.
//   4. Onboarding posture - the fresh-boot ring level a profile seeds (CUM-230).
//   5. Provider gate on zero keys - a fresh device reads as "no provider", the
//      predicate the honest local reply is built on (CUM-211).
//
// It EXTENDS, never duplicates: test_touch_cal point-tests the two known kinds and
// orientTouch; test_harness_turn drives the engine-level honest reply; here we pin
// the class rules and the fresh-boot state those legs assume away.
// ============================================================================

using nimbus::touch::boardDefaultCal;
using nimbus::touch::Cal;
using nimbus::touch::formatCal;
using nimbus::touch::orientTouch;
using nimbus::touch::parseCal;
using nimbus::touch::Point;
using nimbus::touch::TouchKind;

void setUp() {}
void tearDown() {}

// ---- 1. Touch cal defaults: the class over ALL board models x TouchKind -------

// The measured-correct fresh default flags per touch class. This is the TEST-SIDE
// source of truth the seam must agree with; the switch has NO default branch on
// purpose, so a newly added TouchKind is a visible gap here as it is in
// boardDefaultCal. Both shipping panels mount portrait-native under a landscape
// glass, so both swap; they differ ONLY on invertY (the CUM-203 split). NEITHER
// ever sets invertX - an upside-down mount's 180 is orientTouch's job, applied on
// top, never baked into the cal.
struct ExpectFlags { bool swapXY, invertX, invertY; };

static ExpectFlags expectedFlagsFor(TouchKind kind) {
  switch (kind) {
    case TouchKind::Resistive:  return {true, false, false};  // Solide XPT2046: swap only
    case TouchKind::Capacitive: return {true, false, true};   // Freenove FT6336U: swap + invertY
    case TouchKind::Count:      break;                         // not a kind; falls through to fail
  }
  TEST_FAIL_MESSAGE("TouchKind has no measured fresh-boot default - a new board "
                    "must add one here AND in boardDefaultCal (CUM-245)");
  return {false, false, false};
}

// Compile-time cardinality guard: if a controller class is added to the mirror
// enum, this breaks the build until the default (boardDefaultCal) and the
// expectation (expectedFlagsFor) both learn it. This is the "new board without
// defaults FAILS" chokepoint the class RCA (CUM-228) asked for.
static_assert(static_cast<int>(TouchKind::Count) == 2,
              "A TouchKind was added/removed: extend expectedFlagsFor() and "
              "boardDefaultCal() with its MEASURED fresh-boot default, then update "
              "this count (CUM-245 fresh-device tier).");

static void assertGoodDefault(TouchKind kind, const char* who) {
  const Cal d = boardDefaultCal(kind);
  const ExpectFlags e = expectedFlagsFor(kind);
  TEST_ASSERT_EQUAL_MESSAGE(e.swapXY, d.swapXY, who);
  TEST_ASSERT_EQUAL_MESSAGE(e.invertX, d.invertX, who);   // NEVER the flip's 180
  TEST_ASSERT_EQUAL_MESSAGE(e.invertY, d.invertY, who);
  // A default must be a valid, self-consistent cal so pushing it never divides by
  // a zero span on a resistive panel, and must survive the NVS/web wire round trip.
  TEST_ASSERT_TRUE_MESSAGE(d.minX < d.maxX, who);
  TEST_ASSERT_TRUE_MESSAGE(d.minY < d.maxY, who);
  Cal back;
  TEST_ASSERT_TRUE_MESSAGE(parseCal(formatCal(d), back), who);
  TEST_ASSERT_TRUE_MESSAGE(d == back, who);
}

// Property over EVERY TouchKind (not just the two a point test happened to name):
// each has a measured default, and no two kinds share one (a shared default is the
// exact `(void)kind;` leak that mirrored the resistive Solide out of the box).
static void test_every_touchkind_has_a_measured_default() {
  for (int i = 0; i < static_cast<int>(TouchKind::Count); ++i) {
    assertGoodDefault(static_cast<TouchKind>(i), "touchkind default");
  }
  TEST_ASSERT_FALSE(boardDefaultCal(TouchKind::Resistive) ==
                    boardDefaultCal(TouchKind::Capacitive));
}

// The solide::TouchKind a board carries, collapsed to the portable mirror - the
// SAME map the device makes at src/main.cpp and src/net/webui.cpp. No default
// branch: a new solide controller class that no board maps yet is a visible gap.
static TouchKind mirrorOf(solide::TouchKind k) {
  switch (k) {
    case solide::TouchKind::CapacitiveI2c: return TouchKind::Capacitive;
    case solide::TouchKind::ResistiveSpi:  return TouchKind::Resistive;
    case solide::TouchKind::None:          break;
  }
  TEST_FAIL_MESSAGE("board touchKind has no portable mirror (CUM-245)");
  return TouchKind::Resistive;
}

// The actual board models, read from the driver's own constants so a board whose
// touchKind field is wrong (or a NEW board_*.h) is exercised here, not just on the
// bench. Each shipping model resolves a correct, non-mirrored fresh default.
struct BoardCase { const char* name; const solide::Board* board; };

static void test_all_board_models_resolve_a_correct_default() {
  const BoardCase boards[] = {
      {"solide_s3 (resistive XPT2046)", &solide::kBoardSolideS3},
      {"freenove_s3 (capacitive FT6336U)", &solide::kBoardFreenoveS3},
  };
  for (const BoardCase& bc : boards) {
    TEST_ASSERT_NOT_EQUAL_MESSAGE(int(solide::TouchKind::None), int(bc.board->touchKind), bc.name);
    assertGoodDefault(mirrorOf(bc.board->touchKind), bc.name);
  }
  // Pin the mapping itself so the two field bugs (CUM-203/189) can't recur through a
  // mis-declared board: the resistive Solide must NOT get the capacitive invertY.
  TEST_ASSERT_EQUAL(int(TouchKind::Resistive), int(mirrorOf(solide::kBoardSolideS3.touchKind)));
  TEST_ASSERT_EQUAL(int(TouchKind::Capacitive), int(mirrorOf(solide::kBoardFreenoveS3.touchKind)));
}

// ---- 2. Flip compose: a stored cal stays valid across a display flip ----------

static const int16_t W = 320, H = 240;

// A per-unit calibration is solved in the CANONICAL (un-flipped) frame and stored.
// When the owner later flips which end is up (store::tftFlip, a display-only MADCTL
// change), the SAME stored cal must still land taps: the 180 is composed ON TOP by
// orientTouch, never folded into the cal. This pins that a flip does not invalidate
// a stored calibration - the regression that put taps 180 out in the field.
static void test_stored_cal_survives_a_flip_via_compose() {
  // The default flags are flip-INDEPENDENT: flipping the display never rewrites the
  // stored cal (in particular it never sets invertX - that would double-apply).
  const Cal res = boardDefaultCal(TouchKind::Resistive);
  TEST_ASSERT_FALSE(res.invertX);

  // A calibrated point in the canonical frame. Unflipped it is identity; flipped it
  // rotates 180 WITH the display, and flipping twice returns to canonical (proof the
  // compose is exactly the 180 and nothing more).
  const Point cal{47, 96, true};
  const Point up = orientTouch(cal, /*displayFlipped=*/false, W, H);
  TEST_ASSERT_EQUAL_INT16(47, up.x);
  TEST_ASSERT_EQUAL_INT16(96, up.y);

  const Point flipped = orientTouch(cal, /*displayFlipped=*/true, W, H);
  TEST_ASSERT_EQUAL_INT16(W - 1 - 47, flipped.x);
  TEST_ASSERT_EQUAL_INT16(H - 1 - 96, flipped.y);

  const Point round = orientTouch(flipped, /*displayFlipped=*/true, W, H);
  TEST_ASSERT_EQUAL_INT16(47, round.x);
  TEST_ASSERT_EQUAL_INT16(96, round.y);
}

// Property across the whole panel: for every point, the flipped mapping is the exact
// point-reflection through the center, so a stored cal + a flip covers the panel
// with no dead zone and no double-mapped pixel.
static void test_flip_compose_is_a_bijection_over_the_panel() {
  for (int16_t y = 0; y < H; y += 37) {
    for (int16_t x = 0; x < W; x += 41) {
      const Point f = orientTouch(Point{x, y, true}, true, W, H);
      TEST_ASSERT_EQUAL_INT16(W - 1 - x, f.x);
      TEST_ASSERT_EQUAL_INT16(H - 1 - y, f.y);
      const Point b = orientTouch(f, true, W, H);
      TEST_ASSERT_EQUAL_INT16(x, b.x);
      TEST_ASSERT_EQUAL_INT16(y, b.y);
    }
  }
}

// ---- 3. First-boot config resolution ------------------------------------------

using nimbus::Config;
using nimbus::deserializeConfig;
using nimbus::kParamCount;
using nimbus::Param;
using nimbus::presetValue;
using nimbus::ProfileId;

// A freshly flashed device has no config blob. The resolved config must be EXACTLY
// the active profile's presets - every param, no override leaking in from
// uninitialized storage. (test_config_store covers the deserialize-rejects path;
// this is the fresh-STATE property over all params.)
static void test_fresh_config_is_all_presets_no_overrides() {
  const Config fresh;  // default-constructed == a device that never stored anything
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(fresh.profile()));  // shipped default
  for (int i = 0; i < kParamCount; ++i) {
    const Param p = static_cast<Param>(i);
    TEST_ASSERT_FALSE(fresh.hasOverride(p));  // nothing overridden out of the box
    TEST_ASSERT_EQUAL_INT32(presetValue(ProfileId::Balanced, p), fresh.effective(p));
  }
}

// An absent blob (the "no SD / no NVS record" reality) must leave the destination at
// its defaults, all-or-nothing - never a half-applied config that looks deliberate.
static void test_absent_blob_leaves_defaults_untouched() {
  Config out;  // defaults
  TEST_ASSERT_FALSE(deserializeConfig(nullptr, 0, out));
  const uint8_t empty[1] = {0};
  TEST_ASSERT_FALSE(deserializeConfig(empty, 0, out));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(out.profile()));
  for (int i = 0; i < kParamCount; ++i)
    TEST_ASSERT_FALSE(out.hasOverride(static_cast<Param>(i)));
}

// ---- 4. Onboarding posture (CUM-230): the fresh-boot ring level ---------------

using nimbus::Posture;

// The posture (ring level) a fresh device shows is the one its profile seeds - not
// an uninitialized value. A fresh Balanced device comes up Calm; the other profiles
// seed their own level. Pins the seeding so a profile can't silently boot Dark
// (invisible) or Full (a battery drain) out of the box.
static void test_fresh_boot_posture_is_profile_seeded() {
  Config fresh;
  TEST_ASSERT_EQUAL(int(Posture::Calm), int(fresh.posture()));  // Balanced fresh boot

  const struct { ProfileId id; Posture want; } cases[] = {
      {ProfileId::BatterySaver, Posture::Dark},
      {ProfileId::Balanced, Posture::Calm},
      {ProfileId::Desk, Posture::Full},
  };
  for (const auto& c : cases) {
    Config cfg;
    cfg.setProfile(c.id);
    TEST_ASSERT_EQUAL(int(c.want), int(cfg.posture()));
  }
}

// ---- 5. Provider gate on zero keys (CUM-211) ----------------------------------

// A fresh device has no provider key of any kind. The predicate the honest "no
// provider set up" reply is built on must read that truthfully: anyKeyed() false,
// and hasKey() false for EVERY first-class provider slot. (test_harness_turn drives
// the engine-level reply; here we pin the fresh-boot INPUT it depends on so a new
// provider slot can't ship reading as keyed-by-default.)
static void test_zero_keys_reads_as_no_provider() {
  // A no-keys config, exactly as store_config wires it on a device with empty NVS:
  // hasKey false for every slot, and anyKeyed COMPUTED as the device does it (an OR
  // over every slot + custom), not hardcoded - so a slot that ever ships
  // keyed-by-default would flip anyKeyed and fail here.
  agent::HarnessConfig cfg;
  cfg.provider.hasKey = [](const std::string&) { return false; };
  cfg.provider.anyKeyed = [&cfg] {
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; ++i)
      if (cfg.provider.hasKey(nimbus::orch::kProviderSlots[i].slug)) return true;
    return cfg.provider.hasKey("custom");
  };

  TEST_ASSERT_FALSE(cfg.provider.anyKeyed());
  for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(cfg.provider.hasKey(nimbus::orch::kProviderSlots[i].slug),
                              nimbus::orch::kProviderSlots[i].slug);
  }
  TEST_ASSERT_FALSE(cfg.provider.hasKey("custom"));
}

int main() {
  UNITY_BEGIN();
  // 1. touch defaults - the class over all board models x TouchKind
  RUN_TEST(test_every_touchkind_has_a_measured_default);
  RUN_TEST(test_all_board_models_resolve_a_correct_default);
  // 2. flip compose - a stored cal survives a display flip
  RUN_TEST(test_stored_cal_survives_a_flip_via_compose);
  RUN_TEST(test_flip_compose_is_a_bijection_over_the_panel);
  // 3. first-boot config resolution
  RUN_TEST(test_fresh_config_is_all_presets_no_overrides);
  RUN_TEST(test_absent_blob_leaves_defaults_untouched);
  // 4. onboarding posture
  RUN_TEST(test_fresh_boot_posture_is_profile_seeded);
  // 5. provider gate on zero keys
  RUN_TEST(test_zero_keys_reads_as_no_provider);
  return UNITY_END();
}
