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
// EVERYTHING here runs on the tg_poll task (single-writer discipline), with ONE
// exception: sntpTick() runs on the main task and touches no loop state.

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

// The chat of the turn creating a loop right now ("" when no turn is running, or when
// called off the turn task), so an agent-created loop (loop.create / wakeup.set)
// remembers the channel it was asked for in and delivers there (CUM-505). main.cpp
// installs it. Called with the loops lock held - keep it cheap and lock-free.
using OriginHook = std::function<std::string()>;
void setOriginHook(OriginHook origin);

// The tick - call every pollJobs cycle (tg_poll). millis()-gated internally.
void checkDue(uint64_t nowEpoch, bool turnInFlight, uint32_t freeHeap);

// True while a loop's scheduled turn is running (tg_poll only; CUM-505). Nobody is
// waiting on a reply then, so the turn-completion wiring must never fabricate a
// "Done." into the channel the loop delivers to (the web chat included).
bool firing();

// A fire whose turn never returned (lane M, L19-31: the device restarted
// mid-dream, so the post-fire outcome write never ran). The dream's boot
// reconcile calls this under the loops lock, ONCE per interruption, after it
// cleared the dream's marker durably: lastResult fail, consecFails +1, and
// nextRun at the same-night retry (retryAfterSec 0 = keep the next slot).
// Returns false when the loop is absent.
bool applyInterruptedOutcome(const String& id, uint32_t retryAfterSec, uint64_t nowEpoch);

// SNTP bring-up (CUM-504). Call on EVERY tick of the network-up hook (net::link,
// main task): netUp is the latch verdict, upEdge says an IP session began this tick.
// Kicks SNTP on every up edge (the boot join included, however early it landed) and
// re-kicks on a bounded backoff while the link is up and the clock is still unsynced
// (nimbus/wifi/net_up.h SntpKick). Never on tg_poll: a kick must not wait out a
// long-poll or a turn. It is the ONLY SNTP producer: "Sync now" (drained on tg_poll)
// only requests a kick here, and a timezone edit applies TZ on tg_poll without one.
void sntpTick(uint32_t nowMs, bool netUp, bool upEdge);

// --- owner/agent mutations (call on tg_poll, or stage from the web task) -----
// createLoop parses `schedule` via the pure parseSpec. byAgent => createdBy=Agent,
// approved=false (owner must approve). inScheduledTurn => refused (fork-bomb guard).
struct CreateResult { bool ok = false; std::string id; std::string err;
                      bool approved = true; };   // false => landed pending (ask-first)
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

// --- wake-up approval surface (CUM-27; contract shared with lane N1) ---------
// GET /api/wakeups body: the Once-loop view + arm state + approval mode.
String wakeupsJson();
// "Wake-ups: ask me first" policy. Off (default) = agent wakeups auto-approve;
// on = they land pending for a single owner approval card. Persisted.
bool wakeupsAskFirst();
void setWakeupsAskFirst(bool on);

// --- web staging (POST /api/loops runs on the AsyncTCP task) -----------------
// Stage a mutation JSON ({"action":"create|approve|pause|resume|delete", ...});
// drained on tg_poll so loop records stay single-writer.
void stageWebMutation(const String& json);
void drainWebMutations();

}  // namespace loops
}  // namespace agent
