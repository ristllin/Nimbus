#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/engine.h"   // TurnCeiling (ownerCeiling)
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

// plain/compact.h - the compaction driver of engine 2 (PRD section 3), lanes RB-C2 and RB-C3. When
// the transcript after thru outgrows its budget, the oldest whole turns are taken into ONE cumulative
// structured summary (owner rulings 105 and 106; tx/digest.h "the cumulative summary"), swapped in the
// order of tx/digest.h. The driver decides when, asks the summarizer through one exchange
// (plain/exchange.h) for the CHANGES the new turns make to the summary, applies them in code, and
// swaps; it knows nothing about what a conversation is about.
//
// It keeps the rules of the fold ladder (docs/architecture/fold-ladder.md) with this
// section's numbers, and does not run through agent::FoldDriver (bound to engine 1):
//   - one rung per idle step; the head host's small model first; after two failures the
//     keyed-provider ladder (the binding's rungs, in the owner's order), each failure waiting
//     10 min, 30 min, 2 h, then every 6 h for the idle steps (an urgent step, a turn that opens over
//     the due mark, does not wait and is not held by the daily count: lane E2-FIX9); at most
//     kCompactPaidPerDay attempts (one exchange each: RB-EX's same-host retries inside kCompactRequestMs; a cut answer is asked once more
//     with room, kSummarizerRetryMaxTokens, and never stored cut)
//     start a day on the device; no pause state: nothing stops it for longer than the longest
//     wait or the day, and a wait or a day count a clock that ran ahead left behind does not hold;
//   - owner first: nothing runs while an owner message waits, and an automatic step needs
//     kCompactQuietS of quiet; at most kCompactRequestsPerWindow requests per idle window;
//   - a manual request (/compact, Summarize now) skips the quiet wait, the backoff and the
//     daily cap (it is counted, and manual runs stop at the cap plus kCompactManualRequests),
//     chains at most kCompactManualRequests requests and is always answered once: done,
//     nothing to compact, failed, or waiting for the clock.
//
// The decision rule (planSlice): due when the resident bytes after thru are above H, the whole
// prompt of the last turn's last request (tx::TurnEndFacts::lastPromptTokens) reached the head's
// due mark in tokens, or a manual run asks; no hour makes it due (no night fold: owner ruling 81,
// ruling 83 item 8). The slice is whole turns from thru forward, at most kCompactSliceMaxBytes of
// summarizer input, and only while what stays after it is at least L, the owner's recent part
// (a manual run takes all it may); a slice may span days (the input names each day). Never in a
// slice: the newest kNewestOwnerTurnsKept owner turns while they fit under H, a turn with no end
// (an unanswered message), a turn with an intent and no result, a turn whose agent job runs, and
// everything after the first of these: thru never jumps over a turn. The v5.1.2 text keep holds
// only for an owner's recent part that reaches it (ruling 106 item 2).
//
// Ahead of time (ruling 106 item 3): once the last request reached kPreparePercent of the due mark,
// an idle step (the same quiet and windows) updates a pending summary over the turns already older
// than the recent part, in RAM, without swapping it; at the due mark it is swapped in with no
// request. Urgent steps (IdleCtx::urgency, the engine at turn open and when a request does not fit)
// skip the quiet and the window: IfDue compacts only when due (the pending summary first), Now
// compacts what may go; each says before a request that the turn waits (IdleCtx::waiting).
//
// The summarizer input: the summary so far (its lines with their ids), then texts::summarizer::kSince,
// any day digests a chat still holds (dated lines), and the turns: a day line where the day changes,
// owner and assistant text verbatim with the local time; each tool result one bracketed line that
// names the tool (file writes '[file change]', other file calls '[file work]', outside results
// '[outside content read]'); never a call's arguments, a result's bytes or a device note. The
// assistant text of a turn that read outside content goes inside the data frame, and every line that
// slice adds to the summary is stored inside the frame. The Files lines are the device's, from the
// slice's successful file-change calls (their path argument). Accepted only when the answer was not
// cut, not refused, and reads as changes (tx::parseSummaryEdits: at least one, or "=", or only lines of
// the summary's own frame such as a title alone: SummaryEdits::taken, lane E2-FIX9); a rewrite or
// prose is a failed attempt, never a summary rebuilt from nothing. The answer's form is AnswerForm's:
// the change lines as a structured list first on the Anthropic host, then, for an answer not taken
// (refused, the form refused, no change line), once more as free lines at once; every answer not
// taken logs its cause and its first bytes. The result is fitted to the head's summary
// budget (tx::fitSummary); every line that left goes to the archive before the swap. Otherwise
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
// One update's turns (summarizer input): a cycle from the due mark down to the recent part takes one
// to three updates on a large head (most of a tool result is one line here).
// The one user message stays under tx::kMaxLineBytes (60,000 B, escapes included) with the day digests a
// chat may still hold (6,144 B); the summary so far rides in the system text.
inline constexpr uint32_t kCompactSliceMaxBytes = 40960;
// The ahead-of-time summary starts once the last request reached this share of the due mark.
inline constexpr uint32_t kPreparePercent = 80;
// Sized so one due-to-keep cycle completes (ruling 91 T4-ADV-05; ruling 82 item 5: a cost bound never
// makes the answer worse): a cycle on Sonnet or luna at full is about four slices, so one quiet spell
// holds one, and the daily count is a runaway guard over three chats' cycles a few times a day,
// not a cost cap (failures still wait 10 min, 30 min, 2 h, 6 h).
inline constexpr uint8_t kCompactRequestsPerWindow = 4;
inline constexpr uint32_t kCompactRequestMs = 45000;
// Lane E2-FIX7 (M5): what one attempt may keep a person's turn waiting (an urgent step: turn open, or a
// request that does not fit), every request of it together: two requests' worth, what one ask with its
// cut retry could take before lane E2-CMP added the second try. A turn makes at most two urgent attempts
// (IfDue at its open, Now inside it), so it waits at most 180 s for compaction, as before. The second try
// runs only inside what is left of it, and never after a refusal (the same content is refused again).
inline constexpr uint32_t kCompactUrgentMs = 2 * kCompactRequestMs;
// A request is not started with less than this left of an urgent attempt's time.
inline constexpr uint32_t kCompactMinRequestMs = 10000;
// The summarizer's output cap is sized to the summary budget's default (4,000 tokens), not to one
// line: a head that writes long, or thinks first, has room to finish (ruling 89 item 4).
// A cut answer is asked once more, in the same attempt, with kSummarizerRetryMaxTokens; a cut is
// never stored and never asked again at the same cap.
inline constexpr uint16_t kSummarizerMaxTokens = 4096;   // the changes, at most a whole summary
inline constexpr uint16_t kSummarizerRetryMaxTokens = 8192;
inline constexpr uint32_t kCompactPaidPerDay = 48;
inline constexpr uint32_t kCompactHeadTries = 2;           // failures on rung 1 before the ladder
inline constexpr uint32_t kCompactWaitS[] = {600, 1800, 7200, 21600};
inline constexpr uint32_t kCompactWindowS = 3600;          // an idle window ends at a turn or after this
inline constexpr uint32_t kCompactManualRequests = 8;
// Lane E2-FIX15: the slices one crossing of the due mark may prepare before its one swap (the bench's first crossing
// of a large imported conversation took four).
inline constexpr uint32_t kCompactCrossingSlices = 8;
inline constexpr uint32_t kCompactThinFloor = 120;
inline constexpr uint32_t kCompactThinSlice = 2048;
// kDayStartDefaultMin (04:00) lives in plain/turn.h: TurnCtx carries the day start too.
inline constexpr size_t kCompactCtlChats = 8;              // chats whose backoff the control file keeps
// The arena room compaction keeps (or a quarter of the arena, the smaller): under it a compaction is
// due, so the engine never has to drop whole turns from RAM with no digest (ruling 91 table/F1).
inline constexpr size_t kCompactArenaRoomBytes = 128 * 1024;
// The room a compaction keeps free in an arena of `capacity`: at least kCompactArenaRoomBytes, an
// eighth of a large arena (a busy day of tool rounds stores more than 128 KB between two idle spells:
// lane RB-T4b), never more than a quarter of a small one.
inline size_t compactArenaRoom(size_t capacity) {
  const size_t eighth = capacity / 8, quarter = capacity / 4;
  const size_t room = eighth > kCompactArenaRoomBytes ? eighth : kCompactArenaRoomBytes;
  return room < quarter ? room : quarter;
}
// The most of the stored-but-not-resident turns one step reads back from the card (PSRAM).
inline constexpr size_t kCompactReadBackBytes = 96 * 1024;

