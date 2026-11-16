#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/model_window.h"   // modelCtxTokens + compactThreshold: the window table

// compact - the portable core of context compaction (plans/compaction-plan.md,
// v3.6.0). Nimbus cannot edit provider-side conversation chains (opaque ids), so
// compaction is: summarize the chat FROM THE EPISODIC STORE into an anchored
// per-chat summary, then RESET the provider chain; the existing per-chat
// RECENT CONVERSATION window supplies the verbatim tail. This module owns every
// decision that can be host-tested: the fold's schedule (backoff, the daily cap's
// record, never a pause) and the fold-prompt assembly; the
// per-model context table and the trigger threshold math live in model_window.h,
// which this header includes. Device/engine glue (episodic slice, provider call,
// chain reset, UX) stays out.
//
// Research grounding (2026-07-27): every surveyed harness anchors token counts
// on PROVIDER-REPORTED usage (no client tokenizer) and compacts at
// window − reserved-output − buffer, re-injecting a structured summary plus a
// verbatim recent tail. Claude Code's observed constants: 20K output reserve,
// 13K buffer (≈167K trigger on a 200K window).
namespace nimbus {
namespace orch {

// True when the chat's chain (provider-reported input tokens of its last turn)
// has reached the threshold.
inline bool shouldCompact(uint32_t chainTokens, uint32_t threshold) {
  return threshold > 0 && chainTokens >= threshold;
}

// ---- reactive provider-error classification ---------------------------------
// True when a provider error string indicates the CONTEXT WINDOW was exceeded: the
// trim-and-retry path of a head turn (lib/harness loop_common) and the reactive
// force-compact path. Matched case-insensitively on each provider's recorded wording
// (test_orch_compact). An output cap refused ("max_tokens is too large") is not one.
bool isContextOverflowError(const std::string& err);
// How many bytes of a request totalling `totalBytes` a trim gives up for that refusal:
// from the two figures a line names ("N tokens > M") plus a margin, else a fixed share.
inline constexpr size_t kOverflowTrimPercent = 40;
inline constexpr size_t kOverflowTrimMargin = 10;
size_t overflowTrimBytes(const std::string& err, size_t totalBytes);

// ---- the device-wide fold state's storage -------------------------------------
// The fold keeps a little device-wide state beside each chat's own (the paid fold
// count of the day, the last alert, a pending manual request): one small text file,
// /data/fold_ctl.txt on the device and a sibling file on the VN. The default reads
// nothing and stores nothing.
struct FoldCtlIO {
  std::function<std::string()> load = [] { return std::string(); };
  std::function<bool(const std::string& text)> save = [](const std::string&) { return true; };
};

// ---- the fold's schedule (owner ruling 2, 15 Nov 2026: never a permanent pause) ----
// A failed fold cycle waits 15 min, then 1 h, then 4 h, then every 12 h; nothing ever
// pauses the summary for good (v5.1.1 paused it after three failures, and on Lumi the
// pause held for 13 days). A refill right after a completed fold (the thrash case)
// only schedules the next try 15 min out.
constexpr uint32_t kFoldThrashRetryS = 15u * 60u;
// The wait after the n-th failed cycle in a row (n >= 1; 0 = no wait).
uint32_t foldBackoffS(uint32_t failedCycles);
// At most this many paid fold requests a local day on the whole device (automatic
// cycles; a manual Summarize now or /compact is exempt but still counted).
constexpr uint32_t kFoldPaidPerDay = 12;
// The owner hears about a failing summary once a local day, and only after it has
// been failing this long.
constexpr uint32_t kFoldAlertAfterS = 24u * 3600u;
// The digest a cycle takes halves after each failed cycle, never below this.
constexpr size_t kFoldDigestFloor = 8u * 1024u;
size_t foldDigestAfterFailures(size_t stepBytes, uint32_t failedCycles);
// The owner's local day number of `epoch` (0 = the clock is not set: every count
// then sits under day 0).
uint32_t foldLocalDay(uint32_t epoch, int32_t utcOffsetMin);

// The device-wide record /data/fold_ctl.txt holds (lane F of the history fix). A text
// file of "key=value" lines; a line it does not know is ignored, a missing file is a
// fresh device (no paid calls, no alert, nothing queued).
constexpr size_t kFoldManualQueueMax = 4;
struct FoldCtl {
  uint32_t paidDay = 0;        // the local day paidCount counts (0 = clock unset)
  uint32_t paidCount = 0;      // paid fold requests on paidDay, every chat together
  uint32_t lastAlertDay = 0;   // the local day the owner was last told (0 = never)
  // Manual requests not started yet, oldest first (a chat appears once): a request
  // survives a reboot and its chat's slot being evicted from the fold blob.
  std::vector<std::string> manualPendingChat;
  std::string fwAtLastFail;    // the firmware that last failed a cycle ("" = none)

