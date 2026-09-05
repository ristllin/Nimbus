#pragma once
#include <string>

// health - device self-diagnosis (P5). One aggregator, three consumers:
//   1. GET /api/health        -> the Home-tab health panel (green/amber/red)
//   2. the `system.health` orchestrator tool -> the model can query live state
//   3. composeInstructions()  -> the prompt's CAPABILITIES block is live truth,
//      not the old hardcoded Hardware{} flags.
//
// The report is a PASSIVE snapshot: it aggregates already-known state (the HAL
// begin-result, the fault mask, the storage mount, memory stats, the last
// acoustic-loopback result, WiFi/BLE/battery) and NEVER actuates hardware - so
// it is safe to call from any task (the AsyncTCP web task, the turn task). The
// ACTIVE probes (mic VU / speaker beep / loopback / sdprobe) keep their own
// endpoints; running one updates the cached audio verdict below.
namespace agent::health {

// The few facts the agent/hw layer can't see on its own - filled by the caller
// (webui has BLE/WiFi/battery; the turn-task tool passes what it knows). Unset
// fields report as "unknown"/"absent", which is accurate in those contexts.
struct Env {
  bool bleAvail = false;   // BLE stack is relevant (Notifier mode)
  bool bleOn = false, bleConnected = false;
  bool wifiKnown = false, wifiUp = false;
  int  wifiRssi = 0;
  bool battValid = false, battExt = false;
  int  battPct = 0;
  // Debounced honest faults the HAL begin-result cannot see (filled by webui): an
  // open battery sense divider (monitoring on, readings persistently invalid) and a
  // dead resistive touch controller (stuck-high signature). Default false = the
  // contexts that cannot see them (the turn-task tool) report nothing new.
  bool battSenseMissing = false;
  bool touchDegraded = false;
};

// Record the outcome of an acoustic loopback probe so the passive report can say
// "speaker/mic last verified Ns ago". Called by the /api/audio/loopback handler.
void recordLoopback(bool tonePresent, uint32_t whenMs);
// Single-test evidence (owner 2026-07-16: the Mic/Speaker test buttons left the
// health rows Unknown): a mic meter sample / a driven beep also update the verdict
// - weaker than the loopback (which proves both ends acoustically) and worded so.
void recordMicSample(bool ok, uint32_t whenMs);
void recordBeep(bool ok, uint32_t whenMs);

// Build the health report as a JSON object:
//   {"components":[{"key","label","state","detail"}...],"ok":N,"degraded":M,"absent":K}
// state in {"ok","degraded","absent","unknown"}. Self-contained + allocation-
// bounded; safe on any task.
std::string reportJson(const Env& env);

}  // namespace agent::health
