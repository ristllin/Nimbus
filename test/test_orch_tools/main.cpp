#include <unity.h>

#include <ArduinoJson.h>

#include <string>

#include "nimbus/orch/orch_schema.h"
#include "nimbus/orch/tool_registry.h"

using namespace nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;

void setUp() {}
void tearDown() {}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// A registry with two tools: an echo tool and one that fails, plus one reading
// a typed arg - enough to exercise dispatch, schema, and error mapping.
static ToolRegistry buildRegistry() {
  ToolRegistry reg;
  reg.add("memory.write", "store a memory",
          [](ArduinoJson::JsonObjectConst args, const nimbus::orch::Principal&) {
            const char* content = args["content"].is<const char*>()
                                      ? args["content"].as<const char*>() : "";
            if (!content[0]) return ToolResult::fail("missing 'content'");
            return ToolResult::ok(std::string("stored: ") + content);
          },
          R"({"type":"object","properties":{"content":{"type":"string"}},"required":["content"]})");
  reg.add("device.reboot", "reboot the device",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) { return ToolResult::ok("rebooting"); });
  return reg;
}

// ---- registry basics --------------------------------------------------------

static void test_registry_add_and_manifest() {
  ToolRegistry reg = buildRegistry();
  TEST_ASSERT_EQUAL_INT(2, reg.size());
  TEST_ASSERT_TRUE(reg.has("memory.write"));
  TEST_ASSERT_FALSE(reg.has("nope"));
  auto man = reg.manifest();
  TEST_ASSERT_EQUAL_INT(2, (int)man.size());
  TEST_ASSERT_EQUAL_STRING("memory.write", man[0].name.c_str());  // add order stable
}

// toolSpecs() carries the schema that manifest() drops, so each provider adapter
// can wrap it into a callable function-tool. Assert: (a) one spec per tool in add
// order, (b) the schema is preserved verbatim and parses as a JSON object, (c) a
// tool registered with NO schema normalizes to "{}" (a valid object schema), so an
// adapter never emits a tool whose `parameters` is an empty/invalid string.
static void test_registry_tool_specs_carry_schema() {
  ToolRegistry reg = buildRegistry();
  auto specs = reg.toolSpecs();
  TEST_ASSERT_EQUAL_INT(2, (int)specs.size());
  TEST_ASSERT_EQUAL_STRING("memory.write", specs[0].name.c_str());  // add order
  TEST_ASSERT_EQUAL_STRING("device.reboot", specs[1].name.c_str());

  // memory.write's schema round-trips to a valid object with the required field.
  JsonDocument sd;
  TEST_ASSERT_TRUE(deserializeJson(sd, specs[0].schemaJson) ==
                   ArduinoJson::DeserializationError::Ok);
  TEST_ASSERT_TRUE(sd.is<JsonObject>());
  TEST_ASSERT_TRUE(has(specs[0].schemaJson, "\"required\":[\"content\"]"));

  // device.reboot was added with the default (empty) schema -> normalized to "{}".
  TEST_ASSERT_EQUAL_STRING("{}", specs[1].schemaJson.c_str());
  JsonDocument ed;
  TEST_ASSERT_TRUE(deserializeJson(ed, specs[1].schemaJson) ==
                   ArduinoJson::DeserializationError::Ok);
  TEST_ASSERT_TRUE(ed.is<JsonObject>());
}

static void test_registry_add_replaces_in_place() {
  ToolRegistry reg = buildRegistry();
  reg.add("memory.write", "new desc",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) { return ToolResult::ok("v2"); });
  TEST_ASSERT_EQUAL_INT(2, reg.size());  // replaced, not appended
  auto man = reg.manifest();
  TEST_ASSERT_EQUAL_STRING("new desc", man[0].description.c_str());
}

static void test_dispatch_success_and_failure() {
  ToolRegistry reg = buildRegistry();
  JsonDocument d;
  d["content"] = "hello world";
  ToolResult ok = reg.dispatch("memory.write", d.as<JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(ok.success);
  TEST_ASSERT_TRUE(has(ok.output, "stored: hello world"));

  JsonDocument empty;
  empty.to<JsonObject>();
  ToolResult bad = reg.dispatch("memory.write", empty.as<JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_FALSE(bad.success);
  TEST_ASSERT_TRUE(has(bad.error, "missing 'content'"));
}

static void test_dispatch_unknown_tool() {
  ToolRegistry reg = buildRegistry();
  JsonDocument d; d.to<JsonObject>();
  ToolResult r = reg.dispatch("ghost.tool", d.as<JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(has(r.error, "unknown tool"));
}

// ---- MCP JSON-RPC dispatch --------------------------------------------------

static void test_rpc_tools_list() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "\"id\":1"));
  TEST_ASSERT_TRUE(has(resp, "memory.write"));
  TEST_ASSERT_TRUE(has(resp, "device.reboot"));
  // inputSchema nests as an object (not a quoted string)
  TEST_ASSERT_TRUE(has(resp, "\"inputSchema\":{"));
  TEST_ASSERT_TRUE(has(resp, "\"required\":[\"content\"]"));
}

