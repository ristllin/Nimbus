#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/intents.h"

namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
class TxLog;
struct HeadState;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/resume.h - crash safety (PRD section 7): at-least-once reply, at-most-once effects.
// Registered through hooks.h only (bootResume, HookSlot::Resume); the body is
// plain_resume.cpp (lane RB-X1), the rules it reads are nimbus/orch/intents.h.
//
// At boot, after the engine filled the arena from the card, the seam does exactly this:
// 1. Every stored call with no stored result is answered first, and never run again:
//    'outcome unknown' when its intent is stored (it may have taken effect), the restart line
//    (texts::tool_results::kRestartedBeforeRun) when no intent is (it changed nothing).
// 2. The newest turn, by what is stored (intents::bootAction):
//    - its e item is stored: nothing;
//    - its d item is stored: the e item only (no send, no model call);
//    - a turn no person started (a routine, an agent report, a notice): the e item only;
//    - the final answer is stored and whole: it is sent with no model call, then d and e. The
//      boot cannot know whether a cut fell before or inside the earlier send, so a reply that
//      went out just before the cut goes out twice; the e item names it ("resume:sent-stored");
//    - no final answer: one automatic resume, a turn with trigger Resume, the same turn
//      number and attempt + 1, through the engine's own runTurn, from everything stored (owner
//      ruling 81: the request carries every item of the log);
//    - one resume was already made (or the stored answer is not whole): the plain crash
//      notice (nimbus::orch::crashNotice) as the one status line, then the e item.
//    Before a resume or a send the attempt is written to head.json (the write-ahead counter):
//    a cut inside the resume itself is counted, so the next boot sends the notice. When the
//    counter cannot be written, the seam sends the notice instead: never a second resume.
// The RTC record is not read or written (its layout is unchanged): a power cut, the commonest
// cut, empties it, so the count lives on the card. Restart, install and head switch run after
// the reply is sent and the turn is closed (TurnOutcome::deferred), so a restart they cause
// is never resumed, and the seam never runs a deferred effect of the turn it closes.
namespace agent {
namespace plain {

inline constexpr uint8_t kMaxAutoResumes = 1;

// What the seam did at boot (ResumeDeps::done).
struct ResumeReport {
  nimbus::orch::intents::BootAction action = nimbus::orch::intents::BootAction::None;
  uint32_t turn = 0;          // the turn it acted on (0: none)
  size_t answered = 0;        // open calls it answered (step 1)
  bool counted = false;       // the attempt was written to head.json before it acted
  Status status = Status::success();
  // Resume only: the resumed turn's outcome. The binding runs outcome.deferred as it does
  // after any runTurn (they are the resumed run's own effects, from results it stored), once
  // boot returns.
  TurnOutcome outcome;
};

struct ResumeDeps {
  nimbus::orch::tx::TxLog* log = nullptr;       // the chat's log (the engine's; opened at boot)
  nimbus::orch::tx::TxArena* arena = nullptr;   // the chat's arena, filled at boot
  // The engine's head (PlainEngine::head): the seam moves nextQ, openTurn and attempts and
  // writes it. Required.
  std::function<nimbus::orch::tx::HeadState*()> head;
  // The engine's PlainEngine::runTurn, for the one automatic resume. Required.
  std::function<Status(const TurnInput& in, TurnOutcome& out)> runTurn;
  // The engine's delivery (EngineDeps::deliver, else deliverTurn over the channel). Required.
  DeliverFn deliver;
  // The chat's context as the binding gives an owner turn (chatKey, chatId, head, model,
  // dialect, profile, who); the seam sets turn, trigger, attempt and the start. Required.
  std::function<Status(TurnCtx& out)> context;
  std::function<uint64_t()> now;                // epoch seconds, 0: clock not set (null: 0)
  std::string deviceName;                       // the crash notice's name ("": Nimbus)
  std::function<void(const ResumeReport&)> done;   // optional: what the seam did
};

// Registers the seam in HookSlot::Resume. InvalidArgument (nothing registered) when a
// required dependency is missing; Busy when the slot is taken.
Status installResume(PlainHooks& hooks, const ResumeDeps& deps);

}  // namespace plain
}  // namespace agent
