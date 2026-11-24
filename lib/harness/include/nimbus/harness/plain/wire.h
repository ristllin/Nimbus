#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/output_caps.h"   // tableCap: the head's effort (E2-FIX14)
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/tx/rules.h"   // BoundTools (E2-FIX5)

namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
}  // namespace tx
struct ConnectorInfo;   // nimbus/orch/connectors_wire.h (lane E2-P-CONN2)
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

struct Route;   // plain/route.h (lane E2-R4a)

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
  // A custom endpoint whose verify accepted a tool call and found a window that holds the fixed part
  // (router design section 3.3, lane E2-R8a): its route offers tools; false, its chat row is text mode.
  bool toolsVerified = false;
  std::function<std::string()> key;
  // Lane E2-P-CONN2: the owner's connectors as engine 1's attach builders read them
  // (nimbus/orch/connectors_wire.h, the Code sandbox switch applied) and each one's bearer, read at
  // render time and never stored. Null: no provider-side connector or sandbox rides the request.
  std::function<std::vector<nimbus::orch::ConnectorInfo>()> connectors;
  std::function<std::string(const nimbus::orch::ConnectorInfo&)> bearer;
};

// The reasoning effort a head request carries for `model` at `provider` (lane E2-FIX14): the output
// table's head-round value (providers::tableCap, the one table engine 1's head rounds read: medium on a
// Claude family that takes the field, nothing on one that answers 400 to it). The device's binding and
// nimbusd's engine 2 both read it, so the same model gets the same effort from either.
inline std::string headEffortFor(const std::string& provider, const std::string& model) {
  return providers::tableCap(provider, model, providers::RoundKind::ToolRound).effort;
}

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
  // Lane E2-P-PHOTO: photo refs a provider refused in this chat (an image refusal); each renders as
  // tx::photoAbsentLine(Refused), never as pixels. Null: none.
  const std::vector<std::string>* refusedPhotos = nullptr;
  const LadderState* ladder = nullptr;                // the overflow ladder (null: no rung)
  // Lane E2-CACHE: the cache points on the stable prefix (the system blocks and the end of the
  // previous turn) carry the provider's 1-hour lifetime; the moving last block keeps 5 minutes.
  // False: every point has the 5-minute lifetime (what the request carried before).
  bool longCache = false;
  // Lane E2-R4a: the head's route (plain/route.h: model, path, auth, version, the table rows), set by the
  // exchange's pre-flight on every render; null: the wire's defaults (a direct Anthropic head as before).
  const Route* route = nullptr;
  uint32_t bodyCap = 0;                               // the body cap this request is held to (0 = not given)
  // Lane E2-REP5: the pre-flight's late bits (route.h PreflightState::lateDropped): the planner marks a
  // photo it sent as its line for the route's real-byte limit (dropped::kPhotos). Null: not recorded.
  uint16_t* lateDropped = nullptr;
};

// Why the model stopped. Context (lane E2-R4a): the window filled, so a larger output cap cannot
// help; the exchange climbs one rung instead of the cut retry.
enum class StopReason : uint8_t { EndTurn = 0, ToolUse = 1, MaxTokens = 2, Refusal = 3, Other = 4, Context = 5 };
inline const char* stopSlug(StopReason s) {
  switch (s) {
    case StopReason::EndTurn: return "end";
    case StopReason::ToolUse: return "tool";
    case StopReason::MaxTokens: return "cut";
    case StopReason::Refusal: return "refusal";
    case StopReason::Other: return "other";
    case StopReason::Context: return "context";
  }
  return "other";
}

// The replay tag of a request (lane E2-R4b, router design section 5 item 4): "<dialect slug>/<d or r>/<model>",
// d direct, r routed. The engine stamps it on every k item it writes (the item's `wr`); the planner sends a
// stored k item only to a request with the same tag. A k item stored without one reads as
// replayTag(AnthropicMessages, false, <the model its turn's t item names>).
inline std::string replayTag(Dialect d, bool routed, const std::string& model) {
  return std::string(dialectSlug(d)) + (routed ? "/r/" : "/d/") + model;
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
  // Lane E2-P-CONN2: a provider-side connector ran inside this answer (an Anthropic mcp_tool_result,
  // an OpenAI mcp_call): its result is outside content, so the turn is tainted.
  bool outside = false;
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
