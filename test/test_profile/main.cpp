#include <unity.h>

#include <cstring>
#include <map>
#include <string>

#include "nimbus/profile.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

static void test_presets_match_plan_table() {
  TEST_ASSERT_EQUAL(int(Posture::Dark), presetValue(ProfileId::BatterySaver, Param::Posture));
  TEST_ASSERT_EQUAL(int(Posture::Calm), presetValue(ProfileId::Balanced, Param::Posture));
  TEST_ASSERT_EQUAL(int(Posture::Full), presetValue(ProfileId::Desk, Param::Posture));
  TEST_ASSERT_EQUAL(10, presetValue(ProfileId::BatterySaver, Param::RingBrightness));
  TEST_ASSERT_EQUAL(30, presetValue(ProfileId::Balanced, Param::RingBrightness));
  TEST_ASSERT_EQUAL(60, presetValue(ProfileId::Desk, Param::RingBrightness));
  TEST_ASSERT_EQUAL(60000, presetValue(ProfileId::BatterySaver, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(30000, presetValue(ProfileId::Balanced, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(15000, presetValue(ProfileId::Desk, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(300, presetValue(ProfileId::Balanced, Param::DwellMs));
}

static void test_effective_uses_preset_then_override() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  TEST_ASSERT_EQUAL(30, c.effective(Param::RingBrightness));
  c.setOverride(Param::RingBrightness, 99);
  TEST_ASSERT_EQUAL(99, c.effective(Param::RingBrightness));
  c.clearOverride(Param::RingBrightness);
  TEST_ASSERT_EQUAL(30, c.effective(Param::RingBrightness));
}

// The core promise of the sparse-override model: switching profiles changes
// non-overridden values and preserves user overrides.
static void test_override_survives_profile_switch() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::AttnLedIndex, 22);
  c.setProfile(ProfileId::Desk);
  TEST_ASSERT_EQUAL(22, c.effective(Param::AttnLedIndex));
  TEST_ASSERT_EQUAL(60, c.effective(Param::RingBrightness));  // Desk preset
  TEST_ASSERT_EQUAL(int(Posture::Full), int(c.posture()));
}

static void test_clear_all_overrides() {
  Config c;
  c.setOverride(Param::RingFps, 5);
  c.setOverride(Param::DwellMs, 1000);
  TEST_ASSERT_TRUE(c.hasOverride(Param::RingFps));
  c.clearAllOverrides();
  TEST_ASSERT_FALSE(c.hasOverride(Param::RingFps));
  TEST_ASSERT_FALSE(c.hasOverride(Param::DwellMs));
}

static void test_selector_precedence_forced_vbus_user() {
  Selector s;
  s.setUser(ProfileId::Balanced);
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.resolve()));

  s.setVbus(true);
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(s.resolve()));

  s.setForced(true);  // battery T1 beats everything
  TEST_ASSERT_EQUAL(int(ProfileId::BatterySaver), int(s.resolve()));

  s.setForced(false);
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(s.resolve()));
  s.setVbus(false);
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.resolve()));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.user()));  // user intent kept
}

static void test_attn_hue_auto_sentinel() {
  Config c;
  for (int p = 0; p < kProfileCount; ++p) {
    c.setProfile(ProfileId(p));
    TEST_ASSERT_EQUAL(-1, c.effective(Param::AttnHue));
  }
}

// paramMeta/stepParam are the single source of truth for editable ranges; the
// settings menu leans on them. Guard the shapes the editor depends on.
static void test_param_meta_shapes() {
  // Every param has a non-empty, ordered range.
  for (int i = 0; i < kParamCount; ++i) {
    ParamMeta m = paramMeta(Param(i));
    TEST_ASSERT_TRUE(m.max >= m.min);
    if (m.kind == ParamKind::Int) TEST_ASSERT_TRUE(m.step > 0);
  }
  // Spot-check the sentinel/enum shapes.
  ParamMeta hue = paramMeta(Param::AttnHue);
  TEST_ASSERT_EQUAL(-1, hue.min);          // -1 = auto
  TEST_ASSERT_EQUAL(255, hue.max);
  ParamMeta anim = paramMeta(Param::AttnAnim);
  TEST_ASSERT_EQUAL(int(ParamKind::Enum), int(anim.kind));
  TEST_ASSERT_EQUAL(0, anim.min);
  TEST_ASSERT_EQUAL(5, anim.max);          // ring::Anim Off..Fade
}

