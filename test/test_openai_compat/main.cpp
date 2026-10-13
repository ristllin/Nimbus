#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"

using namespace harness_test;
using agent::providers::CompatEndpoint;
using agent::providers::CompatWire;
using agent::providers::openaiCompatDispatch;
using agent::providers::openaiCompatPoll;

void setUp() {}
void tearDown() {}

static agent::Directive makeDirective() {
  agent::Directive d;
  d.category = "ops";
  d.instruction = "write a haiku";
  return d;
}

// OpenAI/Mistral wire: POST <base>/chat/completions, reply at choices[0].message.content.
static void test_openai_wire_hits_chat_completions() {
  FakeProviderDeps rig;
  rig.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"hi-oai\"}}]}"});
  agent::providers::ProviderDeps pd = rig.contract();
  CompatEndpoint ep;
  ep.host = "api.z.ai";
  ep.basePath = "/api/paas/v4";
  ep.key = "tok";
  ep.model = "glm-5";
  ep.backendTag = "zai";
  ep.wire = CompatWire::OpenAIChat;
  char jobId[72] = {};
  agent::FabricErr err = openaiCompatDispatch(pd, ep, makeDirective(), jobId);
  TEST_ASSERT_TRUE(err == agent::FabricErr::Ok);
  TEST_ASSERT_TRUE(rig.http.seen.back().path.find("/api/paas/v4/chat/completions") !=
                   std::string::npos);
  TEST_ASSERT_EQUAL_STRING("api.z.ai", rig.http.seen.back().host.c_str());
  agent::ResultEnvelope envp;
  TEST_ASSERT_TRUE(openaiCompatPoll(pd, "zai", jobId, envp) == agent::FabricErr::Ok);
  TEST_ASSERT_EQUAL_STRING("hi-oai", envp.reply);
}

// The regression guard (CUM-30): a router's ANTHROPIC upstream speaks the Messages
// wire, NOT chat-completions. Sending /chat/completions there 403s on the real
// router (endpoint_not_allowed). This pins the correct path + body + reply parse.
static void test_anthropic_wire_hits_messages_not_chat() {
  FakeProviderDeps rig;
  rig.http.script.push_back({"", "", 200, "{\"content\":[{\"type\":\"text\",\"text\":\"hi-ant\"}]}"});
  agent::providers::ProviderDeps pd = rig.contract();
  CompatEndpoint ep;
  ep.host = "app.cumulo-nimbus.ai";
  ep.basePath = "/router/anthropic/v1";
  ep.key = "routerkey";
  ep.model = "claude-haiku-4-5";
  ep.backendTag = "cumulo";
  ep.wire = CompatWire::AnthropicMessages;
  char jobId[72] = {};
  agent::FabricErr err = openaiCompatDispatch(pd, ep, makeDirective(), jobId);
  TEST_ASSERT_TRUE(err == agent::FabricErr::Ok);
  const std::string& path = rig.http.seen.back().path;
  TEST_ASSERT_TRUE(path.find("/router/anthropic/v1/messages") != std::string::npos);
  TEST_ASSERT_TRUE(path.find("/chat/completions") == std::string::npos);   // never this wire
  // Messages wire: system is top-level and max_tokens is required.
  TEST_ASSERT_TRUE(rig.http.lastBodyContains("\"system\""));
  TEST_ASSERT_TRUE(rig.http.lastBodyContains("\"max_tokens\""));
  agent::ResultEnvelope envp;
  TEST_ASSERT_TRUE(openaiCompatPoll(pd, "cumulo", jobId, envp) == agent::FabricErr::Ok);
  TEST_ASSERT_EQUAL_STRING("hi-ant", envp.reply);   // parsed from content[0].text
}

// Auth + error mapping: a 401 maps to FabricErr::Auth; a bad model to BadRequest.
static void test_error_mapping() {
  {
    FakeProviderDeps rig;
    rig.http.script.push_back({"", "", 401, "{\"error\":{\"message\":\"bad key\"}}"});
    agent::providers::ProviderDeps pd = rig.contract();
    CompatEndpoint ep;
    ep.host = "api.z.ai"; ep.basePath = "/api/paas/v4"; ep.key = "x"; ep.model = "glm-5";
    ep.backendTag = "zai";
    char jobId[72] = {};
    TEST_ASSERT_TRUE(openaiCompatDispatch(pd, ep, makeDirective(), jobId) == agent::FabricErr::Auth);
  }
  {
    FakeProviderDeps rig;
    agent::providers::ProviderDeps pd = rig.contract();
    CompatEndpoint ep;
    ep.host = "api.z.ai"; ep.basePath = "/api/paas/v4"; ep.key = "x"; ep.model = "";  // no model
    ep.backendTag = "zai";
    char jobId[72] = {};
    TEST_ASSERT_TRUE(openaiCompatDispatch(pd, ep, makeDirective(), jobId) ==
                     agent::FabricErr::BadRequest);
  }
}

