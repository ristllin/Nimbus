#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

namespace nimbus {
namespace orch {
namespace tx {
struct HeadBudget;
}
}  // namespace orch
}  // namespace nimbus

// plain/guards.h - the only guards of the plain engine, scaled to the head (PRD section 1,
// step 5, as amended by rulings 81 to 85). Guards stop runaways, never honest work.
//
// Unattended turns (no person's message started them: routines, agent reports, notices) keep
// today's limits (ruling 84 item 9): 16 rounds (a stored orchLoopRnds of 8 or less reads as 16;
// 9 to 32 is honoured), 240 s, the last round to 420 s.
// Attended turns (ruling 84 item 2): the soft guards are those 16 rounds and 240 s, checked
// again every 16 rounds and every 240 s; at a soft guard a turn that is progressing goes on
// (the device shows a status line through RB-E3's progress class) and one that is not stops.
// The hard guards are 64 rounds and 1,200 s (the time guard trips 180 s before it, so the last
// round ends inside it), the size guard and the spend guard.
// Every turn: a loop (kLoopRounds tool rounds in a row with no progressing call) trips at once.
// Size: prompt tokens at most max(4 x the head's aim, R x its request at the due mark of the
// profile in force); spend at most max(0.50 USD, R warm requests plus one cold at that size);
// R is the attended turn's hard round limit (64), so a progressing turn is never stopped by a cap
// sized for a shorter one; an unattended turn keeps today's R = 16.
// On a trip: one '[device] limit reached (<slug>): ...' item that asks the last round to say what
// is done and what is left, one last round with tool choice none, its text is delivered; an
// attended turn's owner line asks the one question whether to go on.
// Also identical-call suppression of pure tools inside one turn, through beforeDispatch.
//
// Seam declared by lane RB-B0a; the bodies (plain_guards.cpp) are lane RB-E1b's; the soft
// guards, the loop and the table at full are lane RB-X3's (ruling 84 items 1 and 2).
namespace agent {
namespace plain {

inline constexpr uint16_t kDefaultRounds = 16;   // the soft rounds (attended), the round limit (unattended)
inline constexpr uint16_t kMaxRounds = 32;       // the stored orchLoopRnds is honoured up to here
inline constexpr uint16_t kHardRounds = 64;      // an attended turn's hard round guard
inline constexpr uint32_t kSoftWallMs = 240000;
inline constexpr uint32_t kHardWallMs = 420000;  // an unattended turn's last round ends inside it
inline constexpr uint32_t kAttendedHardWallMs = 1200000;   // an attended turn ends inside it
// The last round's own time: the time guard trips this long before the turn's hard wall.
inline constexpr uint32_t kLastRoundReserveMs = kHardWallMs - kSoftWallMs;
// One exchange, its retries included, ends inside this: a hung read is a stall, not honest work,
// and ends inside 450 s without bytes on any turn (ruling 85 item 3).
inline constexpr uint32_t kExchangeMaxMs = 450000;
inline constexpr uint8_t kLoopRounds = 3;        // tool rounds in a row with no progressing call
inline constexpr uint32_t kGuardOutputTokens = 740;   // a request's output when the guards price one
inline constexpr uint32_t kMinSpendCapMicroUsd = 500000;   // 0.50 USD
// A head that does not say its request size: the 85,000 B floor body at about 3.5 B a token.
inline constexpr uint32_t kDefaultRequestTokens = 24000;

// Slugs are the '(rounds|time|spend|size|loop)' of the limit item: frozen.
enum class GuardTrip : uint8_t { None = 0, Rounds = 1, Time = 2, Spend = 3, Size = 4, Loop = 5 };
inline const char* guardTripSlug(GuardTrip g) {
  switch (g) {
    case GuardTrip::None: return "none";
    case GuardTrip::Rounds: return "rounds";
    case GuardTrip::Time: return "time";
    case GuardTrip::Spend: return "spend";
    case GuardTrip::Size: return "size";
    case GuardTrip::Loop: return "loop";
  }
  return "none";
}

// What the limits are scaled from, and what one request costs (the binding fills it from the
// head's budget and its price row; RB-E1b). Rates are micro-USD per 1M tokens; a cache rate of
// 0 is 0.1x (read) and 1.25x (write) of the input rate. All rates 0: the head is not priced
// and the spend guard measures nothing (rounds, time and size still bound the turn).
struct HeadCost {
  uint32_t targetTokens = 0;           // the head's prompt target
  uint32_t requestTokensAtTrigger = 0; // its request size at the profile trigger (0: kDefaultRequestTokens)
  uint32_t warmRequestMicroUsd = 0;
  uint32_t coldRequestMicroUsd = 0;
  uint64_t inMicroPerM = 0, outMicroPerM = 0, cacheReadMicroPerM = 0, cacheWriteMicroPerM = 0;
};

// The rates a head is billed at (micro-USD per 1M tokens; a cache rate of 0 as in HeadCost).
struct HeadRates {
  uint64_t in = 0, out = 0, cacheRead = 0, cacheWrite = 0;
};
// HeadCost for a head at the profile in force (the binding's one call): the aim is the target,
// the due mark the request at the trigger, a warm request reads 90% of it from the cache, a cold
// one none, each with kGuardOutputTokens of output.
HeadCost guardCostFor(const nimbus::orch::tx::HeadBudget& budget, const HeadRates& rates);

struct GuardLimits {
  uint16_t rounds = kDefaultRounds;           // the soft rounds (attended) or the round limit
  uint32_t softWallMs = kSoftWallMs;          // the soft time (attended), checked again every softWallMs
  uint16_t hardRounds = kDefaultRounds;       // the round guard trips here
  uint32_t timeLimitMs = kSoftWallMs;         // the time guard trips when elapsed + kMinRequestMs reaches it
  uint32_t lastRoundWallMs = kHardWallMs;     // the guard's last round runs inside it
  bool attended = false;                      // soft guards apply
  uint32_t maxPromptTokens = 0;
  uint32_t maxSpendMicroUsd = 0;
};

// What the turn has used so far. promptTokens is the request size summed over the turn's
// rounds, cached tokens included (prompt + cache read + cache write: the size guard measures
// size, not price); spendMicroUsd is requestMicroUsd summed over the rounds.
struct GuardState {
  uint16_t rounds = 0;
  uint32_t elapsedMs = 0;
  uint32_t promptTokens = 0;
  uint32_t spendMicroUsd = 0;
  uint8_t idleRounds = 0;   // tool rounds in a row with no progressing call (GuardProgress)
};

// Whether a turn is progressing (ruling 84 item 2). A call progresses unless it was answered
// from suppression or failed with the same error text as the previous attempt of the same call
// (tool and arguments) in this turn; text the model writes beside its calls that it has not
// written before in this turn is new answer text. One per turn, reset at its open.
struct GuardProgress {
  uint16_t nextRounds = 0;   // the next soft check by rounds (0: limits.rounds)
  uint32_t nextMs = 0;       // the next soft check by time (0: limits.softWallMs)
  bool progressed = false;   // a progressing call or new text since the last soft check
  bool roundProgressed = false;
  uint8_t idle = 0;          // tool rounds in a row with no progressing call
  std::vector<std::pair<uint32_t, uint32_t>> attempts;   // call key hash, its last outcome (0: success)
  std::vector<uint32_t> texts;                           // hashes of the text written beside calls

