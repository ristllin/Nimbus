#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/token_usage.h"

// Local Loops - the PURE, host-tested core. Owns every decision: schedule parsing,
// next-fire math, the fire/skip/block circuit-breaker + cost state machine, and
// (de)serialization. NO Arduino, NO libc tz - the device subsystem converts
// epoch<->local civil time (libc localtime_r/mktime) and drives this. Schedule
// math for Daily/Weekly is done in LOCAL civil space so it stays tz/DST-free and
// trivially testable; the device supplies the local CivilTime + the epoch it maps
// to. See docs plan "Local Loops".

namespace nimbus {
namespace orch {

// --- schedule spec ---------------------------------------------------------
enum class SchedKind : uint8_t { Interval = 0, Daily = 1, Weekly = 2, Once = 3 };

// --- one-shot wakeups (W20, owner design) -----------------------------------
// "it runs something and wants to follow up... a single wakeup... shouldn't
// need user approval, just a tool call" - a Once loop fires ONE turn carrying
// the model's own note, then retires. Bounds: at least 2 minutes out (the tick
// is 20 s-gated; anything sooner is a busy-loop), at most 7 days (past that it
// is a routine and should be one), and at most 4 armed at a time.
constexpr uint32_t kWakeupMinSec     = 120;
constexpr uint32_t kWakeupMaxSec     = 7u * 24 * 3600;
constexpr int      kWakeupMaxPending = 4;

// Approval policy in ONE testable place: agent-created RECURRING loops start
// unapproved (a persistent standing instruction is a prompt-injection
// persistence channel - the owner must bless it), but an agent-created ONCE
// wakeup is auto-approved BY DESIGN: it is a single bounded fire, visible in
// /loops and the web Routines list, cancellable, and it consumes the same
// daily fire/token governor as everything else - chaining wakeups cannot
// outrun the caps a recurring loop lives under.
inline bool autoApproved(SchedKind kind, bool byAgent) {
  return !byAgent || kind == SchedKind::Once;
}

struct SchedSpec {
  SchedKind kind        = SchedKind::Interval;
  uint32_t  intervalSec = 21600;   // Interval: >= caps.minIntervalSec (default 6 h)
  uint16_t  minuteOfDay = 510;     // Daily/Weekly: 0..1439 LOCAL wall-clock (08:30)
  uint8_t   weekMask    = 0x7F;    // Weekly: bit0=Sun .. bit6=Sat; Daily ignores it
};

// --- record ----------------------------------------------------------------
enum class LastResult : uint8_t { None = 0, Ok, Fail, Skipped, Paused };
enum class CreatedBy  : uint8_t { Owner = 0, Agent = 1 };

struct LoopRecord {
  std::string id;       // "lp" + 6 hex - stable, owner/model-facing
  std::string name;     // <= kLoopNameMax
  std::string prompt;   // <= kLoopPromptMax - fired as the turn input
  std::string chatId;   // target channel ("" => owner default)
  SchedSpec   sched;
  CreatedBy   createdBy = CreatedBy::Owner;
  bool        enabled   = true;
  bool        approved  = true;    // agent-created loops start false (owner must approve)

