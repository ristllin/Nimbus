#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

// runFabricLoop (Stage 2 phase 5) - the engine-owned multi-provider loop.
// The scenario that IS the feature: anthropic serves round 0 and a tool
// executes; anthropic then dies mid-turn (network, twice - initial + same-host
// retry); the loop switches to openai, which re-runs the SAME round against the
// shared transcript (the executed result carries over - NOTHING re-dispatches)
// and finishes the turn.

using harness_test::bodyHas;
using harness_test::FakeProviderDeps;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

void setUp() {}
void tearDown() {}

static std::string antToolUseBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"tea\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";
}
static std::string oaiFinalBody() {
  return "{\"id\":\"resp_f\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
         "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}],"
         "\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}";
}

static void test_midturn_failover_carries_results_no_redispatch() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antToolUseBody()});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 0, ""});   // round 1 dies
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 0, ""});   // same-host retry dies
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiFinalBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  int notifies = 0;
  std::string nFrom, nTo;
  std::string out, err;
  orch::TokenUsage u;
  bool ok = providers::runFabricLoop(
      pd, {"anthropic", "openai"}, "SYS", "USER", out, err, rig.ht, &u,
      [&](const std::string& f, const std::string& t) { notifies++; nFrom = f; nTo = t; });
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(4, (int)d.http.seen.size());
  // The tool ran EXACTLY once - the switch re-ran the round, not the dispatch.
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  // One switch, correctly attributed.
  TEST_ASSERT_EQUAL(1, notifies);
  TEST_ASSERT_EQUAL_STRING("anthropic", nFrom.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", nTo.c_str());
  // The openai request (req #3) replays the SHARED transcript: the user seed,
  // the anthropic-executed call, and its result - as OpenAI wire shapes.
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"content\":\"USER\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"type\":\"function_call\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"call_id\":\"tu_1\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"type\":\"function_call_output\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"output\":\"tool-ok\""));
  TEST_ASSERT_TRUE(bodyHas(d, 3, "\"store\":false"));
  // The turn completed on the fallback host.
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
  // Usage summed across BOTH hosts (50+60 / 10+15).
  TEST_ASSERT_EQUAL(110, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(25, (int)u.completionTokens);
}

static void test_all_hosts_down_fails_soft_with_error() {
  FakeProviderDeps d;
  for (int i = 0; i < 4; i++)              // A initial+retry, B initial+retry
    d.http.script.push_back({"", "", 0, ""});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  int notifies = 0;
  std::string out, err;
  bool ok = providers::runFabricLoop(
      pd, {"anthropic", "openai"}, "S", "U", out, err, rig.ht, nullptr,
      [&](const std::string&, const std::string&) { notifies++; });
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_STRING("network", err.c_str());
  TEST_ASSERT_EQUAL(1, notifies);          // switched once, then B exhausted too
  TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());
}

static void test_single_host_list_never_switches() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 0, ""});
  d.http.script.push_back({"", "", 0, ""});   // same-host retry
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  int notifies = 0;
  std::string out, err;
  bool ok = providers::runFabricLoop(
      pd, {"mistral"}, "S", "U", out, err, rig.ht, nullptr,
      [&](const std::string&, const std::string&) { notifies++; });
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(0, notifies);
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());   // initial + one retry only
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_midturn_failover_carries_results_no_redispatch);
  RUN_TEST(test_all_hosts_down_fails_soft_with_error);
  RUN_TEST(test_single_host_list_never_switches);
  return UNITY_END();
}
