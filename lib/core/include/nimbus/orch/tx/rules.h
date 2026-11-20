#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/rules.h - the pure rules of what a request shows (PRD section 2, "The stub rule"):
// the one monotonic stub pointer and its moves, the stub texts, superseded reads and day
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

inline constexpr uint32_t kColdGapS = 270;            // over this since the last request: cold open
inline constexpr uint32_t kWarmBulkBytes = 24576;     // whole internal bulk above the pointer
inline constexpr uint32_t kWarmBulkBytesFloor = 8192; // the same on 85,000 B heads
inline constexpr size_t kStubMaxBytes = 400;
inline constexpr size_t kStubPreviewBytes = 200;
inline constexpr size_t kStubArgsPreviewBytes = 120;
inline constexpr size_t kStubToolMaxBytes = 64;       // a longer tool name is cut (UTF-8 safe)
inline constexpr size_t kStubPathMaxBytes = 96;       // the store's paths are at most 73 B
inline constexpr uint32_t kNewestOwnerTurnsKept = 6;  // never omitted by the ladder
inline constexpr uint32_t kPhotoIdleS = 3600;         // file-reference photos leave view after this
inline constexpr uint32_t kPhotoOwnerTurns = 2;       // photos stay for the next two owner turns
inline constexpr size_t kFileRefPhotosMax = 4;        // file-reference heads: at most 4 in view
inline constexpr size_t kInlinePhotosMax = 2;         // inline heads: newest 2 carried one turn
inline constexpr size_t kInlinePhotoB64Max = 200000;  // inline heads: carried base64 at most this
inline constexpr size_t kArrivalPhotosMax = 4;        // the arrival turn (ladder rung 2's photo bound)
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

// The facts the turn-open decision reads.
struct OpenFacts {
  uint64_t now = 0;              // epoch seconds; 0 = clock not set (reads as cold)
  uint64_t lastReqAt = 0;        // HeadState::lastReqAt (0 = never: cold)
  bool afterRestart = false;
  uint64_t pointer = 0;          // the current stub pointer
  uint64_t lastClosedTurnQ = 0;  // the q of the last item of the last closed turn
  uint32_t wholeBulkAbove = 0;   // bytes of internal bulk above the pointer shown whole
  bool floorHead = false;        // an 85,000 B head (lean profile)
};

struct PointerMove {
  bool move = false;
  uint64_t to = 0;
  MoveWhy why = MoveWhy::ColdOpen;
};

// Rules (1) and (2), run once at turn open; the move is stored as an x item before the
// first request is rendered. Cold (over kColdGapS since the last request, clock not set,
// a clock behind the last request, never requested, or after a restart): move to the last
// closed turn. Warm: move there only when whole internal bulk above the pointer exceeds
// kWarmBulkBytes (kWarmBulkBytesFloor on floor heads). Never backwards and never nowhere:
// when lastClosedTurnQ is not above the pointer there is no move. Always ok.
Status decideTurnOpen(const OpenFacts& facts, PointerMove& out);

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
};

// The stub text, one line of valid UTF-8, at most kStubMaxBytes:
//  internal (Receipt, Bulk):
//   '[stub <tool> r<q> <bytes>B <lines>L <HH:MM>[ path=<p> sha8=<s>]] <preview> ... (full: view id=r<q>)'
//   the preview is the first kStubPreviewBytes of the result on one line (control bytes as
//   spaces, invalid UTF-8 as '?', cut on a character), shortened further only to fit 400 B.
//  outside: '[stub <tool>(<args, 120 B>) r<q> <bytes>B outside content, not shown: view id=r<q>]'
//   built from the tool, the call's arguments, q and the size: never a byte of the result.
//  noCard: the view hint becomes 'full result not kept: no storage card' (PRD section 2).
// Every field is one line: control bytes, C1 controls, U+2028 and U+2029 become spaces, bidi
// controls and invalid bytes '?', a sequence cut at a field's end is dropped. The tool name
// keeps [A-Za-z0-9_.-] (others '?', at most kStubToolMaxBytes); in the path and the outside
// arguments '[' and ']' become '(' and ')', so a model-chosen field cannot close the stub or
// open a '[device]' text inside it. The preview of an internal result is kept as it reads.
// No clock: <HH:MM> reads '--:--'. InvalidArgument for an empty tool or q 0.
Status stubText(const StubFacts& facts, std::string& out);
// A long call argument: '[stub <bytes>B sha:<sha8>: view id=r<q>]'. InvalidArgument for q 0
// or a sha8 that is not 8 hex characters.
Status longArgStub(uint32_t bytes, const std::string& sha8, uint64_t q, std::string& out);
// Long text kept inline (owner rulings 81 to 83, PRD section 2 tier 1 as amended; lane RB-H81): an
// assistant text or a saved owner paste over the inline limit keeps its first `cap` bytes (the
// head's v5.1.2 message cap at least: tx::HeadBudget::messageCap), cut on a character, then on its
// own line '[text continues: <rest>B more: <where>]'. `where` is the pointer to the saved whole:
// 'view id=r<q>' for a text in the bulk file, 'read_file path=<path>' for a saved paste; empty, the
// whole was not kept: 'not kept: no storage card'. Text at or under `cap` is returned whole.
// InvalidArgument for a cap of 0.
Status longTextHead(const std::string& text, size_t cap, const std::string& where, std::string& out);
// The pointers longTextHead names.
inline std::string viewPointer(uint64_t q) { return "view id=r" + std::to_string(q); }
inline std::string pathPointer(const std::string& path) { return "read_file path=" + path; }