  uint32_t paidOn(uint32_t day) const { return paidDay == day ? paidCount : 0; }
  void notePaid(uint32_t day, uint32_t n);
  // Queue a manual request (a chat already queued keeps its place). False when full.
  bool queueManual(const std::string& chatId);
  void dropManual(const std::string& chatId);
  std::string encode() const;
  static FoldCtl decode(const std::string& text);
};

// ---- the fold prompt ---------------------------------------------------------
// Instructions for the compaction call. Text-only, no tools; the reply IS the
// summary (the engine never applies any other field of the turn). Anchored:
// when a previous summary exists it is UPDATED, not replaced (opencode pattern),
// so cost stays bounded no matter how long a chat lives.
extern const char* const ORCH_COMPACT_PROMPT;
// The idle re-check of a parked provider: the shortest request that proves the account
// can be served.
extern const char* const ORCH_PARK_RECHECK_PROMPT;

// Assemble the compaction call's input: the previous anchored summary (may be
// empty) + the new episodic digest since the last compaction. digest should
// already be byte-bounded by the caller (the device stages ≤64 KB in PSRAM).
// Lane CTX: `day` (the engine's foldDayContext: a "[TODAY] <date> (<zone>)" line
// and the food ledger's view) closes the input so the summary dates its facts and
// carries today's ledger totals. It is added only while the whole input stays
// under kJsonStrGuard: whole, else its [TODAY] and totals lines, else not at all.
// `summaryCap` (lane F): a head that keeps a summary over 4,096 B is told its
// budget in a [SUMMARY BUDGET] line ahead of the rest, while the input stays under
// the guard; 0 or 4,096 adds nothing (the prompt's own "about 3,500 bytes" holds).
std::string buildCompactInputs(const std::string& prevSummary,
                               const std::string& digest,
                               const std::string& day = std::string(),
                               size_t summaryCap = 0);

// Cap a produced summary, UTF-8-safe (the fold output is re-injected into every
// future prompt - a torn sequence would 400 them all). The cap follows the head
// (lane F of the history fix): 6,144 B for a head of 60k tokens or more, else
// 4,096 B (v5.1.1's single cap). A v5.1.1 image re-caps a longer stored summary to
// 4,096 B on load, so a rollback keeps the summary, shortened.
constexpr size_t kChatSummaryMax = 6144;     // the largest summary any head keeps
constexpr size_t kChatSummarySmall = 4096;   // heads under kSummaryLargeHeadTokens
constexpr uint32_t kSummaryLargeHeadTokens = 60000;
inline size_t summaryCapFor(uint32_t headCtxTokens) {
  return headCtxTokens >= kSummaryLargeHeadTokens ? kChatSummaryMax : kChatSummarySmall;
}

// capSummary is SECTION-AWARE: a summary that fits is kept whole; one that does not
// keeps EVERY section header and trims each section's body in proportion to its
// size, whole lines from the end of the body (a body down to one line longer than
// its share loses that line's tail, UTF-8-safe). The cut is visible (one ellipsis
// at the end), and a capped summary that fits is never cut again, so the next
// anchored fold re-digests from it, not behind it. Headers alone over the cap (no
// summary the fold prompt asks for) take one UTF-8-safe tail cut.

// How many chats keep a fold state. Raised 8 -> 16 (R5): a household or a small
// group easily exceeds eight correspondents, and evicting a chat's slot discards
// its summary - the next turn there starts with no continuity and re-pays the
// whole fold. Sixteen slots at the worst case below is ~66 KB of LittleFS, which
// the 3.5 MB partition absorbs.
constexpr size_t kFoldMaxChats = 16;

// ...but slot COUNT alone does not bound the blob, because a summary is capped
// per chat and nothing capped the total. Sixteen full summaries serialize to
// ~99 KB, and the whole blob is rewritten on every turn. This is the real
// ceiling: serialize() drops whole least-recently-used records - summary and all -
// until the blob fits, so a device with many chatty correspondents degrades by
// forgetting the oldest conversation's summary rather than by growing without
// limit. 64 KB keeps about ten chats with a full 6,144 B summary. The device-wide
// fold state (the daily cap, a queued manual request) lives in FoldCtl, so an
// eviction never resets it.
constexpr size_t kFoldBlobMax = 64u * 1024;
std::string capSummary(const std::string& s, size_t maxBytes = kChatSummaryMax);

// ---- per-chat fold state (the durable store) ---------------------------------
// PRISM plan-review revision 2: fold state must NOT ride the NVS conv map (the
// shipped parser treats everything after the first '|' as the convId, and an OTA
// rollback would feed it a poisoned previous_response_id). ALL of it lives in
// ONE LittleFS blob a pre-v3.6 image simply ignores - rollback-safe by
// construction. The trigger basis is provider-independent (revision 1): episodic
// BYTES + MESSAGE COUNT accumulated on the chat since its last fold (only OpenAI
// even has a growing provider chain; on Anthropic/Mistral the fold is purely the
// long-term continuity mechanism).
struct ChatFold {
  std::string summary;             // anchored summary, ≤ kChatSummaryMax
  uint32_t bytesSinceFold  = 0;    // message bytes captured since the last fold
  uint32_t msgsSinceFold   = 0;
  uint32_t lastFoldEpoch   = 0;    // wall epoch of the last fold (0 = never/pre-sync)
  uint32_t turnsSinceFold  = 0;    // thrash guard input (saturating)
  // Lane F (ruling 2): the schedule that replaced the pause. failedCycles counts
  // failed cycles in a row (the backoff step and the digest halving), firstFail is
  // the epoch of the first of them (the 24 h alert), nextTry the epoch before which
  // an automatic cycle does not start (0 = due when the trigger says so). Stored as
  // ';' suffixes of the failures field, which an older image's number parser stops
  // before; the field itself and the pause field are written 0, so a rollback
  // reads a chat that is neither failing nor paused.
  uint32_t failedCycles    = 0;
  uint32_t firstFail       = 0;
  uint32_t nextTry         = 0;
  // H3 (turn-structured history): the newest row id the summary covers (the
  // history starts after it) and the overflow shift (rows up to it are neither
  // verbatim nor summarized until the next fold digests them). Both forward
  // only; "" = none. Stored as suffixes of the epoch and turns fields
  // ("1234;m0000a1f3"), which an older image's number parser stops before, so
  // an OTA rollback still reads the blob.
  std::string thru;
  std::string softStart;
  // RAM only (L19-26, fold on content): a hash of the chat's last owner message,
  // so a re-served copy of it adds nothing toward the next fold.
  uint32_t lastOwnerHash = 0;
};

// Owner messages a chat may gather before a fold is due by count alone (the
// byte threshold is the owner's compactAtK). Only DISTINCT messages count.
constexpr uint32_t kFoldMsgsThreshold = 200;

// Device implements against LittleFS /data/chatsum.txt; host tests in-memory.
struct FoldStoreIO {
  virtual ~FoldStoreIO() = default;
  virtual std::string load() = 0;             // "" if none
  virtual void        save(const std::string& blob) = 0;
};

// The fold-due decision, including the thrash guard (revision 7, lane F): a chat
// over the byte or message threshold again within kThrashTurns turns of a
// completed fold is due once and its next try is scheduled kFoldThrashRetryS out,
// so the identical request can never loop and nothing ever pauses. Hidden rows
// alone are never thrash: a backlog larger than one fold is folded again, each
// fold moving the anchor forward.
enum class FoldDue : uint8_t { No = 0, Yes };
constexpr uint32_t kThrashTurns = 2;

class FoldStore {
 public:
  void begin(FoldStoreIO* io);                 // load + parse (tolerant)

