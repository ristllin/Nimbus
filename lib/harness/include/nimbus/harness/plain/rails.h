#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/unattended_policy.h"   // ToolUse, toolUseFor: the one tool classification (RB-Q2a)

// plain/rails.h - the safety rails on the plain engine (PRD section 8.1). None reads the
// reply for words.
//
// Outside-content taint is a property of each item (itemOutside): a result of class
// Outside (the web, a fetched page, a connector, an unknown tool, a device-written path), a
// document the owner sent (a File part of his message: his own typed text never taints), a
// sub-agent's report (the Agent device note), a routine's reply after outside content (a Routine
// device note the device fenced with jobs.h fenceOutsideContent). A turn is tainted while its
// request carries an outside result whole, and when it is the first turn whose request carries an
// outside note (a report, a fenced routine reply; TurnCtx::tainted, set by the engine when it
// stores the turn's input, when an outside item was stored since the last closed turn, and when it
// stores an outside result; a resumed turn keeps it). A note is kept whole (ruling 81): later
// turns read it inside its fence, as data. An outside result shows whole only inside its own turn:
// from the next turn on the request carries its stub, which names it and how to view it
// (tx::showResult), never its bytes; owner and assistant text are never removed (ruling 81).
//
// In a tainted turn every call is judged by ONE named table, kTaintExemptions (read with
// taintExemptions): the call's class is RB-Q2a's toolUseFor (a `more` call is the tool it
// reaches), the first row that matches decides, and a call no row names is refused (fail
// closed). Note, skill_save, agent_start and a routine create or update (any schedule update)
// go to pending (the binding keeps them for the owner's approval; the tool does not run); a
// send to another chat, device configuration, tenant changes, agent_stop and anything unnamed
// are refused with the reason; a new fixed-text reminder to the same chat is allowed. An
// untainted turn is never touched here (the unattended policy, HookSlot::Policy, and the
// access rules decide it). Registered in HookSlot::Rails, before Suppression: a rail's answer
// is never suppressed.
//
// Seam declared by lane RB-B0a; the body (plain_rails.cpp) is lane RB-RL's.
namespace agent {
namespace plain {

// What a rail does with one call in a tainted turn.
enum class RailVerdict : uint8_t { Allow = 0, Pending = 1, Refuse = 2 };
const char* railVerdictSlug(RailVerdict v);   // "allow" | "pending" | "refuse"

// How a row of the table reads the call's arguments (JSON object).
enum class RailArgs : uint8_t {
  Any = 0,             // every call of the row's class
  Listing = 1,         // a `more` listing: no `name` (a call whose target is unknown is not one)
  ThisChat = 2,        // a send to the turn's own chat: no `chat_id` but its own, and only the
                       // keys kind, path, ref, text, caption, chat_id
  ScheduleList = 3,    // schedule op=list
  FixedReminder = 4,   // a new schedule kind=reminder (op create or absent), a non-empty text, only
                       // the keys op, kind, when, text, chat_id, and this chat (an update names an id
                       // the rail cannot see: it goes to pending)
  ScheduleChange = 5,  // schedule op create or update (a routine, or a reminder beyond fixed
                       // text), to this chat
};

// One row of the table. `tool` null: every tool of `use`; else that tool only.
struct RailRow {
  nimbus::orch::ToolUse use;
  const char* tool;
  RailArgs args;
  RailVerdict verdict;
};

// The one named table, kTaintExemptions in plain_rails.cpp: what a tainted turn may still do,
// in order. A call no row matches is refused.
const RailRow* taintExemptions(size_t& count);

// ---- taint, one rule for every item ------------------------------------------------------
// Is this stored item outside content: a Result of class Outside, a User item with a File
// part (a document the owner sent), an Agent device note (a sub-agent's report), a Routine
// device note carrying the outside-content fence (a routine's reply after outside content)?
bool itemOutside(const nimbus::orch::tx::Item& it);
// Does a turn's input taint it from the start: a sub-agent's report (Trigger::AgentResult)
// or a document part in the owner's message? Photos and voice do not; typed text never does.
bool inputOutside(Trigger trigger, const std::vector<nimbus::orch::tx::Part>& parts);
// Is the result of this call outside content because of what it reads: a path (path, name)
// or a list prefix under a device-written folder whose content came from outside (inbox/*,
// the files people send; agents/*, the sub-agents' reports; any letter case)? Arguments that are
// not a JSON object read as outside (fail closed); an empty string is no arguments.
bool readsOutsidePath(const std::string& tool, const std::string& argsJson);
// Is the turn tainted: TurnCtx::tainted, or a sub-agent report turn.
bool turnTainted(const TurnCtx& turn);

// ---- the gate ------------------------------------------------------------------------------
// The verdict for one call (the tool a `more` call reaches, with its arguments) in `turn`:
// Allow whenever the turn is not tainted; else the first matching row of the table, Refuse
// when none matches or the arguments are not a JSON object (an empty string is {}).
// `declaredEffect` is the registry's class of the tool: a name no static table knows (no
// action, class or taint row: a tool the device declared at runtime) that the registry
// declares pure or read reads as Read, as toolUseFor's own contract says; every other name
// keeps toolUseFor's class. `row`, when given, gets the index of the deciding row (count when
// none decided).
Status railFor(const TurnCtx& turn, const ToolCall& call, bool declaredEffect, RailVerdict& out,
               size_t* row = nullptr);

// The binding's approval store for a Pending verdict: keep `call` (the tool a `more` call
// reaches, with its arguments) for the owner's approval; it must not run the tool. Not ok:
// nothing was kept and the call is refused.
using RailPendingFn = std::function<Status(const TurnCtx& turn, const ToolCall& call)>;

struct RailsDeps {
  RailPendingFn pending;   // null: a Pending verdict is refused (nothing is kept, nothing runs)
};

// Registers the gate in HookSlot::Rails on beforeDispatch: Allow is Run, Pending is Answer
// with texts::tool_results::kRailPending once `pending` kept the call (kRailNoPending as a
// refusal otherwise), Refuse is Refuse with kRailRefused. Err::Busy when the slot is taken.
Status installRails(PlainHooks& hooks, const RailsDeps& deps);

}  // namespace plain
}  // namespace agent
