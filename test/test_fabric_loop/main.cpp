#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
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
using harness_test::reqBody;
namespace providers = agent::providers;
namespace orch = nimbus::orch;

// Every plain operator new in this test binary is counted (the array and nothrow
// forms call it; the over-aligned forms are not used on this path), so a test can
// read how many allocations are live at a moment (the device turn path must not
// grow the heap).
static long g_live = 0;
static size_t g_largest = 0;   // the largest single allocation since it was reset
void* operator new(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  ++g_live;
  if (n > g_largest) g_largest = n;
  return p;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  --g_live;
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

void setUp() { harness_test::LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static std::string antToolUseBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"tea\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";
}
static std::string antFinalBody() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_2\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\","
         "\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
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

// Glass Box P3: the loop hands its canonical transcript to HeadTools::onBrief on
// the way out, so the turn dossier can show the tool loop's MIDDLE (the richest
// per-turn artifact used to be destroyed at loop exit - renderBrief() had no
// caller at all). The brief must carry the user seed, the call WITH its args,
// and the result - and must arrive even when the turn FAILED, since a failed
// turn's middle is exactly what needs debugging.
static void test_brief_reaches_onbrief_with_calls_and_results() {
  FakeProviderDeps d;
  // One host, two rounds: a tool call, then the terminal orch_turn.
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antToolUseBody()});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalBody()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string brief;
  int briefCalls = 0;
  rig.ht.onBrief = [&](const std::string& b) { brief = b; briefCalls++; };
  std::string out, err;
  orch::TokenUsage u;
  bool ok = providers::runFabricLoop(pd, {"anthropic"}, "SYS", "USER", out, err,
                                     rig.ht, &u, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(1, briefCalls);              // exactly once per turn
  TEST_ASSERT_TRUE(brief.find("[user] USER") != std::string::npos);
  TEST_ASSERT_TRUE(brief.find("[tool] memory_search") != std::string::npos);
  TEST_ASSERT_TRUE(brief.find("tea") != std::string::npos);       // the ARGS
  TEST_ASSERT_TRUE(brief.find("[result]") != std::string::npos);
  TEST_ASSERT_TRUE(brief.find("tool-ok") != std::string::npos);   // the RESULT
}

static void test_brief_delivered_even_when_the_turn_fails() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antToolUseBody()});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 0, ""});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 0, ""});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  std::string brief;
  rig.ht.onBrief = [&](const std::string& b) { brief = b; };
  std::string out, err;
  orch::TokenUsage u;
  bool ok = providers::runFabricLoop(pd, {"anthropic"}, "SYS", "USER", out, err,
                                     rig.ht, &u, nullptr);
  TEST_ASSERT_FALSE(ok);
  // The work that DID happen before the failure is still recoverable.
  TEST_ASSERT_TRUE(brief.find("[tool] memory_search") != std::string::npos);
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

// Gate G5 inside the fabric: a step that captured a served-model echo on its HTTP 200
// and then failed (empty content) never names the model of the attempt that serves
// the round - not on the same-host retry, not after a switch to another host.
static std::string antEchoEmptyBody() {
  return "{\"model\":\"claude-haiku-4-5\",\"content\":[],\"stop_reason\":\"max_tokens\","
         "\"usage\":{\"input_tokens\":7,\"output_tokens\":3}}";
}

static void test_failed_step_echo_never_names_the_serving_model() {
  {   // same-host retry serves the round, with no echo
    FakeProviderDeps d;
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antEchoEmptyBody()});
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalBody()});
    auto pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string out, err;
    orch::TokenUsage u;
    TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err,
                                              rig.ht, &u, nullptr));
    TEST_ASSERT_EQUAL_STRING("", u.servedModel.c_str());
    TEST_ASSERT_EQUAL(17, (int)u.promptTokens);   // the failed step's tokens still count
  }
  {   // both anthropic attempts echo and fail; openai serves the round, with no echo
    FakeProviderDeps d;
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antEchoEmptyBody()});
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antEchoEmptyBody()});
    d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiFinalBody()});
    auto pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string out, err;
    orch::TokenUsage u;
    TEST_ASSERT_TRUE(providers::runFabricLoop(pd, {"anthropic", "openai"}, "S", "U", out, err,
                                              rig.ht, &u, nullptr));
    TEST_ASSERT_EQUAL_STRING("", u.servedModel.c_str());
    TEST_ASSERT_EQUAL(7 + 7 + 60, (int)u.promptTokens);
  }
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

