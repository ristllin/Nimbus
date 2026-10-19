#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/fault_transport.h"
#include "nimbus/harness/providers.h"

// FaultInjectingTransport - the test decorator the live lab (tools/harness-lab
// --fault=...) wraps around its real transport so retry, failover and error
// paths run against otherwise real traffic. Pinned here: the spec grammar,
// per-host counting, pass-through of every other request, the status-0 texts
// wire.h classifies, and one end-to-end fabric failover driven only by the
// decorator.

using agent::FaultInjectingTransport;
using agent::FaultPlan;
using agent::FaultRule;
using agent::parseFaultRule;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
using harness_test::FakeProviderDeps;

void setUp() { harness_test::LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static agent::HttpRequest req(const char* host, const char* path = "/v1/x") {
  agent::HttpRequest r;
  r.method = "POST";
  r.host = host;
  r.path = path;
  return r;
}

static FaultRule mustParse(const char* spec) {
  FaultRule r;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(parseFaultRule(spec, r, why), (std::string(spec) + ": " + why).c_str());
  return r;
}

static void test_parse_single_range_and_alias() {
  FaultRule r = mustParse("anthropic:2:529:overloaded");
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", r.host.c_str());
  TEST_ASSERT_EQUAL_UINT32(2, r.from);
  TEST_ASSERT_EQUAL_UINT32(2, r.to);
  TEST_ASSERT_EQUAL(529, r.status);
  TEST_ASSERT_TRUE(r.body.find("overloaded_error") != std::string::npos);
}

static void test_parse_ranges() {
  FaultRule a = mustParse("api.mistral.ai:1-3:0:noresp");
  TEST_ASSERT_EQUAL_UINT32(1, a.from);
  TEST_ASSERT_EQUAL_UINT32(3, a.to);
  TEST_ASSERT_EQUAL(0, a.status);
  TEST_ASSERT_TRUE(a.err.find("no response") != std::string::npos);
  FaultRule b = mustParse("openai:4-:500");
  TEST_ASSERT_EQUAL_UINT32(4, b.from);
  TEST_ASSERT_EQUAL_UINT32(0, b.to);   // open-ended
  FaultRule c = mustParse("*:*:0");
  TEST_ASSERT_EQUAL_STRING("*", c.host.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, c.from);
  TEST_ASSERT_EQUAL_UINT32(0, c.to);
  TEST_ASSERT_TRUE(c.err.find("connect failed") != std::string::npos);   // default detail
}

static void test_parse_raw_body_keeps_colons() {
  FaultRule r = mustParse(R"(zai:1:400:body={"error":{"message":"a:b"}})");
  TEST_ASSERT_EQUAL_STRING("api.z.ai", r.host.c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"error":{"message":"a:b"}})", r.body.c_str());
}

static void test_parse_rejects_malformed() {
  const char* bad[] = {
      "",                       // nothing
      "anthropic",              // no range/status
      "anthropic:1",            // no status
      ":1:500",                 // empty host
      "anthropic:0:500",        // requests are 1-based
      "anthropic:3-2:500",      // inverted range
      "anthropic:x:500",        // non-numeric range
      "anthropic:1:99",         // below the HTTP range
      "anthropic:1:600",        // above the HTTP range
      "anthropic:1:-1",         // signed
      "anthropic:1:0:credit",   // an HTTP body detail on a transport fault
      "anthropic:1:500:noresp", // a transport detail on an HTTP fault
      "anthropic:1:500:nope",   // unknown detail
  };
  for (const char* s : bad) {
    FaultRule r;
    std::string why;
    TEST_ASSERT_FALSE_MESSAGE(parseFaultRule(s, r, why), s);
    TEST_ASSERT_FALSE_MESSAGE(why.empty(), s);   // a refusal always says why
  }
}

