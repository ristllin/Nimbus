#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/router_refusal.h"
#include "nimbus/orch/router_route.h"

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
  // SECURITY (mirror custom.cpp): the master Cumulo key must NEVER ride a cleartext
  // http socket. Over an http:// base, applyRouter drops the key entirely - no
  // Authorization and no x-api-key leave the device.
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "x-api-key").c_str());
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

// ---- a router refusal on the device's cumulo head (CUM-539 parity, CUM-530) ----
// The device cumulo head (src/agent/orchestrator.cpp cumuloAttempt/cumuloTurn)
// runs each attempt over the router with every request through the refusal tap,
// and the turn through the shared ladder. This rig is that head on the fake.
namespace rf = agent::router_refusal;

struct DeviceCumuloHead {
  FakeProviderDeps d;
  rf::RouterTap tap{nullptr, kRouterHost};
  std::string cv, out, err;
  DeviceCumuloHead() { d.toolLoop = false; }
  bool attempt(const std::string& sel, bool fresh, agent::HeadTools* tools = nullptr) {
    if (fresh) {
      cv.clear();
      out.clear();
      err.clear();
    }
    const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(sel);
    agent::providers::ProviderDeps pd;
    makeRouterPd(d, pd, kRouterHost, rr.model);
    tap.setInner(pd.http);
    pd.http = &tap;
    if (rr.upstream == "anthropic") return providers::orchTurnAnthropic(pd, cv, "SYS", "USER", out, err, tools, nullptr);
    if (rr.upstream == "mistral") return providers::orchTurnMistral(pd, cv, "SYS", "USER", out, err, tools, nullptr);
    if (rr.upstream == "openai") return providers::orchTurnOpenAI(pd, cv, "SYS", "USER", out, err, tools, nullptr);
    providers::configureRouterCompatHead(pd, rr.upstream, rr.model);
    return providers::orchTurnCustom(pd, cv, "SYS", "USER", out, err, tools, nullptr);
  }
  rf::LadderResult turn(const std::string& picked, const std::string& lastGood = "",
                        agent::HeadTools* tools = nullptr) {
    return rf::runLadder(tap, picked, lastGood, "gpt-5.6",
                         [&](const std::string& sel, bool fresh) { return attempt(sel, fresh, tools); });
  }
  void push(const char* path, int status, const std::string& body) {
    d.http.script.push_back({kRouterHost, path, status, body});
  }
};

static const char* kInvalidModel400 =
    "{\"object\":\"error\",\"message\":\"Invalid model: mistral/zai-glm-5-3\",\"type\":\"invalid_model\","
    "\"param\":null,\"code\":\"1500\"}";
static std::string routerRefusal(const char* reason) {
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason + "\",\"charged\":false,\"message\":\"refused\"}";
}

// The bench: the pick mistral/mistral/zai-glm-5-3 reached the router, the upstream
// answered 400 "Invalid model", and the device failed over with "cumulo is
// unavailable" while the pick stayed. Now the SAME turn answers on the router
// default with the honest line, on a fresh conversation.
static void test_device_invalid_model_pick_answers_on_default() {
  DeviceCumuloHead h;
  h.push("/router/mistral/v1/conversations", 400, kInvalidModel400);
  h.push("/router/openai/v1/responses", 200, oaiBody());
  const std::string picked = "mistral/mistral/zai-glm-5-3";
  const rf::LadderResult r = h.turn(picked);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, h.err.c_str());
  TEST_ASSERT_TRUE(r.fellBack());
  TEST_ASSERT_TRUE(r.kind == rf::RefusalKind::Selector);
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.used.c_str());
  TEST_ASSERT_EQUAL(2, (int)h.d.http.seen.size());
  TEST_ASSERT_TRUE(h.d.http.seen[0].body.find("\"model\":\"mistral/zai-glm-5-3\"") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/responses", h.d.http.seen[1].path.c_str());
  rf::prefixReply(h.out, rf::fallbackNotice(r.kind, picked, r.used));
  TEST_ASSERT_TRUE_MESSAGE(h.out.find("The Cumulo router does not offer mistral/mistral/zai-glm-5-3 for this key, so "
                                      "gpt-5.6 answered instead. Choose another model under Models.") !=
                               std::string::npos,
                           h.out.c_str());
  TEST_ASSERT_TRUE(h.out.find("unavailable") == std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("router refused mistral/mistral/zai-glm-5-3 () - answering on gpt-5.6"));
}

