#pragma once
#include <cstddef>
#include <string>

// builtin_skills - the device's built-in capsules (architecture, deep-research,
// hardware, tools-guide, deliver-pdf): the [SKILLS] index line and the body
// skill.get serves. Portable and flash-resident (const tables, no heap until a
// body is rendered), so the host suite reads exactly what the model reads.
//
// CUM-523: a capsule never names a Telegram-only tool (reply.telegram,
// files.send) on a turn that was not offered one. The index lines are
// channel-neutral; a body is a list of parts, and a part written for one side
// (a Telegram device, or a device whose turn has no Telegram) is served only on
// that side. The device picks the side with the same per-turn sample the engine
// filters the tool list with (TurnEngine::turnTelegram).
namespace agent {

struct BuiltinSkill {
  const char* id;
  const char* title;
  const char* desc;   // the one-line [SKILLS] index entry (channel-neutral)
};

// The built-ins, in index order: builtinSkill(0 .. builtinSkillCount()-1).
size_t builtinSkillCount();
const BuiltinSkill& builtinSkill(size_t i);

// True when `id` names a built-in capsule.
bool isBuiltinSkill(const std::string& id);

// The body of built-in `id` for a turn where the Telegram-only tools are
// (telegram=true) or are not offered. 'hardware' is this build's board
// inventory (buildBoardIdentity). "" for an id that is not a built-in.
std::string builtinSkillBody(const std::string& id, bool telegram);

}  // namespace agent
