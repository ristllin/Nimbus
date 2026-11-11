#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/hooks.h"      // agent::TurnSource
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
//      one per turn, never on a quick turn, and only for a turn a person
//      started (see progressAllowedForSource); the line is phrased in the
//      assistant's own voice in chat ("Still looking through my memory for
//      that, about half a minute more."),
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

// True when the turn was started by a person's message: the owner or an approved
// member in chat (TurnSource::Owner), or the serial console (TurnSource::Serial).
// Only these turns may show progress signals (typing, the one line) to the chat:
// a dream, routine, loop, sub-agent/synthesis or other system turn never does, so
// the owner never gets an unexplained "still working" from a turn they never
// started (Lumi v5.0.0, 11 Nov 03:31Z: a dream turn's line reached the owner).
bool progressAllowedForSource(agent::TurnSource source);

// The plan for one round boundary. `maxRounds` is the loop's round cap
// (HeadLoopConfig::maxRounds); with it the line's rough wait is derived from
// the pace the turn has actually run at, not a guess. `source` gates the visible
// signals: a non-person turn gets no keep-alive and no line (the queue check
// still runs - a message that arrived mid-turn is the owner's own and still
// deserves its ack).
TurnProgressPlan planTurnProgress(const orch::HeadProgress& hp, int maxRounds, bool lineAlreadySent,
                                  agent::TurnSource source);

// The plain-words activity for the tools the previous round dispatched ("what
// it is doing"). Host-tested: every registry tool name maps to a phrase or the
// honest default, so the line never leaks a machine name.
std::string progressActivity(const std::vector<std::string>& tools);

// The rough-wait tail for the line ("about a minute more"), derived from the
// observed pace and the rounds left. Host-tested buckets.
std::string progressWait(uint32_t elapsedMs, int round, int maxRounds);

}  // namespace harness
}  // namespace nimbus
