#pragma once
#include <functional>
#include <string>

#include "nimbus/orch/memory_tools.h"   // MemoryContext (the standing route)
#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/prefs_tools.h"
#include "nimbus/orch/tool_registry.h"

// lab_prefs - the lab's stand-in for the device's standing-rules glue
// (src/agent/prefs_subsystem.cpp): the portable OwnerPrefs store over an
// in-memory file, the prefs.edit tool registered EXACTLY as the device
// registers it, and the [STANDING RULES] compose input the rig feeds for an
// owner chat. The next-day legs advance the store's clock, so "a rule stated
// on day N applies on day N+1" runs against the real persistence path.
namespace lab {

class LabPrefs {
 public:
  LabPrefs();

  // prefs.edit (the device's registration, owner-only rails inside).
  void registerTools(nimbus::orch::ToolRegistry& reg);
  // Wire the rig's MemoryContext.saveStanding (a mem_write standing:true item
  // routes here, never the vector store) - call before registerMemoryTools.
  void bindStanding(nimbus::orch::MemoryContext& mc);
  // The [STANDING RULES] body for the compose input ("" when the chat is not
  // the owner's - the rig has one owner, so only the owner chat gets it).
  std::string blockFor(const std::string& chatId);
  // Day rollover for the next-day legs: the store's clock moves forward.
  void advanceHours(uint32_t h) { nowHours_ += h; }

  nimbus::orch::OwnerPrefs& store() { return prefs_; }
  uint32_t nowHours() const { return nowHours_; }

 private:
  nimbus::orch::OwnerPrefs prefs_;
  nimbus::orch::PrefsToolContext ctx_;
  uint32_t nowHours_ = 497800;   // ~2026-10-23 in device epoch hours
};

}  // namespace lab