// A photo out of view: '[photo p<ref8> <when> not shown: view id=p<ref8>]'. ref8 is the
// photo ref without its 'p' (1 to 16 letters or digits); when is photoWhen's text (empty when
// the clock was not set: the field is left out). InvalidArgument for a bad ref.
Status photoStub(const std::string& ref8, const std::string& when, std::string& out);
// The same with the photo's note (tx::Part::note, lane RB-H81, ruling 84 item 5): '[photo
// p<ref8> <when>: <note>; not shown: view id=p<ref8>]', the note on one line, at most
// kPhotoNoteMax, '[' and ']' as '(' and ')' so it cannot close the stub. An empty note gives the
// stub above.
Status photoStub(const std::string& ref8, const std::string& when, const std::string& note, std::string& out);

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
// the answer as the sup list of an x item (why "superseded") at the close of the writing turn.
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
  const std::vector<uint64_t>* superseded = nullptr;   // every x item's sup, sorted (may be null)
  uint64_t ladderStubThrough = 0;         // ladder rung 2: open-turn tier-2 results at or below
                                          // this q (older than the newest two rounds) are stubs
};
// How a stored r item renders: Stub when superseded; a receipt otherwise Whole (Ruling 16);
// a tier-2 result at or below the pointer, an outside result outside its own open turn, or
// open-turn tier-2 at or below ladderStubThrough: Stub. InvalidArgument for a non-r item or
// an unsorted superseded list.
Status showResult(const Item& result, const ShowFacts& facts, ShowAs& out);

// A call's arguments as a request shows them (PRD section 2 tier 2 as amended by rulings 81 to 83;
// lane RB-H81): a string argument over 1,024 B is whole while its own turn is open (the c item
// keeps the model's arguments in args, bounded like tier-2 bulk) and its stub form (st: each long
// string as its long-argument stub) from the close of the turn or at ladder rung 2 (the call at or
// below ladderStubThrough). A call with no stub form shows its args. Pure.
inline const std::string& shownArgs(const Item& call, uint32_t openTurn, uint64_t ladderStubThrough) {
  const bool stub = call.turn != openTurn || call.q <= ladderStubThrough;
  return stub && !call.st.empty() ? call.st : call.args;
}

// ---- photos ------------------------------------------------------------------------------------

// How photos reach a head. Floor heads (85,000 B: routed, custom, unqualified) are Inline.
enum class PhotoMode : uint8_t { FileRef = 0, Inline = 1, None = 2 };
struct PhotoFacts {
  PhotoMode mode = PhotoMode::Inline;
  uint64_t now = 0;                      // 0 = clock not set: no photo is carried
  uint64_t lastOwnerTurnAt = 0;          // when the previous owner turn started
  bool afterRestart = false;
  bool noCard = false;                   // no storage card: the arrival turn only
  std::vector<std::string> candidates;   // refs of recent photo-carrying owner turns, newest first
  std::vector<uint32_t> ownerTurnsSince; // per candidate: owner turns since it arrived (0 = this turn)
  std::vector<uint32_t> b64Bytes;        // per candidate base64 size (Inline cap); empty = unknown
};
// The photo refs this request shows as pixels, newest first; the rest render as photoStub.
//  arrival (ownerTurnsSince 0): shown on every head with vision, at most kArrivalPhotosMax.
//  FileRef: the photos of the last two photo-carrying owner turns stay for the next
//    kPhotoOwnerTurns owner turns, at most kFileRefPhotosMax in all, until over kPhotoIdleS
//    idle, a restart or no clock.
//  Inline (floor and routed heads included): carried to the next owner turn only, if it
//    starts within kColdGapS, newest kInlinePhotosMax, at most kInlinePhotoB64Max of base64
//    (the bound needs b64Bytes: with sizes unknown it is not applied, so the photo lane passes
//    them).
//  None, or no card: nothing carried (None shows nothing at all).
// InvalidArgument when the per-candidate lists do not match the candidates.
Status visiblePhotos(const PhotoFacts& facts, std::vector<std::string>& out);

// ---- the overflow ladder -------------------------------------------------------------------------

enum class LadderRung : uint8_t {
  None = 0,
  StubClosedTurns = 1,   // (1) stub all closed turns
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
// for 0 turns.
Status omissionLine(uint32_t turns, const std::string& dates, std::string& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
