#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/claim_check.h"
#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/head_switch.h"     // OwnerMessage - who may confirm a head switch
#include "nimbus/orch/memory_tools.h"   // kMemOutcome* codes
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/turn.h"

// apply - turn-application POLICY, lifted from src/agent/orchestrator.cpp
// (Stage E). The portable side owns every decision: who may run each device
// action and config knob (the caller rule table, nimbus/orch/action_rules.h:
// owner, member, guest, unattended turn, LAN), the owner-visible risk
// notes, the tts voice-off gate, orch_model key/choice validation, reply+ask
// composition and the bare-"Done." suppression. The injected ApplyDeps are
// EXECUTION ONLY - raw writes, staged device actions, tool dispatch, delivery -
// with no policy branches, so the security rails are host-tested
// (test_harness_apply) and can never silently regress device-side.
namespace agent {

// What an inline scratchpad update did (ApplyDeps::applyScratch).
enum class ScratchOutcome : uint8_t { Applied = 0, AppliedRamOnly, Skipped };

struct ApplyDeps {
  // Device-action execution (no policy inside):
  //   execConfig: apply the inline NVS knobs still flagged on the (already
  //   policy-stripped) action + live sfx refresh. Returns whether a stored
  //   value CHANGED (before != after): false is a no-op action (23 Oct: a
  //   model invented a config field, nothing switched, the reply said it had)
  //   and the device action then reports "no change", so the claim ledger backs
  //   no "switched" reply. stageDevice: hand a validated action to the main
  //   loop via the DeviceSink (led/lights/reboot + the posture/profile/theme/
  //   attnHold config knobs).
  std::function<bool(const nimbus::orch::ValidatedAction&)> execConfig;
  std::function<void(const nimbus::orch::ValidatedAction&)> stageDevice;  // null => (noop) paths
  std::function<void(const std::string& text)> speak;                    // null => tts(no-sink)
  bool ttsEnabled = false;

  // orch_model validation + write (validation data lives device-side).
  std::function<bool(const std::string& prov, const std::string& model)> modelIsValid;
  std::function<bool(const std::string& prov)> providerHasKey;
  std::function<void(const std::string& prov, const std::string& model)> setOrchHostModel;
  // An orch_model the provider's cached list does not hold: the device asks for
  // one list refresh (at most one per provider per hour, never a connection here)
  // and returns the head's answer, "not in <provider> list cached <date>; refresh
  // queued" (models_tool.h modelMissText). Null => "not in <provider> list".
  std::function<std::string(const std::string& prov, const std::string& model)> modelMiss;
  // A pause never moves the head (nimbus/orch/head_switch.h). headHost: the head the
  // owner chose (before any failover). headPause: what holds paid requests to `prov` off
  // now (nothing set = open). ownerMessage: the owner's message this turn and
  // its number (empty for a turn the owner did not write: a member, a guest, a routine).
  // nowMs: the device clock. Null headPause => no guard.
  std::function<std::string()> headHost;
  std::function<nimbus::orch::HeadPause(const std::string& prov)> headPause;
  std::function<nimbus::orch::OwnerMessage()> ownerMessage;
  std::function<uint32_t()> nowMs;

  // Model working memory (OrchMemory): returns true when truncated to cap.
  std::function<bool(const std::string& chatId, const std::string& v)> setModelMemory;
  std::function<void()> syncMemEcho;
  // v4.1.0: apply an inline scratchpad update to the device
  // scratchpad - replace only the tiers whose has* flag is set; persist after.
  // The outcome says whether the save landed: AppliedRamOnly means the notes
  // changed but live in RAM only until restart (the claim check holds a
  // "survives a reboot" reply against it). Skipped = nothing was applied.
  // Null => scratchpad updates ignored (host rigs without a scratchpad).
  std::function<ScratchOutcome(const nimbus::orch::ScratchUpdate&)> applyScratch;

