#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"

// Stage H wire suite - the custom/proxy provider: base-URL parsing (http/https,
// port), the keyless-on-http rule (R_CUST_keyless), the per-convention
// sub-session wire (openai chat-completions vs anthropic messages), and the NEW
// head-custom single-shot structured turn (response_format json_schema with a
// schema-less 400 fallback) that makes a fully keyless LAN orchestrator
// possible (tests/hil/test_l12).

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
using harness_test::headerOf;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static std::string chatBody(const char* content) {
  return std::string("{\"choices\":[{\"message\":{\"content\":\"") + content +
         "\"}}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";
}

// ---- sub-session ------------------------------------------------------------

// R_CUST_keyless: an http:// base NEVER sends the key - even when one is set -
// and parses host:port from the base URL.
static void test_keyless_on_http() {
  FakeProviderDeps d;
  d.custBase = "http://192.168.1.5:11434/v1";
  d.custKey = "sk-secret-never-on-http";
  d.http.script.push_back({"192.168.1.5", "/v1/chat/completions", 200, chatBody("hi")});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "say hi"; dir.category = "ops";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::customDispatch(pd, dir, jobId));
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_FALSE(r.tls);
  TEST_ASSERT_EQUAL(11434, (int)r.port);
  TEST_ASSERT_EQUAL_STRING("192.168.1.5", r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "x-api-key").c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("NOT sending key in cleartext"));
  TEST_ASSERT_EQUAL_STRING("custom:", std::string(jobId).substr(0, 7).c_str());
}

static void test_https_sends_bearer() {
  FakeProviderDeps d;
  d.custBase = "https://proxy.example.com";
  d.custKey = "sk-proxy";
  d.http.script.push_back({"proxy.example.com", "/v1/chat/completions", 200, chatBody("ok")});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "x";
  char jobId[72];
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::customDispatch(pd, dir, jobId));
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_TRUE(r.tls);
  TEST_ASSERT_EQUAL(443, (int)r.port);
  TEST_ASSERT_EQUAL_STRING("Bearer sk-proxy", headerOf(r, "Authorization").c_str());
}

static void test_anthropic_convention_wire() {
  FakeProviderDeps d;
  d.custBase = "https://claude-proxy.local";
  d.custKey = "sk-ant-proxy";
  d.custConv = "anthropic";
  d.http.script.push_back({"claude-proxy.local", "/v1/messages", 200,
      "{\"content\":[{\"type\":\"text\",\"text\":\"the answer\"}]}"});
  auto pd = d.contract();
  agent::Directive dir; dir.instruction = "solve it";
  char jobId[72];
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::customDispatch(pd, dir, jobId));
  const agent::HttpRequest& r = d.http.seen[0];
  TEST_ASSERT_EQUAL_STRING("sk-ant-proxy", headerOf(r, "x-api-key").c_str());
  TEST_ASSERT_EQUAL_STRING("2023-06-01", headerOf(r, "anthropic-version").c_str());
  TEST_ASSERT_EQUAL_STRING("", headerOf(r, "Authorization").c_str());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"max_tokens\":1024"));
  // poll serves the cached anthropic-parsed reply as Done, once.
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::customPoll(pd, jobId, env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  TEST_ASSERT_EQUAL_STRING("the answer", env.reply);
  TEST_ASSERT_EQUAL((int)agent::FabricErr::NotFound, (int)providers::customPoll(pd, jobId, env));
}

static void test_sub_error_mapping() {
  agent::Directive dir; dir.instruction = "x";
  char jobId[72];
  {  // no base configured
    FakeProviderDeps d;
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
                      (int)providers::customDispatch(pd, dir, jobId));
    TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  }
  {  // remote 401 still maps to Auth (a required-but-missing key fails honestly)
    FakeProviderDeps d; d.custBase = "https://p.example";
    d.http.script.push_back({"", "", 401, "{}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Auth,
                      (int)providers::customDispatch(pd, dir, jobId));
  }
  {  // 200 with empty content = ParseFail (never a raw-JSON leak)
    FakeProviderDeps d; d.custBase = "https://p.example";
    d.http.script.push_back({"", "", 200, "{\"choices\":[]}"});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::ParseFail,
                      (int)providers::customDispatch(pd, dir, jobId));
  }
  {  // transport fail
    FakeProviderDeps d; d.custBase = "https://p.example";
    d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Network,
                      (int)providers::customDispatch(pd, dir, jobId));
  }
}