  // runtime state (persisted; survives reboot)
  uint64_t    nextRun     = 0;     // UTC epoch s; 0 => (re)compute at begin/boot
  uint64_t    lastRun     = 0;     // UTC epoch s
  LastResult  lastResult  = LastResult::None;
  uint16_t    firesToday  = 0;
  uint32_t    tokensToday = 0;
  uint32_t    lastSyncedDay = 0;   // wall-day the today-counters belong to (0 = never synced)
  uint8_t     consecFails = 0;     // circuit breaker
  uint64_t    lastReplyHash = 0;   // FNV-64 of last reply (semantic-repeat guard)
  uint8_t     repeatRun   = 0;     // consecutive identical replies
};

// --- caps (runtime; seeded from caps.h. Owner may only TIGHTEN.) ------------
struct LoopCaps {
  int      maxCount        = kLoopMaxCount;
  uint32_t minIntervalSec  = kLoopMinIntervalSec;
  int      maxFiresPerDay  = kLoopMaxFiresPerDay;
  uint32_t maxTokensPerDay = kLoopMaxTokensPerDay;
  uint32_t devTokensPerDay = kLoopDevTokensPerDay;
  int      devFiresWindow  = kLoopDevFiresWindow;
  uint32_t windowSec       = kLoopWindowSec;
  int      maxConsecFails  = kLoopMaxConsecFails;
  int      maxRepeats      = kLoopMaxRepeats;
};

// --- device-wide counters (rolled daily / per rate-window) -----------------
struct DeviceCounters {
  uint32_t tokensToday   = 0;
  uint16_t firesInWindow = 0;
  uint64_t windowStart   = 0;   // epoch s the current rate window opened
  uint32_t dayNumber     = 0;   // wall-day the device counters belong to
};

// --- the pure fire decision (HARD-CODED breakers, never LLM-judged) --------
enum class FireDecision : uint8_t {
  Fire = 0,
  SkipNotDue,
  BlockedDisabled,
  BlockedUnapproved,     // agent-created, awaiting owner approval
  BlockedChatRevoked,    // target chat no longer allowlisted (fire-time re-check)
  BlockedConsecFails,    // auto-disabled after N failures
  BlockedLoopFires,      // per-loop daily fire ceiling hit
  BlockedLoopTokens,     // per-loop daily token ceiling hit
  BlockedDeviceTokens,   // device-wide daily token ceiling hit
  BlockedRateWindow,     // device-wide rate window full
  BlockedNoClock,        // Daily/Weekly loop but the clock isn't synced yet
};

// The device supplies the fire-relevant facts (clockValid, chatAllowed) so the
// core stays pure. Returns the single decision for THIS loop at THIS tick.
FireDecision evaluate(const LoopRecord& l, const DeviceCounters& dev,
                      const LoopCaps& caps, uint64_t nowEpoch,
                      bool clockValid, bool chatAllowed);

// --- executor seam (Local now; Remote deployment later drops in here) ------
struct LoopFireRequest {
  std::string id, name, chatId, prompt;
  uint64_t scheduledFor = 0;
  bool once = false;   // Once wakeup - the executor renders a [WAKEUP] preamble
                       // (the recurring "[SCHEDULED LOOP]" text would LIE here)
  bool ownerReminder = false;  // owner-set one-time /remind - [REMINDER] framing
                               // (the OWNER scheduled it, not the model itself)
};

// Parse a human duration ("45s", "30m", "2h", "1d", or a bare number = minutes)
// to seconds. Returns -1 on a malformed/empty/unknown-unit string, and clamps a
// clearly-oversized value to just past the 7-day ceiling so the caller's bounds
// check rejects it cleanly (never an overflow). Powers the /remind command.
long parseDurationSecs(const std::string& s);
struct FireOutcome {
  bool        ok = false;
  TokenUsage  tokens;    // real provider spend for the fire (Phase 0)
  std::string detail;    // reply text / error - feeds the semantic-repeat hash
};
struct LoopExecutor {
  virtual ~LoopExecutor() = default;
  virtual FireOutcome fire(const LoopFireRequest&) = 0;
};

// --- civil-time math (pure; Howard Hinnant algorithms) ---------------------
// wday: 0=Sunday .. 6=Saturday.
struct CivilTime { int year = 1970, month = 1, day = 1, hour = 0, min = 0, sec = 0, wday = 4; };
uint32_t  daysFromCivil(int y, unsigned m, unsigned d);   // y/m/d -> epoch-day number
CivilTime civilFromDays(uint32_t dayNum);                 // epoch-day -> y/m/d (+wday)

// --- schedule math (pure) --------------------------------------------------
uint64_t  nextIntervalEpoch(const SchedSpec& s, uint64_t base);
// Next Daily/Weekly LOCAL civil occurrence STRICTLY AFTER nowLocal. The device
// maps the result to epoch via libc mktime. No-backfill lives here: the result
// is always in the future, so a stale target skips to its next occurrence.
CivilTime nextWallClockLocal(const SchedSpec& s, const CivilTime& nowLocal);
bool      isDue(uint64_t nextRun, uint64_t nowEpoch);

// --- validation + state machine (pure) -------------------------------------
// Parse+validate a `schedule` JSON object into a SchedSpec (min-interval floor,
// ranges, non-empty weekMask). Returns false + err on a bad spec.
// What to do with a Once loop after its single fire: retire it - except ONE
// short retry when the turn itself failed (a wakeup silently lost to a
// transient provider error would break the model's follow-up promise); a
// second failure retires it and the caller alerts the owner.
enum class OnceAfterFire : uint8_t { NotOnce = 0, Retire, RetryShort };
inline OnceAfterFire onceAfterFire(const LoopRecord& l, bool ok) {
  if (l.sched.kind != SchedKind::Once) return OnceAfterFire::NotOnce;
  if (!ok && l.consecFails <= 1) return OnceAfterFire::RetryShort;
  return OnceAfterFire::Retire;
}
constexpr uint32_t kWakeupRetrySec = 300;

bool parseSpec(ArduinoJson::JsonObjectConst schedule, const LoopCaps& caps,
               SchedSpec& out, std::string& err);
// Roll a loop's daily counters - CLOCK-SAFE: only when the clock is valid AND the
// new wall-day strictly exceeds lastSyncedDay (never resets in the boot-relative
// window, so a reboot can't wipe the daily ceiling - prism fix).
void rollDayIfNeeded(LoopRecord& l, uint32_t curDayNum, bool clockValid);
// TIMEZONE change: adopt the new local-day frame with counters UNTOUCHED.
// Westward the day number decreases (strict-`>` roll would stall the daily
// counters for a day); eastward it jumps (a roll would grant a free daily
// budget). Adoption keeps a tz edit budget-neutral either way.
void adoptLocalDay(LoopRecord& l, uint32_t curDayNum);
void rollDeviceDayIfNeeded(DeviceCounters& dev, const LoopCaps& caps,
                           uint32_t curDayNum, uint64_t nowEpoch, bool clockValid);
// Apply a fire's outcome (POST-fire): tokensToday += real usage, lastResult,
// consecFails (reset on ok / ++ on fail), and the semantic-repeat run. The tick
// bumps firesToday + device counters PRE-fire (at-most-once); this handles the result.
void onFireResult(LoopRecord& l, bool ok, const TokenUsage& used, uint64_t replyHash, uint64_t nowEpoch);
// Reschedule a due loop WITHOUT counting a fire (the pre-fire idle gate's defer,
// e.g. the dream loop while the device is busy): nextRun moves strictly after
// now; firesToday / lastRun / device counters are deliberately untouched, so a
// deferred tick never consumes any daily ceiling. Clock-rollover-safe by
// construction: it only ever moves nextRun forward from `nowEpoch`.
void deferLoop(LoopRecord& l, uint64_t nowEpoch, uint32_t deferSec);
bool isSemanticRepeat(const LoopRecord& l, int maxRepeats);
uint64_t fnv64(const std::string& s);

// --- serialization (pure, ArduinoJson) -------------------------------------
std::string dumpLoops(const std::vector<LoopRecord>& loops);
bool        loadLoops(const std::string& json, std::vector<LoopRecord>& out);

}  // namespace orch
}  // namespace nimbus
