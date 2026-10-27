#pragma once
#include <cstddef>
#include <string>

// builtin_skills - the device's built-in capsules (architecture, deep-research,
// hardware, tools-guide, deliver-pdf, log-rows, food-log, food-coach): the [SKILLS] index line and the body
// skill.get serves. Portable and flash-resident (const tables, no heap until a
// body is rendered), so the host suite reads exactly what the model reads.
//
// CUM-523: a capsule never names a Telegram-only tool (reply.telegram,
// files.send) on a turn that was not offered one. The index lines are
// channel-neutral; a body is a list of parts, and a part written for one side
// (a turn offered the Telegram-only tools, or a turn that is not) is served only
// on that side. The device picks the side with the same per-turn sample the engine
// filters the tool list with (TurnEngine::turnTelegram).
namespace agent {

// The food playbooks' ids (lane CTX): the engine attaches food-log when the owner
// reports food, food-coach when they ask about it, both when the words do not say.
inline constexpr const char kFoodLogSkillId[] = "food-log";
inline constexpr const char kFoodCoachSkillId[] = "food-coach";

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

// The body of built-in `id` for a turn where reply.telegram is (telegram) or is not
// offered, and files.send is (fileSend) or is not (CUM-523: a public-mode guest
// turn has reply.telegram but not files.send). 'hardware' is this build's board
// inventory (buildBoardIdentity). "" for an id that is not a built-in.
std::string builtinSkillBody(const std::string& id, bool telegram, bool fileSend);
// Both tools offered, or neither.
std::string builtinSkillBody(const std::string& id, bool telegram);

}  // namespace agent
