// connectors_approval.cpp - the owner's /mcp approve|deny <name> (CUM-33), shared by both engines'
// /mcp (lane E2-P-CONN1 moved it here from engine 1's dispatcher unchanged; connectors.h). Owner-facing
// device copy, never sent to a model.
#include <ArduinoJson.h>

#include "connectors.h"
#include "nimbus/store/persist.h"   // kStorageFullMessage
#include "store.h"                  // connectorsJson, setConnectorsJson

namespace agent {
namespace connectors {
namespace mcp {

std::string approvalCommand(const std::string& args) {
  // Cross-lane (CUM-33): owner approves/denies a device-dialed MCP server so
  // its tools can be discovered. Flips the `appr` bit on the mcp entry in the
  // connectors blob - the SAME NVS rail as the token-gated /api/connectors
  // write. Enforcement (discovery/retraction) lives in N4's connectors sync.
  String rest = String(args.c_str()); rest.trim();
  int sp = rest.indexOf(' ');
  String action = (sp < 0) ? rest : rest.substring(0, sp); action.toLowerCase();
  String name = (sp < 0) ? String("") : rest.substring(sp + 1); name.trim();
  if ((action != "approve" && action != "deny") || name.length() == 0)
    return "Usage: /mcp approve <name>  |  /mcp deny <name>";
  JsonDocument blob;
  bool ok = !deserializeJson(blob, store::connectorsJson());   // truthy == error
  bool found = false;
  if (ok && blob.is<ArduinoJson::JsonArray>()) {
    for (ArduinoJson::JsonObject e : blob.as<ArduinoJson::JsonArray>()) {
      if (String((const char*)(e["kind"] | "")) == "mcp" &&
          String((const char*)(e["name"] | "")) == name) {
        e["appr"] = (action == "approve") ? 1 : 0;
        found = true;
        break;
      }
    }
  }
  const std::string n = name.c_str();
  if (!found) return "No MCP server named \"" + n + "\". Add it on the web page first.";
  String out; serializeJson(blob, out);
  // Reply only after the change landed: an approval a full NVS refused would
  // otherwise read as done and silently revert at the next restart.
  if (!store::setConnectorsJson(out)) return nimbus::store::kStorageFullMessage;
  return action == "approve"
             ? "Approved MCP server \"" + n + "\". Its tools will be discovered shortly."
             : "Removed approval for MCP server \"" + n + "\". Its tools are retracted.";
}

}  // namespace mcp
}  // namespace connectors
}  // namespace agent