static void test_rpc_tools_call_success() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(
      R"({"jsonrpc":"2.0","id":"abc","method":"tools/call",)"
      R"("params":{"name":"memory.write","arguments":{"content":"note"}}})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "\"id\":\"abc\""));   // string id echoed as string
  TEST_ASSERT_TRUE(has(resp, "stored: note"));
  TEST_ASSERT_TRUE(has(resp, "\"isError\":false"));
  TEST_ASSERT_TRUE(has(resp, "\"type\":\"text\""));
}

static void test_rpc_tools_call_error_maps_iserror() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
      R"("params":{"name":"memory.write","arguments":{}}})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "\"isError\":true"));
  TEST_ASSERT_TRUE(has(resp, "missing 'content'"));
}

static void test_rpc_unknown_method_and_parse_errors() {
  ToolRegistry reg = buildRegistry();
  std::string m = reg.handleRpc(R"({"jsonrpc":"2.0","id":9,"method":"bogus"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(m, "-32601"));  // method not found
  std::string p = reg.handleRpc("{not json", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(p, "-32700"));  // parse error
}

static void test_rpc_notification_no_response() {
  ToolRegistry reg = buildRegistry();
  // no "id" => JSON-RPC notification => empty response string
  std::string resp = reg.handleRpc(R"({"jsonrpc":"2.0","method":"ping"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(resp.empty());
}

static void test_rpc_ping() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(R"({"jsonrpc":"2.0","id":7,"method":"ping"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "\"id\":7"));
  TEST_ASSERT_TRUE(has(resp, "\"result\":{}"));
}

// A string id carrying a double-quote must be ESCAPED, not spliced raw - else
// the response is malformed JSON. Assert the whole reply re-parses and the id
// round-trips verbatim (this is the externally-reachable RPC surface).
static void test_rpc_string_id_is_escaped() {
  ToolRegistry reg = buildRegistry();
  // JSON source: id is the 3-char string  a"b
  std::string resp = reg.handleRpc(R"({"jsonrpc":"2.0","id":"a\"b","method":"ping"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  JsonDocument out;
  TEST_ASSERT_TRUE(deserializeJson(out, resp) == ArduinoJson::DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("a\"b", out["id"].as<const char*>());
  TEST_ASSERT_TRUE(out["result"].is<JsonObject>());
}

// Top-level JSON that is not an object -> -32600 invalid request (untested path).
static void test_rpc_non_object_is_invalid_request() {
  ToolRegistry reg = buildRegistry();
  TEST_ASSERT_TRUE(has(reg.handleRpc("[1,2,3]", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin)), "-32600"));
  TEST_ASSERT_TRUE(has(reg.handleRpc("42", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin)), "-32600"));
}

// tools/call for a tool that isn't registered maps to an MCP isError result
// (the failed ToolResult), not a JSON-RPC method error.
static void test_rpc_tools_call_unknown_tool() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
      R"("params":{"name":"ghost.tool","arguments":{}}})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "\"isError\":true"));
  TEST_ASSERT_TRUE(has(resp, "unknown tool"));
}

// tools/call with no tool name -> -32602 invalid params (untested path).
static void test_rpc_tools_call_missing_name() {
  ToolRegistry reg = buildRegistry();
  std::string resp = reg.handleRpc(
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"arguments":{}}})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "-32602"));
}

// ---- remove / removeByPrefix (dynamic tool retraction, e.g. outbound MCP) ---

static void test_registry_remove_one() {
  ToolRegistry reg = buildRegistry();
  int before = reg.size();
  TEST_ASSERT_TRUE(reg.has("device.reboot"));
  TEST_ASSERT_TRUE(reg.remove("device.reboot"));
  TEST_ASSERT_FALSE(reg.has("device.reboot"));
  TEST_ASSERT_EQUAL(before - 1, reg.size());
  TEST_ASSERT_FALSE(reg.remove("device.reboot"));  // already gone -> false
  TEST_ASSERT_TRUE(reg.has("memory.write"));       // survivor intact
}

