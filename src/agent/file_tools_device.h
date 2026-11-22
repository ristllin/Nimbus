#pragma once
#include "nimbus/orch/tool_registry.h"

// file_tools_device - the device binding of engine 2's plain file tools (lanes RB-N2b, E2-TOOLS; PRD section 4):
// the portable FileTools (nimbus/orch/file_tools.h, lib/core orch_file_tools.cpp) over the card seam
// files_subsystem.cpp gives (agent::files::toolsIo()). Engine 2 only (tools/engine/engine2_only.txt).
namespace agent::filetoolsdev {

// Register read_file, write_file, append_file, edit_file, list_files and delete_file on `reg` with the manifest's
// description and schema (plain::toolDecl, plain::declSchema), the write cap that schema states
// enforced by the library. Every call runs under the PSRAM-first scope. Called once, from the engine-2
// family hook (tools_plain.cpp), with or without a card: with none each tool answers the one no-card line.
void registerFileTools(nimbus::orch::ToolRegistry& reg);

}  // namespace agent::filetoolsdev