// paramLabel + paramDescription feed the menu row and help pane. Panel
// constraints: labels are short (<= 26 chars, one row), descriptions non-empty
// and <= 108 chars (wrap to <= 3 lines at 48 cols), and every char is printable
// ASCII 32-126 (the 5x7 font renders anything else as '?').
static void test_param_labels_and_descriptions_fit_the_panel() {
  for (int i = 0; i < kParamCount; ++i) {
    const char* lbl = paramLabel(Param(i));
    TEST_ASSERT_NOT_NULL(lbl);
    const size_t ln = std::strlen(lbl);
    TEST_ASSERT_TRUE_MESSAGE(ln > 0 && ln <= 26, paramName(Param(i)));
    for (size_t j = 0; j < ln; ++j)
      TEST_ASSERT_TRUE_MESSAGE((unsigned char)lbl[j] >= 32 &&
                               (unsigned char)lbl[j] <= 126, paramName(Param(i)));

    const char* d = paramDescription(Param(i));
    TEST_ASSERT_NOT_NULL(d);
    const size_t n = std::strlen(d);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, paramName(Param(i)));
    TEST_ASSERT_TRUE_MESSAGE(n <= 108, paramName(Param(i)));
    for (size_t j = 0; j < n; ++j) {
      const unsigned char ch = (unsigned char)d[j];
      TEST_ASSERT_TRUE_MESSAGE(ch >= 32 && ch <= 126, paramName(Param(i)));
    }
  }
}

// The call-to-action hold (Param::AttnHoldMs) is posture-scaled since 2026-08-04:
// Full can afford to insist for 5 min; on Balanced/Dark the same hold read as
// "the ring is stuck" (owner report - a rainbow breathing for minutes on an idle
// desk). Still a tunable Int spanning 30 s .. 30 min in every mode.
static void test_attn_hold_default_and_range() {
  TEST_ASSERT_EQUAL_INT32(60000,  presetValue(ProfileId(0), Param::AttnHoldMs));
  TEST_ASSERT_EQUAL_INT32(120000, presetValue(ProfileId(1), Param::AttnHoldMs));
  TEST_ASSERT_EQUAL_INT32(300000, presetValue(ProfileId(2), Param::AttnHoldMs));
  ParamMeta m = paramMeta(Param::AttnHoldMs);
  TEST_ASSERT_EQUAL_INT32(30000, m.min);
  TEST_ASSERT_EQUAL_INT32(1800000, m.max);
  TEST_ASSERT_EQUAL_INT32(int(ParamKind::Int), int(m.kind));
}

static void test_step_param_clamp_and_wrap() {
  // Int clamps, no wrap.
  TEST_ASSERT_EQUAL(0, stepParam(Param::RingBrightness, 0, -1));       // at min
  TEST_ASSERT_EQUAL(255, stepParam(Param::RingBrightness, 255, +1));   // at max
  TEST_ASSERT_EQUAL(35, stepParam(Param::RingBrightness, 30, +1));     // +step 5
  // AttnHue sentinel boundary: 0 - step clamps down to the -1 auto min.
  TEST_ASSERT_EQUAL(-1, stepParam(Param::AttnHue, 0, -1));
  // Enum wraps at both ends.
  TEST_ASSERT_EQUAL(0, stepParam(Param::AttnAnim, 5, +1));             // 5 -> 0
  TEST_ASSERT_EQUAL(5, stepParam(Param::AttnAnim, 0, -1));             // 0 -> 5
  // Bool toggles.
  TEST_ASSERT_EQUAL(1, stepParam(Param::TgLowBattPing, 0, +1));
  TEST_ASSERT_EQUAL(0, stepParam(Param::TgLowBattPing, 1, +1));        // wrap
}

