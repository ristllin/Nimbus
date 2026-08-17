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

std::vector<ToolInfo> ToolRegistry::manifest() const {
  std::vector<ToolInfo> out;
  out.reserve(tools_.size());
  for (const auto& t : tools_) out.push_back(ToolInfo{t.name, t.description});
  return out;
}

std::vector<ToolRegistry::Spec> ToolRegistry::toolSpecs() const {
  std::vector<Spec> out;
  out.reserve(tools_.size());
  // Carry the schema through (manifest() drops it): each adapter wraps these into
  // its provider's function-tool shape. Normalize an empty/blank schema to "{}" so
  // a consumer that parses it always gets a valid object schema.
  for (const auto& t : tools_) {
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
  if (!t) return ToolResult::fail("unknown tool: " + name);
  if (!t->handler) return ToolResult::fail("tool has no handler: " + name);
  return t->handler(args, who);
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
