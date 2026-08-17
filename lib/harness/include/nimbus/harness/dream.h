#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/loops.h"

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
struct DreamGate {
  uint32_t minQuietMs = 10u * 60u * 1000u;  // no turn ended within 10 min
  uint32_t minHeap = 30000;                 // ORCH_AUTO_TURN_MIN_HEAP-equivalent
  bool requireNoJobs = true;                // no live sub-agent sessions
  uint32_t deferSec = kDeferSec;
};
struct GateInputs {
  uint32_t nowMs = 0;
  // millis() when the last turn ENDED; 0 = none since boot, which deliberately
  // counts the boot itself as activity (no dreaming in the first quiet window).
  uint32_t lastTurnEndMs = 0;
  int activeJobs = 0;
  uint32_t freeHeap = 0;
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

// The [DREAM] input block: instructions (distill up to 7 durable mem_write facts (0 on a quiet day),
// groom stale scratchpad goals, refresh the running `memory` summary, flag
// contradictions, reply "" unless the owner is genuinely needed) + [MEMORY
// STATS] + the scratchpad block (as rendered by Scratchpad::appendPromptBlock,
// "" skips it) + [YESTERDAY] digest (defensively re-capped to kDigestCapBytes).
std::string buildDreamInputs(const std::string& episodicDigest,
                             const std::string& scratchpadSummary,
                             const MemStats& stats);

}  // namespace dream
}  // namespace agent
