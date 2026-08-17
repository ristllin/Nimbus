#pragma once
#include <ArduinoJson.h>

#include <functional>
#include <string>

#include "nimbus/orch/device_actions.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/turn.h"

// apply - turn-application POLICY, lifted from src/agent/orchestrator.cpp
// (Stage E). The portable side owns every decision: the scheduled-turn refusal
// rails (reboot/ttsOn/devName/sleepOvr/brightOvr), the owner-visible risk
// notes, the tts voice-off gate, orch_model key/choice validation, reply+ask
// composition and the bare-"Done." suppression. The injected ApplyDeps are
// EXECUTION ONLY - raw writes, staged device actions, tool dispatch, delivery -
// with no policy branches, so the security rails are host-tested
// (test_harness_apply) and can never silently regress device-side.
namespace agent {

struct ApplyDeps {
  // Device-action execution (no policy inside):
  //   execConfig: apply the inline NVS knobs still flagged on the (already
  //   policy-stripped) action + live sfx refresh. stageDevice: hand a validated
  //   action to the main loop via the DeviceSink (led/lights/reboot + the
  //   posture/profile/theme/attnHold config knobs).
  std::function<void(const nimbus::orch::ValidatedAction&)> execConfig;
  std::function<void(const nimbus::orch::ValidatedAction&)> stageDevice;  // null => (noop) paths
  std::function<void(const std::string& text)> speak;                    // null => tts(no-sink)
  bool ttsEnabled = false;

  // orch_model validation + write (validation data lives device-side).
  std::function<bool(const std::string& prov, const std::string& model)> modelIsValid;
  std::function<bool(const std::string& prov)> providerHasKey;
  std::function<void(const std::string& prov, const std::string& model)> setOrchHostModel;

  // Model working memory (OrchMemory): returns true when truncated to cap.
  std::function<bool(const std::string& chatId, const std::string& v)> setModelMemory;
  std::function<void()> syncMemEcho;
  // v4.1.0: apply an inline scratchpad update to the device
  // scratchpad - replace only the tiers whose has* flag is set; persist after.
  // Null => scratchpad updates ignored (host rigs without a scratchpad).
  std::function<void(const nimbus::orch::ScratchUpdate&)> applyScratch;

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
  std::function<void(const nimbus::orch::Spawn&, const std::string& chatId, bool quiet)> enqueueSpawn;
  std::function<bool(const std::string& id)> cancelSession;
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
  std::string* pendingMemResults = nullptr;  // deferred [MEMORY RESULTS] accumulator
  // v3.7.0: may THIS turn's chat read/write the shared namespace? The device
  // sets it for the owner's chats and the token-authenticated local surfaces;
  // a Telegram member never gets it. Combined with the turn's chatId it forms
  // the Principal every memory tool is dispatched under.
};

// Apply one parsed turn. spawnedOut reports whether any sub-agent was enqueued
// (the caller suppresses the bare "Done." only when true).
void applyTurn(const nimbus::orch::Turn& t, const std::string& chatId,
               const ApplyDeps& d, ApplyState& st, bool& spawnedOut);

// Validate + policy-strip + execute ONE device[] element (raw JSON). The shared
// engine behind applyTurn's device[] loop AND the mid-turn `device.control`
// registry tool - one security envelope for both. Returns the summary token(s)
// ("cfg ", "protected-BLOCKED ", "reboot-refused(scheduled) ", ...); appends any
// owner-visible risk note to *riskNote when non-null.
std::string applyDeviceElement(const std::string& json, bool scheduledTurn,
                               const std::string& chatId, const ApplyDeps& d,
                               std::string* riskNote);

}  // namespace agent
