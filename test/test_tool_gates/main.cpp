#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/harness/websearch.h"
#include "nimbus/orch/tool_gates.h"
#include "nimbus/orch/tool_registry.h"

// CUM-508: a provider-gated tool must be callable the moment its key appears -
// never "registered at boot if the key existed". These tests walk EVERY row of
// kGatedTools (the one table of provider-gated tools), so a new gated tool is
// covered with no new test, and a tool that slips back to boot-time
// registration fails here.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static const Principal kAdmin = principalForRole("web", Role::Admin);
static const Principal kMember = principalForRole("1002", Role::User);

static bool listed(const ToolRegistry& reg, const std::string& name) {
  bool inManifest = false, inSpecs = false, inAdmin = false, inMember = false;
  for (const auto& t : reg.manifest()) inManifest |= (t.name == name);
  for (const auto& s : reg.toolSpecs()) inSpecs |= (s.name == name);
  for (const auto& s : reg.toolSpecsFor(kAdmin)) inAdmin |= (s.name == name);
  for (const auto& s : reg.toolSpecsFor(kMember)) inMember |= (s.name == name);
  const std::string rpc = reg.handleRpc(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", kAdmin);
  const bool inRpc = rpc.find("\"name\":\"" + name + "\"") != std::string::npos;
  // Every listing surface must agree - the model, /api/tools and MCP clients.
  TEST_ASSERT_EQUAL_MESSAGE(inManifest, inSpecs, name.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(inManifest, inAdmin, name.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(inManifest, inMember, name.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(inManifest, inRpc, name.c_str());
  return inManifest;
}

static ToolResult call(const ToolRegistry& reg, const std::string& name, const char* args) {
  JsonDocument d;
  deserializeJson(d, args);
  return reg.dispatch(name, d.as<JsonObjectConst>(), kAdmin);
}

// The device boot shape for every row: the tool registers unconditionally, the
// gates go on (in either order), and NO key exists yet. Then the owner saves a
// key mid-session: the tool must list and run on the next look, no restart -
// and vanish again if the key is removed.
static void test_every_gated_tool_appears_when_its_key_is_added_after_boot() {
  for (int gatesFirst = 0; gatesFirst < 2; ++gatesFirst) {
    for (const GatedTool& row : kGatedTools) {
      ToolRegistry reg;
      std::map<ToolGate, bool> keys;   // all absent at boot
      int handled = 0;
      auto registerTool = [&] {
        reg.add(row.tool, "gated tool", [&handled](JsonObjectConst, const Principal&) {
          ++handled;
          return ToolResult::ok("ran");
        });
      };
      auto gates = [&] { applyToolGates(reg, [&keys](ToolGate g) { return keys[g]; }); };
      if (gatesFirst) { gates(); registerTool(); } else { registerTool(); gates(); }

      TEST_ASSERT_TRUE_MESSAGE(reg.has(row.tool), row.tool);         // registered regardless
      TEST_ASSERT_FALSE_MESSAGE(listed(reg, row.tool), row.tool);    // not advertised keyless
      ToolResult r = call(reg, row.tool, "{}");
      TEST_ASSERT_FALSE(r.success);
      TEST_ASSERT_EQUAL_STRING(row.refusal, r.error.c_str());        // honest refusal
      TEST_ASSERT_EQUAL(0, handled);                                 // never reached the handler

      keys[row.gate] = true;   // the owner adds the key; nothing re-registers
      TEST_ASSERT_TRUE_MESSAGE(listed(reg, row.tool), row.tool);
      TEST_ASSERT_TRUE(call(reg, row.tool, "{}").success);
      TEST_ASSERT_EQUAL(1, handled);

      keys[row.gate] = false;  // key removed -> hidden again, refused again
      TEST_ASSERT_FALSE_MESSAGE(listed(reg, row.tool), row.tool);
      TEST_ASSERT_FALSE(call(reg, row.tool, "{}").success);
    }
  }
}

// One gate never leaks onto another tool, and ungated tools are untouched.
static void test_gates_are_per_tool() {
  ToolRegistry reg;
  for (const GatedTool& row : kGatedTools)
    reg.add(row.tool, "g", [](JsonObjectConst, const Principal&) { return ToolResult::ok("ran"); });
  reg.add("memory.search", "always", [](JsonObjectConst, const Principal&) { return ToolResult::ok("ran"); });
  std::map<ToolGate, bool> keys;
  applyToolGates(reg, [&keys](ToolGate g) { return keys[g]; });
  TEST_ASSERT_TRUE(listed(reg, "memory.search"));
  for (const GatedTool& open : kGatedTools) {
    keys.clear();
    keys[open.gate] = true;
    for (const GatedTool& row : kGatedTools)
      TEST_ASSERT_EQUAL_MESSAGE(row.gate == open.gate, listed(reg, row.tool), row.tool);
  }
  reg.setGate("memory.search", nullptr, "");   // a null gate is a no-op removal
  TEST_ASSERT_TRUE(listed(reg, "memory.search"));
}

// The refusal is model-facing text the assistant relays: it names the fix and
// keeps to the copy rules (printable ASCII, no em dash).
static void test_every_refusal_names_the_fix() {
  for (const GatedTool& row : kGatedTools) {
    const std::string r = row.refusal;
    TEST_ASSERT_TRUE_MESSAGE(r.find("web app") != std::string::npos, row.tool);
    for (unsigned char c : r) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7F, row.tool);
  }
}

// The REAL web.search registration (the code the device runs): registered with
// no key, listed once the key exists, and the handler keeps its rails - the
// budget refusal fires before a billable call, a missing query fails, and a
// success is counted once.
static void test_real_web_search_registration_follows_the_key() {
  ToolRegistry reg;
  int searches = 0, recorded = 0;
  bool overBudget = false, hasKey = false;
  agent::websearch::WebSearchToolDeps wd;
  wd.search = [&searches](const std::string& q, int k) {
    ++searches;
    agent::websearch::Result r;
    r.ok = true;
    r.digest = "answer for " + q + " k=" + std::to_string(k);
    return r;
  };
  wd.overBudget = [&overBudget] { return overBudget; };
  wd.recordCall = [&recorded] { ++recorded; };
  agent::websearch::registerWebSearchTool(reg, wd);
  applyToolGates(reg, [&hasKey](ToolGate g) { return g == ToolGate::WebSearchKey && hasKey; });

  TEST_ASSERT_FALSE(listed(reg, "web.search"));   // booted with no key
  TEST_ASSERT_FALSE(call(reg, "web.search", R"({"query":"bristol weather"})").success);
  TEST_ASSERT_EQUAL(0, searches);

  hasKey = true;                                  // key saved after boot
  TEST_ASSERT_TRUE(listed(reg, "web.search"));
  ToolResult r = call(reg, "web.search", R"({"query":"bristol weather","max_results":3})");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_EQUAL_STRING("answer for bristol weather k=3", r.output.c_str());
  TEST_ASSERT_EQUAL(1, searches);
  TEST_ASSERT_EQUAL(1, recorded);

  TEST_ASSERT_FALSE(call(reg, "web.search", "{}").success);   // missing query
  overBudget = true;
  r = call(reg, "web.search", R"({"query":"x"})");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(r.error.find("budget") != std::string::npos);
  TEST_ASSERT_EQUAL(1, searches);                 // refused before spending a call
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_gated_tool_appears_when_its_key_is_added_after_boot);
  RUN_TEST(test_gates_are_per_tool);
  RUN_TEST(test_every_refusal_names_the_fix);
  RUN_TEST(test_real_web_search_registration_follows_the_key);
  UNITY_END();
  return 0;
}
