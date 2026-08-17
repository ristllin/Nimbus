#include <unity.h>

#include <ArduinoJson.h>

#include <string>

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

int main(int, char**) {
  UNITY_BEGIN();
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
  UNITY_END();
  return 0;
}
