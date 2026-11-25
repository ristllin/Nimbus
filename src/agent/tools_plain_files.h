#pragma once
#include <ArduinoJson.h>

#include "nimbus/orch/tool_registry.h"

// tools_plain_files - engine 2's files_admin and media tools over engine 1's own handlers (lane E2-P-OUT2;
// PRD section 4): image_generate, pdf_write, share_file, file_stat and fetch_url (files_admin, media) and
// media_play, media_pause, media_stop (media), all behind `more`. Each maps engine 2's arguments (a store
// path, project/name) onto the engine-1 handler's ({project, name, ...}) and calls it: one copy of each
// tool for both engines. A generated image or PDF is stored and its path returned; it reaches the person
// only through `send` (lane E2-P-OUT1). fetch_url queues a download under the owner's download setting;
// /fetch approve|deny and the tg_poll pump (orchestrator_plain.cpp pollJobs) are engine 1's.
//
// The registration is portable (host-tested in test_plain_files_tools with fake handlers); the device
// binding names engine 1's handlers (files_subsystem.cpp, pdf_tool.cpp, src/sfx/music.cpp).
namespace agent::plainfiles {

// Engine 1's handlers, called with engine 1's arguments. A tool whose handler is empty is not registered.
struct OldHandlers {
  nimbus::orch::ToolHandler stat;    // files.stat {project, name}
  nimbus::orch::ToolHandler share;   // files.share {project, name, share}
  nimbus::orch::ToolHandler fetch;   // files.fetch {url, project, name}
  nimbus::orch::ToolHandler pdf;     // files.pdf {project, name, title, text}
  nimbus::orch::ToolHandler image;   // image.generate {prompt, project, name, size, quality}
  nimbus::orch::ToolHandler play;    // media.play {path, queue}
  nimbus::orch::ToolHandler pause;   // media.pause {}
  nimbus::orch::ToolHandler stop;    // media.stop {}
  nimbus::orch::ToolHandler list;    // media.list {}
};

// Register the engine-2 tools on `reg` over `old`. Their groups are the class table's
// (orch_tool_groups.cpp), so none is in the always-on array.
void registerTools(nimbus::orch::ToolRegistry& reg, const OldHandlers& old);

#ifdef ARDUINO
// The device: registerTools over engine 1's handlers. Called once from the engine-2 family hook
// (tools_plain.cpp registerMemoryFamilies).
void registerDevice(nimbus::orch::ToolRegistry& reg);
#endif

}  // namespace agent::plainfiles
