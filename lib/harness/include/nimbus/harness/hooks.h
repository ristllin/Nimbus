#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/turn_anatomy.h"  // HistoryFacts (TurnDebugEv)
#include "nimbus/orch/failure_chain.h"   // FailureChain - why a turn failed
#include "nimbus/orch/head_loop.h"       // HeadToolCall / HeadToolResult
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/turn_spend.h"      // TurnSpend - who billed a turn

// Hooks - the harness lifecycle observer contract (v1: observers ONLY; no hook
// may mutate the turn). The device wires SFX cues, the /api/lastturn debug
// capture, and log breadcrumbs; tests wire recorders; a future web SSE trace
// tab consumes the same events. All slots optional - a null hook is skipped.
//
// The one deliberate exception to observer-only is planned for later and is NOT
// here: a sync PreToolUse policy verdict will arrive with the ToolPolicy table
// (P7 substrate) as an additive field.
namespace agent {

namespace dream { struct NightReport; }   // dream.h - the night's outcome (lane M)

enum class TurnSource : uint8_t { Owner = 0, Synthesis = 1, Loop = 2, Serial = 3 };

// The machine slug of a turn's source for rows and logs: "owner" | "serial" |
// "synthesis" | "loop", and "dream" for the reserved nightly loop (attribution
// "loop:dream").
inline const char* turnSourceSlug(TurnSource s, const std::string& attribution) {
  switch (s) {
    case TurnSource::Owner:     return "owner";
    case TurnSource::Serial:    return "serial";
    case TurnSource::Synthesis: return "synthesis";
    case TurnSource::Loop:      return attribution == "loop:dream" ? "dream" : "loop";
  }
  return "owner";
}

// True for a turn a person started (the owner's chat or the serial console).
// Spelled here, in this header: on device builds Arduino.h can macro-ize the
// token `Serial`, so TUs that include Arduino.h (via ArduinoJson) before their
// own body must use this helper instead of writing TurnSource::Serial.
inline bool isPersonSource(TurnSource s) { return s == TurnSource::Owner || s == TurnSource::Serial; }

struct TurnStartEv { TurnSource source; std::string chatId; };
struct TurnEndEv {
  bool ok = false;
  // The request did not fit and conversation history was left out (turn_fit.h): the chat
  // needs its fold even though the turn was answered.
  bool trimmedForSize = false;
  std::string host;
  nimbus::orch::TokenUsage usage;   // the turn's billed total (every host) - spend->total()
  int rounds = 0;            // executed mid-turn tool calls (0 = single-shot; the
                             // per-provider ROUND count lives inside the loop and
                             // isn't surfaced - v1 proxy, rename when it is)
  size_t replyBytes = 0;
  std::string chatId;        // the turn's routing chat (v3.6.0 fold accounting)
  std::string error;         // the LAST provider/parse error when !ok ("" on success) -
                             // the device classifies context-overflow here (reactive fold)
  // Served-by disclosure (CUM-236): what actually answered vs what was asked for.
  // `host` above is the failover-final (served) provider; these carry the model
  // detail and the requested head so a display can say "served by <p> <m>" honestly.
  std::string requestedHost;   // the head chosen before any failover
  std::string requestedModel;  // orchModel(requestedHost) (may be empty)
  std::string servedModel;     // the model key `host` billed under ("" when it billed nothing)
  bool fallback = false;        // served host != requested, or served model differs from requested;
                                // false when no host billed (nothing was served)
  // Lane DEGRADE: whether the host the turn ended on (`host`) billed, i.e. someone answered it, and the
  // providers the turn ran on in order ("a -> b -> c") when it switched; "" when it did not.
  bool billed = false;
  std::string path;
  // Lane T (frozen at turn end, so the hook never reads a store that the turn's own
  // device actions may already have changed):
  std::string model;           // the model key of the last host that billed ("" = none)
  TurnSource  source = TurnSource::Owner;
  std::string attribution;     // spend tag: "turn" | "synthesis" | "loop:<id>"
  // Lane ECHO (owner design, 9/11 Nov): a background turn (routine, loop,
  // sub-agent result, the upkeep) that ran during quiet hours and produced a
  // reply does NOT deliver it at night and does NOT hold it for the owner's next
  // message - the reply rides here so the onTurnEnd glue arms ONE self-wakeup
  // (nimbus/orch/self_wakeup.h) due at the end of quiet hours. Empty on every
  // other turn.
  std::string wakeReply;
  // Borrowed, valid only during the hook call: the per-host spend (every host that
  // billed, filed under the model it billed with) and, when !ok, why it failed (the
  // first cause first; never empty on a failed turn).
  const nimbus::orch::TurnSpend*    spend = nullptr;
  const nimbus::orch::FailureChain* chain = nullptr;
};
struct SpawnEv  {
  std::string tag, backend, category, model;
  std::string task;     // the brief's task text (B5 - the debug UI shows what was asked)
  std::string chatId;   // the spawning chat (B5 - capture rows land in ITS session)
};
struct ResultEv {
  std::string tag; uint8_t state; bool terminal;
  std::string reply;    // terminal Done/Error only: the sub-agent's reply text (B5)
  std::string chatId;   // the spawning chat (B5)
};
// DREAMING lifecycle (fired by the dream glue around its two phases, not by the
// TurnEngine): DreamStart before maintenance, DreamEnd after the reflection turn.
struct DreamStartEv { uint64_t epoch = 0; };
struct DreamEndEv {
  bool turnOk = false;     // the reflection turn's FireOutcome.ok
  int prunedVectors = 0;   // removed by pruneExpired in the maintenance phase
  int dedupedVectors = 0;  // removed by deduplicate in the maintenance phase
  uint32_t tokens = 0;     // real billed spend of the reflection turn (this turn's own)
  std::string cause;       // FireOutcome.error: the first cause when the turn failed
  // Lane M: the whole night (outcome, cause, write counts, retry, streak). Borrowed,
  // valid only during the hook call; renderDreamRow(*report) is the timeline row.
  const dream::NightReport* report = nullptr;
};
// Mid-turn round prose ("thinking") from the head loop (Glass Box A4). Fired
// once per round whose provider step produced non-tool text; the device
// persists it as a kind=llm_response episodic row on the turn's chat.
struct ThinkingEv {
  std::string chatId;
  std::string text;
  int round = 0;
};

struct TurnDebugEv {
  // Introspection snapshot for /api/lastturn: what the model actually got.
  std::string host;
  bool convContinued = false;
  const std::string* instructions;   // borrowed, valid only during the hook call
  const std::string* inputs;
  const std::string* rawOut;
  // The tool loop's canonical transcript brief (Glass Box P3) - per-round prose,
  // tool calls WITH args, results. Borrowed like the others; may be null, and is
  // empty for a single-shot turn that never entered a loop.
  const std::string* transcriptBrief = nullptr;
  bool ok = false;
  // Why the turn failed (borrowed like the others; null or empty on success).
  const nimbus::orch::FailureChain* failure = nullptr;
  // The turn ran the stateless tool loop (no provider-side conversation).
  bool loopTurn = false;
  // What the device sent as history in the system prompt (turn_anatomy.h).
  HistoryFacts history;
  // The model the answering host really ran (SpendSlot::served: its last response's
  // "model" field, an alias resolved through the provider's list; "" = none named one)
  // and the model the device asked that host for.
  std::string servedModel;
  std::string askedModel;
};

// A round boundary of a running turn (lane PROG): fired at EVERY boundary of
// the head loop - before each provider request, round 0 included - which is the
// only moment no request is in flight, so a surface may signal work (a typing
// keep-alive, a busy state) or look for messages that arrived meanwhile. The
// plain-words plan for what to show is turn_progress.h; this event carries the
// raw facts. `tools` names the calls the PREVIOUS round dispatched (empty on
// round 0), `lastRound` means the forced final round is starting (the answer is
// imminent - stay quiet), and `maxRounds` is the loop's round cap.
struct TurnProgressEv {
  std::string chatId;
  int round = 0;
  uint32_t elapsedMs = 0;
  bool lastRound = false;
  std::vector<std::string> tools;
  int maxRounds = 0;
  // Lane ECHO fix pass (review finding, 11 Nov): the turn's source, so a surface
  // can keep progress signals (typing, the one line) to turns a person started -
  // a dream, routine, loop or synthesis turn never signals the chat.
  TurnSource source = TurnSource::Owner;
  std::string attribution;   // spend tag: "turn" | "synthesis" | "loop:<id>"
};

struct Hooks {
  std::function<void(const TurnStartEv&)> onTurnStart;
  std::function<void(const nimbus::orch::HeadToolCall&)> onToolCall;
  std::function<void(const nimbus::orch::HeadToolResult&)> onToolResult;
  std::function<void(const ThinkingEv&)> onThinking;
  std::function<void(const TurnProgressEv&)> onTurnProgress;
  std::function<void(const TurnEndEv&)> onTurnEnd;
  std::function<void(const SpawnEv&)> onSpawn;
  std::function<void(const ResultEv&)> onResult;
  std::function<void(const TurnDebugEv&)> onTurnDebug;
  std::function<void(const DreamStartEv&)> onDreamStart;
  std::function<void(const DreamEndEv&)> onDreamEnd;
  std::function<void(const char* stage, const char* err)> onError;
  // Lane ECHO (owner design, 9/11 Nov): is the local time inside quiet hours
  // right now (the owner's quiet-hours-end setting, default 09:00 local)? A
  // background turn that has something worth telling the owner while this is
  // true arms a self-wakeup for the end of quiet hours instead of messaging at
  // night or holding a note for the owner's next message.
  std::function<bool()> quietHoursNow;
};

}  // namespace agent