// CUM-465: a 429 (Z.ai, the Cumulo router) is RateLimited, not a generic remote
// failure, and hands back the window its text names - so the spawn refusal says
// the provider is limiting requests instead of a bare "Couldn't start".
static void test_429_names_the_window() {
  struct Case { const char* body; agent::RateLimit want; };
  const Case cases[] = {
      {"{\"error\":{\"message\":\"Rate limit reached on requests per day (RPD)\"}}",
       agent::RateLimit::Daily},
      {"{\"error\":{\"code\":\"1302\",\"message\":\"High concurrency usage of this API\"}}",
       agent::RateLimit::Unknown},
  };
  for (const Case& k : cases) {
    FakeProviderDeps rig;
    rig.http.script.push_back({"", "/chat/completions", 429, k.body});
    agent::providers::ProviderDeps pd = rig.contract();
    CompatEndpoint ep;
    ep.host = "api.z.ai"; ep.basePath = "/api/paas/v4"; ep.key = "x"; ep.model = "glm-5";
    ep.backendTag = "zai";
    agent::RateLimit window = agent::RateLimit::kCount;   // sentinel: never written
    agent::Directive d = makeDirective();
    d.rateLimit = &window;
    char jobId[72] = {};
    TEST_ASSERT_TRUE(openaiCompatDispatch(pd, ep, d, jobId) == agent::FabricErr::RateLimited);
    TEST_ASSERT_EQUAL_MESSAGE((int)k.want, (int)window, k.body);
  }
}

// CUM-544: a Z.ai sub-agent (direct, or the router's zai upstream) carries the
// same bounded reasoning effort as the Z.ai head - the effort the adapters set from
// reasoningEffortFor(). Other endpoints send no field.
static void test_cum544_zai_sub_carries_low_effort() {
  FakeProviderDeps rig;
  rig.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"});
  rig.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"});
  agent::providers::ProviderDeps pd = rig.contract();
  CompatEndpoint ep;
  ep.host = "api.z.ai";
  ep.basePath = "/api/paas/v4";
  ep.model = "glm-5.3";
  ep.backendTag = "zai";
  ep.reasoningEffort = agent::providers::reasoningEffortFor("zai");
  char jobId[72] = {};
  TEST_ASSERT_TRUE(openaiCompatDispatch(pd, ep, makeDirective(), jobId) == agent::FabricErr::Ok);
  TEST_ASSERT_TRUE(rig.http.seen[0].body.find("\"reasoning_effort\":\"low\"") != std::string::npos);

  CompatEndpoint oai;   // a router openai upstream: no field
  oai.host = "app.cumulo-nimbus.ai";
  oai.basePath = "/router/openai/v1";
  oai.model = "gpt-5.6";
  oai.backendTag = "cumulo";
  oai.reasoningEffort = agent::providers::reasoningEffortFor("openai");
  TEST_ASSERT_TRUE(openaiCompatDispatch(pd, oai, makeDirective(), jobId) == agent::FabricErr::Ok);
  TEST_ASSERT_TRUE(rig.http.seen[1].body.find("reasoning_effort") == std::string::npos);
}

// One compat dispatch whose transport fails after `takesMs` on the rig clock, at
// the endpoint `base` names; `deadline` is the read deadline it must have judged.
static agent::FabricErr dispatchFailingAfter(uint32_t takesMs, CompatWire wire,
                                             const char* backendTag, const char* base,
                                             uint32_t deadline, const char* err = "no response") {
  FakeProviderDeps rig;
  rig.http.script.push_back(harness_test::transportCut(takesMs, err));
  agent::providers::ProviderDeps pd = rig.contract();
  CompatEndpoint ep;
  ep.host = "api.example"; ep.basePath = base; ep.key = "x"; ep.model = "m1";
  ep.backendTag = backendTag;
  ep.wire = wire;
  char jobId[72] = {};
  const agent::FabricErr e = openaiCompatDispatch(pd, ep, makeDirective(), jobId);
  TEST_ASSERT_EQUAL_UINT32(deadline, rig.http.seen.back().timeoutMs);   // the deadline judged
  return e;
}