// ---- head-custom (NEW) ------------------------------------------------------

static void test_head_single_shot_shape_and_parse() {
  FakeProviderDeps d;
  d.custBase = "http://10.0.0.2:11434";
  d.http.script.push_back({"10.0.0.2", "/v1/chat/completions", 200,
      chatBody("{\\\"reply\\\":\\\"hello there\\\",\\\"memory\\\":\\\"\\\"}")});
  auto pd = d.contract();
  std::string conv, out, err;
  orch::TokenUsage u;
  bool ok = providers::orchTurnCustom(pd, conv, "SYS", "USER", out, err, nullptr, &u);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  // chat-completions dialect: system carries the instructions verbatim, the
  // user message the inputs; the model comes from customModel.
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"model\":\"mock-model\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"system\",\"content\":\"SYS\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"role\":\"user\",\"content\":\"USER\""));
  // Structured output via response_format json_schema (strict) carrying the
  // canonical orch schema (descriptions intact - enforcement is server-side
  // when the backend supports it).
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"response_format\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"type\":\"json_schema\""));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "\"strict\":true"));
  TEST_ASSERT_TRUE(bodyHas(d, 0, "Text to send the owner now"));
  // v1: NO tools on the head-custom wire.
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tools\""));
  // Keyless http:// - no auth header.
  TEST_ASSERT_EQUAL_STRING("", headerOf(d.http.seen[0], "Authorization").c_str());
  // Stateless marker + parsed turn + usage.
  TEST_ASSERT_EQUAL_STRING("chat", conv.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"hello there\",\"memory\":\"\"}", out.c_str());
  TEST_ASSERT_EQUAL(42, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(7, (int)u.completionTokens);
}

// CUM-242: the Cumulo/Z.ai heads reuse orchTurnCustom over a fixed base + a
// customPathPrefix that REPLACES the default "/v1". Lock the exact wire path for
// both: Cumulo keeps a /v1 under its router sub-path, Z.ai has none. The scripted
// exchange fails the turn (transport error) if the path does not match, so ok=true
// IS the path assertion.
static void test_head_cumulo_path_prefix() {
  FakeProviderDeps d;
  d.custBase = "https://app.cumulo-nimbus.ai";
  d.custPathPrefix = "/router/openai/v1";
  d.custKey = "cumulo_sk_x";
  harness_test::Exchange e; e.expectHost = "app.cumulo-nimbus.ai";
  e.expectPathContains = "/router/openai/v1/chat/completions";
  e.status = 200; e.body = chatBody("{\\\"reply\\\":\\\"ok\\\",\\\"memory\\\":\\\"\\\"}");
  d.http.script.push_back(e);
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr), err.c_str());
}

static void test_head_zai_path_prefix_drops_v1() {
  FakeProviderDeps d;
  d.custBase = "https://api.z.ai";
  d.custPathPrefix = "/api/paas/v4";   // no /v1 - the prefix replaces it
  d.custKey = "zai_x";
  harness_test::Exchange e; e.expectHost = "api.z.ai";
  e.expectPathContains = "/api/paas/v4/chat/completions";   // NOT /api/paas/v4/v1/...
  e.status = 200; e.body = chatBody("{\\\"reply\\\":\\\"ok\\\",\\\"memory\\\":\\\"\\\"}");
  d.http.script.push_back(e);
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr), err.c_str());
}

