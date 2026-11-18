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

// What the limits are scaled from.
struct HeadCost {
  uint32_t targetTokens = 0;           // the head's prompt target
  uint32_t requestTokensAtTrigger = 0; // its request size at the profile trigger
  uint32_t warmRequestMicroUsd = 0;
  uint32_t coldRequestMicroUsd = 0;
};

struct GuardLimits {
  uint16_t rounds = kDefaultRounds;
  uint32_t softWallMs = kSoftWallMs;
  uint32_t maxPromptTokens = 0;
  uint32_t maxSpendMicroUsd = 0;
};

struct GuardState {
  uint16_t rounds = 0;
  uint32_t elapsedMs = 0;
  uint32_t promptTokens = 0;
  uint32_t spendMicroUsd = 0;
};

Status guardLimitsFor(int storedRounds, const HeadCost& cost, GuardLimits& out);
Status checkGuards(const GuardLimits& limits, const GuardState& state, GuardTrip& out);
// Declared pure: suppression may answer a repeat from the earlier result.
Status isPureTool(const std::string& name, bool& out);
// Register identical-call suppression in HookSlot::Suppression.
Status installSuppression(PlainHooks& hooks);

}  // namespace plain
}  // namespace agent