// CUM-538 gate: a Z.ai or Cumulo sub runs INSIDE its one POST (50 s on Z.ai, under
// its gateway cut; 120 s on every other upstream). Once the request went out and no
// answer came back ("no response": the deadline, or the connection dropped
// mid-wait), or a failure took at least nine tenths of the deadline, the run most
// likely started: Timeout, which tells the head not to spawn the paid work again. A
// request that never fully left ("head write failed") or a fast failure ("connect
// failed") never delivered the work: Network ("did not start"). The fake clock moves
// 10 ms per read, so elapsed = takesMs + 10. Every backend, upstream and wire.
static void test_cum538_a_sub_that_outruns_its_post_is_a_timeout() {
  const agent::FabricErr kT = agent::FabricErr::Timeout, kN = agent::FabricErr::Network;
  struct Ep { const char* tag; const char* base; uint32_t deadline; };
  const Ep eps[] = {{"zai", "/api/paas/v4", 50000},
                    {"cumulo", "/router/zai", 50000},
                    {"cumulo", "/router/openai/v1", 120000},
                    {"cumulo", "/router/anthropic/v1", 120000}};
  for (const Ep& e : eps) {
    const uint32_t D = e.deadline, edge = D / 10 * 9 - 10;   // elapsed == 9/10 D
    for (CompatWire w : {CompatWire::OpenAIChat, CompatWire::AnthropicMessages}) {
      TEST_ASSERT_EQUAL_MESSAGE((int)kT, (int)dispatchFailingAfter(D - 1000, w, e.tag, e.base, D), e.base);
      TEST_ASSERT_EQUAL_MESSAGE((int)kT, (int)dispatchFailingAfter(1000, w, e.tag, e.base, D), e.base);  // sent, then dropped
      TEST_ASSERT_EQUAL_MESSAGE((int)kT, (int)dispatchFailingAfter(edge, w, e.tag, e.base, D, "connect failed"), e.base);
      TEST_ASSERT_EQUAL_MESSAGE((int)kN, (int)dispatchFailingAfter(edge - 10, w, e.tag, e.base, D, "connect failed"), e.base);
      TEST_ASSERT_EQUAL_MESSAGE((int)kN, (int)dispatchFailingAfter(1000, w, e.tag, e.base, D, "connect failed"), e.base);
      TEST_ASSERT_EQUAL_MESSAGE((int)kN, (int)dispatchFailingAfter(D - 1000, w, e.tag, e.base, D, "head write failed"), e.base);
    }
  }
}

// The deadline rule, by endpoint: only a Z.ai endpoint keeps the Z.ai budget; a
// router path that merely starts with "zai" is not one.
static void test_compat_sub_deadline_by_endpoint() {
  CompatEndpoint ep;
  ep.backendTag = "zai"; ep.basePath = "/api/paas/v4";
  TEST_ASSERT_EQUAL_UINT32(agent::providers::kZaiHeadTimeoutMs, agent::providers::compatSubDeadlineMs(ep));
  ep.backendTag = "cumulo";
  for (const char* b : {"/router/zai", "/router/zai/v1"}) {
    ep.basePath = b;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(agent::providers::kZaiHeadTimeoutMs, agent::providers::compatSubDeadlineMs(ep), b);
  }
  for (const char* b : {"/router/openai/v1", "/router/anthropic/v1", "/router/mistral/v1", "/router/zaix"}) {
    ep.basePath = b;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(120000u, agent::providers::compatSubDeadlineMs(ep), b);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_openai_wire_hits_chat_completions);
  RUN_TEST(test_anthropic_wire_hits_messages_not_chat);
  RUN_TEST(test_error_mapping);
  RUN_TEST(test_429_names_the_window);
  RUN_TEST(test_cum544_zai_sub_carries_low_effort);
  RUN_TEST(test_cum538_a_sub_that_outruns_its_post_is_a_timeout);
  RUN_TEST(test_compat_sub_deadline_by_endpoint);
  UNITY_END();
  return 0;
}
