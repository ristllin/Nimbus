// tools_plain.cpp - the plain engine's (engine 2) tool families on the device's shared registry.
// Lane RB-I0: the stub registers NONE. An engine-2 image links this file instead of tools_e1.cpp
// (tools/engine/engine1_only.txt); memory::begin and model_refresh::begin call these hooks where
// the engine-1 families register. Every other old-engine registration sits inside the engine-1
// guard (tools/engine/list_tool_registrations.py --check). The engine-2 binding lanes register
// their own tools here or in their *_device.cpp files.
#include "engine_tools.h"
#include "notes_device.h"   // lane RB-N1: note

namespace agent {
namespace tools {

// note (lane RB-N1, notes_device.cpp). No origin is bound until the engine runs turns (RB-I2):
// until then every call is kept for approval (fail closed).
void registerMemoryFamilies(nimbus::orch::ToolRegistry& reg, const nimbus::orch::MemoryContext&) {
  notesdev::registerNote(reg, nullptr);
}

void registerModelsFamily(nimbus::orch::ToolRegistry&, const nimbus::orch::ModelsToolHandlers&) {}

}  // namespace tools
}  // namespace agent
