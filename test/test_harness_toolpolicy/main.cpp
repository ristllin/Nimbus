#include <unity.h>

#include <ArduinoJson.h>

#include <string>

#include "nimbus/orch/tool_registry.h"

// ToolPolicy suite - dispatch-level allow/deny/gate ENFORCEMENT (the ESP-Claw
// lesson: hiding a tool is not a boundary; the dispatch seam must refuse).
// Pins: default-allow is byte-identical to the pre-policy registry; Deny/Gated
// return a failed ToolResult carrying the reason and the handler NEVER runs;
// the policy is consulted on BOTH the direct dispatch() path and handleRpc's
// tools/call; the table wins over the resolver; and the device's scheduled-turn
// loop.create refusal (routed through a resolver) denies ONLY while the
// scheduled flag is up, with the SAME owner-visible refusal string.

using nimbus::orch::ToolRegistry;
using nimbus::orch::ToolResult;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

static int g_handlerRuns = 0;

static ToolRegistry buildReg() {
  g_handlerRuns = 0;
  ToolRegistry reg;
  reg.add("memory.search", "search", [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    g_handlerRuns++;
    return ToolResult::ok("found it");
  });
  reg.add("loop.create", "create a loop", [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    g_handlerRuns++;
    return ToolResult::ok("loop created");
  });
  return reg;
}

static ToolResult call(ToolRegistry& reg, const char* name) {
  JsonDocument d;
  deserializeJson(d, "{}");
  return reg.dispatch(name, d.as<JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
}

// One JSON-RPC tools/call round trip; returns (isError, text).
static void rpcCall(ToolRegistry& reg, const char* name, bool& isError, std::string& text) {
  std::string req = std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                "\"params\":{\"name\":\"") + name + "\",\"arguments\":{}}}";
  std::string resp = reg.handleRpc(req, nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  JsonDocument d;
  TEST_ASSERT_EQUAL(ArduinoJson::DeserializationError::Ok, deserializeJson(d, resp).code());
  isError = d["result"]["isError"] | false;
  text = std::string(d["result"]["content"][0]["text"] | "");
}

void setUp() {}
void tearDown() {}

// ---- default allow ----------------------------------------------------------

static void test_default_allow_runs_handler() {
  ToolRegistry reg = buildReg();
  ToolResult r = call(reg, "memory.search");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_EQUAL_STRING("found it", r.output.c_str());
  TEST_ASSERT_EQUAL(1, g_handlerRuns);
}

static void test_unknown_tool_still_fails_normally() {
  ToolRegistry reg = buildReg();
  ToolResult r = call(reg, "nope.nothing");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("unknown tool: nope.nothing", r.error.c_str());
}

// ---- static table: deny / gated ---------------------------------------------

static void test_deny_blocks_handler_and_surfaces_reason() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("memory.search", ToolRegistry::Verdict::deny("owner disabled search"));
  ToolResult r = call(reg, "memory.search");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("owner disabled search", r.error.c_str());
  TEST_ASSERT_EQUAL(0, g_handlerRuns);   // enforcement: the handler never ran
}

static void test_gated_blocks_handler_and_surfaces_reason() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("loop.create", ToolRegistry::Verdict::gated("needs owner approval"));
  ToolResult r = call(reg, "loop.create");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("needs owner approval", r.error.c_str());
  TEST_ASSERT_EQUAL(0, g_handlerRuns);
}

static void test_deny_default_reason_when_empty() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("memory.search", ToolRegistry::Verdict::deny(""));
  ToolResult r = call(reg, "memory.search");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("tool denied by policy: memory.search", r.error.c_str());
}

static void test_allow_entry_clears_a_table_deny() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("memory.search", ToolRegistry::Verdict::deny("blocked"));
  reg.setPolicy("memory.search", ToolRegistry::Verdict::allow());   // removes the entry
  ToolResult r = call(reg, "memory.search");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_EQUAL(1, g_handlerRuns);
}

// ---- resolver hook ----------------------------------------------------------

static bool g_scheduledTurn = false;

