// builtin_skills_none.cpp - engine 2 links no built-in skill (PRD section 4; lane E2-P-SKILL).
// The skill store (skill_store.cpp) is shared by both engines and asks builtin_skills.h for its
// built-ins; an engine-2 device image does not link builtin_skills.cpp (engine1_only.txt), so this
// file answers "none" there: the store lists, serves and guards only the owner's stored skills.
// Host builds (native, nimbusd, harness-lab) compile both engines and link the real table, so the
// whole file is engine 2's only (engine2_only.txt drops it from an engine-1 device build).
#if defined(NIMBUS_ENGINE) && NIMBUS_ENGINE == 2

#include "nimbus/harness/builtin_skills.h"

namespace agent {

size_t builtinSkillCount() { return 0; }

const BuiltinSkill& builtinSkill(size_t) {
  static const BuiltinSkill kNone{"", "", ""};
  return kNone;
}

bool isBuiltinSkill(const std::string&) { return false; }

std::string builtinSkillBody(const std::string&, bool, bool) { return std::string(); }

std::string builtinSkillBody(const std::string&, bool) { return std::string(); }

}  // namespace agent

#endif
