#pragma once
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/world.h"  // SessionInfo

// session_tools - the `session.*` MCP tool surface: the orchestrator's control
// over its own sub-agents. Spawn a session, send it a message AS THE USER, poll
// its reply like the user would, terminate, and list. This is what turns the
// journal's fire-and-forget sub-jobs into addressable conversational agents.
//
// Portable: the actual spawn/tell/poll/terminate are DEVICE operations (heavy
// fabric + provider adapters + journal), injected as std::function handlers. The
// tool schemas, argument validation, and result formatting live here and are
// host-tested (pio test -e native) with fake handlers. The device binds the
// handlers to the real fabric (spawn works today; tell/poll-as-user - continuing
// a provider conversation and reading its reply - is the bidirectional seam).
namespace nimbus {
namespace orch {

struct SessionHandlers {
  // Spawn a sub-agent for `task`. Returns its id ("" + err on failure). provider
  // /model may be empty (device resolves by sub-session priority).
  std::function<std::string(const std::string& provider, const std::string& model,
                            const std::string& task, std::string& err)> spawn;
  // Send `message` into session `id` as a user turn. false + err on failure.
  std::function<bool(const std::string& id, const std::string& message, std::string& err)> tell;
  // Read the latest reply from session `id` (like the user would). Returns the
  // reply text, or "" (with err="" ) when nothing is pending yet.
  std::function<std::string(const std::string& id, std::string& err)> poll;
  // Terminate session `id`. false + err on failure.
  std::function<bool(const std::string& id, std::string& err)> terminate;
  // Snapshot of all sessions (for session.list + the running-sessions digest).
  std::function<std::vector<SessionInfo>()> list;
};

// Register session.spawn / tell / poll / terminate / list on `reg`, backed by
// `h`. `h` is captured by value; the handlers it holds must outlive `reg`. A
// missing handler makes its tool report "not supported on this device".
void registerSessionTools(ToolRegistry& reg, const SessionHandlers& h);

}  // namespace orch
}  // namespace nimbus