static void installScheduledResolver(ToolRegistry& reg) {
  // The device's memory_subsystem resolver, verbatim shape: deny loop.create
  // ONLY during a scheduled turn - SAME refusal string as the old in-handler
  // check ("a scheduled loop cannot create loops").
  reg.setPolicyResolver([](const std::string& name) -> ToolRegistry::Verdict {
    if (name == "loop.create" && g_scheduledTurn)
      return ToolRegistry::Verdict::deny("a scheduled loop cannot create loops");
    return ToolRegistry::Verdict::allow();
  });
}

static void test_resolver_denies_loop_create_only_during_scheduled_turn() {
  ToolRegistry reg = buildReg();
  installScheduledResolver(reg);

  g_scheduledTurn = false;               // owner-driven turn: allowed
  ToolResult r1 = call(reg, "loop.create");
  TEST_ASSERT_TRUE(r1.success);
  TEST_ASSERT_EQUAL(1, g_handlerRuns);

  g_scheduledTurn = true;                // scheduled turn: refused, handler skipped
  ToolResult r2 = call(reg, "loop.create");
  TEST_ASSERT_FALSE(r2.success);
  TEST_ASSERT_EQUAL_STRING("a scheduled loop cannot create loops", r2.error.c_str());
  TEST_ASSERT_EQUAL(1, g_handlerRuns);

  ToolResult r3 = call(reg, "memory.search");   // other tools untouched
  TEST_ASSERT_TRUE(r3.success);
  g_scheduledTurn = false;
}

static void test_table_wins_over_resolver() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("loop.create", ToolRegistry::Verdict::deny("table says no"));
  // A permissive resolver cannot override a table deny (table checked first).
  reg.setPolicyResolver([](const std::string&) { return ToolRegistry::Verdict::allow(); });
  ToolResult r = call(reg, "loop.create");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("table says no", r.error.c_str());
}

// ---- the JSON-RPC path (handleRpc tools/call) --------------------------------

static void test_rpc_path_consults_policy() {
  ToolRegistry reg = buildReg();
  reg.setPolicy("memory.search", ToolRegistry::Verdict::deny("owner disabled search"));
  bool isError = false;
  std::string text;
  rpcCall(reg, "memory.search", isError, text);
  TEST_ASSERT_TRUE(isError);
  TEST_ASSERT_EQUAL_STRING("owner disabled search", text.c_str());
  TEST_ASSERT_EQUAL(0, g_handlerRuns);

  reg.setPolicy("memory.search", ToolRegistry::Verdict::allow());
  rpcCall(reg, "memory.search", isError, text);
  TEST_ASSERT_FALSE(isError);
  TEST_ASSERT_EQUAL_STRING("found it", text.c_str());
  TEST_ASSERT_EQUAL(1, g_handlerRuns);
}

static void test_rpc_path_resolver_scheduled_turn() {
  ToolRegistry reg = buildReg();
  installScheduledResolver(reg);
  g_scheduledTurn = true;
  bool isError = false;
  std::string text;
  rpcCall(reg, "loop.create", isError, text);
  TEST_ASSERT_TRUE(isError);
  TEST_ASSERT_EQUAL_STRING("a scheduled loop cannot create loops", text.c_str());
  TEST_ASSERT_EQUAL(0, g_handlerRuns);
  g_scheduledTurn = false;
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_default_allow_runs_handler);
  RUN_TEST(test_unknown_tool_still_fails_normally);
  RUN_TEST(test_deny_blocks_handler_and_surfaces_reason);
  RUN_TEST(test_gated_blocks_handler_and_surfaces_reason);
  RUN_TEST(test_deny_default_reason_when_empty);
  RUN_TEST(test_allow_entry_clears_a_table_deny);
  RUN_TEST(test_resolver_denies_loop_create_only_during_scheduled_turn);
  RUN_TEST(test_table_wins_over_resolver);
  RUN_TEST(test_rpc_path_consults_policy);
  RUN_TEST(test_rpc_path_resolver_scheduled_turn);
  return UNITY_END();
}