static void test_registry_remove_clears_policy() {
  ToolRegistry reg = buildRegistry();
  reg.setPolicy("device.reboot", ToolRegistry::Verdict::gated("pending"));
  TEST_ASSERT_EQUAL(ToolRegistry::Verdict::Gated, reg.policyFor("device.reboot").kind);
  reg.remove("device.reboot");
  // A same-named tool re-added later must not inherit the retracted gate.
  reg.add("device.reboot", "x", [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return ToolResult::ok("ok");
  });
  TEST_ASSERT_EQUAL(ToolRegistry::Verdict::Allow, reg.policyFor("device.reboot").kind);
}

static void test_registry_remove_by_prefix() {
  ToolRegistry reg;
  auto noop = [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return ToolResult::ok("");
  };
  reg.add("mcp.linear.create_issue", "d", noop);
  reg.add("mcp.linear.list_issues", "d", noop);
  reg.add("mcp.files.read", "d", noop);
  reg.add("memory.search", "d", noop);
  int n = reg.removeByPrefix("mcp.linear.");
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_FALSE(reg.has("mcp.linear.create_issue"));
  TEST_ASSERT_FALSE(reg.has("mcp.linear.list_issues"));
  TEST_ASSERT_TRUE(reg.has("mcp.files.read"));   // different server survives
  TEST_ASSERT_TRUE(reg.has("memory.search"));    // native tool untouched
  TEST_ASSERT_EQUAL(0, reg.removeByPrefix(""));  // empty prefix is a no-op
}

// ---- unknown-tool recovery (CUM-538) ------------------------------------------
// A Mistral head called the orch_turn field `session_ops` and the connector `gcal`
// as tools and got a bare "unknown tool"; it gave up and promised work it never
// queued. Each unknown name must now carry a way back.

