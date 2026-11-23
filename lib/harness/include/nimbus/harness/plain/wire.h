#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/tx/rules.h"   // BoundTools (E2-FIX5)

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

// Where the head lives and how it is asked: what render() needs beyond the transcript.
// The key is read through a function at render time and never stored by a wire.
struct HeadEndpoint {
  std::string host;                 // "api.anthropic.com", a custom base's host, the router
  uint16_t port = 443;
  bool tls = true;
  std::string basePath;             // "" = the dialect's default ("/v1")
  std::string effort;               // reasoning effort ("" = the provider default)
  bool routed = false;              // through the Cumulo router (deadline header, refusal contract)
  std::function<std::string()> key;
};

// A tier-2 payload shown whole, read back by the r item's handle at render time (lane E2-VERB:
// every turn's, until the overflow path stubs it; the payload lives in the bulk file only).
using BulkReadFn = std::function<Status(const std::string& handle, std::string& out)>;
// A visible photo by ref: its bytes for an inline splice, or a provider file id.
struct PhotoBytes {
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  std::string mime;
  std::string fileId;               // Anthropic Files API id when uploaded, else ""
};
using PhotoFn = std::function<Status(const std::string& ref, PhotoBytes& out)>;

// The overflow ladder's state for one request (PRD section 2, "Overflow ladder"; tx::nextRung),
// owned by the engine and changed by its climb between two renders of the same request, so a
// wire reads it through RenderInput::ladder. All zero: no rung applied.
struct LadderState {
  uint64_t closedThrough = 0;   // rung 1: render as if the stub pointer were max(stubPointer, this)
  uint64_t stubThrough = 0;     // rung 2: tx::ShowFacts::ladderStubThrough (open-turn bulk)
  uint64_t omitThrough = 0;     // rung 3: whole turns whose items are all at or below this q are
                                // left out, and omissionLine stands in their place as user text
  std::string omissionLine;     // tx::omissionLine's text (set together with omitThrough)
};

struct RenderInput {
  const TurnCtx* turn = nullptr;
  const HeadEndpoint* head = nullptr;
  const SystemBlocks* system = nullptr;
  const std::vector<ToolSpec>* tools = nullptr;
  const nimbus::orch::tx::TxArena* items = nullptr;   // the transcript to render
  uint64_t stubPointer = 0;                           // items at or below render as stubs
  bool toolChoiceNone = false;                        // the last round after a guard trip
  uint32_t maxTokens = 0;                             // the output cap for this request
  // A JSON schema the answer's text must follow (the provider's structured output, lane E2-CMP: the
  // summarizer's changes). Null: free text. A wire that has no structured output ignores it.
  const char* answerSchema = nullptr;
  BulkReadFn bulk;                                    // whole bulk results (every turn's, E2-VERB)
  PhotoFn photo;                                      // pixels of the visible photos
  nimbus::orch::tx::BoundTools bound;                 // E2-FIX5: the photo stub names view only when bound
  const LadderState* ladder = nullptr;                // the overflow ladder (null: no rung)
  // Lane E2-CACHE: the cache points on the stable prefix (the system blocks and the end of the
  // previous turn) carry the provider's 1-hour lifetime; the moving last block keeps 5 minutes.
  // False: every point has the 5-minute lifetime (what the request carried before).
  bool longCache = false;
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

// One opaque piece of round meta (signed thinking, a reasoning item), kept where it sat
// so a resume renders the same bytes: it came before call `beforeCall` (calls.size() =
// after the last call).
struct MetaPart {
  uint16_t beforeCall = 0;
  std::string bytes;
};

// One response, parsed.
struct ParsedStep {
  std::string text;                 // assistant text (empty when the round only calls)
  std::vector<ToolCall> calls;
  std::vector<MetaPart> meta;       // stored as k items, in order
  std::string refusal;              // the provider's refusal text, when it refused
  StopReason stop = StopReason::Other;
  nimbus::orch::TokenUsage usage;
};

// Named WireCodec, not Wire: Arduino's global I2C object is called Wire.
class WireCodec {
 public:
  virtual ~WireCodec() = default;
  virtual Dialect dialect() const = 0;
  // Fill `out` (host, path, headers, body, splices) for one request.
  virtual Status render(const RenderInput& in, HttpRequest& out) const = 0;
  // Read one 2xx response body.
  virtual Status parse(const HttpResponse& in, ParsedStep& out) const = 0;
};

// The wire of a dialect. NotImplemented until that dialect's wire is COMPLETE (render and
// parse): the engine only ever gets a wire that can read its own answers.
Status wireFor(Dialect dialect, const WireCodec*& out);
// The render half of a dialect, for the property suites: today every landed render belongs to
// a complete wire (Anthropic: render RB-W1a, parse RB-W1b), so it answers as wireFor does. The
// engine never uses it.
Status renderWireFor(Dialect dialect, const WireCodec*& out);

}  // namespace plain
}  // namespace agent