// The router's own refusal names its reason as a STRING "error" plus "reason",
// which the Anthropic wire's error.message filter drops: the tap widens the
// filter, so model_not_priced is read on every upstream wire.
static void test_device_model_not_priced_is_read_through_the_filter() {
  DeviceCumuloHead h;
  h.push("/router/anthropic/v1/messages", 403, routerRefusal("model_not_priced"));
  h.push("/router/openai/v1/responses", 200, oaiBody());
  const rf::LadderResult r = h.turn("anthropic/claude-opus-9");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, h.err.c_str());
  TEST_ASSERT_TRUE(r.kind == rf::RefusalKind::Selector);
  TEST_ASSERT_TRUE(LogCapture::contains("router refused anthropic/claude-opus-9 (model_not_priced)"));
}

// A route the router does not allow is not the pick's fault: answered on the
// default, the notice says the choice is kept (the device moves only a Selector).
static void test_device_route_refusal_answers_on_default_and_keeps_pick() {
  DeviceCumuloHead h;
  h.push("/router/zai/chat/completions", 403, routerRefusal("endpoint_not_allowed"));
  h.push("/router/openai/v1/responses", 200, oaiBody());
  const rf::LadderResult r = h.turn("zai/glm-5.3");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, h.err.c_str());
  TEST_ASSERT_TRUE(r.kind == rf::RefusalKind::Upstream);
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.used.c_str());
  TEST_ASSERT_TRUE(rf::fallbackNotice(r.kind, "zai/glm-5.3", r.used).find("Your model choice is kept.") !=
                   std::string::npos);
}

// The last selector that worked is tried before the router default.
static void test_device_refused_pick_goes_back_to_last_working() {
  DeviceCumuloHead h;
  h.push("/router/mistral/v1/conversations", 400, kInvalidModel400);
  h.push("/router/anthropic/v1/messages", 200, antBody());
  const rf::LadderResult r = h.turn("mistral/mistral/zai-glm-5-3", "anthropic/claude-sonnet-5");
  TEST_ASSERT_TRUE_MESSAGE(r.ok, h.err.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-sonnet-5", r.used.c_str());
}

// Refused everywhere: the turn fails with the router's own cause, which the
// engine's failover notice and failure reply carry instead of "unavailable".
static void test_device_refused_everywhere_names_the_cause() {
  DeviceCumuloHead h;
  h.push("/router/mistral/v1/conversations", 400, kInvalidModel400);
  h.push("/router/openai/v1/responses", 403, routerRefusal("model_not_priced"));
  const rf::LadderResult r = h.turn("mistral/mistral/zai-glm-5-3");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.lastKind == rf::RefusalKind::Selector);
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.lastTried.c_str());
  const std::string cause = rf::refusalCause(r.lastKind, r.lastTried);
  TEST_ASSERT_EQUAL_STRING("the Cumulo router does not offer gpt-5.6 right now. Choose another model under Models",
                           cause.c_str());
  TEST_ASSERT_TRUE(rf::isRefusalCause(cause));
  TEST_ASSERT_EQUAL(2, (int)h.d.http.seen.size());   // each model once
}

// Not every 4xx is a refused model: a key refusal (401) or a 400 about something
// else is answered by the engine's own reply, never on another model.
static void test_device_other_refusals_are_not_answered_elsewhere() {
  DeviceCumuloHead h;
  h.push("/router/openai/v1/responses", 401, "{\"error\":{\"message\":\"Incorrect API key\"}}");
  const rf::LadderResult a = h.turn("openai/gpt-6.1-sol");
  TEST_ASSERT_FALSE(a.ok);
  TEST_ASSERT_TRUE(a.kind == rf::RefusalKind::Auth);
  TEST_ASSERT_EQUAL_STRING("", rf::refusalCause(a.lastKind, a.lastTried).c_str());
  DeviceCumuloHead b;
  b.push("/router/openai/v1/responses", 400, "{\"error\":{\"message\":\"Invalid value for 'input'\"}}");
  const rf::LadderResult rb = b.turn("openai/gpt-6.1-sol");
  TEST_ASSERT_FALSE(rb.ok);
  TEST_ASSERT_TRUE(rb.kind == rf::RefusalKind::Other);
  TEST_ASSERT_EQUAL(1, (int)b.d.http.seen.size());
}

