#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

// CUM-242 x1 (lane L1) - the Cumulo router HEAD rewrite. A device holding only a
// Cumulo key runs the SELECTED upstream's NATIVE head (its real mid-turn tool
// loop) over the router: wire::applyRouter rewrites each head request to
// host = cumuloBase, path = /router/<upstream> + path, auth = Bearer cumuloKey
// (the router injects the upstream's real key server-side, so the device's own
// x-api-key/Bearer is dropped). These tests pin that rewrite at the wire, for all
// three fabric upstreams, plus the LAN plain-HTTP base and the no-op-when-unset
// guarantee that keeps every direct-key turn byte-identical.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::headerOf;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static const char* kRouterHost = "app.cumulo-nimbus.ai";
static const char* kRouterKey = "cumulo_sk_test4chars";

// Turn a healthy keyed FakeProviderDeps into a Cumulo router-head pd: the router
// override + the per-turn model/key the cumulo head closure builds on-device.
static void makeRouterPd(FakeProviderDeps& d, agent::providers::ProviderDeps& pd,
                         const std::string& base, const std::string& model) {
  pd = d.contract();
  pd.routerBase = [base] { return base; };
  pd.routerKey = [] { return std::string(kRouterKey); };
  pd.key = [](const char*) { return std::string(kRouterKey); };
  pd.orchModel = [model](const char*) { return model; };
}

// Canned single-shot bodies (the tool_use / output_text / message.output input IS
// the turn) - mirrors the per-provider wire suites.
static std::string antBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"hi\",\"memory\":\"\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}";
}
static std::string oaiBody() {
  return "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":[{\"type\":\"message\","
         "\"content\":[{\"type\":\"output_text\",\"text\":"
         "\"{\\\"reply\\\":\\\"hi\\\",\\\"memory\\\":\\\"\\\",\\\"ask\\\":\\\"\\\"}\"}]}],"
         "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";
}
static std::string misBody() {
  return "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\","
         "\"content\":\"{\\\"reply\\\":\\\"hi\\\",\\\"memory\\\":\\\"\\\"}\"}],"
         "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";
}

// ---- anthropic upstream over the router -------------------------------------
static void test_anthropic_routes_through_router() {
  FakeProviderDeps d;
  d.toolLoop = false;  // single shot: one request, easy to assert
  d.http.script.push_back({kRouterHost, "/router/anthropic/v1/messages", 200, antBody()});
  agent::providers::ProviderDeps pd;
  makeRouterPd(d, pd, kRouterHost, "claude-opus-5");
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING(kRouterHost, r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/anthropic/v1/messages", r.path.c_str());
  TEST_ASSERT_TRUE(r.tls);
  TEST_ASSERT_EQUAL_UINT16(443, r.port);
  // The router key rides Bearer; the client's own x-api-key is DROPPED.
  TEST_ASSERT_EQUAL_STRING((std::string("Bearer ") + kRouterKey).c_str(),
                           headerOf(r, "Authorization").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "x-api-key").c_str());
  // The bare model (upstream prefix already split off by the head) rides the body.
  TEST_ASSERT_TRUE(r.body.find("\"model\":\"claude-opus-5\"") != std::string::npos);
}

// ---- openai upstream over the router (Responses API) ------------------------
static void test_openai_routes_through_router() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back({kRouterHost, "/router/openai/v1/responses", 200, oaiBody()});
  agent::providers::ProviderDeps pd;
  makeRouterPd(d, pd, kRouterHost, "gpt-5.6");
  std::string conv, out, err;
  bool ok = providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING(kRouterHost, r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/responses", r.path.c_str());
  TEST_ASSERT_EQUAL_STRING((std::string("Bearer ") + kRouterKey).c_str(),
                           headerOf(r, "Authorization").c_str());
}

// ---- mistral upstream over the router ---------------------------------------
static void test_mistral_routes_through_router() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back({kRouterHost, "/router/mistral/v1/conversations", 200, misBody()});
  agent::providers::ProviderDeps pd;
  makeRouterPd(d, pd, kRouterHost, "mistral-large-latest");
  std::string conv, out, err;
  bool ok = providers::orchTurnMistral(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING(kRouterHost, r.host.c_str());
  TEST_ASSERT_TRUE(r.path.rfind("/router/mistral/v1/conversations", 0) == 0);
  TEST_ASSERT_EQUAL_STRING((std::string("Bearer ") + kRouterKey).c_str(),
                           headerOf(r, "Authorization").c_str());
}

// ---- the tool loop actually runs over the router ----------------------------
// The whole point of item 1: a registry tool fires MID-TURN, and EVERY round's
// request travels through /router/anthropic (not the direct host).
static void test_tool_loop_runs_over_router() {
  FakeProviderDeps d;
  d.toolLoop = true;
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  // round 1: prose + memory_search tool_use; round 2: orch_turn (terminal).
  d.http.script.push_back({kRouterHost, "/router/anthropic/v1/messages", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"checking\"},"
      "{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
      "\"input\":{\"q\":\"tea\"}}],\"stop_reason\":\"tool_use\","
      "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}"});
  d.http.script.push_back({kRouterHost, "/router/anthropic/v1/messages", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
      "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\","
      "\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}"});
  agent::providers::ProviderDeps pd;
  makeRouterPd(d, pd, kRouterHost, "claude-opus-5");
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  // The mid-turn tool fired, and both rounds went through the router.
  TEST_ASSERT_EQUAL_UINT(1, rig.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", rig.dispatched[0].name.c_str());
  TEST_ASSERT_TRUE(d.http.seen.size() >= 2);
  for (const auto& r : d.http.seen) {
    TEST_ASSERT_EQUAL_STRING(kRouterHost, r.host.c_str());
    TEST_ASSERT_TRUE(r.path.rfind("/router/anthropic/", 0) == 0);
    TEST_ASSERT_EQUAL_STRING("", headerOf(r, "x-api-key").c_str());
  }
}

// ---- a LAN plain-HTTP base (the bench test rig) -----------------------------
static void test_http_base_uses_plain_http_and_port() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back({"192.168.50.61", "/router/anthropic/v1/messages", 200, antBody()});
  agent::providers::ProviderDeps pd;
  makeRouterPd(d, pd, "http://192.168.50.61:8080", "claude-opus-5");
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING("192.168.50.61", r.host.c_str());
  TEST_ASSERT_FALSE(r.tls);
  TEST_ASSERT_EQUAL_UINT16(8080, r.port);
  TEST_ASSERT_EQUAL_STRING("/router/anthropic/v1/messages", r.path.c_str());
}

// ---- no override => direct wire is byte-identical ---------------------------
static void test_no_override_is_direct() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antBody()});
  auto pd = d.contract();  // NO routerBase/routerKey set
  std::string conv, out, err;
  bool ok = providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/messages", r.path.c_str());
  // Direct anthropic still uses x-api-key, no Bearer.
  TEST_ASSERT_EQUAL_STRING("sk-fake-ant", headerOf(r, "x-api-key").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "Authorization").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_anthropic_routes_through_router);
  RUN_TEST(test_openai_routes_through_router);
  RUN_TEST(test_mistral_routes_through_router);
  RUN_TEST(test_tool_loop_runs_over_router);
  RUN_TEST(test_http_base_uses_plain_http_and_port);
  RUN_TEST(test_no_override_is_direct);
  UNITY_END();
  return 0;
}