// How an update's answer is asked for (lane E2-CMP). Changes: the change lines as one JSON list
// (texts::summarizer::kChangesSchema) through the provider's structured output, so the answer is whole
// by construction and "nothing to change" is an empty list; the first try on a wire that has it
// (Anthropic). Lines: free text read line by line; every other wire, and the one immediate second try
// after an answer that was not taken (refused, unreadable, or no change line), before the stated wait.
enum class AnswerForm : uint8_t { Lines = 0, Changes = 1 };
// A structured answer {"changes":["+F ...", ...]} as the lines tx::parseSummaryEdits reads (each item a
// line; an item holding line ends is several, as free text would be); an empty list is "=". False when
// the text is not that object: the driver then reads the text as lines as it stands.
bool changeLinesOf(const std::string& json, std::string& lines);

// One rung of the summarizer ladder.
struct SummarizerRung {
  const WireCodec* wire = nullptr;   // nullptr: wireFor(dialect)
  HeadEndpoint endpoint;
  std::string host;                  // the provider slug (caps, lines, logs)
  std::string model;
  Dialect dialect = Dialect::AnthropicMessages;
};
// The first form a rung is asked in: Changes on the Anthropic host's own wire, Lines on any other (an
// Anthropic-shaped proxy may not have the structured output).
AnswerForm firstAnswerForm(const SummarizerRung& rung);

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
  // Every paid summarizer request (lane E2-CMP): the tokens a rung billed for one ask (its cut retry
  // and same-host retries summed; an answer not taken was still paid), for the owner's usage and the
  // monthly ceiling, the path a turn's requests take. Unset: not counted.
  std::function<void(const SummarizerRung&, const nimbus::orch::TokenUsage&)> spent;
  std::function<void(const std::string& chatId, const std::string& text)> tell;   // manual answers
  TextIO ctl;        // the control record (unset: kept in RAM)
  TextIO chatsum;    // /data/chatsum.txt, the v5.1.1 mirror (unset: no mirror)
  // The newest q the restart fill gave up for a full arena (TxLog::fill's FillReport::droppedThru;
  // unset or 0: none). A slice that covers such items says so in its input.
  std::function<uint64_t()> omittedThru;
  // The head the marks and the token due mark are read for (tx::budgetFor). Required (ruling 89):
  // installCompaction refuses deps without it, and a driver without it fails every step, each with
  // InvalidArgument and one log line.
  nimbus::orch::tx::HeadKey headKey;
  uint16_t dayStartMin = kDayStartDefaultMin;
  // The owner's settings (the recent part and the summary budget, rulings 105 and 106), read at every
  // step and passed through tx::clampSettings; unset: the defaults.
  std::function<nimbus::orch::tx::EngineSettings()> settings;
  bool searchBound = false;   // E2-FIX5: a slice's input names search only when it is bound
};

