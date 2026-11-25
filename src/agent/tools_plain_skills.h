#pragma once
#include <Arduino.h>

#include <string>

#include "nimbus/harness/plain/turn.h"   // TurnCtx, ToolCall, Status
#include "nimbus/orch/notes.h"           // OriginFn
#include "nimbus/orch/tool_registry.h"

// tools_plain_skills - skills, /skill, /prefs and the held notes on engine 2 (lane E2-P-SKILL;
// the portable part is nimbus/harness/plain/chat_skill_tools.h).
//   - skill_list, skill_save, skill_delete over the shared skill store (skills.h), behind more;
//   - the rails' pending verdict for skill_save: the save is kept waiting in the store;
//   - /skill and /prefs for the owner (commands_plain.cpp routes them here);
//   - the Rules page lists the held notes beside the rules (id "note-<id>") and answers them;
//     the answer runs on tg_poll (pump), the only task that writes notes.
// Engine 2 only (tools/engine/engine2_only.txt).
namespace agent::tools::skillsplain {

// Register the three tools and the Rules page's held notes. `origin` is the note tool's: the turn's.
void registerSkills(nimbus::orch::ToolRegistry& reg, nimbus::orch::notes::OriginFn origin);

// RailsDeps::pending for skill_save; false: `c` is not skill_save.
bool railHeld(const agent::plain::TurnCtx& t, const agent::plain::ToolCall& c, agent::plain::Status& out);

// The owner's /skill or /prefs (verb "skill" or "prefs"): the reply. tg_poll.
String command(const std::string& verb, const std::string& args);

// tg_poll, between turns: the Rules page's answers to held notes, run in order.
void pump();

}  // namespace agent::tools::skillsplain
