#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_http.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/websearch.h"

// test_subsession_websearch - the answer to "did you test websearch with Tavily
// for subsessions?" pinned at the harness wire seam.
//
// The device has TWO unrelated web-search paths, and the owner's question rides
// the gap between them:
//
//   HEAD (this device's turn):   the `web.search` registry tool -> Tavily
//       (agent::websearch::search -> POST api.tavily.com/search). This is the
//       ONLY thing the `tavilyKey` NVS value powers. It runs on the head's turn,
//       never on a sub-agent.
//
//   SUBSESSION (a spawned background sub-agent / deep fan-out wave, a
//       ManagedAgentAdapter dispatch): the sub-agent runs REMOTELY on the
//       provider and gets the PROVIDER's OWN hosted web search - OpenAI's
//       `web_search` tool, Anthropic's built-in agent toolset, Mistral's Studio
//       `web_search` connector. A sub-agent never calls the device tool registry,
//       so it never touches Tavily.
//
// So "Tavily for subsessions" is not a code path: subsession web search is
// provider-hosted, not Tavily. These tests encode that as a class - each
// subsession-capable backend's web posture is pinned so a regression (or a NEW
// backend that silently ships no web access, or wires Tavily into a sub-agent)
// fails here, and the load-bearing check proves NO subsession dispatch ever
// reaches api.tavily.com.

using harness_test::FakeHttpTransport;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static agent::Directive researchDirective() {
  agent::Directive d;
  d.category = "research";
  d.instruction = "find the current thing";
  d.tag = "job0001";
  d.chatId = "1001";
  return d;
}

// True iff any recorded request in this rig was sent to Tavily.
static bool anyRequestHitTavily(const FakeProviderDeps& d) {
  for (const auto& r : d.http.seen)
    if (r.host == "api.tavily.com") return true;
  return false;
}

// ---- HEAD side: Tavily is the head's web.search tool, and it targets Tavily ---

// The contrast anchor: the head path (agent::websearch::search, which backs the
// `web.search` registry tool) DOES go to Tavily. This is what the owner
// configured a Tavily key for. It is a HEAD capability - the sub-agent tests
// below prove the sub-agent path is a different host entirely.
static void test_head_web_search_targets_tavily() {
  FakeHttpTransport http;
  http.script.push_back(harness_test::Exchange{
      "api.tavily.com", "/search", 200,
      R"({"answer":"a.","results":[{"title":"T","url":"https://u","content":"C"}]})", ""});
  auto r = agent::websearch::search(http, "tvly-fake-key", "today's headline", 5);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.err.c_str());
  TEST_ASSERT_EQUAL_size_t(1, http.seen.size());
  TEST_ASSERT_EQUAL_STRING("api.tavily.com", http.seen[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("POST", http.seen[0].method.c_str());
}

// ---- SUBSESSION side: provider-hosted web, never Tavily ----------------------

// OpenAI sub-agent: always-on hosted web_search (openai.cpp: doc["tools"] +=
// {type: web_search}), independent of any owner connector. Not Tavily.
static void test_openai_subsession_gets_hosted_web_search_not_tavily() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200, "{\"id\":\"resp_1\"}"});
  auto pd = d.contract();
  agent::Directive dir = researchDirective();
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai",
                                                "gpt-5.5", "openai", dir, jobId));
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(d, 0, "\"type\":\"web_search\""),
                           "an OpenAI sub-agent must carry the hosted web_search tool");
  TEST_ASSERT_EQUAL_STRING("api.openai.com", d.http.seen[0].host.c_str());
  TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d),
                            "a sub-agent must never reach Tavily");
}

// Anthropic sub-agent: the built-in agent toolset (server-side web search/fetch)
// is pinned on the agent-creation call. Not Tavily.
static void test_anthropic_subsession_gets_builtin_web_toolset_not_tavily() {
  FakeProviderDeps d;   // cold caches -> env, agent, session, turn
  d.http.script.push_back({"api.anthropic.com", "/v1/environments", 200, "{\"id\":\"env_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/agents", 200, "{\"id\":\"ag_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/sessions", 200, "{\"id\":\"sess_1\"}"});
  d.http.script.push_back({"api.anthropic.com", "/v1/sessions/sess_1/events", 200, "{\"data\":[]}"});
  auto pd = d.contract();
  agent::Directive dir = researchDirective();
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId));
  // req[1] is POST /v1/agents - the toolset lives on the agent.
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(d, 1, "agent_toolset_20260401"),
                           "an Anthropic sub-agent must pin the built-in web toolset");
  TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d),
                            "a sub-agent must never reach Tavily");
}

