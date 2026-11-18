#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/token_usage.h"

namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/wire.h - the renderer and parser one dialect implements (PRD section 1,
// "Request on each wire"). render() is a pure function of stored state: the same
// RenderInput gives the same bytes, which is the prefix property the battery tests.
// parse() reads one response into a ParsedStep.
//
// Each wire lives in its own file (lib/harness/src/providers/plain_wire_<dialect>.cpp,
// lanes RB-W1a/W1b, RB-W2, RB-W3, RB-W4) and is reached through wireFor(), whose switch
// sits in lib/harness/src/plain_wire.cpp: a wire lane adds its one case there.
//
// Seam declared by lane RB-B0a.
namespace agent {
namespace plain {

// The system text in its three cache blocks: [core][skills index + pinned notes]
// [digests + recent files].
struct SystemBlocks {
  std::string core;
  std::string notes;
  std::string digests;
};

struct RenderInput {
  const TurnCtx* turn = nullptr;
  const SystemBlocks* system = nullptr;
  const std::vector<ToolSpec>* tools = nullptr;
  const nimbus::orch::tx::TxArena* items = nullptr;   // the transcript to render
  uint64_t stubPointer = 0;                           // items at or below render as stubs
  bool toolChoiceNone = false;                        // the last round after a guard trip
  uint32_t maxTokens = 0;                             // the output cap for this request
};

// Why the model stopped.
enum class StopReason : uint8_t { EndTurn = 0, ToolUse = 1, MaxTokens = 2, Refusal = 3, Other = 4 };
inline const char* stopSlug(StopReason s) {
  switch (s) {
    case StopReason::EndTurn: return "end";
    case StopReason::ToolUse: return "tool";
    case StopReason::MaxTokens: return "cut";
    case StopReason::Refusal: return "refusal";
    case StopReason::Other: return "other";
  }
  return "other";
}

// One response, parsed.
struct ParsedStep {
  std::string text;                 // assistant text (empty when the round only calls)
  std::vector<ToolCall> calls;
  std::string meta;                 // opaque round meta stored as a k item (signed thinking)
  StopReason stop = StopReason::Other;
  nimbus::orch::TokenUsage usage;
};

class Wire {
 public:
  virtual ~Wire() = default;
  virtual Dialect dialect() const = 0;
  // Fill `out` (host, path, headers, body, splices) for one request.
  virtual Status render(const RenderInput& in, HttpRequest& out) const = 0;
  // Read one 2xx response body.
  virtual Status parse(const HttpResponse& in, ParsedStep& out) const = 0;
};

// The wire of a dialect. NotImplemented until that dialect's lane merges.
Status wireFor(Dialect dialect, const Wire*& out);

}  // namespace plain
}  // namespace agent