// CUM-187: the ring-only params (hidden on a ringless board, in both the device
// menu and the web UI) are exactly the seven LED-ring controls; the five non-ring
// params stay visible on every board. This predicate is the single source both
// surfaces read.
static void test_is_ring_param_classifies_the_led_controls() {
  const Param ring[] = {Param::Posture, Param::RingBrightness, Param::RingFps,
                        Param::AttnLedIndex, Param::AttnHue, Param::AttnAnim,
                        Param::AttnPeriodMs};
  const Param nonRing[] = {Param::CoalesceMs, Param::DwellMs, Param::TelemetryPeriodS,
                           Param::TgLowBattPing, Param::AttnHoldMs};
  for (Param p : ring) TEST_ASSERT_TRUE(isRingParam(p));
  for (Param p : nonRing) TEST_ASSERT_FALSE(isRingParam(p));
  int ringCount = 0;
  for (int i = 0; i < kParamCount; ++i)
    if (isRingParam(Param(i))) ++ringCount;
  TEST_ASSERT_EQUAL(7, ringCount);   // the two sets partition every param (7 + 5 == 12)
}

// CUM-395: a battery mode is also a behavior profile, seeding screen-rest and
// sound-level defaults (a NON-Param mapping - no device-menu row). Balanced returns
// the shipped hard defaults so a fresh Balanced device is unchanged; Battery Saver
// rests fast + quiet, Desk stays on + louder.
static void test_profile_screensaver_and_sound_defaults() {
  // Screen-rest minutes: Battery Saver rests fast, Balanced = shipped 5, Desk stays on.
  TEST_ASSERT_EQUAL(2, profileSaverMinutes(ProfileId::BatterySaver));
  TEST_ASSERT_EQUAL(5, profileSaverMinutes(ProfileId::Balanced));
  TEST_ASSERT_EQUAL(0, profileSaverMinutes(ProfileId::Desk));
  // Sound level, Orchestrator mode (notifier=false): quieter on battery, louder on desk.
  TEST_ASSERT_EQUAL(1, profileSfxLevel(ProfileId::BatterySaver, false));
  TEST_ASSERT_EQUAL(2, profileSfxLevel(ProfileId::Balanced, false));   // shipped Orch default
  TEST_ASSERT_EQUAL(3, profileSfxLevel(ProfileId::Desk, false));
  // Sound level, Notifier mode (notifier=true): silent out of the box except on desk.
  TEST_ASSERT_EQUAL(0, profileSfxLevel(ProfileId::BatterySaver, true));
  TEST_ASSERT_EQUAL(0, profileSfxLevel(ProfileId::Balanced, true));    // shipped Notifier default
  TEST_ASSERT_EQUAL(1, profileSfxLevel(ProfileId::Desk, true));
}

// The precedence rule agent::store::applyProfileDefaults() enforces on device: an
// explicit owner value ALWAYS wins; otherwise the selected battery mode's default
// applies and a re-selected mode re-seeds the untouched key. The store's "was set"
// flags are device/NVS-only, so the rule itself is exercised here in portable code.
static void test_profile_default_precedence_owner_value_wins() {
  const int32_t hard = 5;   // caller's hard default (store's saverMin fallback)
  // Owner has NOT set the key: effective follows the profile default, and switching
  // the battery mode changes that effective default.
  TEST_ASSERT_EQUAL(profileSaverMinutes(ProfileId::Desk),
                    effectiveWithProfileDefault(false, hard, profileSaverMinutes(ProfileId::Desk)));
  TEST_ASSERT_EQUAL(profileSaverMinutes(ProfileId::BatterySaver),
                    effectiveWithProfileDefault(false, hard, profileSaverMinutes(ProfileId::BatterySaver)));
  // Owner HAS set the key to 30: it survives every profile switch, ignoring the default.
  const int32_t owner = 30;
  TEST_ASSERT_EQUAL(owner, effectiveWithProfileDefault(true, owner, profileSaverMinutes(ProfileId::Desk)));
  TEST_ASSERT_EQUAL(owner, effectiveWithProfileDefault(true, owner, profileSaverMinutes(ProfileId::BatterySaver)));
  TEST_ASSERT_EQUAL(owner, effectiveWithProfileDefault(true, owner, profileSaverMinutes(ProfileId::Balanced)));
}

