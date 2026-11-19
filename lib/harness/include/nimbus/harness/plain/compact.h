#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/migrate.h"

namespace agent {
class HttpTransport;
struct Platform;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxLog;
class TxArena;
class TxFiles;
struct HeadState;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/compact.h - the compaction driver of engine 2 (PRD section 3), lane RB-C2. When the
// transcript after thru outgrows its budget, the oldest whole turns become one day digest
// each, swapped in the order of tx/digest.h. The driver decides when, asks the summarizer
// through one exchange (plain/exchange.h), and swaps; it knows nothing about what a
// conversation is about.
//
// It keeps the rules of the fold ladder (docs/architecture/fold-ladder.md) with this
// section's numbers, and does not run through agent::FoldDriver (bound to engine 1):
//   - one rung per idle step; the head host's small model first; after two failures the
//     keyed-provider ladder (the binding's rungs, in the owner's order), each failure waiting
//     10 min, 30 min, 2 h, then every 6 h; at most kCompactPaidPerDay attempts (one exchange
//     each: RB-EX's same-host retries inside kCompactRequestMs; a cut answer is not asked again)
//     start a day on the device; no pause state: nothing stops it for longer than the longest
//     wait or the day, and a wait or a day count a clock that ran ahead left behind does not hold;
//   - owner first: nothing runs while an owner message waits, and an automatic step needs
//     kCompactQuietS of quiet; at most kCompactRequestsPerWindow requests per idle window;
//   - a manual request (/compact, Summarize now) skips the quiet wait, the backoff and the
//     daily cap (it is counted, and manual runs stop at the cap plus kCompactManualRequests),
//     chains at most kCompactManualRequests requests and is always answered once: done,
//     nothing to compact, failed, or waiting for the clock.
//
// The decision rule (planSlice): due when the resident bytes after thru are above H, the last
// turn reported prompt tokens at or above 0.8 of the head's target, or between 03:00 and 05:00
// local with resident above 0.8 H. The slice is whole turns from thru forward, all of one local
// day (the day starts at dayStartMin), at most kCompactSliceMaxBytes of summarizer input, and
// only while what stays after it is at least L (a manual run takes all it may). Never in a
// slice: a turn of the owner's current local day (unless that day alone is above H, then only
// its oldest half), the newest kNewestOwnerTurnsKept owner turns, a turn with no end (an
// unanswered message), a turn with an intent and no result, a turn whose agent job runs, and
// everything after the first of these: thru never jumps over a turn.
//
// The summarizer input: owner and assistant text verbatim with the local time; each tool result
// one bracketed line that names the tool (file writes '[file change]', other file calls
// '[file work]', outside results '[outside content read]'); never a call's arguments, a
// result's bytes or a device note. The assistant text of a turn that read outside content goes
// inside the data frame, and that slice's digest is stored inside the frame, so the block shows
// it framed. Accepted only when the answer was not cut, holds at least kCompactThinFloor bytes
// for an input of kCompactThinSlice or more, and makes a digest (tx::digestText). Otherwise
// nothing changes but the failure count.
//
// The swap (RB-C1's order): digestText, digestItem, log.store (the digest joins the live set
// only when stored; a refused store ends the swap with thru unmoved), settleDigests, head.thru
// and writeHead, mirrorChatsum (saved only when it differs), arena.dropThrough. A cut at any
// point leaves every item either after thru or under a stored digest, exactly once.
//
// Memory: every step runs under the PSRAM-first scope (turn_memory.h); a refused allocation is
// Err::TooLarge with nothing stored changed (ruling 30). Single task: every call comes from the
// idle step of the chat's engine.
namespace agent {
namespace plain {

inline constexpr uint32_t kCompactQuietS = 120;
inline constexpr uint32_t kCompactSliceMaxBytes = 24576;
inline constexpr uint8_t kCompactRequestsPerWindow = 2;
inline constexpr uint32_t kCompactRequestMs = 45000;
inline constexpr uint16_t kSummarizerMaxTokens = 400;
inline constexpr uint32_t kCompactPaidPerDay = 6;
inline constexpr uint32_t kCompactHeadTries = 2;           // failures on rung 1 before the ladder
inline constexpr uint32_t kCompactWaitS[] = {600, 1800, 7200, 21600};
inline constexpr uint32_t kCompactWindowS = 3600;          // an idle window ends at a turn or after this
inline constexpr uint32_t kCompactManualRequests = 8;
inline constexpr uint32_t kCompactThinFloor = 120;
inline constexpr uint32_t kCompactThinSlice = 2048;
// kDayStartDefaultMin (04:00) lives in plain/turn.h: TurnCtx carries the day start too.
inline constexpr size_t kCompactCtlChats = 8;              // chats whose backoff the control file keeps

// One rung of the summarizer ladder.
struct SummarizerRung {
  const WireCodec* wire = nullptr;   // nullptr: wireFor(dialect)
  HeadEndpoint endpoint;
  std::string host;                  // the provider slug (caps, lines, logs)
  std::string model;
  Dialect dialect = Dialect::AnthropicMessages;
};

// A small text file the binding keeps (load "" when missing; save false when refused).
struct TextIO {
  std::function<std::string()> load;
  std::function<bool(const std::string&)> save;
};

// The device-wide control record: requests started today and each chat's failures.
struct CompactCtl {
  struct Chat {
    std::string key;
    uint32_t fails = 0;   // failed attempts in a row
    uint64_t next = 0;    // epoch before which an automatic step waits
  };
  uint32_t day = 0;       // the calendar day `paid` counts (days since 1970, local)
  uint32_t paid = 0;      // attempts started on that day, every chat together
  std::vector<Chat> chats;