// Tools are IGNORED on head-custom v1 - a HeadTools bundle must not change the
// wire (no tool advertisement, single request).
static void test_head_ignores_tools_v1() {
  FakeProviderDeps d;
  d.custBase = "http://10.0.0.2:11434";
  d.http.script.push_back({"", "", 200, chatBody("{\\\"reply\\\":\\\"ok\\\"}")});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string conv, out, err;
  bool ok = providers::orchTurnCustom(pd, conv, "S", "U", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  TEST_ASSERT_FALSE(bodyHas(d, 0, "\"tools\""));
  TEST_ASSERT_EQUAL(0, (int)rig.dispatched.size());
}

// A schema-incapable backend 400s the response_format - retry ONCE schema-less
// with the JSON-only nudge appended to the system message.
static void test_head_schema_400_fallback() {
  FakeProviderDeps d;
  d.custBase = "http://10.0.0.2:11434";
  d.http.script.push_back({"", "", 400, "{\"error\":{\"message\":\"response_format unsupported\"}}"});
  d.http.script.push_back({"", "", 200, chatBody("{\\\"reply\\\":\\\"ok\\\"}")});
  auto pd = d.contract();
  std::string conv, out, err;
  bool ok = providers::orchTurnCustom(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(bodyHas(d, 0, "response_format"));
  TEST_ASSERT_FALSE(bodyHas(d, 1, "response_format"));
  TEST_ASSERT_TRUE(bodyHas(d, 1, "Respond with ONLY the JSON object"));
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"ok\"}", out.c_str());
}

static void test_head_guards_and_errors() {
  {  // no base
    FakeProviderDeps d;
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no custom endpoint", err.c_str());
  }
  {  // anthropic convention refused BEFORE any wire traffic (fails over cleanly)
    FakeProviderDeps d;
    d.custBase = "https://claude-proxy.local";
    d.custConv = "anthropic";
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("head-custom supports openai/mistral chat-completions only",
                             err.c_str());
    TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());
  }
  {  // a persistent 400 (both attempts) surfaces the provider message
    FakeProviderDeps d;
    d.custBase = "http://10.0.0.2:11434";
    d.http.script.push_back({"", "", 400, "{\"error\":{\"message\":\"nope\"}}"});
    d.http.script.push_back({"", "", 400, "{\"error\":{\"message\":\"still nope\"}}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("chat HTTP 400: still nope", err.c_str());
  }
  {  // 500
    FakeProviderDeps d;
    d.custBase = "http://10.0.0.2:11434";
    d.http.script.push_back({"", "", 500, "{}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("chat HTTP 500", err.c_str());
  }
  {  // transport fail
    FakeProviderDeps d;
    d.custBase = "http://10.0.0.2:11434";
    d.http.script.push_back({"", "", 0, ""});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("network", err.c_str());
  }
  {  // 200 with no content
    FakeProviderDeps d;
    d.custBase = "http://10.0.0.2:11434";
    d.http.script.push_back({"", "", 200, "{\"choices\":[]}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
    TEST_ASSERT_EQUAL_STRING("no message content", err.c_str());
  }
}

// The Mistral-style error envelope ({"message": ...}) is surfaced too.
static void test_head_mistral_error_envelope() {
  FakeProviderDeps d;
  d.custBase = "https://mistral-proxy.local";
  d.custConv = "mistral";
  d.http.script.push_back({"", "", 422, "{\"message\":\"bad thing\"}"});
  d.http.script.push_back({"", "", 422, "{\"message\":\"bad thing\"}"});
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnCustom(pd, conv, "S", "U", out, err, nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("chat HTTP 422: bad thing", err.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_keyless_on_http);
  RUN_TEST(test_https_sends_bearer);
  RUN_TEST(test_anthropic_convention_wire);
  RUN_TEST(test_sub_error_mapping);
  RUN_TEST(test_head_single_shot_shape_and_parse);
  RUN_TEST(test_head_cumulo_path_prefix);
  RUN_TEST(test_head_zai_path_prefix_drops_v1);
  RUN_TEST(test_head_ignores_tools_v1);
  RUN_TEST(test_head_schema_400_fallback);
  RUN_TEST(test_head_guards_and_errors);
  RUN_TEST(test_head_mistral_error_envelope);
  UNITY_END();
  return 0;
}