// A refusal after a tool already ran is never answered again (the retry would
// repeat the tool): only an attempt's FIRST request is re-run.
static void test_device_refusal_after_a_tool_is_not_rerun() {
  DeviceCumuloHead h;
  h.d.toolLoop = true;
  FakeProviderDeps::ToolRig rig;
  h.d.fillTools(rig);
  h.push("/router/anthropic/v1/messages", 200,
         "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"tea\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}");
  h.push("/router/anthropic/v1/messages", 403, routerRefusal("model_not_priced"));
  const rf::LadderResult r = h.turn("anthropic/claude-opus-5", "", &rig.ht);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL_UINT(1, rig.dispatched.size());
  TEST_ASSERT_EQUAL(2, (int)h.d.http.seen.size());   // no third request on another model
}

// prism: a 404 on a request that continued a Mistral conversation says the
// conversation is gone (the adapter answers "conversation gone" and the engine
// retries on a fresh one); it is not the router refusing the model, so the pick
// is neither answered elsewhere nor moved.
static void test_device_gone_conversation_is_not_a_refused_pick() {
  DeviceCumuloHead h;
  h.cv = "conv_old";
  h.push("/router/mistral/v1/conversations/conv_old", 404, "{\"message\":\"Not found\"}");
  const rf::LadderResult r = h.turn("mistral/mistral-large-latest");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.kind == rf::RefusalKind::Other);
  TEST_ASSERT_EQUAL_STRING("conversation gone", h.err.c_str());
  TEST_ASSERT_EQUAL(1, (int)h.d.http.seen.size());   // no other model was asked
  // A 404 on a new conversation (the model or its upstream unknown) still is one.
  DeviceCumuloHead n;
  n.push("/router/mistral/v1/conversations", 404, "{\"message\":\"Not found\"}");
  n.push("/router/openai/v1/responses", 200, oaiBody());
  const rf::LadderResult rn = n.turn("mistral/mistral-huge");
  TEST_ASSERT_TRUE_MESSAGE(rn.ok, n.err.c_str());
  TEST_ASSERT_TRUE(rn.kind == rf::RefusalKind::Selector);
}

// prism: the pick moves only while it is still the one the turn started with (the
// owner may pick another model while a refused turn is answered elsewhere), and
// only for a refused model; the notice opens the reply either way.
static void test_settle_moves_only_the_turns_own_pick() {
  rf::LadderResult r;
  r.ok = true;
  r.kind = rf::RefusalKind::Selector;
  r.used = "gpt-5.6";
  std::string stored = "mistral/mistral/zai-glm-5-3", out = "{\"reply\":\"hi\"}";
  auto current = [&] { return stored; };
  auto set = [&](const std::string& p) { stored = p; };
  rf::settleFallback(r, "mistral/mistral/zai-glm-5-3", "gpt-5.6", out, current, set);
  TEST_ASSERT_EQUAL_STRING("", stored.c_str());   // the router default is stored as ""
  TEST_ASSERT_TRUE(out.find("does not offer mistral/mistral/zai-glm-5-3") != std::string::npos);
  stored = "anthropic/claude-opus-5";             // the owner picked another meanwhile
  rf::settleFallback(r, "mistral/mistral/zai-glm-5-3", "gpt-5.6", out, current, set);
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5", stored.c_str());
  r.kind = rf::RefusalKind::Upstream;             // a paused upstream keeps the pick
  stored = "zai/glm-5.3";
  std::string out2 = "{\"reply\":\"hi\"}";
  rf::settleFallback(r, "zai/glm-5.3", "gpt-5.6", out2, current, set);
  TEST_ASSERT_EQUAL_STRING("zai/glm-5.3", stored.c_str());
  TEST_ASSERT_TRUE(out2.find("Your model choice is kept.") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_anthropic_routes_through_router);
  RUN_TEST(test_openai_routes_through_router);
  RUN_TEST(test_mistral_routes_through_router);
  RUN_TEST(test_tool_loop_runs_over_router);
  RUN_TEST(test_http_base_uses_plain_http_and_port);
  RUN_TEST(test_no_override_is_direct);
  RUN_TEST(test_device_invalid_model_pick_answers_on_default);
  RUN_TEST(test_device_model_not_priced_is_read_through_the_filter);
  RUN_TEST(test_device_route_refusal_answers_on_default_and_keeps_pick);
  RUN_TEST(test_device_refused_pick_goes_back_to_last_working);
  RUN_TEST(test_device_refused_everywhere_names_the_cause);
  RUN_TEST(test_device_other_refusals_are_not_answered_elsewhere);
  RUN_TEST(test_device_refusal_after_a_tool_is_not_rerun);
  RUN_TEST(test_device_gone_conversation_is_not_a_refused_pick);
  RUN_TEST(test_settle_moves_only_the_turns_own_pick);
  UNITY_END();
  return 0;
}
