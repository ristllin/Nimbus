#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/rules.h - the pure rules of what a request shows (PRD section 2, "The stub rule"):
// the one monotonic stub pointer (moved only on the overflow path), the stub texts, superseded reads and day
// views, photo visibility, the overflow ladder and the omission line. Pure functions of
// stored facts: no clock, no I/O, no global state; the same facts always give the same
// bytes, so a restart renders the same request.
//
// What the rules never do: drop an item without a named line, or rewrite a stored result.
// A stub is computed once at capture (stubText), stored on the r line (Item::st) and only
// ever rendered from there. Receipts (tier 1) stay verbatim at or below the pointer; only
// tier-2 bulk and outside results render as stubs there (Ruling 16).
//
// Seam declared by lane RB-B0a; the bodies (orch_tx_rules.cpp) are lane RB-T3's. Every
// model-facing template of these rules lives in orch_tx_rules.cpp (class "stub templates"
// of the model-text catalog, PRD section 4).
namespace nimbus {
namespace orch {
namespace tx {

// Lane E2-FIX5: the tools a model-facing text may name. A stub, an omission line or an import note
// names `view` or `search` only when that tool is registered for the turn (the engine reads its
// registry); without it the text says what is not shown and names no call. All false: nothing named.
struct BoundTools {
  bool search = false;
  bool view = false;
};

inline constexpr uint32_t kColdGapS = 270;            // over this since the last request: cold (the files list)
inline constexpr size_t kStubMaxBytes = 400;
inline constexpr size_t kStubPreviewBytes = 200;
inline constexpr size_t kStubArgsPreviewBytes = 120;
inline constexpr size_t kStubToolMaxBytes = 64;       // a longer tool name is cut (UTF-8 safe)
inline constexpr size_t kStubPathMaxBytes = 96;       // the store's paths are at most 73 B
inline constexpr size_t kStubWayBackMaxBytes = 160;   // the call a stub names to bring its bytes back
inline constexpr uint32_t kNewestOwnerTurnsKept = 6;  // never omitted by the ladder
// Photos in view (lane E2-FIX4, tx::visiblePhotos): caps that protect the device, never a clock.
// Inline (every engine-2 head on the device today): each request carries the photos' base64 (4/3 of
// the stored bytes) and the device reads each from the card once per turn into PSRAM. Measured on
// unit F1 (Haiku, Telegram-sized 1280x960 JPEGs; lanes/E2-FIX4/PR_BODY.md), added to a request
// over the same conversation without photos: four of 60 KB 1.0 s, four of 120 KB 2.4 s, three of
// 200 KB 3.3 s, four of 200 KB 4.2 s; PSRAM about 1 MB of 5.5 MB free at most; the internal heap
// did not move. So: four photos and 600,000 stored bytes, about 3 s at most. FileRef (a provider file
// id per photo, about 100 B a request): no engine-2 head uses it yet (no uploader is bound); its
// caps equal the inline ones until one is measured.
inline constexpr size_t kInlinePhotosInViewMax = 4;
inline constexpr size_t kInlinePhotoBytesInViewMax = 600000;
inline constexpr size_t kFileRefPhotosInViewMax = 4;
inline constexpr size_t kFileRefPhotoBytesInViewMax = 600000;
inline constexpr size_t kArrivalPhotosMax = 4;        // the arrival turn (ladder rung 2's photo bound)
// What one photo counts for against a head's body cap (plain_exchange.cpp capLength): the text its
// tokens would be. Anthropic bills a photo at about (width x height) / 750 tokens, at most about
// 1,600 for the sizes it keeps (1.15 megapixels); at the wire's dense ratio of about 3.6 B a token
// that is about 5,800 B.
inline constexpr size_t kPhotoCapBytes = 6000;
inline constexpr size_t kOmissionDatesMaxBytes = 64;

// Why the pointer moved (x.why). Slugs frozen: stored and counted as change points.
// ToolFold (lane RB-H81): compaction's tool fold, whose sup lists the results of the oldest closed
// turns that render as their stubs from then on (owner and assistant text stays verbatim).
enum class MoveWhy : uint8_t { ColdOpen = 0, WarmBulk = 1, Outside = 2, Superseded = 3, ToolFold = 4 };
inline const char* moveWhySlug(MoveWhy w) {
  switch (w) {
    case MoveWhy::ColdOpen: return "cold";
    case MoveWhy::WarmBulk: return "bulk";
    case MoveWhy::Outside: return "outside";
    case MoveWhy::Superseded: return "superseded";
    case MoveWhy::ToolFold: return "tools";
  }
  return "cold";
}

// ---- the profile a chat stores (lane RB-H81, ruling 91 blockers/F9 and T4-ADV-04) ---------------
// head.profile and the t item's profile carry a pin marker: a smaller profile is the owner's choice
// only when he pinned it (TurnCtx::profile is his pinned choice or the default, full), and a slug
// with no marker, such as the "standard" engine 2 wrote by default before RB-T4, is no choice at
// all: it reads as full. Compaction, the boot settle and the migration read the stored slug through
// profileOfHead, the same way the turn sets it through headProfileSlug.
inline constexpr char kPinnedProfilePrefix[] = "pin:";
inline std::string headProfileSlug(Profile p) {
  return p == kDefaultProfile ? std::string(profileSlug(p)) : std::string(kPinnedProfilePrefix) + profileSlug(p);
}
inline Profile profileOfHead(const std::string& stored) {
  const size_t n = sizeof(kPinnedProfilePrefix) - 1;
  return stored.compare(0, n, kPinnedProfilePrefix) == 0 ? profileOf(stored.substr(n)) : kDefaultProfile;
}

// ---- the pointer ---------------------------------------------------------------------
// No default move (lane E2-VERB, rulings 81, 82 and 106): under the due mark a request replays
// every earlier call and result whole, as a simple loop does. Stubs come only from the overflow
// path: compaction (its slice, then its tool fold) and the ladder (rung 1 raises the pointer for one
// request, rung 2 stubs old bulk of the open turn). The cold and warm opens of the PRD's first
// draft (rules 1 and 2) moved the pointer to save money and are gone; a pointer an earlier build
// stored is still honoured (nothing stored is rewritten).

// Whether an x item's sup list makes its items render as stubs: the tool fold's lists do
// (compaction, the overflow path); a superseded-read list (rule 4) does not: a simple loop replays
// a stale read too, so the read renders whole until the overflow path stubs it (E2-VERB).
inline bool stubList(const Item& x) { return x.kind == ItemKind::PointerMove && x.why != moveWhySlug(MoveWhy::Superseded); }

// ---- stub texts (computed once at capture, stored on the r line) -----------------------

struct StubFacts {
  std::string tool;
  uint64_t q = 0;
  uint32_t bytes = 0;
  uint32_t lines = 0;
  std::string hhmm;        // local time of the result, "HH:MM" (stubTime); empty = clock not set
  std::string preview;     // the result's first bytes (internal results only; outside: ignored)
  std::string path;        // file tools: path=
  std::string sha8;        // file tools: sha8= (8 hex)
  std::string args;        // outside results: the call's arguments
  ResultClass cls = ResultClass::Receipt;
  bool noCard = false;     // no storage card: the full result is not kept, no view hint
  std::string again;       // the call that reads it again (tx::wayBack; E2-VERB), "" = the view hint
  BoundTools bound;        // E2-FIX5: the view hint only when view is bound
};

// The stub text, one line of valid UTF-8, at most kStubMaxBytes:
//  internal (Receipt, Bulk), lane E2-FIX9 (the words in tx/result_text.h and tx/photo_text.h):
//   '[stub <tool> r<q> <bytes>B <lines>L <HH:MM>[ path=<p> sha8=<s>]] (shown in full earlier in this conversation;
//   not attached to this request; view id=r<q> shows it again)', no preview. The way back: view when bound
//   (E2-FIX5's BoundTools), else `again` (a file read's read_file call, lane E2-VERB: '; <again> shows it again'),
//   else none.
//  outside: '[stub <tool>(<args, 120 B>) r<q> <bytes>B outside content, not shown: view id=r<q>]'
//   built from the tool, the call's arguments, q and the size: never a byte of the result; view not
//   bound: 'outside content, not shown]' (or names `again` when there is one).
//  noCard: '<preview> ... (shown earlier in this conversation; not attached to this request; full result not kept:
//   no storage card)' (PRD section 2): the preview is the first kStubPreviewBytes of the result on one line (control
//   bytes as spaces, invalid UTF-8 as '?', cut on a character), shortened further only to fit 400 B.
// Every field is one line: control bytes, C1 controls, U+2028 and U+2029 become spaces, bidi
// controls and invalid bytes '?', a sequence cut at a field's end is dropped. The tool name
// keeps [A-Za-z0-9_.-] (others '?', at most kStubToolMaxBytes); in the path and the outside
// arguments '[' and ']' become '(' and ')', so a model-chosen field cannot close the stub or
// open a '[device]' text inside it. The preview of an internal result is kept as it reads.
// No clock: <HH:MM> reads '--:--'. InvalidArgument for an empty tool or q 0.
Status stubText(const StubFacts& facts, std::string& out);
// A long call argument: '[stub <bytes>B sha:<sha8>: an argument sent earlier, not content; view
// id=r<q>]', or with `again` (a file write's tx::wayBack, lane E2-VERB) '...; read_file path=<p>
// shows the file]': plain words for what it stood for, so a model reading its own earlier call
// never takes the line for the text it sent. InvalidArgument for q 0 or a sha8 that is not 8 hex.
// View not bound and no `again` (E2-FIX5): '...; not content]'.
Status longArgStub(uint32_t bytes, const std::string& sha8, uint64_t q, std::string& out,
                   const std::string& again = std::string(), const BoundTools& bound = BoundTools());
// The call that brings a stubbed item's bytes back with the file tools (lane E2-VERB), from the tool
// and its arguments JSON: read_file 'read_file path=<p>[ offset=<n>][ limit=<n>][ column=..]...'
// (the same range); write_file, append_file, edit_file 'read_file path=<p>'; any other tool, or
// arguments with no path, "". Pure.
std::string wayBack(const std::string& tool, const std::string& args);
// Whether a text is a stub line and nothing else (lane E2-VERB): one line, at most kStubMaxBytes once
// blanks around it are left out, opening with '[stub ' and closing with ']' or ')'. The write tools
// refuse such a text as content (it is never the text a model meant to store).
bool looksLikeStub(const std::string& text);
// Long text kept inline (owner rulings 81 to 83, PRD section 2 tier 1 as amended; lane RB-H81): an
// assistant text or a saved owner paste over the inline limit keeps its first `cap` bytes (the
// head's v5.1.2 message cap at least: tx::HeadBudget::messageCap), cut on a character, then on its
// own line '[text continues: <rest>B more: <where>]'. `where` is the pointer to the saved whole:
// 'view id=r<q>' for a text in the bulk file, 'read_file path=<path>' for a saved paste; empty, the
// whole was not kept: 'not kept: no storage card'. Text at or under `cap` is returned whole.
// InvalidArgument for a cap of 0. E2-FIX5: `kept` with an empty `where` (the whole is kept but no
// bound tool reads it): '[text continues: <rest>B more]'.
Status longTextHead(const std::string& text, size_t cap, const std::string& where, std::string& out);
Status longTextHead(const std::string& text, size_t cap, const std::string& where, std::string& out, bool kept);
// The pointers longTextHead names (viewPointer: "" when view is not bound, E2-FIX5).
inline std::string viewPointer(uint64_t q, const BoundTools& b) { return b.view ? "view id=r" + std::to_string(q) : ""; }
inline std::string pathPointer(const std::string& path) { return "read_file path=" + path; }

// A photo not attached to this request (lane E2-FIX4; the words are tx/photo_text.h):
// '[photo p<ref8> <when>: shown earlier in this conversation; not attached to this request]'.
// ref8 is the photo ref without its 'p' (1 to 16 letters or digits); when is photoWhen's text
// (empty when the clock was not set: the field is left out). `viewBound`: the engine has bound the
// view tool (E2-SRCH; the caller reads it from the tool registry), and only then the line ends
// '; view id=p<ref8> shows it again]'. No line names a tool the engine has not bound.
// InvalidArgument for a bad ref.
Status photoStub(const std::string& ref8, const std::string& when, std::string& out, bool viewBound = false);
// The same with the photo's note (tx::Part::note, lane RB-H81, ruling 84 item 5): '[photo
// p<ref8> <when>: <note>; shown earlier ...]', the note on one line, at most kPhotoNoteMax, '['
// and ']' as '(' and ')' so it cannot close the line. An empty note gives the line above.
Status photoStub(const std::string& ref8, const std::string& when, const std::string& note, std::string& out,
                 bool viewBound = false);

// Ruling 11 (the year in every stamp) for the rules' time texts, from stored epochs:
// An epoch from before the clock was set (under day_start.h kClockValidEpoch, 0 included)
// is no date: "" and ok, never an invented 1970 one.
//  stubTime     -> "13:02"                  (StubFacts::hhmm; no clock -> "" and ok)
//  photoWhen    -> "Tue 17 Nov 2026 13:02"  (photoStub's when; no clock -> "" and ok)
//  omissionDates-> "17 Nov 2026", "15-16 Nov 2026", "30 Nov-2 Dec 2026",
//                  "31 Dec 2026-1 Jan 2027"; a first with no clock (stored before the
//                  clock was set) reads as last, both give "" and ok; InvalidArgument for
//                  last before first
Status stubTime(uint64_t epoch, int16_t tzMinutes, std::string& out);
Status photoWhen(uint64_t epoch, int16_t tzMinutes, std::string& out);
Status omissionDates(uint64_t firstEpoch, uint64_t lastEpoch, int16_t tzMinutes, std::string& out);

// ---- superseded reads and day views -------------------------------------------------------

// Rule (4): the result q's a successful write supersedes, sorted and unique. Only results
// stored before the write (q below writeCall.q) are read: a read after the write is fresh.
// The caller passes only a write whose result was ok (the second form checks it), and stores
// the answer as the sup list of an x item (why "superseded") at the close of the writing turn: a
// change point it logs, not a stub list (stubList; lane E2-VERB): the stale read renders whole.
//  - write_file, append_file, edit_file, delete_file, move_file (args path, from, to): every
//    earlier read_file, read_pdf or file_stat result of the same path (exact string).
//  - any other tool: nothing. No tool is special for what it stores (ruling 41).
// Results already superseded by an x item in `earlier`, and tier-2 results already at or below
// the pointer there (already stubs), are left out, so one stale read costs one change point.
// Arguments that do not parse (a long-argument stub) supersede nothing. InvalidArgument when
// writeCall is not a c item.
Status supersededBy(const Item& writeCall, const std::vector<Item>& earlier, std::vector<uint64_t>& out);
// The same, also reading the write's own result: a failed write (ok false) supersedes
// nothing. InvalidArgument when writeResult is not an r item.
Status supersededBy(const Item& writeCall, const Item& writeResult, const std::vector<Item>& earlier,
                    std::vector<uint64_t>& out);

// ---- whole or stub ---------------------------------------------------------------------------

enum class ShowAs : uint8_t { Whole = 0, Stub = 1 };
struct ShowFacts {
  uint64_t pointer = 0;                   // the stub pointer (newest x item's stub)
  uint32_t openTurn = 0;                  // the turn being answered (0 = none open)
  const std::vector<uint64_t>* superseded = nullptr;   // the stub lists (stubList x items' sup), sorted
                                                       // (may be null)
  uint64_t ladderStubThrough = 0;         // ladder rung 2: open-turn tier-2 results at or below
                                          // this q (older than the newest two rounds) are stubs
};
// How a stored r item renders: Stub when superseded; a receipt otherwise Whole (Ruling 16);
// a tier-2 result at or below the pointer, an outside result outside its own open turn, or
// open-turn tier-2 at or below ladderStubThrough: Stub. InvalidArgument for a non-r item or
// an unsorted superseded list.
Status showResult(const Item& result, const ShowFacts& facts, ShowAs& out);

// A call's arguments as a request shows them (lane E2-VERB, rulings 81, 82 and 106): the model's own
// arguments, whole, in every turn, as a simple loop replays them (the c item keeps them in args,
// bounded like tier-2 bulk). Its stub form (st: each long string as its long-argument stub) only on
// the overflow path: the call at or below the pointer (ladder rung 1 raises it to the last closed
// turn), an open-turn call at or below rung 2's line, or a call a compaction tool fold listed (its q
// in `f.superseded`, the stub lists). A call with no stub form shows its args. Pure.
inline const std::string& shownArgs(const Item& call, const ShowFacts& f) {
  const bool folded = f.superseded && std::binary_search(f.superseded->begin(), f.superseded->end(), call.q);
  const bool stub = folded || call.q <= f.pointer || (call.turn == f.openTurn && call.q <= f.ladderStubThrough);
  return stub && !call.st.empty() ? call.st : call.args;
}

// ---- photos ------------------------------------------------------------------------------------

// How photos reach a head. Floor heads (85,000 B: routed, custom, unqualified) are Inline.
enum class PhotoMode : uint8_t { FileRef = 0, Inline = 1, None = 2 };
struct PhotoFacts {
  PhotoMode mode = PhotoMode::Inline;
  bool noCard = false;                   // no storage card: an earlier photo cannot be read back
  std::vector<std::string> candidates;   // refs of the photos of the conversation the request shows
                                         // (after compaction's pointer), newest first
  std::vector<uint32_t> ownerTurnsSince; // per candidate: 0 = this turn's own; any other = an earlier message
  std::vector<uint32_t> bytes;           // per candidate stored size; empty = unknown (counts 0)
};
inline size_t photosInViewMax(PhotoMode m) {
  return m == PhotoMode::FileRef ? kFileRefPhotosInViewMax : m == PhotoMode::Inline ? kInlinePhotosInViewMax : 0;
}
inline size_t photoBytesInViewMax(PhotoMode m) {
  return m == PhotoMode::FileRef ? kFileRefPhotoBytesInViewMax : m == PhotoMode::Inline ? kInlinePhotoBytesInViewMax : 0;
}
// The photo refs this request shows as pixels, newest first; the rest render as photoStub (lane
// E2-FIX4). A photo the head was shown stays shown on every later request, as the rest of the
// conversation does, until compaction's pointer passes its turn (the caller offers only the photos
// after it) or the caps push it out: the arrival turn's own photos first (at most kArrivalPhotosMax),
// then the earlier ones newest first while the count stays within photosInViewMax and the stored
// bytes within photoBytesInViewMax; the first earlier photo that does not fit ends the carry, so
// the oldest falls out first and an older photo never takes a newer one's place. No clock, no
// restart, no idle time takes part. None shows nothing; with no card only the arrival turn's own.
// InvalidArgument when the per-candidate lists do not match the candidates.
Status visiblePhotos(const PhotoFacts& facts, std::vector<std::string>& out);

// ---- the overflow ladder -------------------------------------------------------------------------

enum class LadderRung : uint8_t {
  None = 0,
  StubClosedTurns = 1,   // (1) closed turns' bulk as stubs, oldest first, as far as the cap needs (E2-FIX9)
  StubOldBulk = 2,       // (2) this turn's bulk older than the newest two rounds; photos past 4
  OmitOldTurns = 3,      // (3) one omission line, never the newest 6 owner turns
  SizeGuard = 4,         // (4) the size guard trips
};
struct LadderFacts {
  size_t bodyBytes = 0;
  size_t bodyCap = 0;
  LadderRung applied = LadderRung::None;   // rungs already applied to this request
};
// None when the body fits the cap; otherwise the rung after `applied` (SizeGuard stays
// SizeGuard). One rung at a time: the caller re-renders and asks again. InvalidArgument for
// a cap of 0.
Status nextRung(const LadderFacts& facts, LadderRung& out);

// One whole turn after thru, oldest first, as rung 3 sees it.
struct TurnSpan {
  uint32_t turn = 0;
  bool owner = false;       // an owner turn (counts toward the newest 6 kept)
  size_t bytes = 0;         // what the turn adds to the body as rendered now
  uint64_t firstAt = 0;     // epoch of its first item (for omissionDates)
};
// Rung 3: how many of the oldest turns to omit (a prefix of `turns`) so at least needBytes
// are freed, never reaching the newest kNewestOwnerTurnsKept owner turns or anything after
// the oldest of them. When even the whole omissible prefix frees too little it is all
// omitted and the caller climbs to the size guard. The omission line's own bytes (under 120 B)
// are the caller's: it re-renders and asks nextRung again. InvalidArgument for turns out of order.
Status omissionSpan(const std::vector<TurnSpan>& turns, size_t needBytes, uint32_t& omitCount);
// '[N earlier turns, <dates>, not shown: search finds them]' ("turns" for every N: the
// battery's parsers read that literal); with no dates (clock never set) '[N earlier turns,
// not shown: search finds them]'. '[' and ']' in dates become '(' and ')'. InvalidArgument
// for 0 turns. Search not bound (E2-FIX5): the line ends ', not shown]'.
Status omissionLine(uint32_t turns, const std::string& dates, std::string& out, const BoundTools& bound = BoundTools());

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
