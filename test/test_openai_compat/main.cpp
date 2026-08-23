#include <unity.h>

#include <cstring>
#include <string>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_openai_wire_hits_chat_completions);
  RUN_TEST(test_anthropic_wire_hits_messages_not_chat);
  RUN_TEST(test_error_mapping);
  UNITY_END();
  return 0;
}