// The class, not the instance: EVERY named detail parses on the status it belongs
// to and lands its text where the decorator returns it.
static void test_every_preset_parses_and_applies() {
  for (const agent::FaultPreset& p : agent::faultPresets()) {
    const std::string spec = std::string("anthropic:1:") + (p.transport ? "0:" : "503:") + p.name;
    FaultRule r = mustParse(spec.c_str());
    if (p.transport) TEST_ASSERT_EQUAL_STRING(p.text, r.err.c_str());
    else TEST_ASSERT_EQUAL_STRING(p.text, r.body.c_str());
  }
}

// The status-0 texts carry the exact substrings wire.h reads to classify a
// transport failure (never sent / sent but unanswered / low memory / timeout).
static void test_transport_texts_match_wire_classifiers() {
  TEST_ASSERT_TRUE(agent::findFaultPreset("unsent")->text == std::string("request body truncated (injected fault)"));
  TEST_ASSERT_NOT_NULL(strstr(agent::findFaultPreset("unsent")->text, "body truncated"));
  TEST_ASSERT_NOT_NULL(strstr(agent::findFaultPreset("noresp")->text, "no response"));
  TEST_ASSERT_NOT_NULL(strstr(agent::findFaultPreset("lowmem")->text, "low memory"));
  TEST_ASSERT_EQUAL(0, strncmp(agent::findFaultPreset("timeout")->text, "timeout:", 8));
}

static void test_requests_n_to_m_fail_others_pass_through() {
  FakeHttpTransport inner;
  for (int i = 0; i < 4; i++) inner.script.push_back(Exchange{"", "", 200, "{\"ok\":true}"});
  FaultPlan plan;
  plan.add(mustParse("api.anthropic.com:2-3:503"));
  FaultInjectingTransport t(&inner, &plan);
  int statuses[5];
  for (int i = 0; i < 5; i++) {
    agent::HttpResponse out;
    std::string err;
    t.exec(req("api.anthropic.com"), out, err);
    statuses[i] = out.status;
  }
  TEST_ASSERT_EQUAL(200, statuses[0]);
  TEST_ASSERT_EQUAL(503, statuses[1]);
  TEST_ASSERT_EQUAL(503, statuses[2]);
  TEST_ASSERT_EQUAL(200, statuses[3]);
  TEST_ASSERT_EQUAL(200, statuses[4]);
  TEST_ASSERT_EQUAL(3, (int)inner.seen.size());   // the injected two never reached the network
  TEST_ASSERT_EQUAL_UINT32(2, plan.injected());
  TEST_ASSERT_EQUAL_UINT32(5, plan.requests("api.anthropic.com"));
}

