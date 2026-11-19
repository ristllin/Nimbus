#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/guards.h - the only guards of the plain engine, scaled to the head (PRD section 1,
// step 5): 16 rounds (a stored orchLoopRnds of 8 or less reads as 16; 9 to 32 is
// honoured), 240 s soft wall clock, prompt tokens at most max(4 x the head's target,
// 16 x its request size at the profile trigger), spend at most max(0.50 USD, 16 warm
// requests plus one cold at that size). On a trip: one '[device] limit reached (...)'
// item, one last round with tool choice none, the text is delivered.
// Also identical-call suppression of pure tools inside one turn, through beforeDispatch.
//
// Seam declared by lane RB-B0a; the bodies (plain_guards.cpp) are lane RB-E1b's.
namespace agent {
namespace plain {

inline constexpr uint16_t kDefaultRounds = 16;
inline constexpr uint16_t kMaxRounds = 32;
inline constexpr uint32_t kSoftWallMs = 240000;
inline constexpr uint32_t kHardWallMs = 420000;
inline constexpr uint32_t kMinSpendCapMicroUsd = 500000;   // 0.50 USD
// A head that does not say its request size: the 85,000 B floor body at about 3.5 B a token.
inline constexpr uint32_t kDefaultRequestTokens = 24000;

// Slugs are the '(rounds|time|spend|size)' of the limit item: frozen.
enum class GuardTrip : uint8_t { None = 0, Rounds = 1, Time = 2, Spend = 3, Size = 4 };
inline const char* guardTripSlug(GuardTrip g) {
  switch (g) {
    case GuardTrip::None: return "none";
    case GuardTrip::Rounds: return "rounds";
    case GuardTrip::Time: return "time";
    case GuardTrip::Spend: return "spend";
    case GuardTrip::Size: return "size";
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

struct GuardLimits {
  uint16_t rounds = kDefaultRounds;
  uint32_t softWallMs = kSoftWallMs;
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
};

// One request's tokens as the exchange reports them (prompt is the uncached part on every wire).
struct RequestTokens {
  uint32_t prompt = 0, output = 0, cacheRead = 0, cacheWrite = 0;
};

Status guardLimitsFor(int storedRounds, const HeadCost& cost, GuardLimits& out);
// The first limit the turn has reached, in the order rounds, time, spend, size (None: go on).
Status checkGuards(const GuardLimits& limits, const GuardState& state, GuardTrip& out);
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
