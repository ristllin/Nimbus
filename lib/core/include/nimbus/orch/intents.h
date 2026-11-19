#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// nimbus/orch/intents.h - write-ahead intents and what a boot reads from them (PRD section 7,
// "Crash safety"; lane RB-X1). Portable, no allocation beyond the vectors it fills.
//
// The engine keeps one rule (plain_engine.cpp, runCall): an effect call runs only once its
// intent item (i) is stored after its call item (c), and every result (r) is stored before the
// next request leaves. So after a power cut the stored items say, for each call that has no
// result, whether it can have changed anything:
//   - its intent is stored: it may or may not have taken effect (Unknown);
//   - no intent (an effect call that never started, or a pure or read call): it changed
//     nothing (NotStarted).
// Neither is run again by the harness: the model reads which one it was and decides.
//
// For the newest turn the items also say what is left to do (TurnState), and bootAction turns
// that, the resumes already made and the one-resume rule into the boot's single action.
namespace nimbus {
namespace orch {
namespace intents {

enum class CallFate : uint8_t {
  NotStarted = 0,   // no intent stored: it changed nothing
  Unknown = 1,      // its intent is stored: it may or may not have taken effect
};

// A stored call with no stored result, in q order (a result answers the OLDEST open call of
// its id, as the engine and the wires pair them).
struct OpenCall {
  uint64_t q = 0;        // the c item
  uint32_t turn = 0;
  std::string id;
  std::string n;
  CallFate fate = CallFate::Unknown;
};
// Every open call of `arena`. InvalidArgument when an item cannot be read (a damaged arena).
tx::Status openCalls(const tx::TxArena& arena, std::vector<OpenCall>& out);

enum class TurnState : uint8_t {
  Closed = 0,         // nothing stored, or the newest turn's e item is stored
  Unanswered = 1,     // no final answer is stored: the model is asked again (one resume)
  AnswerStored = 2,   // the final answer is stored and no d item: send it, no model call
  Delivered = 3,      // the d item is stored and no e item: close the turn
};
const char* turnStateSlug(TurnState s);

// The newest turn by what is stored (digests left out: they belong to no turn's answer).
struct OpenTurn {
  TurnState state = TurnState::Closed;
  uint32_t turn = 0;          // the newest turn (0: none)
  bool person = false;        // a person's message started it (a u item, or a t item of a person trigger)
  uint8_t attempts = 0;       // the highest attempt of its t items (0: the first run)
  uint64_t userQ = 0;         // its owner message (0: none stored)
  uint64_t answerQ = 0;       // its newest final answer (0: none stored)
};
tx::Status openTurn(const tx::TxArena& arena, OpenTurn& out);

// The boot's one action for the newest turn (lane RB-X1's rule, PRD section 7):
//   Closed: None. Delivered: Close (no send, no model call).
//   A turn no person started (a routine, an agent report, a notice): Close (never resumed).
//   attempts at or over maxResumes: Notice (the plain crash notice, then Close).
//   AnswerStored: SendStored when the stored answer is whole, else Notice.
//   Unanswered: Resume.
// `attempts` is the resumes already made for that turn (its t items and head.json's count).
enum class BootAction : uint8_t { None = 0, Close = 1, SendStored = 2, Resume = 3, Notice = 4 };
const char* bootActionSlug(BootAction a);
BootAction bootAction(const OpenTurn& turn, uint8_t attempts, uint8_t maxResumes, bool answerWhole);

}  // namespace intents
}  // namespace orch
}  // namespace nimbus
