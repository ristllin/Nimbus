#include <unity.h>

#include <cstdio>

#include <string>

#include "nimbus/attention.h"           // ScreenId - the frozen screen vocabulary
#include "nimbus/config_store.h"
#include "nimbus/harness/config.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/profile.h"
#include "nimbus/render_context.h"      // ScreenCtx.needsSetup
#include "nimbus/tft_render/fb565.h"    // Fb565, TapRegion
#include "nimbus/tft_render/screens.h"  // renderScreen - the real screen selection
#include "nimbus/touch_cal.h"
#include "nimbus/wifi/copy.h"           // setupCtaTitle/Hint - the CTA copy contract
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
// The fresh-device claim (distinct from test_touch_cal's orientTouch point tests):
// a board default's flags are FLIP-INDEPENDENT. flipping the display never rewrites
// the stored cal - in particular it never sets invertX, which would double-apply
// against orientTouch and put taps 180 out. Asserted for EVERY kind's default, so a
// kind whose default folded the flip into the cal would fail.
static void test_default_cal_flags_are_flip_independent() {
  for (int i = 0; i < static_cast<int>(TouchKind::Count); ++i) {
    const Cal d = boardDefaultCal(static_cast<TouchKind>(i));
    TEST_ASSERT_FALSE(d.invertX);  // the 180 is orientTouch's job, never baked in
  }
}

// Property across the whole panel: for every point, the flipped mapping is the exact
// point-reflection through the center, and flipping twice returns to canonical - so a
// stored cal stays valid across a flip, covering the panel with no dead zone and no
// double-mapped pixel. (test_touch_cal point-tests orientTouch at fixed corners; this
// is the panel-wide bijection, the fresh-device "cal survives a flip" property.)
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
  // The engine-level honest reply is driven and asserted in test/test_harness_turn
  // (CUM-211); NOT duplicated. What the fresh-device tier owns is the REGISTRY the
  // gate iterates - the recurring CUM-242 bug was a provider that some surface did
  // not enumerate, so it shipped ungated. Pin the canonical registry so a new
  // provider is a first-class slot the gate cannot miss, and that "custom" is routed
  // separately (never a first-class slot).
  TEST_ASSERT_TRUE(nimbus::orch::kProviderSlotCount > 0);
  int recommended = 0;
  for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; ++i) {
    const nimbus::orch::ProviderSlot& s = nimbus::orch::kProviderSlots[i];
    TEST_ASSERT_NOT_NULL(s.slug);
    TEST_ASSERT_TRUE(s.slug[0] != '\0');       // a real machine key
    TEST_ASSERT_NOT_NULL(s.keyField);          // a web key-write field to gate on
    TEST_ASSERT_TRUE(nimbus::orch::isProviderSlug(s.slug));
    TEST_ASSERT_EQUAL_PTR(&s, nimbus::orch::findProviderSlot(s.slug));
    if (s.recommended) ++recommended;
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, recommended, "exactly one recommended flagship slot");
  TEST_ASSERT_FALSE(nimbus::orch::isProviderSlug("custom"));  // routed separately
  TEST_ASSERT_NULL(nimbus::orch::findProviderSlot(""));

  // The fresh-boot INPUT the honest reply depends on: a device with a key on NO slot
  // reads as unconfigured. anyKeyed is the device-truth OR over exactly this registry
  // (plus custom); with no key on any slot it must be false.
  agent::HarnessConfig cfg;
  cfg.provider.hasKey = [](const std::string&) { return false; };
  cfg.provider.anyKeyed = [&cfg] {
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; ++i)
      if (cfg.provider.hasKey(nimbus::orch::kProviderSlots[i].slug)) return true;
    return cfg.provider.hasKey("custom");
  };
  TEST_ASSERT_FALSE(cfg.provider.anyKeyed());
}

// ---- 6. First-run screen selection: no dead ends (CUM-259) --------------------
//
// The owner hit this live: a factory-fresh, credless device that TAPPED away from
// Setup landed on the idle status screen, which looked onboarded and had no way
// back. The class rule the fix must encode is not "StatusIdle got a button" - it is
// "in the unprovisioned + not-onboarded state, EVERY screen the panel can draw is
// either setup-related or carries the way back to it." A NEW screen that strands an
// un-set-up owner must FAIL here, host-side, not on the owner's desk.

using nimbus::attn::ScreenId;
using nimbus::render::ScreenCtx;
using nimbus::tft::Fb565;
using nimbus::tft::Rendered;
using nimbus::tft::renderScreen;
using nimbus::tft::TapRegion;

// The ScreenCtx a first-run device actually builds: Orchestrator, not yet set up.
// needsSetup is the flag main.cpp's buildCtx sets from
// (orchMode && !provisioned && !onboarded).
static ScreenCtx firstRunCtx() {
  ScreenCtx c;
  c.deviceName = "Nimbus";
  c.modeName = "orchestrator";
  c.needsSetup = true;
  c.apName = "Nimbus-setup";
  c.apPass = "nimbus1234";
  c.setupUrl = "http://192.168.4.1/";
  c.fwVersion = "v0.0.0-test";
  return c;
}

