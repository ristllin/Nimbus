#pragma once
#include <cstdint>
#include <functional>
#include <string>

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

struct TurnStartEv { TurnSource source; std::string chatId; };
struct TurnEndEv {
  bool ok = false;
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
  // Lane T (frozen at turn end, so the hook never reads a store that the turn's own
  // device actions may already have changed):
  std::string model;           // the model key of the last host that billed ("" = none)
  TurnSource  source = TurnSource::Owner;
  std::string attribution;     // spend tag: "turn" | "synthesis" | "loop:<id>"
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
};

struct Hooks {
  std::function<void(const TurnStartEv&)> onTurnStart;
  std::function<void(const nimbus::orch::HeadToolCall&)> onToolCall;
  std::function<void(const nimbus::orch::HeadToolResult&)> onToolResult;
  std::function<void(const ThinkingEv&)> onThinking;
  std::function<void(const TurnEndEv&)> onTurnEnd;
  std::function<void(const SpawnEv&)> onSpawn;
  std::function<void(const ResultEv&)> onResult;
  std::function<void(const TurnDebugEv&)> onTurnDebug;
  std::function<void(const DreamStartEv&)> onDreamStart;
  std::function<void(const DreamEndEv&)> onDreamEnd;
  std::function<void(const char* stage, const char* err)> onError;
};

}  // namespace agent
