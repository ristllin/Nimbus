#pragma once
#include <cstddef>
#include <string>

#include "nimbus/harness/compose.h"
#include "nimbus/orch/head_loop.h"   // FitScope

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
//   recalled        the memories recall put in the prompt (a tool can search them again)
//   playbook        the food playbook (a tool can read the skill again)
//   results         the [FRESH RESULTS] and [MEMORY RESULTS] blocks clipped, largest first
//   newest history  the last past turns as whole pairs, then the summary
//                   (those two swap on a turn with nobody's message in it: a synthesis
//                   or a routine exists for its results)
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

  // Give up bytes within `scope` (head_loop.h FitScope): the older history; the next rung
  // that has anything; or the rungs in order until `want` is met. Returns the bytes given
  // up; 0 = nothing left there.
  size_t shrink(size_t want, nimbus::orch::FitScope scope);

  // The prompt recomposed from what is left. Valid after shrink() gave something up.
  const std::string& instructions() const { return instructions_; }
  // The input as the request should send it: the note, then what is left of the input.
  std::string inputs() const;
  bool trimmed() const { return trimmed_; }
  // Enough conversation history was left out that the chat needs its fold. A fold is a paid call:
  // other cuts would come back the same after one, and a sliver of history to make room for a
  // large payload is no reason for one.
  bool historyTrimmed() const { return historyBytesDropped_ >= kFoldWorthBytes; }
  static constexpr size_t kFoldWorthBytes = 4096;

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
  size_t runRung(int rung, size_t want);
  size_t walkLadder(size_t want, bool whole);

  ComposeInputs& ci_;
  HeadTools* tools_;
  std::string input_;
  std::string instructions_;
  bool trimmed_ = false;
  int historyDropped_ = 0;
  int newestDropped_ = 0;
  size_t historyBytesDropped_ = 0;
  int recalledDropped_ = 0;
  int resultBlocksClipped_ = 0;
  size_t resultBytesCut_ = 0;
  size_t inputBytesCut_ = 0;
  bool playbookDropped_ = false;
  bool shelfDropped_ = false;
  bool toolsDropped_ = false;
};

}  // namespace agent