// Mistral sub-agent: web search rides the owner's Studio connectors through the
// attachMistral hook (server-side inside the Conversations POST), so it is
// CONNECTOR-GATED, not always-on. The fake's attachMistral stands in for an
// owner who enabled the web_search connector; the class point is that the web
// tool rides that hook, and the host is Mistral, not Tavily.
static void test_mistral_subsession_web_is_connector_gated_not_tavily() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
      "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\","
      "\"content\":\"ok\"}]}"});
  auto pd = d.contract();
  agent::Directive dir = researchDirective();
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "sub-mistral", dir, jobId));
  TEST_ASSERT_EQUAL_MESSAGE(1, d.mistralAttaches,
                            "owner Studio connectors (web_search) must ride the sub-agent");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(d, 0, "web_search"),
                           "the enabled web_search connector rides the sub-agent body");
  TEST_ASSERT_EQUAL_STRING("api.mistral.ai", d.http.seen[0].host.c_str());
  TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d),
                            "a sub-agent must never reach Tavily");
}

// Custom / OpenAI-compatible proxy sub-agent: a generic endpoint gets NO hosted
// web tool (custom.cpp adds none). This is the honest gap the owner should know:
// a sub-agent on a bare custom provider has no web access at all - and still not
// Tavily. If a future change starts injecting a web tool here, this pins it so
// the decision is deliberate.
static void test_custom_subsession_has_no_hosted_web_and_no_tavily() {
  FakeProviderDeps d;
  d.custBase = "https://proxy.example.com";
  d.custKey = "sk-proxy";
  d.custConv = "openai";
  d.http.script.push_back({"proxy.example.com", "/v1/chat/completions", 200,
      "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],"
      "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}"});
  auto pd = d.contract();
  agent::Directive dir = researchDirective();
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::customDispatch(pd, dir, jobId));
  TEST_ASSERT_FALSE_MESSAGE(bodyHas(d, 0, "web_search"),
                            "a bare custom proxy sub-agent ships no hosted web tool");
  TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d),
                            "a sub-agent must never reach Tavily");
}

// ---- the load-bearing class check -------------------------------------------
// Every subsession-capable backend, dispatched once: NONE may contact Tavily.
// A new backend added to this sweep that routes a sub-agent through Tavily (or
// any test above that regresses) fails here. This is the single assertion that
// answers the owner directly: Tavily is never a sub-agent's web path.
static void test_no_subsession_dispatch_ever_contacts_tavily() {
  agent::Directive dir = researchDirective();
  char jobId[72] = {};

  {  // OpenAI
    FakeProviderDeps d;
    d.http.script.push_back({"api.openai.com", "/v1/responses", 200, "{\"id\":\"r\"}"});
    auto pd = d.contract();
    providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-5.5", "openai", dir, jobId);
    TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d), "openai sub-agent hit Tavily");
  }
  {  // Anthropic
    FakeProviderDeps d;
    d.http.script.push_back({"api.anthropic.com", "/v1/environments", 200, "{\"id\":\"e\"}"});
    d.http.script.push_back({"api.anthropic.com", "/v1/agents", 200, "{\"id\":\"a\"}"});
    d.http.script.push_back({"api.anthropic.com", "/v1/sessions", 200, "{\"id\":\"s\"}"});
    d.http.script.push_back({"api.anthropic.com", "/v1/sessions/s/events", 200, "{\"data\":[]}"});
    auto pd = d.contract();
    providers::antDispatch(pd, "claude-sonnet-4-6", dir, jobId);
    TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d), "anthropic sub-agent hit Tavily");
  }
  {  // Mistral
    FakeProviderDeps d;
    d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
        "{\"conversation_id\":\"c\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"ok\"}]}"});
    auto pd = d.contract();
    providers::mistralDispatch(pd, "sub-mistral", dir, jobId);
    TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d), "mistral sub-agent hit Tavily");
  }
  {  // Custom proxy
    FakeProviderDeps d;
    d.custBase = "https://proxy.example.com";
    d.custKey = "sk-proxy";
    d.http.script.push_back({"proxy.example.com", "/v1/chat/completions", 200,
        "{\"choices\":[{\"message\":{\"content\":\"ok\"}}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}"});
    auto pd = d.contract();
    providers::customDispatch(pd, dir, jobId);
    TEST_ASSERT_FALSE_MESSAGE(anyRequestHitTavily(d), "custom sub-agent hit Tavily");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_head_web_search_targets_tavily);
  RUN_TEST(test_openai_subsession_gets_hosted_web_search_not_tavily);
  RUN_TEST(test_anthropic_subsession_gets_builtin_web_toolset_not_tavily);
  RUN_TEST(test_mistral_subsession_web_is_connector_gated_not_tavily);
  RUN_TEST(test_custom_subsession_has_no_hosted_web_and_no_tavily);
  RUN_TEST(test_no_subsession_dispatch_ever_contacts_tavily);
  return UNITY_END();
}