static ToolResult callUnknown(const ToolRegistry& reg, const char* name) {
  JsonDocument d; d.to<JsonObject>();
  return reg.dispatch(name, d.as<JsonObjectConst>(),
                      nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
}

// Case 1: a reply field. The text says it is not a tool, names the orch_turn
// reply, says how to spawn, and carries the REAL spawn room (T02d claimed 0 while
// the prompt offered 6).
static void test_unknown_tool_turn_field_session_ops() {
  ToolRegistry reg = buildRegistry();
  ToolRegistry::UnknownToolHints h;
  h.spawnRoom = [] { return 6; };
  reg.setUnknownToolHints(h);
  ToolResult r = callUnknown(reg, "session_ops");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(has(r.error, "session_ops is not a tool"));
  TEST_ASSERT_TRUE(has(r.error, "field of your orch_turn reply"));
  TEST_ASSERT_TRUE(has(r.error, "\"op\":\"spawn\""));
  TEST_ASSERT_TRUE(has(r.error, "You can start up to 6 sub-agents this turn"));
  TEST_ASSERT_FALSE(has(r.error, "unknown tool"));
  // An alias points at the real field; dotted / dashed / cased spellings fold.
  ToolResult a = callUnknown(reg, "Session.Spawn");
  TEST_ASSERT_TRUE(has(a.error, "Use the session_ops field of your orch_turn reply"));
  TEST_ASSERT_TRUE(has(a.error, "up to 6"));
  // No room hook installed: never invent a number.
  ToolRegistry bare = buildRegistry();
  TEST_ASSERT_FALSE(has(callUnknown(bare, "session_ops").error, "You can start up to"));
}

// The class, not the instance: EVERY property of the wire schema the head fills
// (ORCH_SCHEMA_BODY) plus the retired spawn/await must answer as a reply field. A
// field added to the schema without a hint fails here.
static void test_unknown_tool_every_schema_field_is_a_reply_field() {
  JsonDocument schema;
  TEST_ASSERT_TRUE(deserializeJson(schema, nimbus::orch::ORCH_SCHEMA_BODY,
                                   ArduinoJson::DeserializationOption::NestingLimit(64)) ==
                   ArduinoJson::DeserializationError::Ok);
  ToolRegistry reg = buildRegistry();
  int n = 0;
  for (ArduinoJson::JsonPairConst kv : schema["properties"].as<ArduinoJson::JsonObjectConst>()) {
    const std::string f = kv.key().c_str();
    TEST_ASSERT_TRUE_MESSAGE(ToolRegistry::isTurnFieldName(f), f.c_str());
    ToolResult r = callUnknown(reg, f.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(r.error, "is not a tool"), f.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(r.error, "orch_turn reply"), f.c_str());
    n++;
  }
  TEST_ASSERT_TRUE(n >= 8);   // the schema really was walked
  for (const char* retired : {"spawn", "await"})
    TEST_ASSERT_TRUE(has(callUnknown(reg, retired).error, "orch_turn reply"));
  // A field that has a live tool twin offers it, in the caller's spelling.
  TEST_ASSERT_TRUE(has(callUnknown(reg, "mem_write").error, "or call memory_write now"));
}

// Case 2: a configured connector. It runs inside sub-agent jobs on its provider;
// the text names the backend and the spawn shape.
static void test_unknown_tool_connector_names_spawn_backend() {
  ToolRegistry reg = buildRegistry();
  ToolRegistry::UnknownToolHints h;
  h.connectorProvider = [](const std::string& n) {
    return (n == "gcal") ? std::string("mistral")
         : (n == "linear") ? std::string("any") : std::string();
  };
  h.spawnRoom = [] { return 4; };
  reg.setUnknownToolHints(h);
  ToolResult r = callUnknown(reg, "gcal");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(has(r.error, "gcal is a connector attached to Mistral sub-agent jobs, not a tool"));
  TEST_ASSERT_TRUE(has(r.error, "Spawn a mistral job with the request"));
  TEST_ASSERT_TRUE(has(r.error, "\"provider\":\"mistral\""));
  TEST_ASSERT_TRUE(has(r.error, "up to 4"));
  // A provider-agnostic connector gets the same shape without a provider name.
  ToolResult any = callUnknown(reg, "linear");
  TEST_ASSERT_TRUE(has(any.error, "linear is a connector attached to sub-agent jobs"));
  TEST_ASSERT_FALSE(has(any.error, "\"provider\""));
}

// Case 3: anything else names what IS callable, in the caller's spelling (the
// head loop sees memory_write, an MCP client memory.write), never a reply-field
// alias the loop hides, and bounded no matter how many tools are registered.
static void test_unknown_tool_lists_callable_names_bounded() {
  ToolRegistry reg = buildRegistry();
  auto noop = [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return ToolResult::ok("");
  };
  reg.add("session.spawn", "hidden from the loop", noop);
  ToolResult u = callUnknown(reg, "calendar_lookup");
  TEST_ASSERT_TRUE(has(u.error, "unknown tool: calendar_lookup."));
  TEST_ASSERT_TRUE(has(u.error, "Callable tools: memory_write, device_reboot."));
  TEST_ASSERT_FALSE(has(u.error, "session_spawn"));
  ToolResult d = callUnknown(reg, "ghost.tool");
  TEST_ASSERT_TRUE(has(d.error, "Callable tools: memory.write, device.reboot."));
  // The LAN MCP endpoint (its own kMcpNs principal) gets the dotted names it can
  // call, whatever the spelling of its bad name.
  nimbus::orch::Principal mcp =
      nimbus::orch::principalForRole(nimbus::orch::kMcpNs, nimbus::orch::Role::User);
  mcp.ns = nimbus::orch::kMcpNs;
  JsonDocument e; e.to<JsonObject>();
  ToolResult m = reg.dispatch("search", e.as<JsonObjectConst>(), mcp);
  TEST_ASSERT_TRUE(has(m.error, "Callable tools: memory.write"));
  TEST_ASSERT_FALSE(has(m.error, "memory_write"));
  for (int i = 0; i < 200; i++)
    reg.add("bulk.tool_number_" + std::to_string(i), "d", noop);
  ToolResult b = callUnknown(reg, "nope");
  TEST_ASSERT_TRUE(has(b.error, "more)."));
  TEST_ASSERT_TRUE(b.error.size() < ToolRegistry::kUnknownToolListMax + 120);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_tool_turn_field_session_ops);
  RUN_TEST(test_unknown_tool_every_schema_field_is_a_reply_field);
  RUN_TEST(test_unknown_tool_connector_names_spawn_backend);
  RUN_TEST(test_unknown_tool_lists_callable_names_bounded);
  RUN_TEST(test_registry_add_and_manifest);
  RUN_TEST(test_registry_tool_specs_carry_schema);
  RUN_TEST(test_registry_add_replaces_in_place);
  RUN_TEST(test_dispatch_success_and_failure);
  RUN_TEST(test_dispatch_unknown_tool);
  RUN_TEST(test_rpc_tools_list);
  RUN_TEST(test_rpc_tools_call_success);
  RUN_TEST(test_rpc_tools_call_error_maps_iserror);
  RUN_TEST(test_rpc_unknown_method_and_parse_errors);
  RUN_TEST(test_rpc_notification_no_response);
  RUN_TEST(test_rpc_ping);
  RUN_TEST(test_rpc_string_id_is_escaped);
  RUN_TEST(test_rpc_non_object_is_invalid_request);
  RUN_TEST(test_rpc_tools_call_unknown_tool);
  RUN_TEST(test_rpc_tools_call_missing_name);
  RUN_TEST(test_registry_remove_one);
  RUN_TEST(test_registry_remove_clears_policy);
  RUN_TEST(test_registry_remove_by_prefix);
  UNITY_END();
  return 0;
}
