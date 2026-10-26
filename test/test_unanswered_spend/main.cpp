#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/seam_meter.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/spend_guard.h"
#include "../../lib/harness/src/providers/wire.h"   // bodyModel, estimateRoute - the rule under test

// Lane QW4 (CUM-776, blocker class): a paid request that left the device and got no
// usable answer was never counted. The provider usually ran it (a non-streamed request
// is billed when it completes, whether or not the device still listens), so the monthly
// spend ceiling read low. Now every such request counts a conservative estimate: one
// input token per two characters sent (each photo at the image estimate), no output, on
// the provider and model it named. Covered here on every path a paid request takes: a
// head round, a sub-agent dispatch (async and in-request), and the caption step; and the
// counter-tests: a refused request (4xx, 5xx) and a request that never left count nothing.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
namespace providers = agent::providers;
namespace orch = nimbus::orch;
namespace wire = agent::providers::wire;
namespace vision = agent::vision;

namespace {

struct Metered {
  orch::SpendSeam seam;
  std::string provider, model;
  uint32_t in = 0, out = 0;
};
std::vector<Metered> g_meter;

void sink(orch::SpendSeam seam, const char* provider, const orch::TurnSpend& s, uint64_t) {
  Metered m;
  m.seam = seam;
  m.provider = provider ? provider : "";
  if (s.n) {
    m.model = s.slot[0].model;
    m.in = s.slot[0].in;
    m.out = s.slot[0].out;
  }
  g_meter.push_back(m);
}

uint32_t estimateOf(const agent::HttpRequest& r) {
  return orch::sentRequestEstimate(r.body.size(), r.splices.size()).promptTokens;
}

}  // namespace

void setUp() {
  LogCapture::install();
  g_meter.clear();
  orch::installSeamMeter(&sink);
  orch::setCustomEndpointPaid(true);
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  orch::installSeamMeter(nullptr);
  orch::setCustomEndpointPaid(true);
}

// ---- the rules ------------------------------------------------------------------------

static void test_what_counts_as_sent_without_an_answer() {
  // Sent, no answer: no status line, or an answer cut in its headers or body.
  for (const char* e : {"no response", "no response: alloc failed (2 failed)", "response headers cut short",
                        "response parse failed: IncompleteInput", "response too large for the device (9 B retained)"})
    TEST_ASSERT_TRUE_MESSAGE(orch::sentWithoutAnswer(0, e), e);
  TEST_ASSERT_TRUE(orch::sentWithoutAnswer(200, "response parse failed: IncompleteInput"));
  // Never left, or refused: nothing billed.
  for (const char* e : {"", "connect failed", "head write failed", "body truncated: write failed after 0 of 900 bytes",
                        "tls work-slot busy", "scripted transport error"})
    TEST_ASSERT_FALSE_MESSAGE(orch::sentWithoutAnswer(0, e), e);
  TEST_ASSERT_FALSE(orch::sentWithoutAnswer(500, "response parse failed: IncompleteInput"));
  TEST_ASSERT_FALSE(orch::sentWithoutAnswer(429, "no response"));
}

static void test_the_estimate_is_two_characters_a_token_and_no_output() {
  orch::TokenUsage u = orch::sentRequestEstimate(1001, 0);
  TEST_ASSERT_EQUAL_UINT32(501, u.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(0, u.completionTokens);
  TEST_ASSERT_EQUAL_UINT32(1, u.responses);
  u = orch::sentRequestEstimate(1000, 2);   // two photos at the image estimate
  TEST_ASSERT_EQUAL_UINT32((1000 + 2 * orch::kImageEstimateChars) / 2, u.promptTokens);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", wire::bodyModel("{\"model\":\"claude-haiku-5-5\",\"x\":1}").c_str());
  TEST_ASSERT_EQUAL_STRING("", wire::bodyModel("{\"events\":[]}").c_str());
}

// ---- every path a paid request takes ---------------------------------------------------

// A head round that got no answer: counted on its provider and model.
static void test_a_head_round_with_no_answer_is_counted() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back(harness_test::transportCut(0, "no response"));
  auto pd = d.contract();
  std::string conv, out, err;
  TEST_ASSERT_FALSE(providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, nullptr, nullptr));
  TEST_ASSERT_TRUE(g_meter.size() >= 1);
  TEST_ASSERT_EQUAL((int)orch::SpendSeam::Wire, (int)g_meter[0].seam);
  TEST_ASSERT_EQUAL_STRING("openai", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("model-openai", g_meter[0].model.c_str());
  TEST_ASSERT_EQUAL_UINT32(estimateOf(d.http.seen[0]), g_meter[0].in);
  TEST_ASSERT_EQUAL_UINT32(0, g_meter[0].out);
  TEST_ASSERT_EQUAL((size_t)d.http.seen.size(), g_meter.size());   // one estimate per request sent
}

