#include "nimbus/sfx_map.h"

#include <cstring>

namespace nimbus::sfx {

namespace {
struct Row {
  Ev          ev;
  const char* slug;
  uint8_t     orchRank;   // min level in Orchestrator mode
  uint8_t     notifRank;  // min level in Notifier mode (kNever = unmapped)
};

// The single source for slugs + per-mode ranks. Slugs MUST match
// tools/sounds/palette.py (the WAV filenames are derived from them).
// Notifier stays quiet by design: no per-job spawn/turn churn at any level.
constexpr Row kRows[] = {
    // ev                slug            orch          notifier
    {Ev::Boot,        "boot",         kLevelLight,  kLevelMedium},
    {Ev::WifiUp,      "wifi_up",      kLevelMedium, kLevelHeavy},
    {Ev::WifiDown,    "wifi_down",    kLevelMedium, kLevelHeavy},
    {Ev::BleUp,       "ble_up",       kLevelMedium, kLevelHeavy},
    {Ev::BleDown,     "ble_down",     kLevelMedium, kLevelHeavy},
    {Ev::BleBond,     "ble_bond",     kLevelMedium, kLevelMedium},
    {Ev::AgentSpawn,  "agent_spawn",  kLevelMedium, kNever},
    {Ev::AgentDone,   "agent_done",   kLevelMedium, kLevelHeavy},
    {Ev::Error,       "error",        kLevelLight,  kLevelLight},
    {Ev::NeedsYou,    "needs_you",    kLevelLight,  kLevelLight},
    {Ev::LowBattery,  "low_battery",  kLevelLight,  kLevelLight},
    {Ev::BatteryOk,   "battery_ok",   kLevelMedium, kLevelHeavy},
    {Ev::ModeSwitch,  "mode_switch",  kLevelMedium, kLevelMedium},
    {Ev::SdMounted,   "sd_mounted",   kLevelMedium, kNever},
    {Ev::SdLost,      "sd_lost",      kLevelMedium, kNever},
    {Ev::TurnStart,   "turn_start",   kLevelHeavy,  kNever},
    {Ev::ReplySent,   "reply_sent",   kLevelHeavy,  kNever},
    {Ev::VoiceListen, "voice_listen", kLevelHeavy,  kNever},
    {Ev::VoiceStop,   "voice_stop",   kLevelHeavy,  kNever},
    {Ev::MemSaved,    "mem_saved",    kLevelHeavy,  kNever},
    {Ev::NetDegraded, "net_degraded", kLevelHeavy,  kNever},
    {Ev::NetOk,       "net_ok",       kLevelHeavy,  kNever},
    {Ev::AskCleared,  "ask_cleared",  kLevelHeavy,  kNever},
    {Ev::SyncDone,    "sync_done",    kLevelHeavy,  kNever},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == (size_t)Ev::COUNT,
              "one row per Ev, in enum order");
}  // namespace

const char* slug(Ev e) {
  const auto i = (unsigned)e;
  if (i >= (unsigned)Ev::COUNT) return nullptr;
  return kRows[i].slug;
}

bool parseSlug(const char* s, Ev& out) {
  if (!s || !s[0]) return false;
  for (const auto& r : kRows) {
    if (strcmp(r.slug, s) == 0) { out = r.ev; return true; }
  }
  return false;
}

uint8_t rank(Ev e, bool orchestratorMode) {
  const auto i = (unsigned)e;
  if (i >= (unsigned)Ev::COUNT) return kNever;
  return orchestratorMode ? kRows[i].orchRank : kRows[i].notifRank;
}

bool shouldPlay(Ev e, uint8_t level, bool orchestratorMode) {
  const uint8_t r = rank(e, orchestratorMode);
  return r != kNever && level >= r && level > kLevelNone;
}

bool RateGate::allow(Ev e, uint32_t nowMs) {
  const auto i = (unsigned)e;
  if (i >= (unsigned)Ev::COUNT) return false;
  if (any_ && (nowMs - lastAnyMs_) < globalGapMs_) return false;
  if (evSeen_[i] && (nowMs - lastEvMs_[i]) < perEventMs_) return false;
  any_ = true;
  lastAnyMs_ = nowMs;
  evSeen_[i] = true;
  lastEvMs_[i] = nowMs;
  return true;
}

void RateGate::reset() {
  any_ = false;
  for (auto& s : evSeen_) s = false;
}

}  // namespace nimbus::sfx