  ChatFold get(const std::string& chatId) const;   // default-constructed if absent
  // The chat's state in place, no copy of the summary (nullptr if absent). Valid until
  // the next call that changes the store; the owning task only.
  const ChatFold* peek(const std::string& chatId) const;

  // Accumulate one captured message on the chat (called at episodic capture
  // time for kind=message rows). Persist is DEFERRED to the next noteTurn/
  // applyFold to keep per-message flash writes off the hot path.
  void noteMessage(const std::string& chatId, size_t bytes);
  // An owner message, counted on content (L19-26): the same text again right
  // after itself (a replayed or re-sent update) counts nothing, so a flood of
  // one line can never make a fold due by count. Returns false for a repeat.
  bool noteOwnerMessage(const std::string& chatId, const std::string& text);

  // H3: the history's overflow shift moved forward to `rowId` (persisted with
  // the turn's blob write). Rows it hides make a fold due.
  void setSoftStart(const std::string& chatId, const std::string& rowId);
  // A due fold found nothing new worth a summary (the slice beyond the kept
  // verbatim tail was empty): the counters restart without a paid call.
  void noteFoldSkipped(const std::string& chatId);

  // One completed turn on the chat: bumps the thrash counter and persists the
  // accumulated state (one small blob write per turn, not per message).
  void noteTurn(const std::string& chatId);