// CUM-395 one-time migration: an updated device may hold a persisted pre-feature
// screen-rest / sound value. Adopt it as an explicit owner override only when it
// DIFFERS from the shipped hard default; a value EQUAL to the default (the old device
// menu wrote sfx unconditionally, so equal-to-default is almost never a real choice)
// stays unset so a battery mode can still seed it. A never-persisted key is never adopted.
static void test_adopt_pre_feature_value_only_when_it_differs() {
  // Orchestrator sound hard default 2.
  TEST_ASSERT_FALSE(adoptAsOwnerSet(false, 0, 2));   // never persisted -> not adopted
  TEST_ASSERT_FALSE(adoptAsOwnerSet(true, 2, 2));    // persisted == default -> treated as unset
  TEST_ASSERT_TRUE(adoptAsOwnerSet(true, 3, 2));     // persisted != default -> owner override
  TEST_ASSERT_TRUE(adoptAsOwnerSet(true, 0, 2));     // an explicit Off differs -> owner override
  // Screen-rest hard default 5.
  TEST_ASSERT_FALSE(adoptAsOwnerSet(true, 5, 5));
  TEST_ASSERT_TRUE(adoptAsOwnerSet(true, 0, 5));     // always-on, owner chose it
  TEST_ASSERT_TRUE(adoptAsOwnerSet(true, 15, 5));
}

