#include "hw/selftest.h"

#include <Esp.h>
#include <WiFi.h>
#include <math.h>

#include <solide/storage.h>
#include <solide/audio.h>
#include <solide/board.h>   // board().hasRing - truthful led item on a ringless board

#include "nimbus/fault.h"
#include "nimbus_config.h"           // NIMBUS_BATT_NOMINAL_MAH (capacity mAh)
#include "agent/agent_config.h"      // heap floors
#include "agent/store.h"             // current config values -> device.status cfg{}
#include "sfx/sound_fx.h"
#include "sys/ota_update.h"          // fw/ota fields -> device.status
#include "version.h"
#include <time.h>

namespace nimbus::hw {

String localTimeStr() {
  // Local wall-clock as "YYYY-MM-DD Day HH:MM TZ", or "" while the clock is
  // UNSYNCED (pre-SNTP the epoch reads ~1970 - never present that as a time).
  time_t t = time(nullptr);
  if (t < 1600000000) return String();   // ~2020-09: anything older = never synced
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[48];
  strftime(buf, sizeof(buf), "%Y-%m-%d %a %H:%M %Z", &lt);
  return String(buf);
}

namespace {
using S = SelfTestItem;
using St = SelfTestItem::Status;

S mk(const char* name, St st, const String& detail) { return S{name, st, detail}; }

// Screen/led/input/memory: PASS when the boot HAL brought it up AND it isn't
// fault-injected; a fault (or boot failure) is a real FAIL the owner should see.
S capItem(const char* name, bool halOk, nimbus::fault::Cap cap) {
  const bool faulted = nimbus::fault::active(cap);
  if (faulted) return mk(name, St::Fail, "fault-injected");
  return mk(name, halOk ? St::Pass : St::Fail, halOk ? "up" : "boot begin() failed");
}
}  // namespace

std::vector<SelfTestItem> runSelfTest(const SelfTestInputs& in, bool audible) {
  std::vector<SelfTestItem> r;
  r.reserve(12);

  // ---- heap (internal) ------------------------------------------------------
  {
    // Health floor, NOT the turn-defer floor: the device runs fine well below
    // ORCH_TURN_HARD_FLOOR (28k) because TLS is PSRAM-backed - measured live at
    // 26k resting in Orchestrator mode (min-ever 21k, survived). FAIL only near
    // the real TLS/lwIP danger zone (~16k); that floor flagged a healthy board.
    static const uint32_t kHeapHealthFloor = 16000;
    const uint32_t free = ESP.getFreeHeap(), mn = ESP.getMinFreeHeap();
    const bool ok = free > kHeapHealthFloor;
    r.push_back(mk("heap", ok ? St::Pass : St::Fail,
                   String("free=") + (free / 1024) + "k min=" + (mn / 1024) + "k"));
  }
  // ---- PSRAM ----------------------------------------------------------------
  {
    const uint32_t fp = ESP.getFreePsram(), tp = ESP.getPsramSize();
    r.push_back(mk("psram", tp && fp > 262144 ? St::Pass : (tp ? St::Fail : St::Skip),
                   tp ? (String("free=") + (fp / 1024) + "k/" + (tp / 1024) + "k") : "absent"));
  }
  // ---- SD / data store ------------------------------------------------------
  {
    const bool sd = solide::storage::available() && !nimbus::fault::active(nimbus::fault::SD);
    if (sd)
      r.push_back(mk("sd", St::Pass,
                     String("free=") + (uint32_t)solide::storage::freeMB() +
                     "/" + (uint32_t)solide::storage::cardSizeMB() + "MB"));
    else
      r.push_back(mk("sd", St::Skip, "absent - running on internal flash (degraded tier)"));
  }
  // ---- screen / led / input / memory (HAL + fault) --------------------------
  r.push_back(capItem("screen", in.halDisplay, nimbus::fault::SCREEN));
  // On a board with no physical ring the "led" is a single status pixel and the
  // notifier ring is drawn on the panel - so a bare PASS reads as "ring works"
  // when there is no ring. Report the truth: the pixel is up and the ring is on
  // the screen (a genuine begin() failure still FAILs).
  if (!solide::board().hasRing)
    r.push_back(mk("led", in.halLeds ? St::Pass : St::Fail,
                   in.halLeds ? "1px status LED (no ring; drawn on the panel)"
                              : "status LED begin() failed"));
  else
    r.push_back(capItem("led", in.halLeds, nimbus::fault::LED));
  // Input is the color touch panel (the only fitted input device).
  r.push_back(mk("input", in.halTouch ? St::Pass : St::Fail,
                 in.halTouch ? "touch up" : "touch controller not responding"));
  r.push_back(capItem("memory", in.halMemory, nimbus::fault::MEMORY));

  // ---- battery --------------------------------------------------------------
  {
    const auto& b = in.battery;
    const auto& e = in.batteryEst;
    if (b.valid)
      r.push_back(mk("battery", St::Pass,
                     String(b.millivolts) + "mV " + e.percent + "% " +
                     nimbus::power::trendStr(e.chargeState)));   // trend, not a charge claim
    else
      r.push_back(mk("battery", St::Skip, "no telemetry (divider not fitted / disabled)"));
  }
  // ---- WiFi -----------------------------------------------------------------
  r.push_back(in.wifiConnected
                  ? mk("wifi", St::Pass, String("rssi=") + in.wifiRssi + "dBm")
                  : mk("wifi", St::Skip, "not connected"));
  // ---- BLE (Notifier only) --------------------------------------------------
  if (in.orchMode)
    r.push_back(mk("ble", St::Skip, "Orchestrator mode (BLE inert)"));
  else
    r.push_back(mk("ble", in.bleAdvertising ? St::Pass : St::Fail,
                   String("adv=") + (in.bleAdvertising ? 1 : 0) + " bonds=" + in.bleBonds));
  // ---- SFX tier -------------------------------------------------------------
  r.push_back(mk("sfx", St::Pass, String("tier=") + ::sfx::tierStr() + " level=" + ::sfx::level() +
                                  (::sfx::isSilent() ? " (silent)" : "")));

  // ---- AUDIBLE: acoustic loopback (speaker plays a tone, mic must hear it) ---
  if (!audible) {
    r.push_back(mk("audio", St::Skip, "silent/owner-gated - audible tests skipped"));
  } else if (nimbus::fault::active(nimbus::fault::MIC) || nimbus::fault::active(nimbus::fault::SPEAKER)) {
    r.push_back(mk("audio", St::Fail, "mic/speaker fault-injected"));
  } else {
    solide::audio::LbDiag diag{};
    const bool tone = solide::audio::loopbackSelfTest(1000, nullptr, nullptr, &diag);
    r.push_back(mk("audio", tone ? St::Pass : St::Fail,
                   String("tonePresent=") + (tone ? 1 : 0) + " toneMag=" + diag.toneMag +
                   " ctrlMag=" + diag.ctrlMag + " peak=" + diag.peak));
    // mic level as a separate signal (records a short window).
    int16_t* buf = (int16_t*)heap_caps_malloc(1920 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf) {
      size_t n = solide::audio::recordToBuffer(buf, 1920, 120, nullptr);
      double sq = 0; int peak = 0;
      for (size_t i = 0; i < n; i++) { int v = buf[i]; sq += (double)v * v; int a = v < 0 ? -v : v; if (a > peak) peak = a; }
      const int rms = n ? (int)sqrt(sq / (double)n) : 0;
      free(buf);
      r.push_back(mk("mic", n && peak > 0 ? St::Pass : St::Fail, String("rms=") + rms + " peak=" + peak));
    }
  }
  return r;
}

// ---- renderers --------------------------------------------------------------

static const char* stStr(St s) { return s == St::Pass ? "PASS" : s == St::Fail ? "FAIL" : "SKIP"; }

String selfTestJson(const std::vector<SelfTestItem>& items) {
  int p = 0, f = 0, sk = 0;
  String out = "{\"items\":[";
  for (size_t i = 0; i < items.size(); i++) {
    const auto& it = items[i];
    if (it.status == St::Pass) p++; else if (it.status == St::Fail) f++; else sk++;
    String d = it.detail; d.replace("\\", "\\\\"); d.replace("\"", "\\\"");
    if (i) out += ",";
    out += "{\"name\":\""; out += it.name;
    out += "\",\"status\":\""; out += stStr(it.status);
    out += "\",\"detail\":\""; out += d; out += "\"}";
  }
  out += "],\"pass\":"; out += p; out += ",\"fail\":"; out += f; out += ",\"skip\":"; out += sk; out += "}";
  return out;
}

String selfTestText(const std::vector<SelfTestItem>& items) {
  String out;
  for (const auto& it : items) {
    out += it.name; out += ": "; out += stStr(it.status);
    if (it.detail.length()) { out += " "; out += it.detail; }
    out += "\n";
  }
  return out;
}

String selfTestSummary(const std::vector<SelfTestItem>& items) {
  int p = 0, f = 0, sk = 0;
  for (const auto& it : items)
    if (it.status == St::Pass) p++; else if (it.status == St::Fail) f++; else sk++;
  return String(p) + "P/" + f + "F/" + sk + "S";
}

// ---- shared provider seam ---------------------------------------------------
static InputsProvider s_provider = nullptr;
void setInputsProvider(InputsProvider p) { s_provider = p; }
static SelfTestInputs gather() { return s_provider ? s_provider() : SelfTestInputs{}; }

std::vector<SelfTestItem> runNow(bool audible) { return runSelfTest(gather(), audible); }

String deviceStatusJson() {
  const SelfTestInputs in = gather();
  const bool sd = solide::storage::available() && !nimbus::fault::active(nimbus::fault::SD);
  String o = "{";
  // Identity + time first (owner 2026-07-24: the model couldn't name its own
  // firmware version, and had NO clock - it hallucinated dates in sub-agent
  // briefs and couldn't answer "say the time").
  o += "\"fw\":\"" NIMBUS_FW_VERSION "\"";
  o += ",\"build\":\""; o += NIMBUS_FW_BUILD; o += "\"";
  o += ",\"ota\":\""; o += otaupd::statusStr(); o += "\"";
  { String lat = otaupd::latestSeen();
    if (lat.length()) { o += ",\"otaLatest\":\""; o += lat; o += "\""; } }
  { String now = localTimeStr();
    o += ",\"time\":\"";
    o += now.length() ? now : "unsynced";
    o += "\""; }
  o += ",\"heapFree\":"; o += (uint32_t)ESP.getFreeHeap();
  o += ",\"heapMin\":"; o += (uint32_t)ESP.getMinFreeHeap();
  o += ",\"psramFree\":"; o += (uint32_t)ESP.getFreePsram();
  o += ",\"sd\":"; o += sd ? "true" : "false";
  if (sd) {
    o += ",\"sdFreeMB\":"; o += (uint32_t)solide::storage::freeMB();
    o += ",\"sdTotalMB\":"; o += (uint32_t)solide::storage::cardSizeMB();
  }
  o += ",\"battValid\":"; o += in.battery.valid ? "true" : "false";
  if (in.battery.valid) {
    o += ",\"battMv\":"; o += in.battery.millivolts;
    o += ",\"battPct\":"; o += in.batteryEst.percent;   // model-corrected SoC
    // TREND, not a charge claim (owner 2026-07-16): the model was reading
    // chargeState/onExtPower here and confidently telling the owner "charging, on
    // external power" - but there is NO charge-detect hardware; the state is a
    // voltage-trend inference. Surface only what the ADC saw.
    o += ",\"battTrend\":\""; o += nimbus::power::trendStr(in.batteryEst.chargeState); o += "\"";
    o += ",\"battHealth\":"; o += in.batteryEst.healthPct;
    o += ",\"battCapacityMah\":";                      // health% × CONFIGURED pack mAh
    o += (int(in.batteryEst.healthPct) * (int)agent::store::battCapMah() / 100);
    if (in.batteryEst.valid && in.batteryEst.minutesToEmpty >= 0) {
      o += ",\"minsToEmpty\":"; o += in.batteryEst.minutesToEmpty;
    }
  }
  o += ",\"wifi\":"; o += in.wifiConnected ? "true" : "false";
  if (in.wifiConnected) { o += ",\"rssi\":"; o += in.wifiRssi; }
  o += ",\"bleBonds\":"; o += in.bleBonds;
  o += ",\"faults\":"; o += nimbus::fault::mask();
  o += ",\"sfxTier\":\""; o += ::sfx::tierStr(); o += "\"";
  o += ",\"silent\":"; o += ::sfx::isSilent() ? "true" : "false";
  // Current CONFIG values (owner 2026-07-16: the model could SET these knobs but
  // not READ them - it told the owner sfxVol was "write-only", claimed TTS was off
  // after enabling it, and conflated LED brightness with speaker volume). Every
  // knob the config device-action can set is readable here, so the model answers
  // "what's the volume?" from data instead of guessing.
  o += ",\"cfg\":{";
  o += "\"ttsOn\":";     o += agent::store::ttsEnabled() ? "true" : "false";
  o += ",\"sfxVol\":";   o += (int)agent::store::sfxVolume();
  o += ",\"sfxLvlN\":";  o += (int)agent::store::sfxLevelNotif();
  o += ",\"sfxLvlO\":";  o += (int)agent::store::sfxLevelOrch();
  o += ",\"sfxTheme\":\""; o += agent::store::sfxTheme(); o += "\"";
  o += ",\"sttProv\":\"";  o += agent::store::sttProvider(); o += "\"";
  o += ",\"ttsProv\":\"";  o += agent::store::ttsProvider(); o += "\"";
  o += ",\"ttsVoice\":\""; o += agent::store::ttsVoice(); o += "\"";
  o += ",\"theme\":\"";    o += agent::store::theme(); o += "\"";
  o += ",\"ledBrightness\":"; o += agent::store::ledBright();
  o += "}";
  o += "}";
  return o;
}

}  // namespace nimbus::hw
