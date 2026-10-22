#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/hooks.h"   // DreamEndEv - the night's one timeline row
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/memory_tools.h"   // MemOpCounters - what the reflection's writes did
#include "nimbus/orch/turn.h"           // Turn - the dream turn's field restriction
#include "nimbus/orch/vector_memory.h"  // maintainVectors

// dream - DREAMING, the pure core of the reserved nightly maintenance +
// reflection loop. The dream is an ordinary Local Loop record fired through the
// ordinary scheduler under all the existing LoopCaps + scheduled-turn refusal
// rails; what this header owns is every DECISION the device glue
// (src/agent/dream_subsystem) must not: the reserved-loop identity + delete
// refusal, the fire-time idle gate, and the [DREAM] prompt assembly (episodic
// digest + scratchpad + memory stats), all host-tested (test_harness_dream).
//
// Two phases per firing (device glue sequences them, this header shapes them):
//   1. non-LLM maintenance FIRST, always runs - vector decay/TTL-prune/dedup
//      (the VectorMemory engine's own ops) + persist;
//   2. a reflection turn - an ordinary injectScheduledTurn whose inputs are
//      buildDreamInputs(); the model's EXISTING turn contract (mem_write /
//      mem_query / memory.scratchpad / `memory`) IS the dream API. No new
//      model-facing surface.
namespace agent {
namespace dream {

// --- reserved loop identity --------------------------------------------------
// Well-known id: main.cpp ensures the record exists at loops::begin time (via
// loops::ensureLoop) and every delete surface refuses it (MCP loop.cancel, web
// delete, Telegram /loop deny). CREATED ENABLED by design: dreaming is
// owner-shipped firmware behavior (the owner asked for it in this build), and
// it stays trivially pausable from the existing owner surfaces (/loop off
// dream, web Loops tab pause) - a pause persists like any loop's, and
// ensureLoop never overrides persisted owner state.
inline const char* kLoopId = "dream";
constexpr uint16_t kDefaultMinuteOfDay = 210;  // 03:30 local (daily wall-clock)
constexpr uint32_t kDeferSec = 900;            // idle-gate defer: retry in 15 min
constexpr size_t kDigestCapBytes = 8192;       // episodic digest byte budget

bool isReserved(const std::string& id);
// "" when `id` may be cancelled; else the owner/model-facing refusal reason
// (shared by the loop.cancel tool handler and the owner surfaces).
std::string cancelRefusal(const std::string& id);
// The reserved record template: daily 03:30 local, owner-created, approved,
// enabled, with a short owner-facing prompt (the REAL inputs are built at fire
// time by buildDreamInputs - the stored prompt is documentation).
nimbus::orch::LoopRecord reservedLoopRecord();

// --- idle gate (pure) --------------------------------------------------------
// Evaluated at fire time with injected device facts. Not idle => the scheduler
// defers the loop (nextRun += deferSec via nimbus::orch::deferLoop) WITHOUT
// counting a fire. NOTE an on-charger preference was considered and SKIPPED:
// no battery/charging accessor exists in the harness contracts (Platform is
// nowMs/freeHeap/delayMs only) and plumbing device power state through for a
// preference isn't worth it - the loop caps + this gate bound the cost anyway.
// The busy-device defers are BOUNDED (lane M): an interval routine, a long owner
// session or a job stuck "running" could hold the dream off all night with no
// record. After maxDefers busy defers (3 h at 900 s) the gate stops deferring for
// "recent-turn" and "active-jobs"; "low-heap" always defers - it is the safety floor.
struct DreamGate {
  uint32_t minQuietMs = 10u * 60u * 1000u;  // no turn ended within 10 min
  uint32_t minHeap = 30000;                 // ORCH_AUTO_TURN_MIN_HEAP-equivalent
  bool requireNoJobs = true;                // no live sub-agent sessions
  uint32_t deferSec = kDeferSec;
  uint8_t maxDefers = 12;                   // busy defers per night before the gate passes
};
struct GateInputs {
  uint32_t nowMs = 0;
  // millis() when the last turn ENDED; 0 = none since boot, which deliberately
  // counts the boot itself as activity (no dreaming in the first quiet window).
  uint32_t lastTurnEndMs = 0;
  int activeJobs = 0;
  uint32_t freeHeap = 0;
  uint8_t defersTonight = 0;   // busy defers already taken this local night
};
struct GateResult {
  uint32_t deferSec = 0;      // 0 => idle: fire now
  const char* why = nullptr;  // "recent-turn" | "active-jobs" | "low-heap"
};
GateResult evaluateGate(const DreamGate& g, const GateInputs& in);

// --- dream prompt (pure) -----------------------------------------------------
struct MemStats {
  int vectors = 0;       // VDB size after maintenance
  int pruned = 0;        // removed by pruneExpired this dream
  int deduped = 0;       // removed by deduplicate this dream
  int scratchItems = 0;  // scratchpad item count
  int episodicMsgs = 0;  // episodic store size
};

// One line per message, hard byte-capped. `msgs` arrive most-recent-first
// (MsgQuery order); the digest renders oldest-first so it reads as a narrative,
// and when the budget clips it is the OLDEST lines that drop (a truncation
// marker notes the omission). Empty input yields an explicit "(no episodic
// messages ...)" placeholder so the prompt never has a dangling header.
// Should tonight's PAID reflection turn be skipped? True only for a provably
// quiet day: empty 24 h digest + scratchpad hash unchanged since the last
// dream (lastHash 0 = no baseline -> run). force (the console DREAM drill)
// always runs. Pure - the truth table is host-tested.
bool skipReflection(bool digestEmpty, uint64_t scratchHash, uint64_t lastHash,
                    bool force);

std::string buildEpisodicDigest(const std::vector<nimbus::orch::EpisodicMessage>& msgs,
                                size_t capBytes = kDigestCapBytes);

// The [YESTERDAY] read of the nightly reflection - the turn that runs as the
// OWNER and writes what it reads into long-term memory. The trailing 24 h of the
// owner's conversation in `sessions` (the owner's local surfaces, "system" and
// the owner's Telegram chats; a member's history stays in their conversation):
// Message rows plus the media kinds (a photo's row IS its description), never
// the ToolOutput / LlmResponse trace rows (last night's own reflection captured
// those, and they made every night look busy). Two kinds of message row are not
// conversation and are left out, exactly like the head's window, the sub-agent
// brief and the fold leave them out:
//   * captured job-engine deliveries (role "tool", src:jobs - spawn acks,
//     failures and raw fallbacks carrying sub-agent WEB CONTENT: an injection
//     path into long-term memory if read here);
//   * /clear boundary rows (a device marker, nobody's words).
// Not cut at a /clear boundary: the assistant starts that chat afresh, but the
// day still happened (memory.episodic still searches cleared history too).
// `empty` = nothing of the owner's in 24 h - the quiet-night signal that skips
// the paid reflection turn, so job rows alone must keep it true. The read pages
// past skipped rows (a busy routine's spawn acks must not crowd the owner's words
// out of the newest page), bounded at kYesterdayPages pages.
constexpr int kYesterdayRows = 48;    // rows the digest may hold (and the page size)
constexpr int kYesterdayPages = 4;    // at most 192 rows read per night
struct Yesterday {
  std::string digest;   // buildEpisodicDigest over the kept rows
  bool empty = true;
};
Yesterday readYesterday(const nimbus::orch::EpisodicStore& store, uint32_t nowHours,
                        const std::vector<std::string>& sessions);

// The per-chat RECENT CONVERSATION window (CUM-485). On a stateless tool-loop
// turn the provider keeps no thread, so this window is the head's ONLY view of
// the previous reply - and the digest above (one flattened 300 B line per row)
// cut a code answer before its variables were defined, so a "now add a plot"
// follow-up guessed the names. `msgs` arrive newest-first; the rule:
//   1. NEWEST ROWS WHOLE FIRST: walking newest-first, a row is kept whole -
//      verbatim, line breaks kept, continuation lines indented two spaces so the
//      row still starts with "- role: " - whenever it fits the remaining budget.
//   2. The newest row, if it alone exceeds the budget, keeps its head (UTF-8
//      safe), marked "...".
//   3. Any other row that does not fit whole falls back to the one-line preview
//      (every line break flattened, clipped to kWindowPreviewBytes) if THAT fits.
//   4. The first row that fits in neither form ends the window, and
//      kConvOmittedNote leads the block.
// Rendered oldest-first; "" for no rows. `capBytes` is the head's derived
// ContextBudget::recentConvBytes; assembleContext then trims whole oldest rows
// if the system prompt has less room left than that.
constexpr size_t kWindowPreviewBytes = 300;
std::string buildConversationWindow(const std::vector<nimbus::orch::EpisodicMessage>& msgs,
                                    size_t capBytes);

// The [DREAM] input block: instructions (distill up to 7 durable mem_write facts (0 on a quiet day),
// groom stale scratchpad goals, refresh the running `memory` summary, flag
// contradictions, reply "" unless the owner is genuinely needed) + [MEMORY
// STATS] + the scratchpad block (as rendered by Scratchpad::appendPromptBlock,
// "" skips it) + [YESTERDAY] digest (defensively re-capped to kDigestCapBytes).
// memoryWritesDown: long-term memory writes cannot land tonight (embeddings are
// unavailable) - the block says so in one line, so the turn grooms the scratchpad
// and the running summary instead of burning rounds on failing memory calls.
std::string buildDreamInputs(const std::string& episodicDigest,
                             const std::string& scratchpadSummary,
                             const MemStats& stats, bool memoryWritesDown = false);

// --- the dream turn's surface (pure; lane M) ---------------------------------
// The dream reads and writes memory and nothing else: no files, sessions, web,
// routines, replies or device actions at 03:30 (it once spent its turn polling for
// jobs that never existed, and could have taken any external action).
// toolAllowed: the registry tools a dream turn may see and call.
bool toolAllowed(const std::string& registryName);
// restrictTurn: drop the orch_turn fields a dream turn may not act on (device[],
// spawn[], await[], session_ops[]); reply / ask / memory / scratchpad / mem_write /
// mem_query stay. Returns how many items were dropped.
int restrictTurn(nimbus::orch::Turn& t);

// --- one night, end to end (pure; lane M) ------------------------------------
// The whole night pipeline lives here so the host tests drive the shipping
// sequence; src/agent/dream_subsystem.cpp only binds the device seams.
//
// Outcome of a night's dream, as the timeline row reports it:
//   Ran      - the reflection turn ran and every memory write it made landed;
//   Skipped  - a provably quiet night (no owner conversation, scratchpad unchanged);
//   Deferred - not run yet (low memory or the device busy); retried this night;
//   Failed   - the reflection turn failed (cause names why);
//   Degraded - the turn ran but memory writes failed, or could not be attempted;
//   Starved  - deferred until the night window closed; no reflection tonight.
enum class DreamOutcome : uint8_t { Ran = 0, Skipped, Deferred, Failed, Degraded, Starved };
const char* outcomeSlug(DreamOutcome o);   // "ran" | "skipped" | "deferred" | ...

// The cause class of a failed or deferred reflection, from FireOutcome.error:
// "low-memory", "timeout", "network", "rate-limit", "provider-5xx", "billing",
// "key", "budget", "parse", "bad-request", "other". Retryable tonight: the
// transient ones (low-memory, timeout, network, rate-limit, provider-5xx).
std::string causeOf(const std::string& error, bool deferred);
bool causeRetryable(const std::string& cause);

constexpr uint16_t kWindowEndMinute = 420;   // 07:00 local: no reflection retry after this
// The night window for a slot: 3.5 h from it (07:00 for the default 03:30), so an
// owner-moved schedule keeps the same retry window.
uint16_t windowEndFor(uint16_t slotMinute);
constexpr uint8_t  kMaxAttempts = 3;         // reflection attempts per night
constexpr uint8_t  kAlertStreak = 3;         // one owner alert at 3 failed nights in a row,
constexpr uint8_t  kAlertEvery = 7;          // then one every 7 more

// Dream state, persisted by the device in /data/dream.json (LittleFS; about 150 B).
// It replaced the NVS key dreamScrHash, which is read once to migrate and never
// written again (NVS is full on real devices).
struct DreamState {
  uint32_t maintDay = 0;        // local day maintenance last ran (once per day)
  uint64_t scrHash = 0;         // scratchpad fnv64 after the last reflection (quiet-skip baseline)
  uint32_t attemptDay = 0;      // local day attemptsTonight counts for
  uint8_t  attemptsTonight = 0;
  uint32_t streakDay = 0;       // local day the streak last moved
  uint8_t  streak = 0;          // nights in a row that failed or degraded
  DreamOutcome lastOutcome = DreamOutcome::Ran;
  std::string lastCause;
  // In-flight marker (L19-31): set just before the reflection turn starts, cleared
  // by the outcome write. A boot that finds it holds an attempt whose outcome never
  // landed - the device restarted mid-reflection - and reconciles it
  // (reconcileBoot). Serialized ONLY while set, so a completed night's file is
  // byte-identical to the marker-less format.
  uint32_t inflightNight = 0;   // local day of the attempt (0 = no marker)
  uint8_t  inflightAttempt = 0; // its 1-based attempt number
  uint64_t inflightStart = 0;   // UTC epoch s the turn started
};
std::string dumpDreamState(const DreamState& s);
// false = unreadable: not a JSON object, or larger than kStateFileMax (a torn or
// foreign file is never half-trusted). A field of the wrong type reads as absent
// and every counter is clamped, so a file another firmware wrote loads safely.
constexpr size_t kStateFileMax = 1024;   // the state is about 200 B with a marker
bool loadDreamState(const std::string& json, DreamState& out);
// The legacy NVS value (16 hex digits, "" when unset) as a fresh state's baseline.
DreamState stateFromLegacyHash(const std::string& hex);

// Phase 1, non-LLM: decay, TTL prune, windowed dedup, persist. The device runs it
// under the memory lock; `persist` reports whether the store landed.
struct MaintResult {
  bool ran = false;
  int pruned = 0, deduped = 0, vectors = 0;
  bool persisted = false;
};
MaintResult maintainVectors(nimbus::orch::VectorMemory& v, float decayFactor, uint32_t nowHours,
                            int dedupWindow, const std::function<bool()>& persist);

struct NightClock {
  uint64_t epoch = 0;
  uint32_t localDay = 0;      // local civil day number (changes at local midnight)
  uint16_t minuteOfDay = 0;   // local minute of day
  bool     synced = false;    // wall clock trustworthy (maintenance needs it)
  uint16_t slotMinute = kDefaultMinuteOfDay;   // the loop's scheduled minute of day
};

struct NightReport {
  DreamOutcome outcome = DreamOutcome::Ran;
  std::string cause;            // class slug ("" when nothing failed)
  std::string error;            // the turn's error text ("" when ok)
  MaintResult maint;            // this fire's maintenance (ran=false on a same-night retry)
  bool maintDoneToday = false;  // maintenance had already run this local day
  nimbus::orch::MemOpCounters ops;   // the reflection's own memory writes
  bool memoryWritesDown = false;
  bool scratchChanged = false;
  uint32_t tokens = 0;          // the reflection turn's own billed tokens
  uint8_t attempt = 0;          // 1-based reflection attempt tonight (0 = none ran)
  uint32_t retryAfterSec = 0;   // > 0: retry tonight in this many seconds
  uint16_t retryAtMinute = 0;   // local minute of that retry
  uint8_t streak = 0;           // failed or degraded nights in a row, after tonight
  bool alert = false;           // tonight crossed an alert point (owner alert due)
  bool stateSaved = true;       // /data/dream.json landed
  std::string stateWhy;         // why it did not ("storage not mounted", "storage write failed")
  std::string held;             // what the reflection had for the owner, held (FireOutcome.held)
};

// The boot's working copy of the dream state. RAM is the authority between fires and
// the file is durability across a restart: with a store that is not mounted or refuses
// writes, the attempt count, the maintenance day and the streak still move, so the
// night stays at kMaxAttempts reflections, decay never runs twice in a day and the
// streak alert still comes. (Re-reading the file at every fire let a failing store
// reset all three each time.)
struct DreamMemo {
  bool loaded = false;
  DreamState st;
};

struct NightDeps {
  std::function<bool(DreamState&)> loadState;          // false = no saved state
  std::function<bool(const DreamState&)> saveState;    // false = not saved
  std::function<bool()> storeReady;                    // false = not mounted: no write is tried
  DreamMemo* memo = nullptr;                           // the boot's working copy (null = reload each fire)
  std::function<MaintResult()> maintain;
  std::function<Yesterday()> yesterday;
  std::function<std::string()> scratchBlock;           // Scratchpad::appendPromptBlock
  std::function<uint64_t()> scratchHash;               // fnv64 of the serialized scratchpad
  std::function<MemStats()> stats;
  std::function<bool()> embeddingsUp;
  std::function<nimbus::orch::MemOpCounters()> memOps;
  std::function<nimbus::orch::FireOutcome(const std::string& inputs)> injectTurn;
  // The night's ev:dream row: called exactly once per runNight, on every exit (ran,
  // skipped, deferred, failed, degraded; a store that is not mounted or refuses the
  // state save included). The device binds it to the timeline hook, which writes
  // renderDreamRow(*ev.report) to the episodic store on the card - never to NVS.
  std::function<void(const agent::DreamEndEv&)> emitRow;
};
struct NightResult {
  nimbus::orch::FireOutcome fire;   // what the scheduler sees (retryAfterSec, per-night detail)
  NightReport report;
};
// force = the console DREAM drill: run the reflection even on a quiet night.
NightResult runNight(const NightDeps& d, const NightClock& c, bool force);
// The timeline event for a night's report: one builder for the fire, the starved night
// and the boot reconcile, so no path assembles its own row or leaves one out.
// ev.report borrows `rep` (valid while the hook runs).
agent::DreamEndEv dreamEndEvent(const NightReport& rep, bool turnOk);

// --- a night the restart cut short (L19-31) -----------------------------------
// The scheduler persists lastRun before a fire and the outcome only after it
// returns, so a restart mid-reflection left the loop record holding the PREVIOUS
// night's verdict under the new lastRun, no timeline row, and no retry inside the
// night's window. The marker closes the hole: runNight sets it just before the turn
// and the outcome write clears it, so a boot can tell "the night never finished"
// from "the night never ran".
struct InterruptedNight {
  bool interrupted = false;   // the marker named an attempt whose outcome never landed
  NightReport report;         // the reconciled outcome: row, retry, streak, lastErr
};
// Reconcile at boot, over the pure night state. A marker with no outcome becomes
// the night's verdict: fail, with the lastErr sentence "the device restarted
// during this run" in report.error. `st` comes back with the verdict recorded
// (lastOutcome/lastCause), the attempt bookkeeping restored to the marker's night,
// and the marker cleared; the streak moves exactly once for that night (a second
// boot of the same interruption moves nothing). report.retryAfterSec asks for a
// retry while the marker's night is still the current local night and its window
// (windowEndFor) is still open; the interrupted attempt is one of the bounded
// retries. The caller persists `st` BEFORE applying the loop side, so a boot whose
// save fails moves no counter and can try again. Call with a synced clock. A marker
// cannot be from the future (one dated after today is anchored to today, so no later
// night's streak or attempts are touched) and always names a started attempt (an
// attempt of 0 reads as 1).
InterruptedNight reconcileBoot(DreamState& st, const NightClock& c);
// The cause of a night the restart cut short: the dream's lastErr, the routine
// record's lastErr and /api/state dream.err all carry this sentence.
inline constexpr const char* kRestartedError = "the device restarted during this run";
// The loop-record half of that outcome (the scheduler's post-fire write never
// ran): lastResult fail with lastErr kRestartedError, and consecFails +1, once -
// the caller clears the marker durably first - plus nextRun at the retry (0 = keep
// the boot's next slot).
void applyInterruptedToLoop(nimbus::orch::LoopRecord& l, uint64_t nowEpoch,
                           uint32_t retryAfterSec);

// --- the boot reconcile driver (pure; program-2026-10-18 PG4) -------------------
// The device's pumpBootReconcile, host-tested end to end. It runs once its seams are
// up - the state store mounted, the local clock real, and the timeline and the loop
// records it writes to ready - and until then it waits, so a pass that comes early
// asks again instead of giving up on a marker it could not read yet. Then:
//   - the state is the working copy a fire already opened this boot (never re-read
//     over it), else the file; a file that is torn, oversized or not JSON holds no
//     marker, and nothing is invented for it; a day stamp dated after today (another
//     clock wrote it) reads as never;
//   - a stale marker is cleared with no row and no counter: the reserved loop ran
//     after the marker's attempt started (markerStale), so a later fire - on this
//     firmware or another one - already put that night or a newer one on record;
//   - an interrupted night is saved first (the marker cleared durably), then the
//     loop side and the row are handed over exactly once;
//   - a store that keeps refusing the save is given up on after kBootSaveTries passes
//     with nothing counted (the marker stays for a boot with a working store).
// Every seam may be null; no path throws or loops.
constexpr uint8_t  kBootSaveTries = 10;
constexpr uint32_t kMarkerStaleSlackSec = 300;   // a fire's lastRun precedes its marker by seconds
constexpr uint64_t kSyncedEpochMin = 1600000000; // the device's "clock is set" floor (Sep 2020)
// The marker names an attempt older than the reserved loop's last fire. A marker
// started under a clock that was never set (a console drill before the time sync)
// has no comparable start, so it is never judged stale: it is reconciled.
bool markerStale(const DreamState& st, uint64_t loopLastRun);

struct BootDeps {
  std::function<bool()> storeReady;    // the state file's store is mounted
  std::function<bool()> clockSynced;   // the night and its window need the local clock
  std::function<bool()> sinksReady;    // the timeline and the loop records are up
  std::function<bool(DreamState&)> loadState;          // false = no file, or unreadable
  std::function<bool(const DreamState&)> saveState;    // false = not saved
  std::function<uint64_t()> loopLastRun;               // the reserved loop's lastRun (0 = none)
  std::function<void(const NightReport&)> applyLoop;   // the loop side, once per interruption
  std::function<void(const NightReport&)> publish;     // status and the timeline row, once
  DreamMemo* memo = nullptr;                           // this boot's working copy
};
enum class BootStep : uint8_t {
  Waiting = 0,  // a seam is not up yet: asked again on the next pass
  Clean,        // no marker: the working copy is primed
  Reconciled,   // the interrupted night is on record (row, counters, retry)
  Stale,        // a later fire superseded the marker: cleared, nothing counted
  SaveFailed,   // the marker could not be cleared durably: tried again next pass
  GaveUp,       // kBootSaveTries refusals: nothing counted, the marker stays on disk
  Done,         // this boot's reconcile has already finished
};
struct BootReconcile {
  bool done = false;
  uint8_t saveTries = 0;
  BootStep pump(const BootDeps& d, const NightClock& c);
};

// The ev:dream timeline row, at most `cap` bytes, always starting "Nightly dream ".
//   Nightly dream ok: +2 memories, 1 updated; maintenance pruned=0 deduped=0; spent 21544 tokens
//   Nightly dream skipped (quiet day); maintenance pruned=0 deduped=0
//   Nightly dream deferred (low memory), retry 1 of 3 at 03:45
//   Nightly dream FAILED (provider-5xx: <cause>), retry 2 of 3 at 04:00; spent 812 tokens
//   Nightly dream interrupted (the device restarted during the reflection), retry 2 of 3 at 03:45
//   Nightly dream degraded (memory writes failed): 0 of 3 stored, 3 failed; ...
std::string renderDreamRow(const NightReport& r, size_t cap = 240);
// The owner alert when report.alert is set (plain words, no ticket numbers).
std::string streakAlertText(const NightReport& r);
// The one message the owner gets at their next conversation (never at night): the
// streak alert and what the reflection held for them (NightReport.held, clipped to
// kHeldNoteMax bytes, UTF-8 safe), each when there is one. "" = nothing to send.
constexpr size_t kHeldNoteMax = 1500;
std::string ownerNoteText(const std::string& streakAlert, const std::string& heldReply);

// Seconds from `c` until the reserved slot (minuteOfDay `slot`) on the next local day.
uint32_t secondsToNextSlot(const NightClock& c, uint16_t slot);

}  // namespace dream
}  // namespace agent