// A tap region that leads back toward Setup: the first-run CTA (Setup), the header
// back chevron (Back/Home), or the gear (OpenMenu -> the always-backable settings
// menu). Any one of these means the screen is not a dead end.
static bool hasWayBack(const Rendered& r) {
  for (const auto& t : r.taps)
    if (t.action == TapRegion::Action::Setup || t.action == TapRegion::Action::Back ||
        t.action == TapRegion::Action::Home || t.action == TapRegion::Action::OpenMenu)
      return true;
  return false;
}

// Screens the device DRIVES itself out of and that register no tap-region exit on
// purpose: TouchCal is the tap-the-crosses modal (each tap advances it; it
// self-terminates), so it is guided, not a place a first-run owner can strand. It is
// the ONLY such screen; everything else must carry a real way back. A new screen
// added here must be a genuine self-terminating modal, chosen consciously.
static bool isGuidedSelfTerminating(ScreenId s) {
  return s == ScreenId::TouchCal;
}

// The class rule. Renders every screen the enum can name with a first-run context
// and requires each to be non-stranding. Pinned to the enum's cardinality so a NEW
// ScreenId cannot be added without deciding, here, how a credless owner gets back.
static void test_no_first_run_screen_strands() {
  // Frozen, append-only enum: TouchCal is the last member today. A new screen bumps
  // this and forces a visit to the rule below (mirror of the CUM-245 TouchKind guard).
  static_assert(static_cast<int>(ScreenId::TouchCal) == 15,
                "A ScreenId was added: decide in test_no_first_run_screen_strands() "
                "whether a first-run tap can reach it and how the owner gets back to "
                "Setup, then update this count (CUM-259 first-run dead-end guard).");

  const ScreenCtx c = firstRunCtx();
  for (int i = 0; i <= static_cast<int>(ScreenId::TouchCal); ++i) {
    const ScreenId s = static_cast<ScreenId>(i);
    Fb565 fb;
    const Rendered r = renderScreen(fb, s, c);
    char msg[128];
    std::snprintf(msg, sizeof msg,
                  "first-run screen %d strands the owner: no way back to Setup", i);
    TEST_ASSERT_TRUE_MESSAGE(hasWayBack(r) || isGuidedSelfTerminating(s), msg);
  }
}

// The specific fix, asserted at the seam: the idle status screen a credless owner
// lands on MUST offer the Setup CTA, and a provisioned device MUST NOT (the CTA is
// exactly the "looks onboarded" screen's replacement, never shown once set up).
static void test_statusidle_offers_setup_only_when_unset_up() {
  {
    Fb565 fb;
    const Rendered r = renderScreen(fb, ScreenId::StatusIdle, firstRunCtx());
    int setupTaps = 0;
    for (const auto& t : r.taps)
      if (t.action == TapRegion::Action::Setup) ++setupTaps;
    TEST_ASSERT_EQUAL_MESSAGE(1, setupTaps,
        "first-run StatusIdle must offer exactly one Set up Wi-Fi tap");
  }
  {
    ScreenCtx c = firstRunCtx();
    c.needsSetup = false;   // a set-up device
    Fb565 fb;
    const Rendered r = renderScreen(fb, ScreenId::StatusIdle, c);
    for (const auto& t : r.taps)
      TEST_ASSERT_MESSAGE(t.action != TapRegion::Action::Setup,
          "a set-up device must never show the Set up Wi-Fi CTA");
  }
  // The CTA copy contract (verb-led, sentence-case hint; ASCII device text).
  TEST_ASSERT_EQUAL_STRING("Set up Wi-Fi", nimbus::wifi::setupCtaTitle().c_str());
  TEST_ASSERT_EQUAL_STRING("Tap to open setup", nimbus::wifi::setupCtaHint().c_str());
}

int main() {
  UNITY_BEGIN();
  // 1. touch defaults - the class over all board models x TouchKind
  RUN_TEST(test_every_touchkind_has_a_measured_default);
  RUN_TEST(test_all_board_models_resolve_a_correct_default);
  // 2. flip compose - a stored cal survives a display flip
  RUN_TEST(test_default_cal_flags_are_flip_independent);
  RUN_TEST(test_flip_compose_is_a_bijection_over_the_panel);
  // 3. first-boot config resolution
  RUN_TEST(test_fresh_config_is_all_presets_no_overrides);
  RUN_TEST(test_absent_blob_leaves_defaults_untouched);
  // 4. onboarding posture
  RUN_TEST(test_fresh_boot_posture_is_profile_seeded);
  // 5. provider gate on zero keys
  RUN_TEST(test_zero_keys_reads_as_no_provider);
  // 6. first-run screen selection: no dead ends
  RUN_TEST(test_no_first_run_screen_strands);
  RUN_TEST(test_statusidle_offers_setup_only_when_unset_up);
  return UNITY_END();
}
