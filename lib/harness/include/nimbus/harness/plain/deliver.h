#pragma once
#include <cstdint>
#include <functional>
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
  // What the request carried beside those (lane E2-P-SURF; the engine sets them, a test leaves the defaults).
  nimbus::orch::tx::BoundTools bound;
  bool longCache = false;
  const std::vector<std::string>* refusedPhotos = nullptr;
  nimbus::orch::tx::ArenaAllocFn alloc;     // the range's own block (PSRAM on the device);
  nimbus::orch::tx::ArenaFreeFn release;    // empty: malloc and free
};

// Render request `in.exchange` of a closed turn again into `out`. InvalidArgument for missing
// inputs or an exchange with no reference; NotFound when the system text or the tools no longer
// hash to the stored ones (the reference is stale), or `in.items` does not hold every item of
// the range; Torn when the render does not hash to what was sent (`out` is then not the
// request). Ok: `out` is the request as sent, byte for byte (headers rebuilt by the wire).
Status rebuildRequest(const RebuildInput& in, HttpRequest& out);

// ---- progress on a long turn (owner ruling 82 item 6) ------------------------------------
// A long turn says that it is still working and on what. The progress line is its own class
// of harness-written message, apart from the closed list of kAllStatusLines: it goes out
// while the turn runs, through the binding's ProgressDeps (never Channel::send, never
// deliverTurn), is edited in place where the channel can edit, is never stored as a
// transcript item (no a or d item; the d item holds the reply's ids only), never counts as
// the answer and never counts against "at most one device-written message after a turn".
// The test_delivery_bytes exemption table names it ("progress"). It never shortens a turn's
// work or its answer: it only reports.
//
// Timings (DECISION, ruling 82): the first line after kProgressFirstMs with no visible update
// (the turn's start counts as one); then, where the channel edits, the same message is edited
// at most every kProgressEditMs when what the turn does has changed; where it cannot edit, a
// new line at most every kProgressNewLineMs and at most kProgressMaxLines a turn. At the
// turn's end an editable line gets its final form; nothing new is sent then.
inline constexpr uint32_t kProgressFirstMs = 20000;
inline constexpr uint32_t kProgressEditMs = 10000;
inline constexpr uint32_t kProgressNewLineMs = 60000;
inline constexpr uint8_t kProgressMaxLines = 3;

// What the turn is doing when a progress event fires.
enum class ProgressKind : uint8_t {
  ToolRound = 0,      // a tool ran (detail: the tool's name)
  ProviderWait = 1,   // a request to the provider is about to leave, or a retry waits (detail: "")
  SoftGuard = 2,      // a soft guard was reached while the turn still progresses (detail: its slug)
  Summary = 3,        // the turn waits for the summary of the earlier conversation (lane RB-C3)
};

struct ProgressEvent {
  ProgressKind kind = ProgressKind::ToolRound;
  uint32_t nowMs = 0;        // the platform's monotonic clock
  std::string detail;
};

// How a binding sends and edits a progress line. `send` gives back the message id an `edit`
// needs; an empty `edit` (or a send that gives no id) means the channel cannot edit.
struct ProgressDeps {
  std::function<bool(const std::string& chatId, const std::string& text, int64_t& msgId)> send;
  std::function<bool(const std::string& chatId, int64_t msgId, const std::string& text)> edit;
};

class ProgressReporter {
 public:
  explicit ProgressReporter(ProgressDeps deps) : deps_(std::move(deps)) {}
  // A new turn: nothing shown yet; `startedMs` is its start (the first visible update).
  void open(const TurnCtx& turn, uint32_t startedMs);
  // One event of the open turn: sends or edits the line when due. True when something went out.
  // A send or edit that fails or throws std::bad_alloc is dropped: progress never ends a turn.
  bool tick(const ProgressEvent& e);
  // The turn ended at `nowMs`: an editable line gets its final form. Nothing new is sent.
  void close(uint32_t nowMs);
  uint16_t lines() const { return lines_; }   // messages sent this turn
  uint16_t edits() const { return edits_; }   // edits this turn
  bool isOpen() const { return open_; }
  uint32_t turn() const { return turn_; }
  // The owner's line for an event at `elapsedMs` into the turn, after `steps` tool rounds.
  static std::string text(const ProgressEvent& e, uint32_t elapsedMs, uint16_t steps);
  static std::string finalText(uint32_t elapsedMs, uint16_t steps);

 private:
  bool show(const std::string& line, uint32_t nowMs);
  ProgressDeps deps_;
  std::string chatId_;
  uint32_t turn_ = 0, startedMs_ = 0, lastVisibleMs_ = 0;
  int64_t msgId_ = 0;
  bool open_ = false, haveMsg_ = false;
  uint16_t lines_ = 0, edits_ = 0, steps_ = 0;
  std::string last_;
};

// Ticks `r` from the seams the engine already calls: afterResult (a tool round; a new turn
// number opens the turn) and afterDeliver (close), both in the Observer slot. The provider wait
// and a soft guard are the engine's to tick at its round boundary (see PR_BODY). Err::Busy
// when the Observer slot of either seam is taken.
Status installProgress(PlainHooks& hooks, ProgressReporter& r, std::function<uint32_t()> nowMs);

}  // namespace plain
}  // namespace agent