  std::string encode() const;
  static CompactCtl decode(const std::string& text);   // anything unreadable reads as fresh
  Chat& chat(const std::string& key);                  // added when missing (at most kCompactCtlChats)
};
// The wait after `fails` failures in a row: none before kCompactHeadTries, then 10 min, 30 min,
// 2 h, then 6 h.
uint32_t compactWaitS(uint32_t fails);
// The rung the next attempt asks: rung 0 for the first kCompactHeadTries, then each other rung
// in turn, round the ladder.
size_t rungFor(uint32_t fails, size_t rungs);

struct CompactDeps {
  const Platform* platform = nullptr;
  HttpTransport* transport = nullptr;
  nimbus::orch::tx::TxLog* log = nullptr;
  nimbus::orch::tx::TxArena* arena = nullptr;
  nimbus::orch::tx::TxFiles* files = nullptr;   // the same storage (digest archive)
  std::string chatKey;                          // /mem/tx/<chatKey>
  std::string chatId;                           // the chatsum record and the manual answer
  // The engine's head state and live digests (PlainEngine::head(), digests()).
  std::function<nimbus::orch::tx::HeadState*()> head;
  std::function<std::vector<nimbus::orch::tx::DigestEntry>*()> digests;
  // The ladder, asked at every attempt: the head host's small model first, then every other
  // keyed, unparked provider of the owner's order. Empty: nothing is asked.
  std::function<void(std::vector<SummarizerRung>&)> rungs;
  std::function<bool(uint32_t turn)> jobRunning;   // a turn whose agent job still runs
  std::function<void(const SummarizerRung&)> park; // a rung's key or credit was refused
  std::function<void(const std::string& chatId, const std::string& text)> tell;   // manual answers
  TextIO ctl;        // the control record (unset: kept in RAM)
  TextIO chatsum;    // /data/chatsum.txt, the v5.1.1 mirror (unset: no mirror)
  // The newest q the restart fill gave up for a full arena (TxLog::fill's FillReport::droppedThru;
  // unset or 0: none). A slice that covers such items says so in its input.
  std::function<uint64_t()> omittedThru;
  uint32_t promptTargetTokens = 0;   // the head's prompt target (0: that trigger is off)
  uint16_t dayStartMin = kDayStartDefaultMin;
};

// ---- the decision rule, pure -----------------------------------------------------------

struct SliceRules {
  uint64_t now = 0;              // epoch seconds (must be set)
  int16_t tzMinutes = 0;
  uint16_t dayStartMin = kDayStartDefaultMin;
  uint32_t high = 0, low = 0;    // H and L of the profile
  bool manual = false;           // take all the rules allow, ignoring L
  std::function<bool(uint32_t turn)> jobRunning;
  uint64_t omittedThru = 0;      // items up to this q are stored but not resident (CompactDeps)
};
struct SlicePlan {
  uint64_t thru = 0;             // the last q the slice covers (0: nothing to summarize)
  std::string day;               // "YYYY-MM-DD", the local day of its turns
  std::string input;             // the summarizer's user message
  bool tainted = false;          // a turn of the slice read outside content
  uint32_t turns = 0;            // turns with text in the slice
  size_t resident = 0;           // bytes after the old thru
  size_t rest = 0;               // bytes after the new thru
  uint64_t lastRow = 0;          // the newest u.m in the slice (the chatsum row pointer)
};
// The local day of `epoch`, where a day starts at dayStartMin past midnight: "YYYY-MM-DD".
std::string localDay(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin);
// Bytes the transcript keeps after `thru`: the stored lines of owner, assistant, call, result
// and device-note items (what a request renders, stubs included).
size_t residentBytes(const nimbus::orch::tx::TxArena& arena, uint64_t thru);
// The slice the next compaction takes (see above). InvalidArgument without a clock or marks.
Status planSlice(const nimbus::orch::tx::TxArena& arena, uint64_t thru, const SliceRules& rules, SlicePlan& out);

// ---- the driver -------------------------------------------------------------------------

enum class CompactStep : uint8_t {
  Idle = 0,     // nothing due, or a gate said not now (owner waiting, quiet, window)
  Waiting,      // due, but the backoff, the daily cap or the clock holds it
  Nothing,      // due, but no turn may be summarized yet
  Failed,       // asked, and the answer was not taken: everything as it was
  Stored,       // a digest is stored and thru moved
  Refused,      // the answer was taken, but storage refused the digest: thru unmoved
};
const char* compactStepSlug(CompactStep s);

struct CompactOutcome {
  bool ran = false;              // a summarizer request was sent
  uint32_t turnsFolded = 0;
  uint32_t digests = 0;
  uint8_t requests = 0;
  CompactStep step = CompactStep::Idle;
  uint64_t thru = 0;             // thru after the step
};

class CompactDriver {
 public:
  explicit CompactDriver(CompactDeps deps) : d_(std::move(deps)) {}
  // One unit of idle work: at most one summarizer exchange and its swap. InvalidArgument for
  // missing deps; TooLarge when an allocation was refused (nothing stored changed).
  Status step(const IdleCtx& ctx, CompactOutcome& out);
  // Queue a manual run; false when one is already queued or running (it is answered once).
  bool requestManual();
  bool manualPending() const { return manual_; }
  const CompactCtl& control() const { return ctl_; }

