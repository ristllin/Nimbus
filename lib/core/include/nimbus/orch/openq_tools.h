#pragma once
// openq_tools - the model's way to work the owner's open-question list
// (open_questions.h holds the store and the raise-once observation).
//
// Rails, mirroring prefs_tools:
//   - owner only: a member or guest turn never sees or changes the list;
//   - never from an unattended turn (routine, wakeup, sub-agent synthesis, the
//     nightly dream): a routine must not plant follow-ups on the owner;
//   - a turn that already read outside content cannot change the list at all
//     (an open item is the owner's, not something a web page may shape).
//
// Portable, Arduino-free, host-tested (test_open_questions).

#include <functional>
#include <string>

#include "nimbus/orch/open_questions.h"
#include "nimbus/orch/tool_registry.h"

namespace nimbus {
namespace orch {

struct OpenQToolContext {
  // Run fn with the store under its lock. false = the store is unavailable.
  std::function<bool(const std::function<void(OpenQuestions&)>&)> withOpenQ;
  std::function<bool()> inScheduledTurn;       // null = never
  std::function<bool()> externalContentSeen;   // null = never
  std::function<uint32_t()> nowHours;          // null = 0
};

// openq.edit: add | remove | list.
void registerOpenQTools(ToolRegistry& reg, const OpenQToolContext& ctx);

}  // namespace orch
}  // namespace nimbus
