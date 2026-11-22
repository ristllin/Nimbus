#pragma once
#include <cstdint>

#include "nimbus/orch/command.h"

// plain/chat_commands.h - where an owner's slash command goes on an engine-2 device (lane E2-CHAT).
// No command reaches the model. Each verb of nimbus::orch::kCommandVerbs has one route:
//   Shared      the handler engine 1 calls (it does not depend on the engine): /help, /start,
//               /update, /ota, /play, /fetch, /loops, /loop, /usage, /model, and /budget with
//               arguments (a limit change and its confirm); /remind (lane E2-SCHED: a reminder
//               made through the schedule tool's own code, as the owner's);
//   Clear       a new conversation on engine 2 (the stored one is kept on the card);
//   Compact     a compaction of this chat now;
//   BudgetView  a bare /budget: engine 2's budget view (the system prompt in tokens), then the
//               spending text a bare /budget gives on engine 1;
//   NotYet      engine 1 only for now, answered with one plain line: /skill (the skill store is
//               engine 1's), /mcp (engine 2 reaches no
//               tool server), /prefs (engine 2 reads notes, not standing rules);
//   Unknown     a verb the table does not know: the same "try /help" answer as engine 1.
// test_plain_chat_commands walks every verb of the table: a verb added there with no route here
// fails it.
namespace agent {
namespace plain {

enum class ChatCommandRoute : uint8_t { Shared = 0, Clear = 1, Compact = 2, BudgetView = 3, NotYet = 4, Unknown = 5 };

inline const char* chatCommandRouteSlug(ChatCommandRoute r) {
  switch (r) {
    case ChatCommandRoute::Shared: return "shared";
    case ChatCommandRoute::Clear: return "clear";
    case ChatCommandRoute::Compact: return "compact";
    case ChatCommandRoute::BudgetView: return "budget-view";
    case ChatCommandRoute::NotYet: return "not-yet";
    case ChatCommandRoute::Unknown: return "unknown";
  }
  return "unknown";
}

// The route of a parsed command (c.isCommand is the caller's check).
inline ChatCommandRoute chatCommandRoute(const nimbus::orch::Command& c) {
  const std::string& v = c.verb;
  if (!nimbus::orch::findCommandVerb(v)) return ChatCommandRoute::Unknown;
  if (v == "clear") return ChatCommandRoute::Clear;
  if (v == "compact") return ChatCommandRoute::Compact;
  if (v == "budget") return c.args.empty() ? ChatCommandRoute::BudgetView : ChatCommandRoute::Shared;
  if (v == "skill" || v == "mcp" || v == "prefs") return ChatCommandRoute::NotYet;
  return ChatCommandRoute::Shared;
}

}  // namespace plain
}  // namespace agent