// A sub-agent dispatch that got no answer (an OpenAI background job may have started):
// counted once, on the model it named.
static void test_a_sub_dispatch_with_no_answer_is_counted() {
  FakeProviderDeps d;
  d.http.script.push_back(harness_test::transportCut(120000, "no response"));
  auto pd = d.contract();
  agent::Directive dir{};
  dir.instruction = "research tea";
  char job[72] = {};
  const agent::FabricErr e =
      providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-6-luna", "openai", dir, job);
  TEST_ASSERT_TRUE(e != agent::FabricErr::Ok);
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_EQUAL_STRING("openai", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-6-luna", g_meter[0].model.c_str());
  TEST_ASSERT_EQUAL_UINT32(estimateOf(d.http.seen[0]), g_meter[0].in);
}

// A sub-agent that runs inside its one request, on a paid custom endpoint: counted on
// "custom" (priced by the endpoint's switch), never on the fail-closed router row; on a
// keyless local endpoint it is filed the same way and that switch prices it free.
static void test_an_in_request_sub_on_the_custom_endpoint_is_counted_as_custom() {
  FakeProviderDeps d;
  d.custBase = "https://openrouter.ai/api";
  d.custKey = "sk-or-fake";
  d.custModel = "meta/llama-405b";
  d.http.script.push_back(harness_test::transportCut(30000, "no response"));
  auto pd = d.contract();
  agent::Directive dir{};
  dir.instruction = "summarize tea";
  char job[72] = {};
  providers::customDispatch(pd, dir, job);
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_EQUAL_STRING("custom", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("meta/llama-405b", g_meter[0].model.c_str());
  // The route rule: a router path keeps its upstream, the custom host files as custom.
  const orch::SeamRoute routed = wire::estimateRoute(pd, "openrouter.ai", "/router/anthropic/v1/messages", "m");
  TEST_ASSERT_EQUAL_STRING("cumulo", routed.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("custom", wire::estimateRoute(pd, "openrouter.ai", "/v1/chat/completions", "m").provider.c_str());
  TEST_ASSERT_EQUAL_STRING("cumulo", wire::estimateRoute(pd, "evil.example", "/v1/chat/completions", "m").provider.c_str());
}

// The caption step (its own transport, not the wire): a photo sent with no answer counts
// its estimate in the caption's billed usage, the photo at the image estimate.
static void test_a_caption_with_no_answer_is_counted() {
  harness_test::FakeHttpTransport http;
  http.script.push_back(harness_test::transportCut(0, "no response"));
  http.script.push_back(harness_test::transportCut(0, "no response"));
  vision::CaptionDeps deps;
  deps.http = &http;
  deps.key = [](const std::string& p) { return p == "openai" ? std::string("key-openai") : std::string(); };
  deps.priority = [] { return std::string("openai"); };
  deps.nowMs = [] { return (uint32_t)5000; };
  const uint8_t photo[6] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  vision::CaptionRequest in;
  in.bytes = photo;
  in.len = sizeof photo;
  in.mime = "image/jpeg";
  const vision::CaptionResult r = vision::caption(deps, in);
  TEST_ASSERT_TRUE(r.text.empty());
  TEST_ASSERT_TRUE(http.seen.size() >= 1);
  // The transport records the body with the photo written in (base64, 8 characters for
  // these 6 bytes); the estimate counted the text skeleton plus the photo's estimate.
  TEST_ASSERT_EQUAL(1, (int)http.spliced[0].size());
  const uint32_t one = orch::sentRequestEstimate(http.seen[0].body.size() - 8, 1).promptTokens;
  TEST_ASSERT_EQUAL_UINT32(one * (uint32_t)http.seen.size(), r.usage.promptTokens);
  TEST_ASSERT_TRUE(r.usage.promptTokens >= orch::kImageEstimateChars / 2);   // the photo counts
  TEST_ASSERT_EQUAL_UINT32(0, r.usage.completionTokens);
}

// ---- the counter-tests -------------------------------------------------------------------

// A refused request (a 500, a 429) and one that never left (the head write failed, a
// connect failure) bill nothing: no estimate.
static void test_refused_or_unsent_requests_count_nothing() {
  {
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 500, "{\"error\":{\"message\":\"boom\"}}"});
    auto pd = d.contract();
    agent::Directive dir{};
    dir.instruction = "x";
    char job[72] = {};
    providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-6-luna", "openai", dir, job);
  }
  for (const char* cut : {"head write failed", "connect failed", "body truncated: write failed after 0 of 9 bytes"}) {
    FakeProviderDeps d;
    d.http.script.push_back(harness_test::transportCut(0, cut));
    auto pd = d.contract();
    agent::Directive dir{};
    dir.instruction = "x";
    char job[72] = {};
    providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-6-luna", "openai", dir, job);
  }
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
  // A free request with no answer (a poll) counts nothing either.
  FakeProviderDeps d;
  d.http.script.push_back(harness_test::transportCut(0, "no response"));
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  providers::oaiPoll(pd, "api.openai.com", "sk-fake-oai", "openai", "openai:resp_1", env);
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_what_counts_as_sent_without_an_answer);
  RUN_TEST(test_the_estimate_is_two_characters_a_token_and_no_output);
  RUN_TEST(test_a_head_round_with_no_answer_is_counted);
  RUN_TEST(test_a_sub_dispatch_with_no_answer_is_counted);
  RUN_TEST(test_an_in_request_sub_on_the_custom_endpoint_is_counted_as_custom);
  RUN_TEST(test_a_caption_with_no_answer_is_counted);
  RUN_TEST(test_refused_or_unsent_requests_count_nothing);
  return UNITY_END();
}