  void noteCall(const std::string& tool, const std::string& args, bool suppressed, bool isError,
                const std::string& result);
  void noteText(const std::string& text);
  void endRound();   // a tool round ended: the loop count rises when no call of it progressed
};

// What a soft check at a round boundary of an attended turn decided.
enum class SoftCheck : uint8_t { None = 0, GoOn = 1, Stop = 2 };

// One request's tokens as the exchange reports them (prompt is the uncached part on every wire).
struct RequestTokens {
  uint32_t prompt = 0, output = 0, cacheRead = 0, cacheWrite = 0;
};

// An unattended turn's limits (today's).
Status guardLimitsFor(int storedRounds, const HeadCost& cost, GuardLimits& out);
// A turn's limits: attended (a person's message started it) or not.
Status guardLimitsFor(int storedRounds, const HeadCost& cost, bool attended, GuardLimits& out);
// The first hard limit the turn has reached, in the order loop, rounds, time, spend, size (None:
// go on).
Status checkGuards(const GuardLimits& limits, const GuardState& state, GuardTrip& out);
// The soft guards of an attended turn (None on an unattended one, or when no check is due). A
// due check of a progressing turn answers GoOn and moves the next check on by one interval;
// one that is not progressing answers Stop with the guard reached in `which`.
SoftCheck checkSoftGuards(const GuardLimits& limits, const GuardState& state, GuardProgress& progress,
                          GuardTrip& which);
// One request's spend at the head's rates (0 for a head with no price).
uint32_t requestMicroUsd(const HeadCost& cost, const RequestTokens& tokens);
// Declared pure: suppression may answer a repeat from the earlier result.
Status isPureTool(const std::string& name, bool& out);
// Register identical-call suppression in HookSlot::Suppression (beforeDispatch and
// afterResult): inside one turn only, only for pure calls (DispatchCtx::pure, the class of the
// tool a `more` call reaches), only while the earlier result is still shown whole (above
// DispatchCtx::stubThrough) and no effect has been tried since. The repeat is answered with
// tool_results::kSameAsEarlier and does not run.
Status installSuppression(PlainHooks& hooks);

}  // namespace plain
}  // namespace agent