 private:
  Status run(const IdleCtx& ctx, CompactOutcome& out);
  bool automaticMayAsk(const IdleCtx& ctx);
  Status planIfDue(const IdleCtx& ctx, SlicePlan& plan, bool& due);
  bool held(const IdleCtx& ctx, const CompactCtl::Chat& chat);
  Status attempt(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out);
  void settle(const SummarizerRung& rung, const SlicePlan& plan, uint32_t fails, uint64_t now, const Status& s,
              CompactOutcome& out);
  struct Asked {
    std::string summary;
    uint8_t requests = 0;
    bool parked = false;
  };
  Status ask(const SummarizerRung& rung, const SlicePlan& plan, uint64_t now, Asked& out);
  Status swap(const SlicePlan& plan, const std::string& summary, uint64_t now, CompactOutcome& out);
  void loadCtl();
  void saveCtl();
  void endManual(const char* text);

  CompactDeps d_;
  CompactCtl ctl_;
  bool manual_ = false;
  uint32_t manualAsks_ = 0;
  uint32_t manualStored_ = 0;
  uint64_t windowQ_ = 0;         // the arena's last q when the window started (or after a swap)
  uint32_t windowStartMs_ = 0;
  uint8_t windowAsks_ = 0;
};

// Digest 0 of a migration (lane RB-MG1a, tx/migrate.h): the binding's tx::Digest0Fn. The old
// summary becomes the assistant text of one closed turn at ask.at in an arena of its own, planSlice
// builds the summarizer input from it under the compaction input rules, and one exchange asks
// rung ask.attempt of deps.rungs (the last rung past the end) with texts::summarizer::kPrompt and
// the driver's acceptance (not cut, not thin). out.text is the digest the swap would store
// (digestText, frame tags made inert). NotFound with no rung; TooLarge for a refused allocation
// (under the PSRAM-first scope). Uses deps.platform, transport, rungs and park only.
Status digest0Answer(const CompactDeps& deps, const nimbus::orch::tx::Digest0Ask& ask,
                     nimbus::orch::tx::Digest0Answer& out);
nimbus::orch::tx::Digest0Fn digest0Asker(CompactDeps deps);

// One step of a driver with fresh RAM state (persisted control only).
Status compactSlice(const CompactDeps& deps, const IdleCtx& ctx, CompactOutcome& out);
// Register a driver on the idle seam (HookSlot::Compaction). The first form owns it; the second
// uses the binding's (manual requests, status), which must outlive the hooks.
Status installCompaction(PlainHooks& hooks, const CompactDeps& deps);
Status installCompaction(PlainHooks& hooks, CompactDriver& driver);

}  // namespace plain

// The device's binding (src/agent/plain_fold_device.cpp, engine-2 device images only, on
// tools/engine/engine2_only.txt): the two small files on the internal flash.
//   deviceCompactCtlIO  /data/compact_ctl.txt, the device-wide attempt count of the day and each
//                       chat's failures (missing or damaged reads as fresh; older images never read it)
//   deviceChatsumIO     /data/chatsum.txt, the v5.1.1 summary store the swap mirrors into
plain::TextIO deviceCompactCtlIO();
plain::TextIO deviceChatsumIO();

}  // namespace agent
