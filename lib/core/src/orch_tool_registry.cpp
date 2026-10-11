#include "nimbus/orch/tool_registry.h"

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonArray;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

namespace nimbus {
namespace orch {

void ToolRegistry::add(const std::string& name, const std::string& description,
                       ToolHandler handler, const std::string& schemaJson) {
  for (auto& t : tools_) {
    if (t.name == name) {  // replace in place (stable position)
      t.description = description;
      t.handler = std::move(handler);
      t.schemaJson = schemaJson;
      return;
    }
  }
  tools_.push_back(Tool{name, description, schemaJson, std::move(handler)});
}

void ToolRegistry::setAdminOnly(const std::string& name, bool v) {
  for (auto& t : tools_)
    if (t.name == name) { t.adminOnly = v; return; }
  // Unknown name: no-op on purpose (see the header) - a renamed tool stays
  // advertised and handler-refused rather than disappearing everywhere.
}

bool ToolRegistry::isAdminOnly(const std::string& name) const {
  const Tool* t = find(name);
  return t && t->adminOnly;
}

const Tool* ToolRegistry::find(const std::string& name) const {
  for (const auto& t : tools_) if (t.name == name) return &t;
  return nullptr;
}
bool ToolRegistry::has(const std::string& name) const { return find(name) != nullptr; }

bool ToolRegistry::remove(const std::string& name) {
  for (size_t i = 0; i < tools_.size(); ++i) {
    if (tools_[i].name == name) {
      tools_.erase(tools_.begin() + i);
      setPolicy(name, Verdict::allow());  // drop any lingering static policy
      return true;
    }
  }
  return false;
}

int ToolRegistry::removeByPrefix(const std::string& prefix) {
  if (prefix.empty()) return 0;
  int removed = 0;
  for (size_t i = 0; i < tools_.size();) {
    if (tools_[i].name.rfind(prefix, 0) == 0) {
      std::string name = tools_[i].name;
      tools_.erase(tools_.begin() + i);
      setPolicy(name, Verdict::allow());
      ++removed;
    } else {
      ++i;
    }
  }
  return removed;
}

const ToolRegistry::GateEntry* ToolRegistry::gateFor(const std::string& name) const {
  for (const auto& g : gates_) if (g.name == name) return &g;
  return nullptr;
}

void ToolRegistry::setGate(const std::string& name, std::function<bool()> open,
                           const std::string& reason) {
  for (size_t i = 0; i < gates_.size(); ++i) {
    if (gates_[i].name != name) continue;
    if (open) gates_[i] = GateEntry{name, std::move(open), reason};
    else gates_.erase(gates_.begin() + i);
    return;
  }
  if (open) gates_.push_back(GateEntry{name, std::move(open), reason});
}

bool ToolRegistry::gateOpen(const std::string& name) const {
  const GateEntry* g = gateFor(name);
  return !g || g->open();
}

std::vector<ToolInfo> ToolRegistry::manifest() const {
  std::vector<ToolInfo> out;
  out.reserve(tools_.size());
  for (const auto& t : tools_)
    if (gateOpen(t.name)) out.push_back(ToolInfo{t.name, t.description});
  return out;
}

std::vector<ToolRegistry::Spec> ToolRegistry::toolSpecs() const {
  std::vector<Spec> out;
  out.reserve(tools_.size());
  // Carry the schema through (manifest() drops it): each adapter wraps these into
  // its provider's function-tool shape. Normalize an empty/blank schema to "{}" so
  // a consumer that parses it always gets a valid object schema.
  for (const auto& t : tools_) {
    if (!gateOpen(t.name)) continue;   // CUM-508: a closed gate is not advertised
    std::string schema = t.schemaJson.empty() ? "{}" : t.schemaJson;
    out.push_back(Spec{t.name, t.description, schema});
  }
  return out;
}

std::vector<ToolRegistry::Spec> ToolRegistry::toolSpecsFor(const Principal& who) const {
  const bool admin = who.perms().manageTenants;
  if (admin) return toolSpecs();
  std::vector<Spec> out;
  out.reserve(tools_.size());
  for (const auto& t : tools_) {
    if (t.adminOnly) continue;   // not callable by this caller -> not advertised
    if (!gateOpen(t.name)) continue;   // prerequisite missing right now (CUM-508)
    out.push_back(Spec{t.name, t.description, t.schemaJson.empty() ? "{}" : t.schemaJson});
  }
  return out;
}

void ToolRegistry::setPolicy(const std::string& name, Verdict v) {
  for (size_t i = 0; i < policies_.size(); ++i) {
    if (policies_[i].name == name) {
      if (v.kind == Verdict::Allow) policies_.erase(policies_.begin() + i);
      else                          policies_[i].verdict = std::move(v);
      return;
    }
  }
  if (v.kind != Verdict::Allow) policies_.push_back(PolicyEntry{name, std::move(v)});
}

ToolRegistry::Verdict ToolRegistry::policyFor(const std::string& name) const {
  for (const auto& p : policies_)
    if (p.name == name) return p.verdict;   // table entries are never Allow
  if (resolver_) {
    Verdict v = resolver_(name);
    if (v.kind != Verdict::Allow) return v;
  }
  return Verdict::allow();
}

ToolResult ToolRegistry::dispatch(const std::string& name, JsonObjectConst args,
                                  const Principal& who) const {
  // Policy gate FIRST - a denied/gated tool must never reach its handler, and
  // this single funnel covers both direct dispatch and handleRpc's tools/call.
  Verdict v = policyFor(name);
  if (v.kind == Verdict::Deny)
    return ToolResult::fail(v.reason.empty() ? ("tool denied by policy: " + name) : v.reason);
  if (v.kind == Verdict::Gated)
    return ToolResult::fail(v.reason.empty()
                                ? ("tool gated pending owner approval: " + name)
                                : v.reason);
  const Tool* t = find(name);
  if (!t) return ToolResult::fail(unknownToolText(name, who));
  // CUM-508: a gated tool whose prerequisite is missing right now refuses with
  // its honest reason, never reaching a handler that would need it.
  const GateEntry* g = gateFor(name);
  if (g && !g->open())
    return ToolResult::fail(g->reason.empty() ? ("tool unavailable: " + name) : g->reason);
  if (!t->handler) return ToolResult::fail("tool has no handler: " + name);
  return t->handler(args, who);
}

// ---- unknown-tool recovery (CUM-538) ------------------------------------------

namespace {

// One orch_turn reply field (or retired alias) a head model may mistake for a
// tool. `field` is the reply field to use instead; `how` says how, in one clause;
// `tool` names a registry tool that does the same thing NOW (rendered in the
// caller's spelling, mentioned only when registered); `spawns` adds the live
// spawn room to the text.
struct TurnFieldHint {
  const char* alias;
  const char* field;
  const char* how;
  const char* tool;
  bool spawns;
};

const char kSpawnHow[] =
    "add {\"op\":\"spawn\",\"provider\":\"<provider>\",\"task\":\"<the request>\"} "
    "to it and finish the turn with orch_turn; the sub-agent starts right after";
const char kResultsHow[] =
    "sub-agent results arrive by themselves in [FRESH RESULTS] on an automatic "
    "turn, so there is nothing to wait for or poll";

const TurnFieldHint kTurnFields[] = {
    {"session_ops", "session_ops", kSpawnHow, nullptr, true},
    {"spawn", "session_ops", kSpawnHow, nullptr, true},
    {"session_spawn", "session_ops", kSpawnHow, nullptr, true},
    {"spawn_agent", "session_ops", kSpawnHow, nullptr, true},
    {"session_tell", "session_ops",
     "a running sub-agent cannot be messaged, so spawn a new one with the follow-up",
     nullptr, true},
    {"session_poll", "session_ops", kResultsHow, nullptr, false},
    {"await", "session_ops", kResultsHow, nullptr, false},
    {"mem_write", "mem_write",
     "add {\"content\":\"<the fact>\"} to store it after this turn", "memory.write", false},
    {"mem_query", "mem_query",
     "add a search string and the results arrive next turn", "memory.search", false},
    {"memory", "memory", "it carries your running notes to the next turn", nullptr, false},
    {"scratchpad", "scratchpad", "set active/short/mid/long to update your plan",
     nullptr, false},
    {"device", "device", "add the device action there to apply it after this turn",
     "device.control", false},
    {"reply", "reply", "put your answer to the owner there", nullptr, false},
    {"ask", "ask", "put your question for the owner there", nullptr, false},
};

// Lowercase, with '.', '-' and ' ' folded to '_': session.ops, Session-Ops and
// session_ops are one name.
std::string foldName(const std::string& n) {
  std::string out = n;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    else if (c == '.' || c == '-' || c == ' ') c = '_';
  }
  return out;
}

const TurnFieldHint* turnFieldFor(const std::string& name) {
  const std::string f = foldName(name);
  for (const auto& h : kTurnFields)
    if (f == h.alias) return &h;
  return nullptr;
}

// The head loop advertises registry names with '.' -> '_' (loopToolName); an MCP
// client uses them dotted. Answer in the spelling the caller used.
std::string spellLike(const std::string& callerName, const std::string& regName) {
  if (callerName.find('.') != std::string::npos) return regName;
  std::string out = regName;
  for (char& c : out)
    if (c == '.') c = '_';
  return out;
}

const char* providerLabel(const std::string& slug) {
  if (slug == "mistral") return "Mistral";
  if (slug == "openai") return "OpenAI";
  if (slug == "anthropic") return "Anthropic";
  if (slug == "zai") return "Z.ai";
  if (slug == "cumulo") return "Cumulo Nimbus";
  return nullptr;
}

}  // namespace

bool ToolRegistry::isTurnFieldName(const std::string& name) {
  return turnFieldFor(name) != nullptr;
}

std::string ToolRegistry::callableNamesText(const std::string& name, const Principal& who) const {
  std::string list;
  int more = 0;
  for (const auto& s : toolSpecsFor(who)) {
    // A registry tool that is ALSO a reply-field alias (session.spawn/tell/poll)
    // is hidden from the head loop; never offer it here as callable.
    if (isTurnFieldName(s.name)) continue;
    const std::string n = spellLike(name, s.name);
    if (list.size() + n.size() + 2 > kUnknownToolListMax) { more++; continue; }
    if (!list.empty()) list += ", ";
    list += n;
  }
  if (list.empty()) return "No tools are callable right now.";
  std::string out = "Callable tools: " + list;
  if (more) out += " (+" + std::to_string(more) + " more)";
  return out + ".";
}

std::string ToolRegistry::unknownToolText(const std::string& name, const Principal& who) const {
  const int room = hints_.spawnRoom ? hints_.spawnRoom() : -1;
  const std::string roomText =
      room < 0 ? std::string()
               : " You can start up to " + std::to_string(room) + " sub-agents this turn.";
  // 1. A field of the orch_turn reply, not a tool.
  if (const TurnFieldHint* h = turnFieldFor(name)) {
    std::string out = name + " is not a tool. ";
    out += (foldName(name) == h->field)
               ? std::string("It is a field of your orch_turn reply: ")
               : std::string("Use the ") + h->field + " field of your orch_turn reply: ";
    out += h->how;
    if (h->tool && has(h->tool)) out += std::string(", or call ") + spellLike(name, h->tool) + " now";
    out += ".";
    if (h->spawns) out += roomText;
    return out;
  }
  // 2. A configured connector: it runs inside sub-agent jobs, never as a tool.
  const std::string prov = hints_.connectorProvider ? hints_.connectorProvider(name) : std::string();
  if (!prov.empty()) {
    const char* label = providerLabel(prov);
    if (!label)   // "any" or an unknown slug: no provider to name
      return name + " is a connector attached to sub-agent jobs, not a tool. Spawn a "
             "sub-agent with the request: a session_ops item {\"op\":\"spawn\",\"task\":"
             "\"<the request>\"} in your orch_turn reply." + roomText;
    return name + " is a connector attached to " + label + " sub-agent jobs, not a tool. "
           "Spawn a " + prov + " job with the request: a session_ops item {\"op\":\"spawn\","
           "\"provider\":\"" + prov + "\",\"task\":\"<the request>\"} in your orch_turn reply." +
           roomText;
  }
  // 3. Anything else: name what IS callable (bounded).
  return "unknown tool: " + name + ". " + callableNamesText(name, who);
}

namespace {
// Build a JSON-RPC error response string. `idJson` is the already-serialized id
// value ("null", a number, or a quoted string) so we echo the caller's id type.
std::string rpcError(const std::string& idJson, int code, const char* message) {
  JsonDocument d;
  d["jsonrpc"] = "2.0";
  JsonObject err = d["error"].to<JsonObject>();
  err["code"] = code;
  err["message"] = message;
  std::string out;
  // Serialize then splice the raw id (ArduinoJson can't hold "raw" easily here).
  serializeJson(d, out);
  // out looks like {"jsonrpc":"2.0","error":{...}} - insert "id":<idJson>, after
  // the opening brace so the response carries the caller's id.
  out.insert(1, "\"id\":" + idJson + ",");
  return out;
}
}  // namespace

std::string ToolRegistry::handleRpc(const std::string& requestJson,
                                    const Principal& who) const {
  JsonDocument req;
  DeserializationError e = deserializeJson(req, requestJson);
  if (e) return rpcError("null", -32700, "parse error");
  if (!req.is<JsonObject>()) return rpcError("null", -32600, "invalid request");

  // Capture the id as a serialized token so we echo its exact JSON type. A
  // request with NO id (or an explicit null) is a notification -> no response.
  // Serialize the id THROUGH ArduinoJson rather than hand-splicing quotes: a
  // string id containing a '"', '\\', or control char must be escaped, or the
  // response is malformed JSON. This endpoint is externally reachable (Ph4 LAN),
  // so a crafted id must never be able to inject structure into the reply.
  bool isNotification = req["id"].isNull();  // absent or explicit null
  std::string idJson = "null";
  if (!isNotification) {
    JsonVariantConst id = req["id"];
    if (id.is<const char*>() || id.is<long long>() || id.is<double>()) {
      serializeJson(id, idJson);  // "a\"b" -> "\"a\\\"b\"" (escaped); 7 -> "7"
    } else {
      isNotification = true;  // unusual id type (object/array/bool) -> notification
    }
  }

  const char* method = req["method"].is<const char*>() ? req["method"].as<const char*>() : "";
  if (!method[0]) {
    return isNotification ? std::string() : rpcError(idJson, -32600, "missing method");
  }

  std::string result;  // serialized "result" object contents

  if (std::string(method) == "ping") {
    result = "{}";
  } else if (std::string(method) == "tools/list") {
    JsonDocument d;
    JsonArray arr = d["tools"].to<JsonArray>();
    for (const auto& t : tools_) {
      if (!gateOpen(t.name)) continue;   // CUM-508: same listing rule as toolSpecs
      JsonObject o = arr.add<JsonObject>();
      o["name"] = t.name;
      o["description"] = t.description;
      // inputSchema is an embedded JSON document - parse the stored schema string
      // so it nests as an object, not a quoted string.
      JsonDocument sd;
      if (deserializeJson(sd, t.schemaJson) == DeserializationError::Ok)
        o["inputSchema"] = sd;
      else
        o["inputSchema"].to<JsonObject>();
    }
    serializeJson(d, result);
  } else if (std::string(method) == "tools/call") {
    JsonObjectConst params = req["params"].as<JsonObjectConst>();
    const char* name = params["name"].is<const char*>() ? params["name"].as<const char*>() : "";
    if (!name[0]) {
      return isNotification ? std::string() : rpcError(idJson, -32602, "missing tool name");
    }
    JsonObjectConst args = params["arguments"].as<JsonObjectConst>();  // null-safe view
    ToolResult r = dispatch(name, args, who);
    JsonDocument d;
    JsonArray content = d["content"].to<JsonArray>();
    JsonObject block = content.add<JsonObject>();
    block["type"] = "text";
    block["text"] = r.success ? r.output : r.error;
    d["isError"] = !r.success;
    serializeJson(d, result);
  } else {
    return isNotification ? std::string() : rpcError(idJson, -32601, "method not found");
  }

  if (isNotification) return std::string();

  std::string out = "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":" + result + "}";
  return out;
}

}  // namespace orch
}  // namespace nimbus
