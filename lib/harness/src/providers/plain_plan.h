#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/wire.h"

// plain_plan.h - the shared planner of engine 2's wires (router design C6, lane E2-R2b; moved out
// of plain_wire_anthropic.cpp). INTERNAL to lib/harness/src/providers/plain_wire_*.cpp.
//
// plan() decides, once for every codec, what one request carries: which stored items (after the
// newest digest's thru, rung 3's omitted turns left out), the pairing (a call only with its one
// result, a result only after its one call, each id once), the send order (a round's results
// before anything stored between a call and its result), whole or stubbed results and call
// arguments (the stub rule and the ladder), which photos go as pixels and which as their line,
// the stored reasoning, the omission line, and the four neutral cache marks. Lane E2-R4b: it reads the
// request's table row (RenderInput::route; none: the Anthropic Messages facts) for the replay tag and
// scope of stored reasoning, the call id rule, text-mode history on a row without tools, the order
// rules, and the route's real-byte limit (the oldest photos sent as their line). A codec
// only spells the Plan in its JSON and reads its own answers; it decides nothing about what is
// sent. The planner names no provider and reads or writes no JSON (the codec says whether a stored
// fragment can be sent as it is). Pure apart from the RenderInput hooks it calls (photo, bulk) and one device log
// line for an unreadable bulk file.

namespace agent {
namespace plain {
namespace plan {

// One block of the request, in send order.
enum class PieceKind : uint8_t {
  Omission,        // user text: the ladder's omission line (RenderInput::ladder), turn 0
  OwnerText,       // user text: the stamp (lines[line], kStamp) and a space, then the item's text
  Photo,           // user image: photos[photo]
  Line,            // user text: lines[line] (a photo's stub or absent line)
  AssistantText,   // assistant text: the item's text
  DeviceNote,      // user text: the item's text
  Meta,            // assistant: the item's text as stored (signed reasoning); never a cache mark
  Call,            // assistant call: id ids[id], the item's name, arguments by `src`
  Result,          // user result: id ids[id], content by `src`, photos[photo] after it (kViewed)
};

// Where a Call's arguments or a Result's text come from.
enum class Src : uint8_t {
  None,   // nothing (a Result with no content; a Call whose arguments are not one object: {})
  X,      // the item's x
  St,     // the item's stored stub (st)
  Args,   // the item's args
  Line,   // lines[line]
};

// Piece::flags.
inline constexpr uint8_t kOpens = 1;        // starts a message of its own (an owner message)
inline constexpr uint8_t kStamp = 2;        // OwnerText: lines[line] is its stamp
inline constexpr uint8_t kError = 4;        // Result: not ok (and has content)
inline constexpr uint8_t kViewed = 8;       // Result: carries photos[photo] after its text
inline constexpr uint8_t kMarkStable = 16;  // cache mark: the last block of the previous turn
inline constexpr uint8_t kMarkMoving = 32;  // cache mark: the moving last block

struct Piece {
  uint32_t item = 0;    // the arena index of its item (not for Omission)
  uint32_t turn = 0;
  uint32_t id = 0;      // Call, Result: index in Plan::ids
  uint32_t line = 0;    // Src::Line, kStamp: index in Plan::lines
  uint16_t photo = 0;   // Photo, kViewed: index in Plan::photos
  PieceKind kind = PieceKind::Line;
  Src src = Src::None;
  uint8_t flags = 0;
};

// A photo attached: by provider file id (no bytes; lines[text] is the id), else its bytes, spliced
// into the body at send time (lines[text] is the media type).
struct Photo {
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  uint32_t text = 0;
};

// The system blocks (core, notes, digests): which are sent and the two stable marks.
struct SystemPlan {
  bool shown[3] = {false, false, false};
  int stableAfter = -1;   // the block the first stable mark closes (notes, else core); -1: none
  bool digestsMark = false;   // the digests block carries the second stable mark
};

struct Plan {
  SystemPlan system;
  std::vector<Piece> pieces;
  std::vector<std::string> ids;      // call ids as sent (one to one with the stored ids)
  std::vector<std::string> lines;    // text the planner made or read (stamps, photo lines, bulk results,
                                     // photo file ids and media types)
  std::vector<Photo> photos;         // photos attached, in send order
  size_t estimate = 0;               // body bytes the rendered items take, about (the reservation)
  bool anyBlock = false;             // a piece that is a message block (a request needs one)
};

// The codec's test that a stored fragment (signed reasoning, call arguments) can be sent as it is;
// one that cannot is left out (reasoning) or sent empty (arguments).
using FragmentOk = bool (*)(const std::string& stored);

// The plan of one request. InvalidArgument: a RenderInput without turn, system or items.
Status plan(const RenderInput& in, FragmentOk sendable, Plan& out);

// Lane E2-P-CONN2: what rides a request beside its function tools, on the provider's own side: the
// owner's connectors and the code sandbox (HeadEndpoint::connectors), decided by engine 1's attach
// builders (nimbus/orch/connectors_wire.h attachOpenAIWire, attachAnthropicWire) with engine 1's
// grant: the owner's connectors only on the owner's own turns (connector_grant.h connectorGrantFor
// the turn's person, never looser than the calling task's grant), the built-ins only for anyone else,
// nothing over the Cumulo router (engine 1's router route: built-ins only, no sandbox) and nothing on
// a request that offers no tools (the summarizer). Approval: none asked, as engine 1 (OpenAI
// require_approval "never"; an Anthropic toolset runs without a prompt). Per dialect:
//   Responses (an openai head)    tools[]: {"type":"mcp",...} and {"type":"code_interpreter",...}
//   Messages (an anthropic head)  mcp_servers[] ({"type":"url","url","name","authorization_token"?})
//                                 and one {"type":"mcp_toolset","mcp_server_name"} tool per server
//   Chat (Mistral and the rest)   nothing: chat/completions has no provider-side tool (engine 1 sent
//                                 its connectors only on the Conversations API, which engine 2 does
//                                 not speak; its chat-completions tool loop sent none either)
// The one piece of the planner that writes JSON: each entry as one JSON object, which the codec
// writes as it is. A custom endpoint gets nothing (engine 1 attached by provider).
struct ProviderTools {
  std::vector<std::string> tools;     // entries for the request's tools[]
  std::vector<std::string> servers;   // entries for the Messages mcp_servers[]
  size_t bytes() const {
    size_t n = 0;
    for (const std::string& t : tools) n += t.size() + 1;
    for (const std::string& t : servers) n += t.size() + 1;
    return n;
  }
};
void providerTools(const RenderInput& in, ProviderTools& out);

}  // namespace plan
}  // namespace plain
}  // namespace agent
