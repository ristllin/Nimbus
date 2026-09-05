#include "health.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Esp.h>
#include <solide/storage.h>

#include "memory_subsystem.h"
#include "telegram.h"
#include "hw/hal_status.h"
#include "nimbus/fault.h"

namespace agent::health {

namespace {
// Last acoustic-loopback verdict (set by the /api/audio/loopback handler). 0 =
// never run. Single aligned writes -> no lock needed across tasks.
volatile bool     s_lbTone = false;
volatile uint32_t s_lbWhenMs = 0;

const char* kOk = "ok", *kDegraded = "degraded", *kAbsent = "absent", *kUnknown = "unknown";

struct Row { const char* key; const char* label; const char* state; String detail; };

// Per-capability evidence from the SINGLE tests too (owner 2026-07-16: pressing
// "Mic test"/"Speaker test" left the rows Unknown forever - only the loopback fed
// the verdict). The loopback stays the STRONGEST evidence (acoustic proof of both
// ends); a mic sample proves the mic path returns real data; a beep proves the TX
// path drove the amp (acoustically unverified - the wording says so).
bool     s_micOk = false;   uint32_t s_micWhenMs = 0;
bool     s_beepOk = false;  uint32_t s_beepWhenMs = 0;

// A capability the firmware can't confirm without an active probe (mic/speaker):
// present per the build; verdict from the freshest evidence, else "verify".
Row audioRow(const char* key, const char* label, bool faulted, bool isMic) {
  if (faulted) return {key, label, kAbsent, "fault-injected (test)"};
  const bool     haveSingle = isMic ? (s_micWhenMs != 0) : (s_beepWhenMs != 0);
  const bool     singleOk   = isMic ? s_micOk : s_beepOk;
  const uint32_t singleMs   = isMic ? s_micWhenMs : s_beepWhenMs;
  // Freshest evidence wins so a new single test updates a stale loopback verdict.
  if (s_lbWhenMs != 0 && (!haveSingle || (int32_t)(s_lbWhenMs - singleMs) >= 0)) {
    const uint32_t ageS = (millis() - s_lbWhenMs) / 1000;
    String d = String("last loopback ") + (s_lbTone ? "PASSED" : "FAILED") + " " + ageS + "s ago";
    return {key, label, s_lbTone ? kOk : kDegraded, d};
  }
  if (haveSingle) {
    const uint32_t ageS = (millis() - singleMs) / 1000;
    String d = isMic
        ? (String(singleOk ? "mic sampled OK " : "mic returned NO samples ") + ageS + "s ago")
        : (String(singleOk ? "beep driven OK " : "beep FAILED ") + ageS +
           "s ago" + (singleOk ? " (acoustically unverified - run loopback)" : ""));
    return {key, label, singleOk ? kOk : kDegraded, d};
  }
  return {key, label, kUnknown, "wired; run the loopback test to verify"};
}
}  // namespace

void recordLoopback(bool tonePresent, uint32_t whenMs) {
  s_lbTone = tonePresent;
  s_lbWhenMs = whenMs ? whenMs : 1;   // never 0 (0 means "never run")
}

void recordMicSample(bool ok, uint32_t whenMs) {
  s_micOk = ok;
  s_micWhenMs = whenMs ? whenMs : 1;
}

void recordBeep(bool ok, uint32_t whenMs) {
  s_beepOk = ok;
  s_beepWhenMs = whenMs ? whenMs : 1;
}

std::string reportJson(const Env& env) {
  using nimbus::fault::active;
  using nimbus::fault::Cap;
  const nimbus::hw::HalHealth& hal = nimbus::hw::halHealth();

  Row rows[12];
  int n = 0;

  // LED ring + color panel: HAL begin-result AND not fault-injected.
  rows[n++] = {"led", "LED ring",
               active(Cap::LED) ? kAbsent : (hal.leds ? kOk : kDegraded),
               active(Cap::LED) ? "fault-injected (test)" : (hal.leds ? "up" : "init failed")};
  rows[n++] = {"screen", "Display (color touch)",
               active(Cap::SCREEN) ? kAbsent : (hal.display ? kOk : kDegraded),
               active(Cap::SCREEN) ? "fault-injected (test)" : (hal.display ? "up" : "init failed")};
  // Touch: "up" from a begin() that succeeded at boot is not proof the controller
  // is still alive. On a resistive board a dead controller (MISO stuck high) reads
  // as degraded via env.touchDegraded even though hal.touch was true at boot - the
  // honest signal that used to be a hardwired "ok" (FIX 4).
  rows[n++] = {"touch", "Touch panel",
               !hal.touch ? kDegraded : (env.touchDegraded ? kDegraded : kOk),
               !hal.touch ? "touch init failed"
                          : (env.touchDegraded ? "touch not responding" : "touch up")};

  // Audio (active-probe capabilities).
  rows[n++] = audioRow("mic", "Microphone (I²S)", active(Cap::MIC), /*isMic=*/true);
  rows[n++] = audioRow("speaker", "Speaker (MAX98357A)", active(Cap::SPEAKER), /*isMic=*/false);

  // Storage: the SD reader/card + the memory tier it backs.
  {
    const bool mounted = solide::storage::available();
    rows[n++] = {"sd_card", "microSD card",
                 mounted ? kOk : kAbsent,
                 mounted ? (String((double)solide::storage::cardSizeMB() / 1024.0, 1) + " GB")
                         : String("not detected (memory runs degraded on flash)")};
  }
  {
    agent::memory::Stats ms = agent::memory::stats();
    const char* st = active(Cap::MEMORY) ? kAbsent
                     : (ms.flashFull ? kDegraded : (hal.memory ? kOk : kDegraded));
    String d = String(ms.vectorCount) + "/" + ms.maxVectors + " vectors, " +
               (ms.sdPresent ? "SD /mem" : "flash /data") + (ms.flashFull ? ", FLASH FULL" : "");
    rows[n++] = {"memory", "Memory subsystem", st, d};
  }

  // PSRAM (the 8 MB pool TLS + the VDB working set live in).
  {
    const uint32_t ps = ESP.getFreePsram();
    rows[n++] = {"psram", "PSRAM",
                 ps > 0 ? kOk : kAbsent,
                 ps > 0 ? (String(ps / 1024) + " KB free of " + (ESP.getPsramSize() / 1024) + " KB")
                        : String("absent")};
  }

  // WiFi / BLE / battery / telegram - the caller-supplied env bits.
  if (env.wifiKnown)
    rows[n++] = {"wifi", "Wi-Fi", env.wifiUp ? kOk : kDegraded,
                 env.wifiUp ? (String("connected ") + env.wifiRssi + " dBm") : String("down")};
  if (env.bleAvail)
    rows[n++] = {"ble", "Bluetooth LE",
                 env.bleConnected ? kOk : (env.bleOn ? kDegraded : kAbsent),
                 env.bleConnected ? "linked" : (env.bleOn ? "advertising" : "off")};
  // Battery: a valid gauge shows percent. An INVALID reading has two honest cases
  // the old row conflated as "desk-powered": monitoring off (genuinely no pack) is
  // absent, but monitoring ON with a persistently invalid reading is an open sense
  // line - a degraded fault, not a desk (FIX 3). battSenseMissing is the debounced
  // (monitoring-on AND invalid) verdict, so it is false on a real desk board.
  rows[n++] = {"battery", "Battery",
               env.battValid ? kOk : (env.battSenseMissing ? kDegraded : kAbsent),
               // Percent only - no "(ext power)" claim: that flag is a voltage-trend
               // inference with no charge-detect hardware behind it, and the model
               // was repeating it to the owner as fact (2026-07-16).
               env.battValid ? (String(env.battPct) + "%")
                             : (env.battSenseMissing ? String("battery sense not detected")
                                                     : String("no gauge (desk-powered)"))};
  rows[n++] = {"telegram", "Telegram",
               agent::telegram::enabled() ? kOk : kAbsent,
               agent::telegram::enabled() ? "configured" : "no token/allowlist"};

  JsonDocument d;
  JsonArray arr = d["components"].to<JsonArray>();
  int ok = 0, deg = 0, abs_ = 0;
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["key"] = rows[i].key;
    o["label"] = rows[i].label;
    o["state"] = rows[i].state;
    o["detail"] = rows[i].detail;
    if (rows[i].state == kOk) ok++;
    else if (rows[i].state == kDegraded) deg++;
    else if (rows[i].state == kAbsent) abs_++;
  }
  d["ok"] = ok;
  d["degraded"] = deg;
  d["absent"] = abs_;
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace agent::health