// Another host's traffic neither fails nor advances the chosen host's counter.
static void test_counter_is_per_host() {
  FakeHttpTransport inner;
  for (int i = 0; i < 3; i++) inner.script.push_back(Exchange{"", "", 200, "{}"});
  FaultPlan plan;
  plan.add(mustParse("anthropic:2:0:noresp"));
  FaultInjectingTransport t(&inner, &plan);
  agent::HttpResponse out;
  std::string err;
  TEST_ASSERT_TRUE(t.exec(req("api.anthropic.com"), out, err));   // anthropic #1
  TEST_ASSERT_TRUE(t.exec(req("api.mistral.ai"), out, err));      // mistral #1
  TEST_ASSERT_TRUE(t.exec(req("api.mistral.ai"), out, err));      // mistral #2 (not anthropic #2)
  TEST_ASSERT_FALSE(t.exec(req("api.anthropic.com"), out, err));  // anthropic #2 fails
  TEST_ASSERT_EQUAL(0, out.status);
  TEST_ASSERT_TRUE(err.find("no response") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(2, plan.requests("api.mistral.ai"));
}

static void test_execjson_parses_injected_body_through_filter() {
  FakeHttpTransport inner;
  FaultPlan plan;
  plan.add(mustParse("anthropic:1:400:credit"));
  FaultInjectingTransport t(&inner, &plan);
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  std::string err;
  const int st = t.execJson(req("api.anthropic.com", "/v1/messages"), doc, filter, err);
  TEST_ASSERT_EQUAL(400, st);
  TEST_ASSERT_TRUE(err.empty());
  const char* msg = doc["error"]["message"] | "";
  TEST_ASSERT_NOT_NULL(strstr(msg, "credit balance is too low"));
  TEST_ASSERT_TRUE(doc["error"]["type"].isNull());   // the filter dropped it, as for a real body
  TEST_ASSERT_EQUAL(0, (int)inner.seen.size());
}

static void test_execjson_transport_fault_returns_zero() {
  FakeHttpTransport inner;
  FaultPlan plan;
  plan.add(mustParse("anthropic:1:0:unsent"));
  FaultInjectingTransport t(&inner, &plan);
  JsonDocument filter, doc;
  filter["x"] = true;
  doc["stale"] = 1;
  std::string err;
  TEST_ASSERT_EQUAL(0, t.execJson(req("api.anthropic.com"), doc, filter, err));
  TEST_ASSERT_TRUE(err.find("body truncated") != std::string::npos);
  TEST_ASSERT_TRUE(doc.isNull() || doc.size() == 0);   // never a stale document
}

static void test_clear_resets_rules_and_counters() {
  FaultPlan plan;
  plan.add(mustParse("*:*:0"));
  TEST_ASSERT_NOT_NULL(plan.onRequest("h"));
  plan.clear();
  TEST_ASSERT_TRUE(plan.empty());
  TEST_ASSERT_NULL(plan.onRequest("h"));
  TEST_ASSERT_EQUAL_UINT32(1, plan.requests("h"));   // counting restarts from zero
  TEST_ASSERT_EQUAL_UINT32(0, plan.injected());
}

// End to end: the decorator alone makes the fabric loop fail over mid-turn.
// Anthropic serves round 0 for real (a tool runs), the decorator kills its next
// two requests (round 1 and the same-host retry), and OpenAI finishes the turn.
static void test_decorator_drives_fabric_failover() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200,
                           "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":"
                           "\"memory_search\",\"input\":{\"q\":\"tea\"}}],\"stop_reason\":"
                           "\"tool_use\",\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}"});
  d.http.script.push_back({"api.openai.com", "/v1/responses", 200,
                           "{\"id\":\"resp_f\",\"output\":[{\"type\":\"function_call\",\"call_id\":"
                           "\"call_f\",\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":"
                           "\\\"done\\\"}\"}],\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}"});
  auto pd = d.contract();
  FaultPlan plan;
  plan.add(mustParse("anthropic:2-3:0:noresp"));
  FaultInjectingTransport ft(&d.http, &plan);
  pd.http = &ft;
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string out, err, from, to;
  int switches = 0;
  nimbus::orch::TurnSpend spend;
  const bool ok = agent::providers::runFabricLoop(
      pd, {"anthropic", "openai"}, "SYS", "USER", out, err, rig.ht, &spend,
      [&](const std::string& f, const std::string& t) { switches++; from = f; to = t; });
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL_UINT32(2, plan.injected());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());   // only the two real exchanges left the box
  TEST_ASSERT_EQUAL(1, switches);
  TEST_ASSERT_EQUAL_STRING("anthropic", from.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", to.c_str());
  TEST_ASSERT_EQUAL(1, (int)rig.dispatched.size());   // the tool ran once
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_single_range_and_alias);
  RUN_TEST(test_parse_ranges);
  RUN_TEST(test_parse_raw_body_keeps_colons);
  RUN_TEST(test_parse_rejects_malformed);
  RUN_TEST(test_every_preset_parses_and_applies);
  RUN_TEST(test_transport_texts_match_wire_classifiers);
  RUN_TEST(test_requests_n_to_m_fail_others_pass_through);
  RUN_TEST(test_counter_is_per_host);
  RUN_TEST(test_execjson_parses_injected_body_through_filter);
  RUN_TEST(test_execjson_transport_fault_returns_zero);
  RUN_TEST(test_clear_resets_rules_and_counters);
  RUN_TEST(test_decorator_drives_fabric_failover);
  return UNITY_END();
}
