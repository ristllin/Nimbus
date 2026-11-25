#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/mcp_client.h"     // ToolDef, RemoteCall
#include "nimbus/orch/tool_registry.h"

// tools_plain_connectors - engine 2's binding of engine 1's remote tool servers (lane E2-P-CONN1;
// PC-0252 to PC-0254). The client, discovery, approval and the call itself are engine 1's
// (connectors.cpp, nimbus/orch/mcp_client.h, mcp_resilience.h); this file only binds them:
//   - a discovered server's tools register as mcp.<server>.<tool> through the shared
//     registration (mcp::registerServerTools); they have no class row, so they sit in the
//     connectors group behind `more` and read as outside content with an effect;
//   - each call marks the running turn as having read outside content (markOutsideCall), the way
//     web_search does, before engine 1's call runs with its own checks (write access, an approved,
//     enabled, device-dialed server, the per-server breaker);
//   - beforeTurn runs engine 1's reconcile and discovery (connectors::mcp::sync) on the poll task
//     before a person's turn, where engine 1 ran it while building its catalog.
// Engine 2 only (tools/engine/engine2_only.txt).
namespace agent::tools::connectorsdev {

// Called by connectors.cpp's registration on engine 2; each handler calls engine 1's
// connectors::mcp::callTool directly (so the tg_poll stack gate follows it).
void addServer(nimbus::orch::ToolRegistry& reg, const std::string& slug,
               const std::vector<nimbus::orch::mcp::ToolDef>& tools);

// Engine 1's reconcile and discovery over the device registry: at most one server is dialed per
// call, and only one not yet discovered. tg_poll, before a person's turn (no turn in flight).
void beforeTurn();

}  // namespace agent::tools::connectorsdev
