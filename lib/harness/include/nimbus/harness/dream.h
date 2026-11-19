#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/hooks.h"   // DreamEndEv - the night's one timeline row
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/vector_memory.h"  // maintainVectors

// dream - the pure core of the reserved nightly memory upkeep. The "dream" is an ordinary
// Local Loop record fired through the ordinary scheduler; what this header owns is every
// decision the device glue (src/agent/dream_subsystem) must not: the reserved-loop identity
// and delete refusal, the fire-time idle gate, and the night itself, host-tested
// (test_harness_dream, test_harness_dream_night).
//
// A night is maintenance only: vector decay, TTL prune and windowed dedup, persisted, once
// per local day. There is no model turn (lane RB-Q2b, v5.2.0): the reflection that wrote
// memories, the running summary and the scratchpad from yesterday's conversation is gone,
// and with it every token the night spent. The conversation itself is untouched.
//
// This header also holds the conversation digest and window builders the head, the
// sub-agent brief, the fold and nimbusd use; they are not part of the night.
namespace agent {
namespace dream {

// --- reserved loop identity --------------------------------------------------
// Well-known id: main.cpp ensures the record exists at loops::begin time (via
// loops::ensureLoop) and every delete surface refuses it (MCP loop.cancel, web
// delete, Telegram /loop deny). CREATED ENABLED by design, and trivially pausable from
// the existing owner surfaces (/loop off dream, web Loops tab pause) - a pause persists
// like any loop's, and ensureLoop never overrides persisted owner state.
inline const char* kLoopId = "dream";
constexpr uint16_t kDefaultMinuteOfDay = 210;  // 03:30 local (daily wall-clock)
constexpr uint32_t kDeferSec = 900;            // idle-gate defer: retry in 15 min
constexpr size_t kDigestCapBytes = 8192;       // episodic digest byte budget

bool isReserved(const std::string& id);
// "" when `id` may be cancelled; else the owner/model-facing refusal reason
// (shared by the loop.cancel tool handler and the owner surfaces).
std::string cancelRefusal(const std::string& id);
// The reserved record template: daily 03:30 local, owner-created, approved, enabled. Its
// prompt is documentation (the night builds no prompt).
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

// --- conversation digest and window (pure) --------------------------------------
// One line per message, hard byte-capped (nimbusd's spawn briefs, the device's sub-agent
// [CONTEXT] block and the legacy fold slice). `msgs` arrive most-recent-first
// (MsgQuery order); the digest renders oldest-first so it reads as a narrative,
// and when the budget clips it is the OLDEST lines that drop (a truncation
// marker notes the omission). Empty input yields an explicit "(no episodic
// messages ...)" placeholder so the prompt never has a dangling header.
std::string buildEpisodicDigest(const std::vector<nimbus::orch::EpisodicMessage>& msgs,
                                size_t capBytes = kDigestCapBytes);

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

// --- one night, end to end (pure) ---------------------------------------------
// Outcome of a night, as the timeline row reports it:
//   Ran      - maintenance ran tonight;
//   Skipped  - maintenance had already run this local day, or the clock is not set;
//   Deferred - not run (the device glue could not allocate the night);
//   Starved  - deferred by the busy gate until the night window closed.
enum class DreamOutcome : uint8_t { Ran = 0, Skipped, Deferred, Starved };
const char* outcomeSlug(DreamOutcome o);   // "ran" | "skipped" | "deferred" | "starved"

constexpr uint16_t kWindowEndMinute = 420;   // 07:00 local: the busy gate gives up after this
// The night window for a slot: 3.5 h from it (07:00 for the default 03:30).
uint16_t windowEndFor(uint16_t slotMinute);

// Dream state, persisted by the device in /data/dream.json (LittleFS; about 20 B): the
// local day maintenance last ran. A file earlier firmware wrote (attempts, streak, the
// in-flight marker, the quiet-night hash) loads with those fields ignored.
struct DreamState {
  uint32_t maintDay = 0;   // local day maintenance last ran (once per day)
};
std::string dumpDreamState(const DreamState& s);
// false = unreadable: not a JSON object, or larger than kStateFileMax (a torn or foreign
// file is never half-trusted).
constexpr size_t kStateFileMax = 1024;
bool loadDreamState(const std::string& json, DreamState& out);

// Decay, TTL prune, windowed dedup, persist. The device runs it under the memory lock;
// `persist` reports whether the store landed.
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
};

struct NightReport {
  DreamOutcome outcome = DreamOutcome::Ran;
  std::string cause;            // a starved night: the gate's reason ("recent-turn", ...)
  MaintResult maint;            // this fire's maintenance (ran=false when skipped)
  bool maintDoneToday = false;  // maintenance had already run this local day
  uint8_t defers = 0;           // a starved night: the busy defers it took
  bool stateSaved = true;       // /data/dream.json landed
  std::string stateWhy;         // why it did not ("storage not mounted", "storage write failed")
};

// The boot's working copy of the dream state. RAM is the authority between fires and the
// file is durability across a restart: with a store that is not mounted or refuses writes,
// the maintenance day still moves, so decay never runs twice in a day.
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
  // The night's ev:dream row: called exactly once per runNight. The device binds it to the
  // timeline hook, which writes renderDreamRow(*ev.report) to the episodic store.
  std::function<void(const agent::DreamEndEv&)> emitRow;
};
struct NightResult {
  nimbus::orch::FireOutcome fire;   // what the scheduler sees (ok, a per-night detail, no tokens)
  NightReport report;
};
NightResult runNight(const NightDeps& d, const NightClock& c);
// The timeline event for a night's report (the fire and the starved night alike).
// ev.report borrows `rep` (valid while the hook runs).
agent::DreamEndEv dreamEndEvent(const NightReport& rep);

// The ev:dream timeline row, at most `cap` bytes, always starting "Nightly dream ".
//   Nightly dream ok: maintenance pruned=0 deduped=1 (412 memories)
//   Nightly dream skipped (maintenance done earlier today)
//   Nightly dream skipped tonight (device busy after 12 tries); maintenance not run tonight
std::string renderDreamRow(const NightReport& r, size_t cap = 240);

// Seconds from `c` until the reserved slot (minuteOfDay `slot`) on the next local day.
uint32_t secondsToNextSlot(const NightClock& c, uint16_t slot);

}  // namespace dream
}  // namespace agent
