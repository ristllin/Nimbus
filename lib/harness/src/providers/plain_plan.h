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
// the open turn's stored reasoning, the omission line, and the four neutral cache marks. A codec
// only spells the Plan in its JSON and reads its own answers; it decides nothing about what is
// sent. The planner names no provider and writes no JSON (it checks only that a stored fragment is
// one JSON object). Pure apart from the RenderInput hooks it calls (photo, bulk) and one device log
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
  Call,            // assistant call: id ids[id], name wireName(item name), arguments by `src`
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
  std::vector<std::string> lines;    // text the planner made or read (stamps, photo lines, bulk results)
  std::vector<PhotoBytes> photos;    // photos attached, in send order
  size_t estimate = 0;               // body bytes the rendered items take, about (the reservation)
  bool anyBlock = false;             // a piece that is a message block (a request needs one)
};

// The plan of one request. InvalidArgument: a RenderInput without turn, system or items.
Status plan(const RenderInput& in, Plan& out);

// A stored tool name as a wire takes it (^[a-zA-Z0-9_-]{1,64}$): each other byte as '_'.
std::string wireName(const std::string& n);

// One JSON object the body writer accepts as it is (call arguments, signed reasoning).
bool oneObject(const std::string& json);

}  // namespace plan
}  // namespace plain
}  // namespace agent
