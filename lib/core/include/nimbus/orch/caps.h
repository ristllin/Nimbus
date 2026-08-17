#pragma once
#include <cstdint>
#include <cstddef>

// Ported from Nuage-Solide include/config.h (Head Orchestrator v2) - the byte
// and concurrency ceilings of the orchestrator brain, relocated into portable
// lib/core so the host tests own the numbers and never depend on device config.
// The device nimbus_config.h may re-export these; this header is the source of
// truth for the portable brain.
namespace nimbus {
namespace orch {

// --- Memory caps (bytes, UTF-8-safe; the device enforces them, never the model) ---
// User directive: user-owned, immutable by the model. Always injected first.
constexpr int kMemDirectiveMax = 600;
// Model running memory: the model maintains it via the `memory` turn field. It
// is the ONLY cross-turn / cross-provider state, so the cap must be UTF-8-safe.
constexpr int kMemModelMax = 1200;

// --- Journal / concurrency ceilings (three DISTINCT numbers - do not conflate) ---
// Journal capacity: records tracked (Queued..terminal) across reboot.
constexpr int kAgentMaxJobs = 6;
// Max simultaneously-dispatched heavy jobs. Enforced by the DEVICE turn loop
// (one dispatched per poll cycle while count() < this); portable code only
// exposes count() so the device can gate on it. Distinct from kAgentMaxJobs.
constexpr int kMaxActiveInflight = 4;   // v2.0.0: raised with the 2-slot TLS arbiter
// Pending-spawn QUEUE depth - how many spawns one turn may enqueue at once. This
// is deliberately DECOUPLED from kAgentMaxJobs (concurrency): the queue drains
// ONE dispatch per poll cycle into the ≤kMaxActiveInflight concurrency window, so
// a deep fan-out (a research run spawning a whole wave) is accepted and runs
// SEQUENTIALLY over waves - NOT concurrently. It is NOT extra parallelism; it is
// a bigger inbox for the same single-file worker (⚠ never raise the concurrency
// caps above - see AGENTS.md / nimbus-no-subagent-concurrency). Bounded so the
// PendingSpawn vector's internal-SRAM backing stays small (~314 B/entry, released
// when the queue drains); waves beyond this come from the next synthesis turn.
constexpr int kMaxPendingSpawns = 12;   // v4.1.0 (W5): was conflated with kAgentMaxJobs (6)

// Stuck-job watchdog (2026-08-12 field: "orange breathing ring for hours" - a
// mistral-accent Running arc; orchestrator arcs have NO ambient expiry, and a
// job whose polls never reach a terminal state breathed forever):
//  - a job whose polls have returned ONLY transient errors for this long is
//    terminated honestly (the provider stopped answering about it);
constexpr uint32_t kJobPollGiveUpMs = 15u * 60 * 1000;
//  - a job still reporting "running" past this age keeps polling but its ring
//    arc PARKS to a dim static segment (ambient grammar: nothing persistent may
//    breathe indefinitely; completion still fires the normal Done/Error cues).
//    Mirrors the head arc's 30-min frozen-children backstop.
constexpr uint32_t kJobArcParkMs = 30u * 60 * 1000;

// --- Spawn item field caps (mirror the device PendingSpawn buffer sizes) ---
// A spawn task's natural-language instruction; the device buffer is task[1024]
// (usable 1023 + NUL device-side, like every other spawn field). Raised from 420
// (Glass Box A6): ~60 words was the real ceiling on delegation quality, and the
// strncpy truncation was SILENT - enqueueSpawn now logs when it clips.
constexpr int kSpawnTaskMax = 4096;   // v4.0.0: was 1024 - OUR buffer, not a provider
                                      // limit; briefs are still bounded by the terminal
                                      // turn's output budget. PendingSpawn.task is a
                                      // PSRAM-backed std::string now, not a char array.
constexpr int kSpawnProjectMax = 25;  // FileStore project segment (24 usable) - tags a
                                      // fan-out run; sub-results auto-persist under it
constexpr int kSpawnAttachMax = 4;    // max attached docs per spawn (device splices
                                      // their content into the instruction at dispatch)
constexpr int kSpawnNameMax = 24;   // short display name (mirrors JobRecord.name)

// --- Per-chat conversation window (Release B1) ---
// min(N messages, C bytes) of the turn's own chat, rendered oldest-first into
// the "## RECENT CONVERSATION" prompt section. Zero flash reads (PSRAM ring).
constexpr int    kRecentTurnsMax  = 12;
constexpr size_t kRecentConvBytes = 3000;
constexpr int kSpawnProviderMax = 16;   // usable 15 + NUL device-side
constexpr int kSpawnModelMax = 40;      // usable 39
constexpr int kSpawnCategoryMax = 16;   // usable 15
constexpr int kSpawnSkillMax = 24;      // usable 23
constexpr int kSpawnNoteMax = 80;       // usable 79

// --- Scratchpad caps (Part B: the model's own working memory) ---------------
// A structured, model-owned scratchpad (short/mid/long-term goals + an
// active-task line) the model rewrites as it works. The device has NVS/RAM
// limits, so every tier is byte- and count-capped, UTF-8-safe. These are
// DEVICE caps - the model proposes, the device enforces - the same contract
// as the memory caps above.
constexpr int kScratchActiveMax = 240;   // the single "what I'm doing now" line
constexpr int kScratchItemMax   = 160;   // one goal/task item
constexpr int kScratchTierItems = 8;     // items per tier (short/mid/long)

// --- Context-assembly budget (Part B §2) ------------------------------------
// Total byte ceiling for the assembled system prompt. The device budgets BYTES
// for RAM (not tokens). Sections are added in priority order and lower-priority
// ones truncate/drop when the budget is hit.
constexpr int kContextBudgetMax = 32768;

// --- Local Loops caps (hard ceilings; the owner may only make them STRICTER via
// clamped NVS overrides, never looser; the model can never touch them). Every
// number here is a circuit breaker - enforced by the pure evaluate(), never LLM-
// judged. Token caps are real (Phase 0 TokenUsage), summed across tool-loop
// rounds + attributed sub-agent/synthesis spend. ---------------------------------
constexpr int      kLoopMaxCount        = 8;       // most loops that can exist at once
constexpr uint32_t kLoopMinIntervalSec  = 300;     // no loop fires faster than every 5 min
constexpr int      kLoopMaxFiresPerDay  = 24;      // per-loop daily fire ceiling
constexpr uint32_t kLoopMaxTokensPerDay = 120000;  // per-loop daily token ceiling
constexpr uint32_t kLoopDevTokensPerDay = 400000;  // device-wide daily token ceiling
constexpr int      kLoopDevFiresWindow  = 6;       // device-wide fires per window (rate limit)
constexpr uint32_t kLoopWindowSec       = 600;     // the rate-limit window (10 min)
constexpr int      kLoopMaxConsecFails  = 3;       // auto-disable a loop after N failures
constexpr int      kLoopMaxRepeats      = 5;       // pause a loop after N identical replies
constexpr int      kLoopNameMax         = 24;      // display name (mirrors kSpawnNameMax)
constexpr int      kLoopPromptMax       = 2048;    // stored loop prompt (a scheduled task needs
                                                   // room for real instructions; enforced at
                                                   // create AND load AND ensure)

}  // namespace orch
}  // namespace nimbus
