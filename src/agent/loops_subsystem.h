#pragma once
#include <Arduino.h>

#include <functional>
#include <string>

#include <ArduinoJson.h>

#include "nimbus/orch/loops.h"

// Local Loops device subsystem - the side-effecting glue around the pure core
// (lib/core/orch_loops.cpp). Owns the LittleFS-persisted loop vector + device
// counters, the millis()-gated tick (driven from pollJobs on the tg_poll task),
// NTP bring-up + the SNTP-landed rebase, and epoch<->local civil conversion via
// libc. Kept FREE of orchestrator/telegram deps: it calls injected hooks so
// main.cpp wires the actual turn-injection, allowlist check, and alert delivery.
// EVERYTHING here runs on the tg_poll task (single-writer discipline).

namespace agent {
namespace loops {

using nimbus::orch::FireOutcome;
using nimbus::orch::LoopFireRequest;

enum class AlertLevel : uint8_t { Info = 0, Warn = 1, Critical = 2 };

// Hooks (set once in begin()):
//   fire        - run a scheduled orchestrator turn synchronously, return real usage.
//   chatAllowed - is chatId currently in the Telegram allowlist? (fire-time re-check).
//   alert       - deliver an owner alert (Telegram + ring) at a severity.
using FireHook        = std::function<FireOutcome(const LoopFireRequest&)>;
using ChatAllowedHook = std::function<bool(const std::string&)>;
using AlertHook       = std::function<void(AlertLevel, const std::string& loopId,
                                           const std::string& msg)>;

void begin(FireHook fire, ChatAllowedHook chatAllowed, AlertHook alert);

// Re-read the owner's Local Loops cap overrides from NVS and re-fold them onto
// the caps.h defaults (tighten-only). Call after the web config path writes a
// governor key so a tightened cap applies live, no reboot (CUM-73).
void reloadCaps();

// --- reserved system loops (DREAMING) ---------------------------------------
// Ensure a well-known record exists (insert-if-missing; NEVER duplicates or
// overrides persisted owner state such as enabled=false) and mark its id
// reserved. Reserved ids cannot be cancelled/deleted from ANY surface (MCP
// loop.cancel, web delete, /loop deny) - pause/resume still work. Call after
// begin() (main.cpp wires the dream record here).
void ensureLoop(const nimbus::orch::LoopRecord& record, bool reserved = true);
bool isReservedId(const String& id);

// Pre-fire idle gate: return 0 to fire now, else seconds to defer (nextRun
// slips by that much via nimbus::orch::deferLoop; a defer does NOT count
// against any fire ceiling). Called on tg_poll with the loops lock held -
// keep it cheap and lock-free. main.cpp installs the dream gate.
using GateHook = std::function<uint32_t(const nimbus::orch::LoopRecord&)>;
void setFireGate(GateHook gate);

// The tick - call every pollJobs cycle (tg_poll). millis()-gated internally.
void checkDue(uint64_t nowEpoch, bool turnInFlight, uint32_t freeHeap);

// SNTP bring-up - call from the WiFi GOT_IP handler (main.cpp).
void onNetworkUp();

// --- owner/agent mutations (call on tg_poll, or stage from the web task) -----
// createLoop parses `schedule` via the pure parseSpec. byAgent => createdBy=Agent,
// approved=false (owner must approve). inScheduledTurn => refused (fork-bomb guard).
struct CreateResult { bool ok = false; std::string id; std::string err; };
CreateResult createLoop(const String& name, const String& prompt, const String& chatId,
                        ArduinoJson::JsonObjectConst schedule, bool byAgent,
                        bool inScheduledTurn);
bool approveLoop(const String& id);          // owner-only
bool setEnabled(const String& id, bool on);  // pause/resume
bool cancelLoop(const String& id);           // disable + remove

// Status for /api/loops + /api/orch (no secrets).
String loopsJson();
String loopsText();   // human-readable list for the Telegram /loops command
int    count();

// --- web staging (POST /api/loops runs on the AsyncTCP task) -----------------
// Stage a mutation JSON ({"action":"create|approve|pause|resume|delete", ...});
// drained on tg_poll so loop records stay single-writer.
void stageWebMutation(const String& json);
void drainWebMutations();

}  // namespace loops
}  // namespace agent
