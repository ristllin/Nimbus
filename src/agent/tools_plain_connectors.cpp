// tools_plain_connectors.cpp - engine 2's binding of the remote tool servers (lane E2-P-CONN1;
// tools_plain_connectors.h).
#include "tools_plain_connectors.h"

#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE != 2
#error "tools_plain_connectors.cpp is engine 2's: build it only in an env with -DNIMBUS_ENGINE=2"
#endif

#include "connectors.h"          // connectors::mcp::sync - engine 1's reconcile and discovery
#include "file_tools_device.h"   // tools::markOutsideCall
#include "memory_subsystem.h"    // memory::registry - the device's one registry

namespace agent::tools::connectorsdev {

void addServer(nimbus::orch::ToolRegistry& reg, const std::string& slug,
               const std::vector<nimbus::orch::mcp::ToolDef>& tools) {
  nimbus::orch::mcp::registerServerTools(
      reg, slug, tools,
      [](const std::string& s, const std::string& tool, ArduinoJson::JsonObjectConst a,
         const nimbus::orch::Principal& who) {
        markOutsideCall();   // marked even when refused, as web_search (conservative)
        return connectors::mcp::callTool(s, tool, a, who);
      });
}

void beforeTurn() { connectors::mcp::sync(memory::registry()); }

}  // namespace agent::tools::connectorsdev
