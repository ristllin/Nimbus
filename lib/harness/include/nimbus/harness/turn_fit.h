#pragma once
#include <cstddef>
#include <string>

#include "nimbus/harness/compose.h"

// turn_fit - what a turn's request gives up when it does not fit, in a defined order,
// and what it says about it.
//
// Lumi, 1 Nov 18:51Z: "headloop: round 0 body 90519 B over the 85000 B budget - folded 0 B
// ... nothing left to fold". A synthesis turn died before any request: its prompt and its
// history were over the budget, and only tool rounds could be folded. The request-body
// budget and a provider's context window are both limits on one thing, so one ladder
// answers both. The head loop walks its own rungs first (arguments of calls that ran,
// older tool rounds); this class holds the rungs above it:
//
//   older history   oldest past turns first; the summary and the newest turn stay
//   then, only when that is not enough:
//   results         the [FRESH RESULTS] and [MEMORY RESULTS] blocks clipped, largest first
//   recalled        the memories recall put in the prompt
//   playbook        the food playbook (the model can read the skill again)
//   newest history  the last past turns, then the summary
//   input           the turn input's middle (its head and its newest words stay)
//   tools           every registry tool: orch_turn alone rides, and the model answers now
//   shelf           the skills index and the standing preferences, the owner's rules: last
//
// Whatever was given up is said: to the model in the request (note()), in the log, and
// to the owner when it touches what the owner asked about (ownerLine()).
//
// One turn long, on the turn's task. Portable, host-tested (test_turn_fit).
namespace agent {

struct HeadTools;

class TurnFit {
 public:
  // `ci` and `tools` outlive this object (the turn's); nullptr tools = a single shot.
  TurnFit(ComposeInputs& ci, const std::string& inputs, HeadTools* tools);

  // Give up at least `want` bytes if the ladder allows it. Without `lastResort` only
  // older history goes. Returns the bytes given up; 0 = nothing left on those rungs.
  size_t shrink(size_t want, bool lastResort);

  // The prompt recomposed from what is left. Valid after shrink() gave something up.
  const std::string& instructions() const { return instructions_; }
  // The input as the request should send it: the note, then what is left of the input.
  std::string inputs() const;
  bool trimmed() const { return trimmed_; }

  // What was left out, for the model; "" = nothing.
  std::string note() const;
  // The same for the owner; "" = nothing the owner needs to hear.
  std::string ownerLine() const;

 private:
  size_t dropOlderHistory(size_t want);
  size_t clipResults(size_t want);
  size_t dropRecalled();
  size_t dropPlaybook();
  size_t dropNewestHistory(size_t want);
  size_t dropShelf();
  size_t clipInput(size_t want);
  size_t dropTools();

  ComposeInputs& ci_;
  HeadTools* tools_;
  std::string input_;
  std::string instructions_;
  bool trimmed_ = false;
  int historyDropped_ = 0;
  int newestDropped_ = 0;
  int recalledDropped_ = 0;
  int resultBlocksClipped_ = 0;
  size_t resultBytesCut_ = 0;
  size_t inputBytesCut_ = 0;
  bool playbookDropped_ = false;
  bool shelfDropped_ = false;
  bool toolsDropped_ = false;
};

}  // namespace agent