  // Memory tools: run `fn` under the device memory Lock (tests: direct call);
  // dispatch a registry tool; persist after mutations.
  std::function<void(const std::function<void()>&)> withMemoryLock;
  std::function<nimbus::orch::ToolResult(const char* name,
                                         ArduinoJson::JsonObjectConst args,
                                         const nimbus::orch::Principal& who)> memDispatch;
  // v3.7.0: who is this chat, in full - namespace, role and quota.
  //
  // This replaced a bool-only isOwnerChat hook, and the reason matters: with a
  // bool, the harness could only build a Principal via principalForChat(), which
  // hardcodes role = owner ? Admin : User and leaves the quota empty. Every RBAC
  // rail downstream then read a role nobody had set, so changing someone's role
  // or their storage limit did NOTHING on the real turn path - while the test
  // seams, which called the device's roleOfChat(), exercised a rail production
  // never used. A bool cannot carry a role; the hook has to return the Principal.
  //
  // Null => a deny-all principal scoped to its own namespace (the safe default).
  std::function<nimbus::orch::Principal(const std::string& chatId)> principalFor;

  // The ONE way the harness builds a principal. Every dispatch path goes through
  // this so a chat cannot be admitted under different rules depending on which
  // code path reached it.
  nimbus::orch::Principal whoFor(const std::string& chatId) const {
    if (principalFor) return principalFor(chatId);
    nimbus::orch::Principal p;                 // no hook: deny-all, own namespace
    p.ns = nimbus::orch::nsForChat(chatId, false);
    return p;
  }
  std::function<void()> persistMemory;

  // Turn produced no user-visible output (no reply/ask, nothing spawned, no tool
  // reply). Async channels (Telegram) stay silent - no fabricated "Done." - but
  // SYNCHRONOUS channels (web chat / on-device voice / serial) resolve their poll
  // ONLY on a delivered message, so the device wiring signals completion there.
  std::function<void(const std::string& chatId)> turnComplete;

  // Sessions (spawn queue + journal stay device-side until Stage F/G).
  // Returns true only when the spawn joined the job queue (false: refused, and
  // the refusal already reached the owner and the head).
  std::function<bool(const nimbus::orch::Spawn&, const std::string& chatId, bool quiet)> enqueueSpawn;
  std::function<bool(const std::string& id)> cancelSession;
  // Lane QW4 (CUM-774): true when that stop is not yet confirmed by the provider (the
  // job stays tracked and its cancel is asked again). Nullable: then "Stopped".
  std::function<bool(const std::string& id)> sessionStopping;
  std::function<void(const std::string& firstTag)> awaitTag;  // aim round-robin + poll now
  std::function<void()> noteSpawned;                          // dispatch-now nudge