// ---- CUM-477 gate: the fabric gets the resend wait too ------------------------
// A LOOP turn on a direct openai/anthropic/mistral head runs the fabric (the
// fleet's default config), which retried a short write at once and then switched
// hosts: the bounded wait for the heap and the memory label never reached it.
// Bench numbers: the write fails with free heap at 9940, under the round floor.
struct LowHeapFabric {
  FakeProviderDeps d;
  FakeProviderDeps::ToolRig rig;
  agent::providers::ProviderDeps pd;
  int delays = 0;
  bool recovered = false;
  explicit LowHeapFabric(bool recovers) {
    harness_test::Exchange unsent;
    unsent.expectHost = "api.openai.com";
    unsent.expectPathContains = "/v1/responses";
    unsent.status = 0;
    unsent.err = "head write failed";
    d.http.script.push_back(unsent);
    pd = d.contract();
    // Healthy until the failed write, then 9940 until the heap recovers.
    pd.freeHeap = [this] { return (d.http.idx >= 1 && !recovered) ? 9940u : 100000u; };
    pd.delayMs = [this, recovers](uint32_t ms) {
      ++delays;
      d.now += ms;
      if (recovers) recovered = true;
    };
    d.fillTools(rig);
    rig.ht.cfg.roundMinHeap = 12000;   // ORCH_LOOP_MIN_HEAP on the device
  }
};

static void test_cum477_fabric_resends_once_after_the_heap_recovers() {
  LowHeapFabric f(/*recovers=*/true);
  f.d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiFinalBody()});
  int notifies = 0;
  std::string out, err;
  const bool ok = providers::runFabricLoop(f.pd, {"openai", "anthropic"}, "SYS", "USER", out, err, f.rig.ht,
                                           nullptr, [&](const std::string&, const std::string&) { notifies++; });
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)f.d.http.seen.size());   // the unsent request and its one resend
  TEST_ASSERT_EQUAL_STRING(reqBody(f.d, 0).c_str(), reqBody(f.d, 1).c_str());
  TEST_ASSERT_TRUE(f.delays >= 1);                    // it waited for the heap first
  TEST_ASSERT_EQUAL(0, notifies);                     // no switch
  TEST_ASSERT_TRUE(harness_test::LogCapture::contains(
      "orchTurn(oai-loop): request not sent (heap 9940) - one same-host resend after the heap recovered"));
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", out.c_str());
}

// The heap never comes back: a bounded wait, no resend, and no switch either (the
// same request fails the same way on every host) - the turn ends as low memory,
// which the engine answers with the memory reply, never "network".
static void test_cum477_fabric_low_memory_does_not_switch() {
  LowHeapFabric f(/*recovers=*/false);   // an anthropic request would be unscripted: loud
  int notifies = 0;
  std::string out, err;
  const bool ok = providers::runFabricLoop(f.pd, {"openai", "anthropic"}, "SYS", "USER", out, err, f.rig.ht,
                                           nullptr, [&](const std::string&, const std::string&) { notifies++; });
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_STRING("low memory: the device could not send the request", err.c_str());
  TEST_ASSERT_EQUAL(1, (int)f.d.http.seen.size());
  TEST_ASSERT_EQUAL(0, notifies);
  TEST_ASSERT_TRUE(f.delays >= 1);
  TEST_ASSERT_TRUE(f.delays <= 31);   // bounded: 3 s of 100 ms polls
}

// Review: the heap came back but the one resend failed anyway (it was sent; no
// answer): the request is not sent a third time by the fabric's own same-host
// retry - the round goes to the next provider.
static void test_cum477_fabric_never_sends_an_unsent_request_three_times() {
  LowHeapFabric f(/*recovers=*/true);
  f.d.http.script.push_back({"api.openai.com", "/v1/responses", 0, ""});   // the resend: sent, no answer
  f.d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalBody()});
  int notifies = 0;
  std::string out, err;
  const bool ok = providers::runFabricLoop(f.pd, {"openai", "anthropic"}, "SYS", "USER", out, err, f.rig.ht,
                                           nullptr, [&](const std::string&, const std::string&) { notifies++; });
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(3, (int)f.d.http.seen.size());   // openai, its one resend, then anthropic
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", f.d.http.seen[2].host.c_str());
  TEST_ASSERT_EQUAL(1, notifies);
}