  // Evaluate the auto-fold trigger. bytesThreshold==0 disables (manual only).
  // Also due when the history's overflow shift hides rows the summary does not
  // cover yet (softStart after thru). Never due before the chat's nextTry
  // (`nowEpoch` 0 = the clock is not set: the caller keeps its own wait). A
  // thrash refill returns Yes once and schedules nextTry kFoldThrashRetryS out
  // (`thrash`, when given, says so: with no clock the caller keeps that wait).
  FoldDue evaluateDue(const std::string& chatId, uint32_t bytesThreshold,
                      uint32_t msgsThreshold, uint32_t nowEpoch, bool* thrash = nullptr);

  // A fold completed: store the summary (capped at `summaryCap`, the head's cap),
  // reset the counters, stamp the epoch, clear the failure count and a backoff
  // wait (a thrash wait stays), persist. (Write order: this blob FIRST - the
  // caller resets the provider chain only after this returns.) `thru` (H3) is
  // the newest row the summary now covers; a shift it covers is cleared.
  void applyFold(const std::string& chatId, const std::string& summary,
                 uint32_t epoch, const std::string& thru = std::string(),
                 size_t summaryCap = kChatSummaryMax);

  // A fold cycle failed (every rung): the chat's next try is scheduled by the
  // backoff (foldBackoffS) and the failure is counted. Returns nextTry (0 when the
  // clock is not set: the caller keeps the wait in RAM). Persisted.
  uint32_t noteCycleFailed(const std::string& chatId, uint32_t nowEpoch);

  // A new firmware is running: every chat's wait restarts (the update may have
  // fixed what failed), so a chat with a reason to fold folds on the next pass.
  void resetWaits();

  // Every chat with a fold state, least recently used first.
  std::vector<std::string> chatIds() const;

  // Reactive path: a provider context-overflow error was classified on this chat
  // - force it over any byte threshold so the next pump pass folds it. The
  // existing zero-tools fresh-retry already recovered the TURN; this recovers
  // the trajectory. Persisted.
  void markOverflow(const std::string& chatId);
  // A served turn left out conversation history to fit its request (turn_fit.h): the chat
  // needs its fold. Inside the thrash window of the last fold it does not count: large payloads
  // are no proof that the fold failed to shrink the chat, so no pause and no alert.
  void noteHistoryTrimmed(const std::string& chatId);
  // An overflow fold is queued for the chat (the next turn needs it, so it does
  // not wait for a quiet moment).
  bool overflowQueued(const std::string& chatId) const {
    return get(chatId).bytesSinceFold >= kOverflowMarkBytes;
  }
  static constexpr uint32_t kOverflowMarkBytes = 1u << 20;   // over any compactKB

  size_t chatCount() const { return chats_.size(); }
  std::string serialize() const;               // exposed for tests/diagnostics

 private:
  struct Slot { std::string chatId; ChatFold f; };
  Slot* find(const std::string& chatId);
  const Slot* find(const std::string& chatId) const;
  Slot& upsert(const std::string& chatId);     // LRU: oldest evicted at kFoldMaxChats
  static std::string encodeSlot(const Slot& s);  // one record, for the byte budget
  void  persist();
  std::vector<Slot> chats_;                    // back = most recently touched
  FoldStoreIO* io_ = nullptr;
};

}  // namespace orch
}  // namespace nimbus
