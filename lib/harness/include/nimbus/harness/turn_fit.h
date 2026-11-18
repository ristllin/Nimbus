#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/compose.h"
#include "nimbus/orch/head_loop.h"   // FitScope

// turn_fit - what a turn's request gives up when it does not fit, in ONE order, and what it
// says about it.
//
// Lumi, 1 Nov 18:51Z: "headloop: round 0 body 90519 B over the 85000 B budget - folded 0 B
// ... nothing left to fold". The request-body budget and a provider's context window are both
// limits on one thing, so one ladder answers both. Lumi, 15 Nov 16:00 (lane H of the history
// fix): the owner's own estimate from five minutes earlier went before the food playbook and
// the recalled memories, which a tool can read again, and the model was not told. So the
// order is the protocol's give-up order (plan v3), the same for the head loop's refits, a
// provider's window refusal and the single shot, and only after a real refusal of the actual
// body (never speculatively at round 0):
//
//   1. the transcript (the head loop's own rungs): stub the arguments of calls that ran,
//      fold the older rounds the model has read
//   2. recalled     the memories recall put in the prompt (a tool can search them again),
//                   only when its rendered bytes are in the body
//   3. skills       the auto-attached skills ([ACTIVE SKILLS])
//   4. results already read: the protocol's own step, which rung 1 has already taken (the
//      transcript's older rounds); no rung of its own here
//   5. older days   live history before today, oldest whole turns first
//   6. today        today's turns beyond the newest exchange, oldest first
//   7. results      the newest unread results (the turn input's [FRESH RESULTS] and
//                   [MEMORY RESULTS] clipped, both ends kept; the transcript's newest round)
//   8. input        the middle of the turn input (its head and newest words stay)
//   9. tools        every registry tool: orch_turn alone rides, and the model answers now
//  10. shelf        the skills index and the standing preferences, the owner's rules: last
//
// The summary pair and the newest exchange are never dropped. Every rung is measured on the
// rendered request (the composed prompt, its history messages and the input with this
// note): a rung that would not make it smaller gives nothing up and leaves no note (15 Nov:
// a recall rung that freed 0 rendered bytes added its 158 B note and forced a second refit).
// Whatever was given up is said: to the model in the request (note(): history cuts name the
// count, the local time range, the row ids and a ready memory.episodic call), in the log, and
// to the owner when it touches what the owner asked about (ownerLine()).
//
// One turn long, on the turn's task. Portable, host-tested (test_turn_fit).
namespace agent {

struct HeadTools;

// What the note needs to name a history cut.
struct FitContext {
  int32_t todayLocalDay = 0;   // the owner's current local day (days since 1970-01-01; 0 = no clock)
  int32_t utcOffsetMin = 0;    // for the note's local times
};

class TurnFit {
 public:
  // `ci` and `tools` outlive this object (the turn's); nullptr tools = a single shot.
  TurnFit(ComposeInputs& ci, const std::string& inputs, HeadTools* tools, const FitContext& ctx = FitContext());

  // Give up bytes within `scope` (head_loop.h FitScope):
  //   OlderHistory  the history rungs (5 then 6) until `want`, down to the keep floor
  //                 (setHistoryKeepFloor; the newest exchange when none is set): a photo
  //                 round's time fit gives up older history before its photos, and only to
  //                 there (RULINGS-v3 item 5);
  //   Context       the next of rungs 2-6 that gives anything (one rung);
  //   NextRung      the next rung of the whole order that gives anything (one rung);
  //   Ladder        the rungs in order until `want` is met;
  //   NewestMessage rungs 2, 3, 7 and 8 (what the newest message holds) until `want`;
  //   GiveBackHistory  the history OlderHistory gave up comes back (its photos went as text
  //                 after all); returns the rendered bytes put back. A later cut of history
  //                 by another scope keeps what it cut (nothing is given back then).
  // Returns the rendered bytes given up; 0 = nothing left there.
  size_t shrink(size_t want, nimbus::orch::FitScope scope);
  // The live history (the summary pair aside) OlderHistory never cuts under: the head's
  // historyKeepBytes (ContextBudget). 0 = down to the newest exchange.
  void setHistoryKeepFloor(size_t bytes) { keepFloor_ = bytes; }

  // The prompt recomposed from what is left. Valid after shrink() gave something up.
  const std::string& instructions() const { return instructions_; }
  // The input as the request should send it: the note, then what is left of the input.
  std::string inputs() const;
  bool trimmed() const { return trimmed_; }
  // The live history rows and messages left out of this request (the turn's leftOutRows; any
  // of them makes the chat's fold due: the protocol's "every live-row cut sets trimmedForSize").
  size_t historyRowsLeftOut() const { return cut_.historyRowsDropped; }
  int historyMessagesLeftOut() const { return cut_.olderDropped + cut_.todayDropped; }
  size_t historyBytesLeftOut() const { return cut_.historyBytesDropped; }

  // What was left out, for the model; "" = nothing.
  std::string note() const;
  // The same for the owner; "" = nothing the owner needs to hear.
  std::string ownerLine() const;

 private:
  enum Rung : int { kRecalled, kSkills, kOlderDays, kToday, kResults, kInput, kTools, kShelf, kRungs };
  size_t runRung(int rung, size_t want);
  std::function<void()> applyRung(int rung, size_t want);   // applied; the result undoes it
  std::function<void()> dropToolsUndoable();
  size_t walk(int from, int to, size_t want, bool whole);
  size_t walkNewestMessage(size_t want);
  size_t shedOlderHistory(size_t want);
  size_t giveBackHistory();
  size_t liveHistoryBytes() const;
  size_t rendered() const;
  // Each rung, applied; the measure and the revert are runRung's.
  void dropRecalled();
  void dropSkills();
  void dropHistory(bool today, size_t want);
  void noteHistoryCut(size_t first, size_t drop);
  void clipResults(size_t want);
  void clipInput(size_t want);
  void dropTools();
  void dropShelf();
  // Is history message `i` a turn of the owner's current day?
  bool isToday(size_t i) const;
  std::string historyPart() const;

  ComposeInputs& ci_;
  HeadTools* tools_;
  FitContext ctx_;
  std::string input_;
  std::string instructions_;
  bool trimmed_ = false;
  size_t keepFloor_ = 0;      // setHistoryKeepFloor
  size_t floorNow_ = 0;       // the floor dropHistory keeps to (OlderHistory only)
  // What went, for the note (one struct: a rung that frees nothing puts it back).
  struct Cuts {
    int olderDropped = 0;     // history messages before today
    int todayDropped = 0;     // today's messages beyond the newest exchange
    size_t historyBytesDropped = 0;
    size_t historyRowsDropped = 0;
    std::string cutFirstId, cutLastId, keptFirstId;   // the history cut: rows, and the first row kept
    uint32_t cutFromUnix = 0, cutToUnix = 0;
    int recalledDropped = 0;
    int resultBlocksClipped = 0;
    size_t resultBytesCut = 0;
    size_t inputBytesCut = 0;
    bool skillsDropped = false;
    bool shelfDropped = false;
    bool toolsDropped = false;
  } cut_;
  // The history before OlderHistory's first cut, to give back (GiveBackHistory).
  struct HistoryShed {
    std::vector<HistMsg> history;
    Cuts cut;
  };
  std::unique_ptr<HistoryShed> shed_;
};

}  // namespace agent
