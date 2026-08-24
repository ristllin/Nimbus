#include "nimbus/profile.h"

namespace nimbus {

namespace {
// Preset table, one row per param: { BatterySaver, Balanced, Desk }.
struct Row { int32_t v[kProfileCount]; };

constexpr Row kPresets[kParamCount] = {
    /* Posture           */ {{int32_t(Posture::Dark), int32_t(Posture::Calm), int32_t(Posture::Full)}},
    /* RingBrightness    */ {{10, 30, 60}},
    /* RingFps           */ {{20, 30, 60}},
    /* AttnLedIndex      */ {{0, 0, 0}},
    /* AttnHue           */ {{-1, -1, -1}},   // auto: derive from state
    /* AttnAnim          */ {{2, 2, 2}},      // solide::ring::Anim::Breathe
    /* AttnPeriodMs      */ {{3200, 2400, 2400}},
    /* CoalesceMs        */ {{60000, 30000, 15000}},
    /* DwellMs           */ {{300, 300, 300}},
    /* TelemetryPeriodS  */ {{300, 120, 60}},
    /* TgLowBattPing     */ {{1, 1, 0}},      // pointless on desk power
    /* AttnHoldMs        */ {{60000, 120000, 300000}},  // 1 / 2 / 5 min - a CTA on a desk
    // display (Full) can afford to insist; on Balanced/Dark the same 5-minute hold
    // read as "the ring is stuck" (owner, 2026-08-04). Still per-mode customizable.
};

constexpr const char* kParamNames[kParamCount] = {
    "posture",       "ring_brightness", "ring_fps",       "attn_led_index",
    "attn_hue",      "attn_anim",       "attn_period_ms", "epd_coalesce_ms",
    "dwell_ms",      "telemetry_period_s", "tg_low_batt_ping",
    "attn_hold_ms",
};

// Human labels for the menu row (Title Case, no snake_case in the user's face).
constexpr const char* kParamLabels[kParamCount] = {
    /* Posture           */ "Ring level",
    /* RingBrightness    */ "Brightness",
    /* RingFps           */ "Animation smoothness",
    /* AttnLedIndex      */ "Attention LED spot",
    /* AttnHue           */ "Attention color",
    /* AttnAnim          */ "Attention motion",
    /* AttnPeriodMs      */ "Attention speed",
    /* CoalesceMs        */ "Screen update pace",
    /* DwellMs           */ "Cursor response delay",
    /* TelemetryPeriodS  */ "On-screen status refresh",
    /* TgLowBattPing     */ "Low-battery Telegram alert",
    /* AttnHoldMs        */ "Needs-you hold time",
};

// User-facing help: what you'd SEE change / why you'd care, plain language.
// Panel-visible strings: printable ASCII 32-126 only, <= 108 chars each.
constexpr const char* kParamDescs[kParamCount] = {
    /* Posture           */ "Dark: one LED, only when a session needs you. Calm: a soft working glow. Full: every session a color arc.",
    /* RingBrightness    */ "How bright the LED ring glows. Lower is dimmer and uses less power.",
    /* RingFps           */ "How smoothly the LED animations move. Higher looks smoother but works the chip harder.",
    /* AttnLedIndex      */ "In Dark/Calm level, which single LED around the ring (0-44) lights up when a session needs you.",
    /* AttnHue           */ "Color of that attention LED. Auto matches the session status: purple=input, amber=approve, red=error.",
    /* AttnAnim          */ "How the attention LED moves to catch your eye: breathe, blink, a sweeping comet, and so on.",
    /* AttnPeriodMs      */ "How fast the attention LED breathes or blinks, in ms. Smaller number means a faster pulse.",
    /* CoalesceMs        */ "Minor changes wait this long and batch together, so the screen updates calmly instead of constantly.",
    /* DwellMs           */ "After you move the cursor, how long to wait before the detail view redraws for the item you landed on.",
    /* TelemetryPeriodS  */ "How often the battery/status numbers on the screen refresh, in seconds. 0 turns updates off.",
    /* TgLowBattPing     */ "In Orchestrator mode, message you on Telegram when the battery first gets low.",
    /* AttnHoldMs        */ "How long a session that needs you (input/approve/error) stays lit after updates stop, in ms.",
};

constexpr const char* kProfileNames[kProfileCount] = {
    "battery_saver", "balanced", "desk",
};

// Editable ranges, one row per param (enum order). Values match the semantics
// documented in profile.h and the preset table. AttnAnim spans solide::ring::Anim
// {Off..Fade} = 0..5. AttnHue's min is the -1 "auto" sentinel.
constexpr ParamMeta kMeta[kParamCount] = {
    /* Posture           */ {0, 2, 1, ParamKind::Enum},        // Dark/Calm/Full (cycles)
    /* RingBrightness    */ {0, 255, 5, ParamKind::Int},
    /* RingFps           */ {5, 60, 5, ParamKind::Int},
    /* AttnLedIndex      */ {0, 44, 1, ParamKind::Enum},       // 45 LEDs, wrap
    /* AttnHue           */ {-1, 255, 8, ParamKind::Int},      // -1 = auto
    /* AttnAnim          */ {0, 5, 1, ParamKind::Enum},        // ring::Anim
    /* AttnPeriodMs      */ {500, 5000, 100, ParamKind::Int},
    /* CoalesceMs        */ {5000, 120000, 5000, ParamKind::Int},
    /* DwellMs           */ {100, 1000, 50, ParamKind::Int},
    /* TelemetryPeriodS  */ {0, 600, 30, ParamKind::Int},
    /* TgLowBattPing     */ {0, 1, 1, ParamKind::Bool},
    /* AttnHoldMs        */ {30000, 1800000, 30000, ParamKind::Int},  // 30 s .. 30 min
};
}  // namespace

const char* paramName(Param p) { return kParamNames[int(p)]; }
const char* paramLabel(Param p) { return kParamLabels[int(p)]; }
const char* paramDescription(Param p) { return kParamDescs[int(p)]; }
const char* profileName(ProfileId id) { return kProfileNames[int(id)]; }

// Display labels (battery modes). Machine keys above stay stable for NVS/API.
constexpr const char* kProfileLabels[kProfileCount] = {"Dark", "Balanced", "Full"};
const char* profileLabel(ProfileId id) { return kProfileLabels[int(id)]; }

ParamMeta paramMeta(Param p) { return kMeta[int(p)]; }

int32_t stepParam(Param p, int32_t cur, int dir) {
  const ParamMeta m = kMeta[int(p)];
  const int d = (dir >= 0) ? 1 : -1;
  if (m.kind == ParamKind::Bool || m.kind == ParamKind::Enum) {
    // Cycle inclusively over [min,max], wrapping at both ends.
    const int32_t span = m.max - m.min + 1;  // >= 1 by construction
    int32_t rel = cur - m.min;
    if (rel < 0) rel = 0;
    if (rel >= span) rel = span - 1;
    rel = (rel + d + span) % span;
    return m.min + rel;
  }
  // Int: step and clamp at the bounds (no wrap).
  int32_t next = cur + d * m.step;
  if (next < m.min) next = m.min;
  if (next > m.max) next = m.max;
  return next;
}

int32_t presetValue(ProfileId id, Param p) {
  return kPresets[int(p)].v[int(id)];
}

int32_t Config::effective(Param p) const {
  return has_[int(p)] ? val_[int(p)] : presetValue(profile_, p);
}

void Config::setOverride(Param p, int32_t v) {
  has_[int(p)] = true;
  val_[int(p)] = v;
}

void Config::clearOverride(Param p) { has_[int(p)] = false; }

bool Config::hasOverride(Param p) const { return has_[int(p)]; }

void Config::clearAllOverrides() {
  for (int i = 0; i < kParamCount; ++i) has_[i] = false;
}

}  // namespace nimbus