  // Delivery + attention + capture + sound cues.
  std::function<void(const std::string& chatId, const std::string& text)> deliver;
  std::function<void()> emitAsk;
  std::function<void(const std::string& chatId, const std::string& text)> captureAssistant;
  std::function<void(const char* cue)> fire;  // "reply" | "memsaved"
};

// Mutable per-turn state threaded from the engine/glue.
struct ApplyState {
  bool scheduledTurn = false;        // g_inScheduledTurn: refusal rails armed
  bool toolRepliedThisTurn = false;  // a mid-loop reply.telegram already answered
  // Quiet turn (the DREAM loop): an all-empty turn delivers NOTHING instead of
  // the bare "Done." fallback - an empty reply is that turn's designed success
  // ("say nothing unless the owner is needed"). Risk notes still deliver.
  bool quietFallback = false;
  std::string riskNote;              // pending owner-visible note; consumed here
  std::string lastReply;             // out: captured for loops' semantic-repeat hash
  ScratchOutcome scratchOutcome = ScratchOutcome::Skipped;   // out: the inline scratchpad write
  // Lane S, the claim check: apply records each real outcome in its ledger (a
  // memory write's result, an accepted or refused spawn, the scratchpad save,
  // the device actions), then a reply claim nothing backs gets one owner line
  // per class, and the history keeps the delivered text. Null => no check.
  ClaimCheck* claims = nullptr;
  std::string delivered;            // out: the reply text as the owner received it
  std::string* pendingMemResults = nullptr;  // deferred [MEMORY RESULTS] accumulator
  // CUM-536: this turn read a sub-agent result whose connector never ran (it
  // opens with the connector-not-called marker). Its mem_write items are refused
  // and the refusal lands in the next turn's feedback, so a guess never becomes a
  // stored fact.
  bool unverifiedConnector = false;
  // Lane L, spawn honesty. Every spawn intent either reaches the job engine or
  // is reported: to the head through this hook (the job engine's [SPAWN
  // REFUSED] lines for the chat), and to the owner as one correction line on
  // this reply. Null => logged only.
  std::function<void(const std::string& name, const std::string& task,
                     const std::string& reason)> noteSpawnRefused;
  // The head called the spawn field as a tool this turn (its items, if any,
  // were merged into the turn already).
  bool spawnAttempted = false;
  int spawnsEnqueued = 0;   // out: spawn intents handed to the job engine
  // Out: this turn's mem_write[] outcomes (a duplicate counts as neither). The
  // turn and dream records carry them, so "ok" never hides a lost write.
  int memStored = 0;
  int memQueued = 0;
  int memRefused = 0;
  // v3.7.0: may THIS turn's chat read/write the shared namespace? The device
  // sets it for the owner's chats and the token-authenticated local surfaces;
  // a Telegram member never gets it. Combined with the turn's chatId it forms
  // the Principal every memory tool is dispatched under.
};

// The refusal a memory write gets while the turn carries an unverified connector
// result (CUM-536) - shared by applyTurn's mem_write[] and the head loop's
// memory.write tool, so both paths say the same thing.
extern const char kUnverifiedMemoryRefusal[];

// One memory.write result, classified by its outcome code. Unknown or missing
// codes on a failure read as Refused, so a new code can never be counted as saved.
enum class MemWriteOutcome { Stored, Duplicate, Queued, Refused };
MemWriteOutcome memWriteOutcome(const nimbus::orch::ToolResult& r);

// Whether `reply` already carries the question in `ask`, so the ask is not
// appended to the delivered message: the reply ends with `ask` verbatim, or its
// final sentence is a question asking the same thing (a reworded copy,
// ASK-DUPLICATE: at least half of the ask's topic words appear in it, or all of the
// ask's key nouns do). A different question is appended. Either empty: false. The ask
// still raises the needs-you state either way.
bool replyAlreadyAsks(const std::string& reply, const std::string& ask);

// Apply one parsed turn. spawnedOut reports whether any sub-agent was enqueued
// (the caller suppresses the bare "Done." only when true).
void applyTurn(const nimbus::orch::Turn& t, const std::string& chatId,
               const ApplyDeps& d, ApplyState& st, bool& spawnedOut);

// What one device element did.
struct DeviceOutcome {
  std::string summary;      // the token(s): "cfg ", "reboot-refused(scheduled) ", ...
  bool refusedWhole = false;   // the caller rule refused the element as a whole
  std::string refusals;     // every part the caller rule refused, in plain words ("" = none)
};

// Validate + check who asked + policy-strip + execute ONE device[] element (raw
// JSON). The shared engine behind applyTurn's device[] loop AND the mid-turn
// `device.control` registry tool - one security envelope for both. `who` is the
// caller's Principal (the turn's chat, or the tool dispatch's), `unattended` the
// turn's flag (routine, wakeup, dream, results synthesis, system turn); the rule
// table (nimbus/orch/action_rules.h) decides each action and config knob. Appends
// any owner-visible note to *riskNote when non-null.
DeviceOutcome applyDeviceAction(const std::string& json, const nimbus::orch::Principal& who,
                                bool unattended, const std::string& chatId, const ApplyDeps& d,
                                std::string* riskNote);

// The same for the turn's own chat (the caller is d.whoFor(chatId)), returning the
// summary token(s): a convenience for host rigs and tests (the firmware calls
// applyDeviceAction).
std::string applyDeviceElement(const std::string& json, bool scheduledTurn,
                               const std::string& chatId, const ApplyDeps& d,
                               std::string* riskNote);

}  // namespace agent