// ---- the decision rule, pure -----------------------------------------------------------

struct SliceRules {
  uint64_t now = 0;              // epoch seconds (must be set)
  int16_t tzMinutes = 0;
  uint16_t dayStartMin = kDayStartDefaultMin;
  uint32_t high = 0, low = 0;    // H and L of the profile
  uint32_t keepText = 0;         // owner and assistant text a slice leaves at least (the v5.1.2 keep,
                                 // only for an owner's recent part that reaches it)
  bool manual = false;           // take all the rules allow, ignoring L
  std::function<bool(uint32_t turn)> jobRunning;
  uint64_t omittedThru = 0;      // items up to this q are stored but not resident (CompactDeps)
  // The arena's own room, a real limit (ruling 91 table/F1): while its free bytes are under
  // arenaRoom a slice may leave less than L and less than keepText, until the slice has freed
  // enough for 2 x arenaRoom free; never a turn the rules above protect. 0: not applied.
  size_t arenaFree = 0, arenaRoom = 0;
  // The arena holds turns RAM gave up, read back from the card: older than every resident turn, so
  // neither the current day nor the newest owner turns protect them (an open turn still does).
  bool readBack = false;
  bool searchBound = false;      // E2-FIX5 (CompactDeps::searchBound)
};
// A file-change call of a slice, as the device writes it into the summary's Files lines.
struct FileNote {
  std::string path;
  uint64_t at = 0;               // epoch of its result
  bool deleted = false;
};
struct SlicePlan {
  uint64_t thru = 0;             // the last q the slice covers (0: nothing to summarize)
  std::string day;               // "YYYY-MM-DD", the local day of its newest turn
  std::string input;             // the summarizer's user message
  bool tainted = false;          // a turn of the slice read outside content
  uint32_t turns = 0;            // turns with text in the slice
  size_t resident = 0;           // bytes after the old thru
  size_t rest = 0;               // bytes after the new thru
  size_t restText = 0;           // owner and assistant text after the new thru
  uint64_t lastRow = 0;          // the newest u.m in the slice (the chatsum row pointer)
  size_t freed = 0;              // arena bytes the slice's items hold (what the swap frees)
  bool textHeld = false;         // the oldest turn may not go only because of keepText
  bool belowKeep = false;        // the slice leaves under L or keepText: the arena's room asked
  std::vector<FileNote> files;   // its successful file-change calls, oldest first
};
// The tool fold (ruling 91 table/F1; owner ruling 81: tools must not eat the dialogue). When a due
// slice is held only by the text floor, the results of the oldest closed turns (before the
// protected ones) render as their stubs instead, oldest first, then (lane E2-VERB, ruling 106) their
// long call arguments, oldest first, until what a request renders of the transcript is at L: an x
// item (why "tools") lists them in sup (sorted), stored like any change point. The owner and
// assistant text stays verbatim; each stub keeps its way back. `sup` empty:
// nothing whole is left to fold. InvalidArgument without a clock or marks.
Status planToolFold(const nimbus::orch::tx::TxArena& arena, uint64_t thru, const SliceRules& rules,
                    std::vector<uint64_t>& sup, size_t& saved);
