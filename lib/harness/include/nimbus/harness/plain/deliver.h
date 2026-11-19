#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"

namespace agent {
struct Channel;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxLog;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/deliver.h - the delivery seam (PRD section 1, step 6, and section 8.9): the model's
// text goes through one portable deliver function (markup sanitiser, Telegram HTML,
// splitting) used by the device and nimbusd; then the d and e items. A harness-written
// message is one of a closed list, at most one after a turn, each separate from the reply.
//
// Seam declared by lane RB-B0a; the bodies (plain_deliver.cpp) are lane RB-E3's.
namespace agent {
namespace plain {

struct ExchangeResult;   // plain/exchange.h

// StatusLine, the closed list of harness-written messages, lives in plain/turn.h (the
// beforeDeliver seam carries one too).

struct DeliverDeps {
  const Channel* channel = nullptr;
  const PlainHooks* hooks = nullptr;
  nimbus::orch::tx::TxLog* log = nullptr;
};

// What one delivery sends. Nothing is copied: the reply and the status line are the
// engine's own strings, read for the length of the call (a 60 KB reply is never held twice
// at the point memory is tightest). A retry after a refused allocation passes `fromPart`,
// the reply parts that already went out: deliverTurn skips them, so nothing is sent twice.
struct DeliverInput {
  const TurnCtx* turn = nullptr;
  const std::string* text = nullptr;   // the model's text, exactly as returned; null or empty: none
  uint16_t fromPart = 0;               // reply parts already sent (skip them)
  StatusLine status = StatusLine::None;
  const std::string* statusText = nullptr;   // the one status line, separate from the reply
};

// Progress, recorded the moment each send succeeds (not when the call returns): when a
// later part throws, the engine reads what already went out and never sends it again.
// `out` carries the progress of every earlier attempt of this turn's delivery: deliverTurn never
// resets it (no `out = DeliverOutcome{}`); it adds to msgIds, parts and bytes and only sets the
// flags, so a retry with fromPart keeps part 1's message id and its sent state (ruling 79).
struct DeliverOutcome {
  bool sent = false;                   // some of the reply went out
  uint16_t parts = 0;                  // reply parts that went out, in order
  bool replyDone = false;              // every part of the reply went out
  bool statusSent = false;             // the status line went out
  std::vector<int64_t> msgIds;
  uint32_t bytes = 0;                  // the delivered bytes (after markup and splitting)
};

// One delivery (lane RB-E3). The reply goes out as nimbus::orch::telegramParts(*in.text) makes
// it (lib/core/src/orch_deliver_text.cpp: the markup sanitiser, then the split): the same
// function the device's Telegram send and nimbusd call, so a host test sees the device's bytes.
// Each part is one Channel::send, in order, from part `in.fromPart` on; nothing else is added
// to or removed from the model's text. Then the status line, as its own message, only when
// `in.status` is one of kAllStatusLines (the closed list of harness-written messages) and the
// line is not blank (it is the harness's own text: not sanitised): a line under any other name is never sent (InvalidArgument, the reply
// still goes out). A line longer than one part goes out as its first part, never as two
// messages. A part the channel refuses (send returns false) ends the reply there (the parts
// after it would arrive out of order); the status line still goes; Io is returned.
// The channel receives each part already made deliverable: a binding sends it as
// nimbus::orch::telegramPart(part) (Telegram HTML, plain resend), never through telegramParts
// again. Channel gives no message ids: a binding that has them delivers through its own
// EngineDeps::deliver. deps.hooks and deps.log are not read (the engine runs the seams).
// InvalidArgument without a channel or a turn.
Status deliverTurn(const DeliverDeps& deps, const DeliverInput& in, DeliverOutcome& out);

// The e item of a turn (lane RB-E3): `out` (a TurnEnd item with its q, turn and time set by the
// caller) gets the facts, bounded so the line always encodes, measured at the widest q and time
// the store may give it: at most kMaxListItems events ("events:cut" names a cut) and exchanges
// (the newest kept, "exchanges:cut"); when the line would still pass kMaxLineBytes, the oldest
// request references are shed first ("dossier:shed"), then the oldest exchanges, then events.
// An e item that cannot be stored leaves the turn open, so it never fails for its size.
// InvalidArgument when `out` is not a TurnEnd item, or the turn has no number.
Status turnEndItem(const TurnCtx& turn, const nimbus::orch::tx::TurnEndFacts& facts, nimbus::orch::tx::Item& out);

// A full arena with nothing closed left to drop (ruling 60) sheds the e item's request
// references before the e item is refused: the oldest exchange's first (its three figures
// stay), and the event "dossier:shed" says so. False when `e` is not a TurnEnd item or holds
// no reference. The references are the dossier's, never the conversation's: they never cost
// a resident turn.
bool shedReference(nimbus::orch::tx::Item& e);

// ---- the dossier by reference (PRD section 7, "/api/lastturn rebuilds the exact request by
// re-rendering"; DIAGNOSIS DR-22) ------------------------------------------------------------
// The e item keeps each request as a reference, not a copy: the turn's prompt and manifest
// hashes, and per exchange (tx::ExchangeFacts) the item range it rendered, the render state
// (stub pointer, ladder, output cap, tool choice, effort) and a hash of what was sent.
// rebuildRequest renders it again through the same wire and proves it byte for byte.
//
// Owner ruling 81: the reference never describes less than the request carried. The range is
// taken from the items the engine rendered (every resident item, oldest to newest), never
// chosen; a rebuild renders exactly that range and refuses, rather than renders less, when
// the items it is given miss any of it.

// fnv1a-32 of the three system blocks and of the tools, as the engine's e item stores them.
uint32_t promptHashOf(const SystemBlocks& system);
uint32_t manifestHashOf(const std::vector<ToolSpec>& tools);
// fnv1a-32 of a request's path, body and spliced bytes in order (never a header: the key rides
// one). Never 0 (0 is "no reference").
uint32_t requestHashOf(const HttpRequest& req);

// The reference fields of `out` from the request `in` rendered and `sent` (the exchange calls
// this for each request it sends; the last one stands). requestBytes, ms and httpStatus are
// left to the caller.
void recordRequest(const RenderInput& in, const HttpRequest& sent, nimbus::orch::tx::ExchangeFacts& out);
// The e item's facts of one exchange: its last request's reference (ExchangeResult::ref) with
// the request's size, the exchange's time and the last answer's status.
nimbus::orch::tx::ExchangeFacts exchangeFacts(const ExchangeResult& ex);

struct RebuildInput {
  const nimbus::orch::tx::TurnEndFacts* end = nullptr;   // the turn's e item facts
  size_t exchange = 0;                                   // which request (0 = the first)
  const TurnCtx* turn = nullptr;            // the turn (number, model, day start) as it ran
  const HeadEndpoint* head = nullptr;       // its head; the effort comes from the reference
  const WireCodec* wire = nullptr;          // the wire it was rendered by
  const SystemBlocks* system = nullptr;     // must hash to end->promptHash
  const std::vector<ToolSpec>* tools = nullptr;   // must hash to end->manifestHash
  const nimbus::orch::tx::TxArena* items = nullptr;   // holds the range (the live arena, or one
                                                      // filled from the card); more is fine
  BulkReadFn bulk;                          // the bulk payloads, by handle
  PhotoFn photo;                            // the visible photos, by ref
  nimbus::orch::tx::ArenaAllocFn alloc;     // the range's own block (PSRAM on the device);
  nimbus::orch::tx::ArenaFreeFn release;    // empty: malloc and free
};

// Render request `in.exchange` of a closed turn again into `out`. InvalidArgument for missing
// inputs or an exchange with no reference; NotFound when the system text or the tools no longer
// hash to the stored ones (the reference is stale), or `in.items` does not hold every item of
// the range; Torn when the render does not hash to what was sent (`out` is then not the
// request). Ok: `out` is the request as sent, byte for byte (headers rebuilt by the wire).
Status rebuildRequest(const RebuildInput& in, HttpRequest& out);

}  // namespace plain
}  // namespace agent
