#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/head_loop.h"   // HeadProgress

// turn_progress (lane PROG) - the portable plan for what a surface should show
// at a round boundary of a long turn, so a turn never looks frozen. The head
// loop fires its progress hook at EVERY round boundary (the only moment no
// provider request is in flight); this file turns those facts into the three
// user-visible signals the lane requires:
//   1. a keep-alive between every tool round and provider request (a Telegram
//      typing action, a busy state on the web chat and the screen),
//   2. ONE short plain progress line naming what the turn is doing and a
//      rough wait, after about 30 s or at the third round - never more than
//      one per turn, never on a quick turn,
//   3. a look for messages that arrived mid-turn, so they get an immediate
//      short acknowledgement instead of silence until the turn ends.
// The plan is pure: the surface owns the per-turn state (whether the line was
// already sent) and executes the plan with its own rate limits. Host-tested in
// test/test_turn_progress.
namespace nimbus {
namespace harness {

struct TurnProgressPlan {
  // Send a keep-alive now (typing / busy state). True at every boundary except
  // the forced final round, where an answer is imminent and a "still working"
  // signal would be a lie.
  bool keepAlive = false;
  // Send the ONE progress line now. The caller passes lineAlreadySent so the
  // plan stays stateless; a true here means this is the turn's only line.
  bool sayLine = false;
  // The line to send when sayLine: plain words, no internals, no em dash.
  std::string line;
  // Look for messages that arrived while the turn runs, and acknowledge any
  // that did. False on the forced final round: a queued message is about to be
  // answered, and an ack over an imminent answer is noise.
  bool checkQueue = false;
};

// The plan for one round boundary. `maxRounds` is the loop's round cap
// (HeadLoopConfig::maxRounds); with it the line's rough wait is derived from
// the pace the turn has actually run at, not a guess.
TurnProgressPlan planTurnProgress(const orch::HeadProgress& hp, int maxRounds, bool lineAlreadySent);

// The plain-words activity for the tools the previous round dispatched ("what
// it is doing"). Host-tested: every registry tool name maps to a phrase or the
// honest default, so the line never leaks a machine name.
std::string progressActivity(const std::vector<std::string>& tools);

// The rough-wait tail for the line ("about a minute more"), derived from the
// observed pace and the rounds left. Host-tested buckets.
std::string progressWait(uint32_t elapsedMs, int round, int maxRounds);

}  // namespace harness
}  // namespace nimbus
