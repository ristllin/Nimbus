#pragma once
#include <cstdint>

// profile - power profiles and the sparse-override configuration model.
//
// A profile is a named set of DEFAULTS, not a mode switch:
//   effective(key) = userOverride(key) ?? activeProfile.preset(key)
// Overrides are stored sparsely (only keys the user touched), so switching
// profiles never loses user intent and profiles never become a settings swamp.
//
// Profile selection has three sources with fixed precedence:
//   forced (battery threshold 1)  >  VBUS auto (Desk)  >  user choice
// The precedence lives in Selector; debounce/hysteresis of the inputs is the
// power policy's job (lib/core/power), not Selector's.

namespace nimbus {

// Ring level (user-facing "Dark / Calm / Full"): how much the LED ring shows.
//   Dark - ring off; at most ONE LED lights, only when a job needs you.
//   Calm - Dark + a soft activity glow (orchestrator "working" heartbeat +
//          brief pulses when a sub-agent starts/finishes) + a lowest-precedence
//          Done cue so a FINISHED session is glanceable (self-clears on the 30 s
//          ambient hold). No persistent multi-segment arcs.
//   Full - every job rendered as a colored arc (full segment treatment).
// Ordered by intensity so the knob cycles Dark->Calm->Full.
enum class Posture : uint8_t { Dark = 0, Calm = 1, Full = 2 };

enum class ProfileId : uint8_t { BatterySaver = 0, Balanced = 1, Desk = 2 };
constexpr int kProfileCount = 3;

enum class Param : uint8_t {
  Posture = 0,        // Posture enum as int
  RingBrightness,     // 0-255 global cap
  RingFps,            // LED render task rate
  AttnLedIndex,       // Dark/Calm levels: which LED carries attention
  AttnHue,            // 0-254; 255=white; -1 = auto (derive from state)
  AttnAnim,           // solide::ring::Anim as int
  AttnPeriodMs,       // breathe/blink period for the attention LED
  EpdCoalesceMs,      // ambient refresh window
  DwellMs,            // encoder settle before detail render
  FullRefreshEveryN,  // ghosting: full clear after N fast/partials
  TelemetryPeriodS,   // battery/status telemetry cadence (0 = off)
  TgLowBattPing,      // 0/1: Telegram low-battery ping (Orchestrator mode)
  AttnHoldMs,         // notifier: how long a call-to-action (input/approval/error)
                      // holds after the broker goes quiet (ambient uses ambientHoldFor)
  COUNT
};
constexpr int kParamCount = int(Param::COUNT);

const char* paramName(Param p);   // machine key, e.g. "ring_brightness" (web/NVS)
const char* profileName(ProfileId id);   // machine key: battery_saver|balanced|desk (NVS/API/AI schema - STABLE)
// User-facing battery-mode label (owner 2026-07-18: one vocabulary, the mode IS
// the light level): "Dark" | "Balanced" | "Full". Display only - never persisted.
const char* profileLabel(ProfileId id);

// Human-friendly display label for the menu row, e.g. "Brightness" (Title Case,
// no snake_case keys in the user's face). ASCII, <= ~20 chars.
const char* paramLabel(Param p);

// Plain-English help for a param, shown in the menu's help pane and the web UI.
// Leads with what the user OBSERVES / why they'd care, not the mechanism.
// Panel-visible: printable ASCII 32-126 only, <= 108 chars (wraps to <= 3
// 48-col lines on the e-ink). Guarded by test_profile.
const char* paramDescription(Param p);

// Editable-range descriptor for a Param. Ranges live here (portable, host-
// tested) rather than in the device menu so the editor and its tests share one
// source of truth. `step` is the per-detent delta a knob applies.
//   Int   - clamp to [min,max] (no wrap); e.g. brightness, periods.
//   Bool  - 0/1 toggle; step ignored, rotate flips.
//   Enum  - cycle [min,max] inclusive, wrapping; e.g. AttnAnim over ring::Anim.
// AttnHue is an Int whose min is the -1 "auto" sentinel: -1 then 0..255.
enum class ParamKind : uint8_t { Int = 0, Bool, Enum };

struct ParamMeta {
  int32_t   min;
  int32_t   max;
  int32_t   step;
  ParamKind kind;
};

ParamMeta paramMeta(Param p);

// Apply one knob detent (dir = +1 / -1) to `cur` under a param's meta, and
// return the new value. Int clamps at the bounds; Bool/Enum wrap. Deterministic
// and side-effect free so both the menu FSM and its tests can call it.
int32_t stepParam(Param p, int32_t cur, int dir);

// Preset value for a (profile, param) pair. All values are int32 for uniform
// storage; enum-typed params hold the enum's integer value.
int32_t presetValue(ProfileId id, Param p);

// Sparse user overrides on top of the active profile.
class Config {
 public:
  int32_t effective(Param p) const;
  Posture posture() const { return Posture(effective(Param::Posture)); }

  void setProfile(ProfileId id) { profile_ = id; }
  ProfileId profile() const { return profile_; }

  void setOverride(Param p, int32_t v);
  void clearOverride(Param p);
  bool hasOverride(Param p) const;
  void clearAllOverrides();

 private:
  ProfileId profile_ = ProfileId::Balanced;
  bool      has_[kParamCount] = {};
  int32_t   val_[kParamCount] = {};
};

// Profile-selection precedence: forced > VBUS auto > user.
class Selector {
 public:
  void setUser(ProfileId id) { user_ = id; }
  void setForced(bool on) { forced_ = on; }        // battery T1: Battery Saver
  void setVbus(bool present) { vbus_ = present; }  // debounced by power policy

  ProfileId resolve() const {
    if (forced_) return ProfileId::BatterySaver;
    if (vbus_) return ProfileId::Desk;
    return user_;
  }
  ProfileId user() const { return user_; }

 private:
  ProfileId user_ = ProfileId::Balanced;
  bool forced_ = false;
  bool vbus_   = false;
};

}  // namespace nimbus