// The local day of `epoch`, where a day starts at dayStartMin past midnight: "YYYY-MM-DD".
std::string localDay(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin);
// Bytes a request renders of the transcript after `thru` (the unit of H and L, ruling 91 table/F6):
// each owner, assistant, call, result and device-note item's text as it shows (a result whole or
// as its stub by the stub rule, a whole bulk result at its payload size, a photo as its stub line)
// plus kItemFramingBytes of wire framing. Not the stored lines: a result line also holds its stub
// and a bulk line only a handle. Photo pixels are counted by the hot trigger in real tokens.
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
  ToolsFolded,  // no request: the oldest closed turns' results now render as stubs (planToolFold)
  Prepared,     // ahead of time: the pending summary now covers more turns (nothing swapped)
  Swapped,      // the pending summary was swapped in at the due mark: no request
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
  // The summary prepared ahead of time, not yet swapped in ("" when none is valid), and the last q it covers.
  std::string pendingSummary() const { return pendingValid() ? pending_.text : std::string(); }
  uint64_t pendingThru() const { return pending_.thru; }

 private:
  bool turnAsks(const IdleCtx& ctx) const;   // lane E2-FIX9: a turn's attempt that cannot run says why in the log
  Status run(const IdleCtx& ctx, CompactOutcome& out);
  bool automaticMayAsk(const IdleCtx& ctx);
  Status planIfDue(const IdleCtx& ctx, SlicePlan& plan, bool& due);
  Status foldTools(const SliceRules& rules, CompactOutcome& out);
  Status noSlice(const SlicePlan& plan, CompactOutcome& out);
  Status dueBy(const nimbus::orch::tx::HeadBudget& b, const SlicePlan& plan, bool& due);
  Status crossing(const IdleCtx& ctx, SlicePlan plan, CompactOutcome& out);   // lane E2-FIX15: one write a crossing
  bool crossingMayAsk(bool urgent, uint32_t startMs) const;
  SliceRules rulesFor(const IdleCtx& ctx, const nimbus::orch::tx::HeadBudget& b) const;
  Status planDropped(const IdleCtx& ctx, uint64_t omitted, SlicePlan& plan, bool& read);
  SliceRules rules_;   // the last planIfDue's rules (the tool fold reads them)
  // A compaction that became due runs on, slice by slice, until what stays is at L (ruling 82
  // item 4: due at the aim, keeps about 70% of it), not one slice under H each turn.
  bool cycle_ = false;
  // The hot trigger reads each e item once: a cycle it started that ended spends it (hotQ_ is the
  // newest e item's q at the last look), so a stale reading never re-arms an empty cycle.
  uint64_t hotQ_ = 0, spentHotQ_ = 0;
  void endCycle() {
    cycle_ = false;
    spentHotQ_ = hotQ_;
  }
  bool held(const IdleCtx& ctx, const CompactCtl::Chat& chat);
  Status attempt(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out, bool prepareOnly = false);
  // The summary ahead of time (ruling 106 item 3), and its swap at the due mark.
  struct Pending {
    std::string text;            // the encoded summary
    uint64_t thru = 0;           // the last q it covers
    uint64_t baseThru = 0;       // head.thru when it was started: valid only while thru has not moved
    uint64_t lastRow = 0;
    uint32_t turns = 0;
    std::string day;
  };
  Pending pending_;
  nimbus::orch::tx::HeadBudget budget_;   // the last planIfDue's budget (the summary's cap)
  bool pendingValid() const;
  Status prepare(const IdleCtx& ctx, CompactOutcome& out);
  Status swapPending(uint64_t now, CompactOutcome& out);
  // The summary the next update builds on: the pending one, else the newest in force; the day digests
  // older than any summary are passed as dated lines.
  Status baseFor(nimbus::orch::tx::Summary& base, std::string& baseText, std::string& legacy) const;
  Status merge(const SlicePlan& plan, const std::string& answer, std::string& text);
  Status onDue(const IdleCtx& ctx, const SlicePlan& plan, CompactOutcome& out);
  Status keep(const SlicePlan& plan, const std::string& answer, uint64_t now, bool prepareOnly, CompactOutcome& out);
  Status takeEdits(const SlicePlan& plan, const std::vector<nimbus::orch::tx::SummaryEdit>& edits,
                   nimbus::orch::tx::Summary& sum, std::vector<nimbus::orch::tx::SummaryLine>& left, size_t& unread) const;
  void addFiles(const SlicePlan& plan, nimbus::orch::tx::Summary& sum, std::vector<nimbus::orch::tx::SummaryLine>& left) const;
  Status archiveLeft(const SlicePlan& plan, const std::vector<nimbus::orch::tx::SummaryLine>& left) const;
  Status archiveLegacy();
  void keepPending(const SlicePlan& plan, const std::string& text, CompactOutcome& out);
  // The summary so far (the request's system text beside the prompt) and the one user message.
  Status messageFor(const SlicePlan& plan, std::string& sofar, std::string& message) const;
  int16_t tz_ = 0;   // the step's local offset (the Files lines' times)
  void settle(const SummarizerRung& rung, const SlicePlan& plan, uint32_t fails, uint64_t now, const Status& s,
              CompactOutcome& out);
  struct Asked {
    std::string summary;
    uint8_t requests = 0;
    bool parked = false;
  };
  // `untilMs`: the platform clock (nowMs) by which an urgent attempt ends; 0: no bound beyond each request's own.
  Status ask(const SummarizerRung& rung, const SlicePlan& plan, uint64_t now, AnswerForm form, uint32_t untilMs,
             Asked& out);
  Status askAndKeep(const IdleCtx& ctx, const SummarizerRung& rung, const SlicePlan& plan, bool prepareOnly,
                    CompactOutcome& out, bool& parked);
  AnswerForm formFor(const SummarizerRung& rung) const;
  void noteForm(const SummarizerRung& rung, const Status& s);
  bool secondTry(const Status& s, const Asked& asked, AnswerForm form, uint32_t untilMs) const;
  Status swap(const SlicePlan& plan, const std::string& text, uint64_t now, CompactOutcome& out);
  void loadCtl();
  void saveCtl();
  void endManual(const char* text);

  // Lane E2-FIX7 (M6): "host/model" of summarizers whose provider refused the structured form by name; they
  // are asked in lines until a restart (a model change is another key). At most kNoFormModels.
  static constexpr size_t kNoFormModels = 8;
  std::vector<std::string> noForm_;
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

