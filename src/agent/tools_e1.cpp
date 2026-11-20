// tools_e1.cpp - the old engine's tool families on the device's shared registry (lane RB-SEAM2).
// Engine 1 only: tools/engine/guard_files.txt marks it engine1_only, and an engine-2 image
// links tools_plain.cpp instead. The calls moved here verbatim from memory::begin and
// model_refresh::begin, which now call these at the same point (engine_tools.h).
#include "engine_tools.h"

#include "nimbus/orch/memory_tools.h"   // registerMemoryTools
#include "nimbus/orch/models_tool.h"    // registerModelsTool
#include "nimbus/orch/openq_tools.h"    // registerOpenQTools
#include "nimbus/orch/prefs_tools.h"    // registerPrefsTools
#include "prefs_subsystem.h"            // the prefs and openq contexts

namespace agent {
namespace tools {

void registerMemoryFamilies(nimbus::orch::ToolRegistry& reg, const nimbus::orch::MemoryContext& ctx) {
  nimbus::orch::registerMemoryTools(reg, ctx);
  nimbus::orch::registerPrefsTools(reg, prefs::context());
  nimbus::orch::registerOpenQTools(reg, prefs::openQContext());   // lane POL2: openq.edit
}

void registerModelsFamily(nimbus::orch::ToolRegistry& reg, const nimbus::orch::ModelsToolHandlers& h) {
  nimbus::orch::registerModelsTool(reg, h);
}

}  // namespace tools
}  // namespace agent
