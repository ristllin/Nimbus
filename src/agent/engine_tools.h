#pragma once
// engine_tools.h - the tool families an engine registers on the device's shared registry
// (lane RB-SEAM2). The neutral subsystems call these at the point of their own begin()
// where the family always registered, so the registry's order is unchanged:
//   tools_e1.cpp     the old engine's families (memory.*, prefs.edit, openq.edit, models.list)
//   tools_plain.cpp  the plain engine's (RB-I0 onward) defines the same calls
// A device image links exactly one of the two. Registrations whose descriptions or result
// texts live in a subsystem file stay there behind the engine-1 guard (moving them would
// move model-facing text to another source); tools/engine/guard_files.txt lists them.

namespace nimbus {
namespace orch {
class ToolRegistry;
struct MemoryContext;
struct ModelsToolHandlers;
}  // namespace orch
}  // namespace nimbus

namespace agent {
namespace tools {

// memory.*, prefs.edit and openq.edit (from memory::begin, after calc).
void registerMemoryFamilies(nimbus::orch::ToolRegistry& reg, const nimbus::orch::MemoryContext& ctx);

// models.list (from model_refresh::begin).
void registerModelsFamily(nimbus::orch::ToolRegistry& reg, const nimbus::orch::ModelsToolHandlers& h);

}  // namespace tools
}  // namespace agent
