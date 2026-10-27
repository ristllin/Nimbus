#include "nimbus/orch/session_tools.h"

#include "nimbus/orch/action_rules.h"   // sessionVisible - whose sub-agents a caller sees (CUM-830)

using ArduinoJson::JsonObjectConst;

namespace nimbus {
namespace orch {

namespace {
std::string arg(JsonObjectConst a, const char* k) {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
}

// A session id the caller may not see answers exactly like one that does not exist
// (no probing for the owner's sub-agents). With no list handler nothing is checked.
bool hiddenFrom(const SessionHandlers& h, const std::string& id, const Principal& who) {
  if (!h.list) return false;
  const ActionCaller c = actionCaller(who, false);
  for (const SessionInfo& s : h.list())
    if (s.id == id) return !sessionVisible(s, c);
  return false;   // unknown here: the handler answers for it
}

ToolResult notRunning(const std::string& id) { return ToolResult::fail("no running session '" + id + "'"); }
}  // namespace

void registerSessionTools(ToolRegistry& reg, const SessionHandlers& h) {
  reg.add("session.spawn",
          "Start a sub-agent to work on a task in parallel. Returns its session id.",
          [h](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> ToolResult {
            if (!h.spawn) return ToolResult::fail("spawn not supported on this device");
            std::string task = arg(a, "task");
            if (task.empty()) return ToolResult::fail("missing 'task'");
            std::string err;
            std::string id = h.spawn(arg(a, "provider"), arg(a, "model"), task, err);
            if (id.empty()) return ToolResult::fail(err.empty() ? "spawn failed" : err);
            return ToolResult::ok("spawned session " + id);
          },
          R"({"type":"object","properties":{"task":{"type":"string"},)"
          R"("provider":{"type":"string"},"model":{"type":"string"}},"required":["task"]})");

  reg.add("session.tell",
          "Send a message to a running sub-agent, as if you were its user.",
          [h](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.tell) return ToolResult::fail("tell not supported on this device");
            std::string id = arg(a, "id"), msg = arg(a, "message");
            if (id.empty() || msg.empty()) return ToolResult::fail("need 'id' and 'message'");
            if (hiddenFrom(h, id, who)) return notRunning(id);
            std::string err;
            if (!h.tell(id, msg, err)) return ToolResult::fail(err.empty() ? "tell failed" : err);
            return ToolResult::ok("sent to " + id);
          },
          R"({"type":"object","properties":{"id":{"type":"string"},"message":{"type":"string"}},)"
          R"("required":["id","message"]})");

  reg.add("session.poll",
          "Read the latest reply from a sub-agent (like reading its message to you).",
          [h](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.poll) return ToolResult::fail("poll not supported on this device");
            std::string id = arg(a, "id");
            if (id.empty()) return ToolResult::fail("missing 'id'");
            if (hiddenFrom(h, id, who)) return notRunning(id);
            std::string err;
            std::string reply = h.poll(id, err);
            if (!err.empty()) return ToolResult::fail(err);
            return ToolResult::ok(reply.empty() ? "(no reply yet)" : reply);
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");

  reg.add("session.terminate",
          "Stop a running sub-agent.",
          [h](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.terminate) return ToolResult::fail("terminate not supported on this device");
            std::string id = arg(a, "id");
            if (id.empty()) return ToolResult::fail("missing 'id'");
            if (hiddenFrom(h, id, who)) return notRunning(id);
            std::string err;
            if (!h.terminate(id, err)) return ToolResult::fail(err.empty() ? "terminate failed" : err);
            return ToolResult::ok("terminated " + id);
          },
          R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})");

  reg.add("session.list",
          "List your running sub-agents and their state.",
          [h](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.list) return ToolResult::fail("list not supported on this device");
            const std::vector<SessionInfo> s = visibleSessions(h.list(), actionCaller(who, false));
            if (s.empty()) return ToolResult::ok("No running sessions.");
            std::string out = renderSessions(s);
            return ToolResult::ok(out);
          },
          R"({"type":"object","properties":{}})");
}

}  // namespace orch
}  // namespace nimbus