// ---- CUM-477 gate: the fabric keeps no heap residents for its hosts ------------
// runFabricLoop kept two vectors and every switched-away host's step closure (a
// copy of the deps) alive for the whole turn. After two switches the live
// allocations at the answering host's request equal a one-host turn's.
struct LiveAtAnswer : agent::HttpTransport {
  int cuts = 0;            // requests that get no answer before the one that does
  std::string body;        // that answer
  int calls = 0;
  long liveAtAnswer = -1;  // live allocations as the answered request arrives
  bool exec(const agent::HttpRequest&, agent::HttpResponse& out, std::string& err) override {
    if (++calls <= cuts) {
      out.status = 0;
      err = "connect failed";
      return false;
    }
    liveAtAnswer = g_live;
    out.status = 200;
    out.body = body;
    return true;
  }
};
static long liveWhenTheLastHostAnswers(const std::vector<std::string>& hosts) {
  FakeProviderDeps d;
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  LiveAtAnswer tx;
  tx.cuts = 2 * (int)(hosts.size() - 1);   // each earlier host: its request and the retry
  tx.body = antFinalBody();
  agent::providers::ProviderDeps pd = d.contract();
  pd.http = &tx;
  std::string out, err;
  const bool ok = providers::runFabricLoop(pd, hosts, "S", "U", out, err, rig.ht, nullptr, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(tx.cuts + 1, tx.calls);
  return tx.liveAtAnswer;
}
static void test_cum477_switched_hosts_leave_nothing_on_the_heap() {
  agent::hlog::setSink(nullptr);   // a log recorder would keep the switch lines
  const std::vector<std::string> three = {"openai", "mistral", "anthropic"};
  const std::vector<std::string> one = {"anthropic"};
  liveWhenTheLastHostAnswers(three);   // any first-use static is made here, not below
  const long single = liveWhenTheLastHostAnswers(one);
  const long switched = liveWhenTheLastHostAnswers(three);
  TEST_ASSERT_EQUAL_MESSAGE(single, switched, "live allocations when the third host answers");
}

// ... and nothing sized to the host list: a list of 257 hosts (the engine passes at
// most three, the fabric uses the first three) makes no allocation of 257 steps'
// worth, which the old per-turn vectors were.
static void test_cum477_fabric_allocates_nothing_per_listed_host() {
  agent::hlog::setSink(nullptr);
  const std::vector<std::string> many(257, "anthropic");
  FakeProviderDeps d;
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  LiveAtAnswer tx;
  tx.body = antFinalBody();
  agent::providers::ProviderDeps pd = d.contract();
  pd.http = &tx;
  std::string out, err;
  g_largest = 0;
  const bool ok = providers::runFabricLoop(pd, many, "S", "U", out, err, rig.ht, nullptr, nullptr);
  const size_t largest = g_largest;
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_LESS_THAN_size_t(many.size() * sizeof(orch::HeadStepFn), largest);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_midturn_failover_carries_results_no_redispatch);
  RUN_TEST(test_brief_reaches_onbrief_with_calls_and_results);
  RUN_TEST(test_brief_delivered_even_when_the_turn_fails);
  RUN_TEST(test_all_hosts_down_fails_soft_with_error);
  RUN_TEST(test_single_host_list_never_switches);
  RUN_TEST(test_failed_step_echo_never_names_the_serving_model);
  RUN_TEST(test_cum477_fabric_resends_once_after_the_heap_recovers);
  RUN_TEST(test_cum477_fabric_low_memory_does_not_switch);
  RUN_TEST(test_cum477_fabric_never_sends_an_unsent_request_three_times);
  RUN_TEST(test_cum477_switched_hosts_leave_nothing_on_the_heap);
  RUN_TEST(test_cum477_fabric_allocates_nothing_per_listed_host);
  return UNITY_END();
}