// ---- CUM-408: full-NVS seed migration - the {fresh, seeded, full} class -------
//
// planSeedAttempt() (above) is the SHIPPED decision point; store.cpp is device-only and
// not host-linkable, so the RAM latch + early-return ordering + verify-after-write glue
// in adoptPreFeatureValuesAsOwnerSet() is exercised by the device build and the bench leg,
// not here. To pin the DECISION class on the host, a fake NVS drops a NEW key once it is
// full (mirrors Preferences::putInt returning 0 -> solide::memory::setInt false on a full
// 'orchmem' partition), and a SeedSim wires planSeedAttempt() to a latch exactly as the
// store glue does. Switching battery modes repeatedly must run the migration AT MOST ONCE
// and never thrash - the property store.cpp is built to hold.
namespace {
struct FakeNvs {
  std::map<std::string, int32_t> kv;
  size_t cap;   // max distinct keys; a new key beyond cap is dropped (NVS full)
  explicit FakeNvs(size_t c) : cap(c) {}
  int32_t getInt(const std::string& k, int32_t d) const {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
  bool setInt(const std::string& k, int32_t v) {
    if (!kv.count(k) && kv.size() >= cap) return false;   // full: a new key silently drops
    kv[k] = v;
    return true;
  }
};

struct SeedSim {
  FakeNvs nvs;
  bool latch = false;   // s_profSeedNvsFull
  int warns = 0;        // loud NVS-full log lines emitted
  int seeds = 0;        // completed migrations
  explicit SeedSim(size_t cap) : nvs(cap) {}
  void batteryModeSwitch() {   // == one adoptPreFeatureValuesAsOwnerSet() call
    if (nvs.getInt("profSeedInit", 0) != 0) return;   // steady state: already done
    if (latch) return;                                 // full this boot: no retry, no re-log
    const bool wrote = nvs.setInt("profSeedInit", 1);
    const bool persisted = wrote && nvs.getInt("profSeedInit", 0) == 1;
    switch (planSeedAttempt(/*markerPresent=*/false, persisted)) {
      case SeedAttempt::Seeded: ++seeds; break;
      case SeedAttempt::DeferredNvsFull: latch = true; ++warns; break;
      case SeedAttempt::AlreadyDone: break;   // unreachable here (marker was absent)
    }
  }
};
}  // namespace

static void test_plan_seed_attempt_predicate() {
  TEST_ASSERT_EQUAL(int(SeedAttempt::AlreadyDone), int(planSeedAttempt(true, true)));
  TEST_ASSERT_EQUAL(int(SeedAttempt::AlreadyDone), int(planSeedAttempt(true, false)));
  TEST_ASSERT_EQUAL(int(SeedAttempt::Seeded), int(planSeedAttempt(false, true)));
  TEST_ASSERT_EQUAL(int(SeedAttempt::DeferredNvsFull), int(planSeedAttempt(false, false)));
}

static void test_seed_fresh_applies_once_and_never_reruns() {
  SeedSim s(/*cap=*/8);   // plenty of headroom
  for (int i = 0; i < 5; ++i) s.batteryModeSwitch();
  TEST_ASSERT_EQUAL_INT(1, s.seeds);   // seeded exactly once across 5 switches
  TEST_ASSERT_EQUAL_INT(0, s.warns);
  TEST_ASSERT_EQUAL_INT32(1, s.nvs.getInt("profSeedInit", 0));   // marker latched in NVS
}

static void test_seed_already_seeded_is_noop() {
  SeedSim s(8);
  s.nvs.kv["profSeedInit"] = 1;   // a prior boot completed the migration
  for (int i = 0; i < 5; ++i) s.batteryModeSwitch();
  TEST_ASSERT_EQUAL_INT(0, s.seeds);   // never re-seeds
  TEST_ASSERT_EQUAL_INT(0, s.warns);
}

static void test_seed_full_nvs_defers_once_and_stops_rerunning() {
  SeedSim s(/*cap=*/0);   // NVS full: no new key can ever be written
  for (int i = 0; i < 5; ++i) s.batteryModeSwitch();
  TEST_ASSERT_EQUAL_INT(0, s.seeds);                          // never completes
  TEST_ASSERT_EQUAL_INT(1, s.warns);                          // loud EXACTLY once, not per switch
  TEST_ASSERT_EQUAL_INT32(0, s.nvs.getInt("profSeedInit", 0));// marker never persisted
  TEST_ASSERT_TRUE(s.latch);                                  // latched off for the boot
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_plan_seed_attempt_predicate);
  RUN_TEST(test_seed_fresh_applies_once_and_never_reruns);
  RUN_TEST(test_seed_already_seeded_is_noop);
  RUN_TEST(test_seed_full_nvs_defers_once_and_stops_rerunning);
  RUN_TEST(test_is_ring_param_classifies_the_led_controls);
  RUN_TEST(test_profile_screensaver_and_sound_defaults);
  RUN_TEST(test_profile_default_precedence_owner_value_wins);
  RUN_TEST(test_adopt_pre_feature_value_only_when_it_differs);
  RUN_TEST(test_presets_match_plan_table);
  RUN_TEST(test_param_meta_shapes);
  RUN_TEST(test_attn_hold_default_and_range);
  RUN_TEST(test_param_labels_and_descriptions_fit_the_panel);
  RUN_TEST(test_step_param_clamp_and_wrap);
  RUN_TEST(test_effective_uses_preset_then_override);
  RUN_TEST(test_override_survives_profile_switch);
  RUN_TEST(test_clear_all_overrides);
  RUN_TEST(test_selector_precedence_forced_vbus_user);
  RUN_TEST(test_attn_hue_auto_sentinel);
  return UNITY_END();
}
