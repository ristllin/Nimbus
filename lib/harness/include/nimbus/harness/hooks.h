#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/orch/head_loop.h"       // HeadToolCall / HeadToolResult
#include "nimbus/orch/token_usage.h"

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

struct TurnStartEv { TurnSource source; std::string chatId; };
struct TurnEndEv {
  bool ok = false;
  std::string host;
  nimbus::orch::TokenUsage usage;
  int rounds = 0;            // executed mid-turn tool calls (0 = single-shot; the
                             // per-provider ROUND count lives inside the loop and
                             // isn't surfaced - v1 proxy, rename when it is)
  size_t replyBytes = 0;
  std::string chatId;        // the turn's routing chat (v3.6.0 fold accounting)
  std::string error;         // provider/parse error text when !ok ("" on success) -
                             // the device classifies context-overflow here (reactive fold)
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
  uint32_t tokens = 0;     // real billed spend of the reflection turn
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
  bool ok = false;
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
