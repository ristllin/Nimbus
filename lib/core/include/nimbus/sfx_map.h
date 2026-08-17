#pragma once
#include <cstdint>

// sfx_map - the PORTABLE core of the sound state language: the event
// vocabulary, the per-mode tier ranks (which sound level voices which event in
// Notifier vs Orchestrator mode), and the rate gate that keeps "heavy" from
// becoming cacophony. No Arduino/FS/audio deps -> host-tested via
// `pio test -e native`. The device layer (src/sfx/sound_fx.*) owns file
// resolution + playback; the clip curation lives in tools/sounds/palette.py
// (slugs here MUST match that table's keys - the WAV filenames are built from
// them).
//
// Levels mirror the ring-posture ladder: 0 none / 1 light / 2 medium / 3 heavy.
// An event plays iff rank(ev, mode) <= level AND rank != kNever. Notifier is
// deliberately far sparser at every level (the broker floods JobState during
// coding sessions): most orchestrator slugs are kNever there even on heavy.
namespace nimbus::sfx {

enum class Ev : uint8_t {
  Boot = 0,
  WifiUp, WifiDown,
  BleUp, BleDown, BleBond,
  AgentSpawn, AgentDone,
  Error, NeedsYou,
  LowBattery, BatteryOk,
  ModeSwitch, SdMounted, SdLost,
  TurnStart, ReplySent,
  VoiceListen, VoiceStop,
  MemSaved,
  NetDegraded, NetOk,
  AskCleared, SyncDone,
  COUNT
};

constexpr uint8_t kLevelNone   = 0;
constexpr uint8_t kLevelLight  = 1;
constexpr uint8_t kLevelMedium = 2;
constexpr uint8_t kLevelHeavy  = 3;
constexpr uint8_t kNever       = 255;  // never voiced in this mode, at any level

// The event's WAV slug ("agent_spawn" -> /sfx/<pool>/agent_spawn-<n>.wav and
// the embedded basic clip name). nullptr for an out-of-range value.
const char* slug(Ev e);
// Parse a slug back to an event (console SFX <slug>). false if unknown.
bool parseSlug(const char* s, Ev& out);

// Minimum level at which `e` is voiced in the given mode (kNever = unmapped).
uint8_t rank(Ev e, bool orchestratorMode);
// The gating rule: does this event play at this level in this mode?
bool shouldPlay(Ev e, uint8_t level, bool orchestratorMode);

// Rate gate: a global minimum gap between ANY two sounds plus a per-event
// cooldown, so bursts (job churn, reconnect flapping) collapse into one sound.
// Time is injected (millis() on device, a counter in host tests). allow()
// returns true AND records the play; a rejected event records nothing.
class RateGate {
 public:
  explicit RateGate(uint32_t globalGapMs = 300, uint32_t perEventMs = 2000)
      : globalGapMs_(globalGapMs), perEventMs_(perEventMs) {}
  bool allow(Ev e, uint32_t nowMs);
  void reset();

 private:
  uint32_t globalGapMs_, perEventMs_;
  uint32_t lastAnyMs_ = 0;
  uint32_t lastEvMs_[(int)Ev::COUNT] = {};
  bool     any_ = false;
  bool     evSeen_[(int)Ev::COUNT] = {};
};

}  // namespace nimbus::sfx