// ---- the owner's settings beyond compaction (rulings 105 item 4 and 106) -----------------------

// The attended turn's bounds from the owner's max rounds and hard time limit (tx::EngineSettings): the
// binding sets TurnSetup::ceiling to this on a person's turn. applyCeiling only lowers, so at the
// defaults (64 rounds, 1,200 s) nothing changes; the time guard keeps the last round's reserve
// (kLastRoundReserveMs) inside the hard time. Unattended turns keep their own ceilings.
TurnCeiling ownerCeiling(const nimbus::orch::tx::EngineSettings& settings);

// What /budget and the web page show beside the settings, in tokens (rulings 105 and 106): the system
// prompt as composed now (core, skills and notes, the recent-files list), the tools, the summary in
// force and the transcript after it, and the head's marks with the settings it carries. Token figures
// are on the head's wire ratio (HeadBudget::bodyMilliBytesPerToken): measured on the Anthropic wire,
// nominal elsewhere (`nominal`).
struct BudgetView {
  uint32_t systemTokens = 0;     // core + notes + recent files (the fixed text, without the summary)
  uint32_t toolsTokens = 0;
  uint32_t fixedTokens = 0;      // system + tools: the one number ruling 105 item 2 shows
  uint32_t summaryTokens = 0;    // the summary in force now
  uint32_t recentTokens = 0;     // the transcript after it now (what a request renders)
  uint32_t summaryBudgetTokens = 0, recentPartTokens = 0;   // the settings as this head carries them
  uint32_t dueTokens = 0, aimTokens = 0;
  uint16_t maxRounds = 0;
  uint32_t hardTimeS = 0;
  bool nominal = false;
};
struct BudgetViewIn {
  const nimbus::orch::tx::HeadBudget* budget = nullptr;        // tx::budgetFor(head, profile, settings)
  nimbus::orch::tx::EngineSettings settings;
  const SystemBlocks* system = nullptr;                        // composeSystem's blocks for this chat now
  const std::vector<ToolSpec>* tools = nullptr;                // toolManifest's array
  const std::vector<nimbus::orch::tx::DigestEntry>* live = nullptr;   // PlainEngine::digests()
  const nimbus::orch::tx::TxArena* arena = nullptr;
  uint64_t thru = 0;                                           // PlainEngine::head()->thru
};
Status budgetView(const BudgetViewIn& in, BudgetView& out);   // InvalidArgument for a missing input

// One step of a driver with fresh RAM state (persisted control only).
Status compactSlice(const CompactDeps& deps, const IdleCtx& ctx, CompactOutcome& out);
// Register a driver on the idle seam (HookSlot::Compaction). The first form owns it; the second
// uses the binding's (manual requests, status), which must outlive the hooks.
Status installCompaction(PlainHooks& hooks, const CompactDeps& deps);
Status installCompaction(PlainHooks& hooks, CompactDriver& driver);

}  // namespace plain

// The device's binding (src/agent/plain_fold_device.cpp, engine-2 device images only, on
// tools/engine/engine2_only.txt): the small file on the internal flash.
//   deviceCompactCtlIO  /data/compact_ctl.txt, the device-wide attempt count of the day and each
//                       chat's failures (missing or damaged reads as fresh; older images never read it)
// No /data/chatsum.txt binding: engine 2 never writes the earlier engine's fold record (lane E2-FIX2B,
// finding P5); the way back finds that record as the earlier engine left it.
plain::TextIO deviceCompactCtlIO();

}  // namespace agent
