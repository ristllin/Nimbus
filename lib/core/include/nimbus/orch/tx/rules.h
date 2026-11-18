#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/rules.h - the pure rules of what a request shows (PRD section 2, "The stub rule"):
// the one monotonic stub pointer and its moves, the stub texts, superseded reads, photo
// visibility, the overflow ladder and the omission line. Pure functions of stored facts:
// the same facts always give the same bytes, so a restart renders the same request.
//
// Seam declared by lane RB-B0a; the bodies (orch_tx_rules.cpp) are lane RB-T3's.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr uint32_t kColdGapS = 270;            // over this since the last request: cold open
inline constexpr uint32_t kWarmBulkBytes = 24576;     // whole internal bulk above the pointer
inline constexpr uint32_t kWarmBulkBytesFloor = 8192; // the same on 85,000 B heads
inline constexpr size_t kStubMaxBytes = 400;
inline constexpr size_t kStubPreviewBytes = 200;
inline constexpr size_t kStubArgsPreviewBytes = 120;
inline constexpr uint32_t kNewestOwnerTurnsKept = 6;  // never omitted by the ladder
inline constexpr uint32_t kPhotoIdleS = 3600;         // file-reference photos leave view after this

// Why the pointer moved (x.why). Slugs frozen: stored and counted as change points.
enum class MoveWhy : uint8_t { ColdOpen = 0, WarmBulk = 1, Outside = 2, Superseded = 3 };
inline const char* moveWhySlug(MoveWhy w) {
  switch (w) {
    case MoveWhy::ColdOpen: return "cold";
    case MoveWhy::WarmBulk: return "bulk";
    case MoveWhy::Outside: return "outside";
    case MoveWhy::Superseded: return "superseded";
  }
  return "cold";
}

// The facts the turn-open decision reads.
struct OpenFacts {
  uint64_t now = 0;              // epoch seconds; 0 = clock not set (reads as cold)
  uint64_t lastReqAt = 0;        // HeadState::lastReqAt
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

// Rules (1) and (2): the move stored as an x item before the first request is rendered.
Status decideTurnOpen(const OpenFacts& facts, PointerMove& out);

// What a stub is computed from, once, at capture.
struct StubFacts {
  std::string tool;
  uint64_t q = 0;
  uint32_t bytes = 0;
  uint32_t lines = 0;
  std::string hhmm;        // local time of the result, "HH:MM"
  std::string preview;     // the result's first bytes (internal results only)
  std::string path;        // file tools: path=
  std::string sha8;        // file tools: sha8=
  std::string args;        // outside results: the call's arguments
  ResultClass cls = ResultClass::Receipt;
};

// The stub text (at most kStubMaxBytes): internal
// '[stub <tool> r<q> <bytes>B <lines>L <HH:MM>] <preview> ... (full: view id=r<q>)'
// or outside '[stub <tool>(<args>) r<q> <bytes>B outside content, not shown: view id=r<q>]'.
Status stubText(const StubFacts& facts, std::string& out);
// A long call argument: '[stub <bytes>B sha:<sha8>: view id=r<q>]'.
Status longArgStub(uint32_t bytes, const std::string& sha8, uint64_t q, std::string& out);
// A photo out of view: '[photo p<ref8> <date time> not shown: view id=p<ref8>]'.
Status photoStub(const std::string& ref8, const std::string& when, std::string& out);

// Rule (4): the result q's a successful write supersedes (earlier whole reads of the same
// path; for a ledger_write, earlier views of the same day).
Status supersededBy(const Item& writeCall, const std::vector<Item>& earlier, std::vector<uint64_t>& out);

// How photos reach a head.
enum class PhotoMode : uint8_t { FileRef = 0, Inline = 1, None = 2 };
struct PhotoFacts {
  PhotoMode mode = PhotoMode::Inline;
  uint64_t now = 0;
  uint64_t lastOwnerTurnAt = 0;
  bool afterRestart = false;
  std::vector<std::string> candidates;   // refs of recent photo-carrying owner turns, newest first
  std::vector<uint32_t> ownerTurnsSince; // per candidate: owner turns since it arrived
};
// The photo refs this request shows as pixels; the rest render as photoStub.
Status visiblePhotos(const PhotoFacts& facts, std::vector<std::string>& out);

// The overflow ladder, rung by rung, when the next request would pass the body cap.
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
Status nextRung(const LadderFacts& facts, LadderRung& out);
// '[N earlier turns, <dates>, not shown: search finds them]'
Status omissionLine(uint32_t turns, const std::string& dates, std::string& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
