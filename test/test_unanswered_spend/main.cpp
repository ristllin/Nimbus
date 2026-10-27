#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/harness/websearch.h"
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
  TEST_ASSERT_TRUE(g_meter[0].out <= orch::kEstimateOutputTokensPerSec);   // failed at once: at most a second's writing
  TEST_ASSERT_EQUAL((size_t)d.http.seen.size(), g_meter.size());   // one estimate per request sent
}

// A sub-agent dispatch that got no answer (an OpenAI background job may have started):
// counted once, on the model it named, at least the session estimate (round 3), with the
// output it could have written in its two minutes (capped by its own output cap).
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
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, g_meter[0].in);
  const uint32_t cap = orch::bodyOutputCap(d.http.seen[0].body);
  const uint32_t held = 120u * orch::kEstimateOutputTokensPerSec;
  const uint32_t want = cap && cap < held ? cap : held;   // (+ the fake clock's few ms)
  TEST_ASSERT_TRUE(g_meter[0].out >= want && g_meter[0].out <= want + orch::kEstimateOutputTokensPerSec);
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

// The Z.ai head borrows the custom adapter (its base set to the Z.ai host): its estimate
// is Z.ai's (price and monthly limit), never "custom" priced by the owner's own endpoint.
static void test_a_head_borrowing_the_custom_adapter_is_filed_on_its_provider() {
  FakeProviderDeps d;
  d.custBase = "https://api.z.ai";
  auto pd = d.contract();
  const orch::SeamRoute r = wire::estimateRoute(pd, "api.z.ai", "/api/paas/v4/chat/completions", "glm-5.3");
  TEST_ASSERT_EQUAL_STRING("zai", r.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("glm-5.3", r.model.c_str());
  d.custBase = "https://api.openai.com";   // an official host as the custom base: that provider
  auto po = d.contract();
  TEST_ASSERT_EQUAL_STRING("openai", wire::estimateRoute(po, "api.openai.com", "/v1/chat/completions", "m").provider.c_str());
}

// An Anthropic sub whose starting turn got no answer may be running with no job id to
// poll or stop it: the session is interrupted at once, and the confirmed interrupt reads
// the session back and counts what it used (beside the turn's own estimate).
static void test_an_anthropic_turn_with_no_answer_interrupts_its_session() {
  providers::antForgetIds();
  FakeProviderDeps d;
  d.antEnv = "env_1";
  d.antAgents = "claude-sonnet-5=ag_1;";
  d.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"sess_1\"}"});
  d.http.script.push_back(harness_test::transportCut(2000, "no response"));
  d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200, "{\"data\":[]}"});
  d.http.script.push_back({"", "/v1/sessions/sess_1", 200,
                           "{\"id\":\"sess_1\",\"status\":\"idle\",\"agent\":{\"model\":{\"id\":\"claude-sonnet-5\"}},"
                           "\"usage\":{\"input_tokens\":900,\"output_tokens\":40}}"});
  auto pd = d.contract();
  agent::Directive dir{};
  dir.instruction = "research tea";
  char job[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Timeout, (int)providers::antDispatch(pd, "claude-sonnet-5", dir, job));
  TEST_ASSERT_EQUAL(4, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(d.http.seen[2].body.find("user.interrupt") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("GET", d.http.seen[3].method.c_str());
  TEST_ASSERT_EQUAL(2, (int)g_meter.size());
  TEST_ASSERT_EQUAL((int)orch::SpendSeam::Wire, (int)g_meter[0].seam);   // the turn's estimate
  TEST_ASSERT_EQUAL_STRING("anthropic", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_UINT32(estimateOf(d.http.seen[1]), g_meter[0].in);
  TEST_ASSERT_EQUAL((int)orch::SpendSeam::Sub, (int)g_meter[1].seam);    // the session read back
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", g_meter[1].model.c_str());
  TEST_ASSERT_EQUAL_UINT32(900, g_meter[1].in);
  TEST_ASSERT_EQUAL_UINT32(40, g_meter[1].out);
  TEST_ASSERT_EQUAL_STRING("", providers::takeUnseenRun().c_str());   // stopped and counted: nothing left
  // Round 3 (merge review blocker 1): the interrupt failed too. The session id is never
  // dropped: the engine gets it (and keeps stopping and counting it in the background).
  providers::antForgetIds();
  FakeProviderDeps f;
  f.antEnv = "env_1";
  f.antAgents = "claude-sonnet-5=ag_1;";
  f.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"sess_3\"}"});
  f.http.script.push_back(harness_test::transportCut(2000, "no response"));
  f.http.script.push_back(harness_test::transportCut(2000, "no response"));
  auto pf = f.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Timeout, (int)providers::antDispatch(pf, "claude-sonnet-5", dir, job));
  TEST_ASSERT_EQUAL(3, (int)f.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("anthropic:sess_3", providers::takeUnseenRun().c_str());
  TEST_ASSERT_EQUAL_STRING("", providers::takeUnseenRun().c_str());   // taken once
  // A turn that never left starts nothing: no interrupt.
  providers::antForgetIds();
  FakeProviderDeps u;
  u.antEnv = "env_1";
  u.antAgents = "claude-sonnet-5=ag_1;";
  u.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"sess_2\"}"});
  u.http.script.push_back(harness_test::transportCut(0, "head write failed"));
  auto pu = u.contract();
  providers::antDispatch(pu, "claude-sonnet-5", dir, job);
  TEST_ASSERT_EQUAL(2, (int)u.http.seen.size());
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
  // Each attempt is filed on the provider it went to.
  uint32_t filed = 0;
  for (const auto& sp : r.spends) {
    TEST_ASSERT_EQUAL_STRING("openai", sp.provider.c_str());
    filed += sp.usage.promptTokens;
  }
  TEST_ASSERT_EQUAL_UINT32(r.usage.promptTokens, filed);
}

// Two providers tried, neither answered: each estimate stays on its own provider (never
// summed onto the last or the best one), so each is priced and limited as its own.
static void test_caption_attempts_are_filed_on_their_own_providers() {
  harness_test::FakeHttpTransport http;
  for (int i = 0; i < 6; ++i) http.script.push_back(harness_test::transportCut(0, "no response"));
  vision::CaptionDeps deps;
  deps.http = &http;
  deps.key = [](const std::string& p) { return p == "openai" || p == "anthropic" ? "key-" + p : std::string(); };
  deps.priority = [] { return std::string("openai,anthropic"); };
  deps.nowMs = [] { return (uint32_t)7000; };
  const uint8_t photo[6] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  vision::CaptionRequest in;
  in.bytes = photo;
  in.len = sizeof photo;
  in.mime = "image/jpeg";
  const vision::CaptionResult r = vision::caption(deps, in);
  TEST_ASSERT_TRUE(r.text.empty());
  bool sawOpenai = false, sawAnthropic = false;
  for (const auto& sp : r.spends) {
    sawOpenai = sawOpenai || sp.provider == "openai";
    sawAnthropic = sawAnthropic || sp.provider == "anthropic";
    TEST_ASSERT_TRUE(sp.usage.promptTokens >= orch::kImageEstimateChars / 2);
  }
  TEST_ASSERT_TRUE(sawOpenai);
  TEST_ASSERT_TRUE(sawAnthropic);
}

// A web search sent with no answer (or answered 2xx and cut) may have been billed: the
// result says so, and the tool counts it like a search (test_spend_seams pins the count).
static void test_a_search_with_no_answer_is_marked_billed() {
  harness_test::FakeHttpTransport http;
  http.script.push_back(harness_test::transportCut(0, "no response"));
  TEST_ASSERT_TRUE(agent::websearch::search(http, "k", "tea", 3).billed);
  harness_test::FakeHttpTransport refused;
  refused.script.push_back({"", "", 429, "{\"detail\":{\"error\":\"quota\"}}"});
  TEST_ASSERT_FALSE(agent::websearch::search(refused, "k", "tea", 3).billed);
  harness_test::FakeHttpTransport unsent;
  unsent.script.push_back(harness_test::transportCut(0, "connect failed"));
  TEST_ASSERT_FALSE(agent::websearch::search(unsent, "k", "tea", 3).billed);
}

// Round 3 (merge review major 6): a request held open to its deadline may have had a
// model writing all that time, and a background start may have begun a whole run: the
// estimate counts output for the time it was held, and a run's start counts at least the
// session estimate.
static void test_a_request_held_to_its_deadline_counts_output_too() {
  FakeProviderDeps d;
  d.toolLoop = false;
  d.http.script.push_back(harness_test::transportCut(120000, "no response"));
  auto pd = d.contract();
  std::string conv, out, err;
  providers::orchTurnOpenAI(pd, conv, "SYS", "USER", out, err, nullptr, nullptr);
  TEST_ASSERT_TRUE(g_meter.size() >= 1);
  TEST_ASSERT_TRUE(g_meter[0].out > 0);   // two minutes of possible writing
  g_meter.clear();
  FakeProviderDeps b;   // an OpenAI background start: a whole run may have begun
  b.http.script.push_back(harness_test::transportCut(1000, "no response"));
  auto pb = b.contract();
  agent::Directive dir{};
  dir.instruction = "research tea";
  char job[72] = {};
  providers::oaiDispatch(pb, "api.openai.com", "sk-fake-oai", "gpt-6-luna", "openai", dir, job);
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_TRUE(g_meter[0].in >= orch::kSessionInputEstimateTokens);
}

// The estimate's rule, by itself.
static void test_the_held_open_estimate_rule() {
  orch::UnansweredRequest r;
  r.bodyChars = 1000;
  TEST_ASSERT_EQUAL_UINT32(0, orch::unansweredEstimate(r).completionTokens);   // failed at once
  r.heldMs = 120000;
  TEST_ASSERT_EQUAL_UINT32(120u * orch::kEstimateOutputTokensPerSec, orch::unansweredEstimate(r).completionTokens);
  r.outputCap = 4096;
  TEST_ASSERT_EQUAL_UINT32(4096, orch::unansweredEstimate(r).completionTokens);   // never past its own cap
  TEST_ASSERT_EQUAL_UINT32(500, orch::unansweredEstimate(r).promptTokens);
  r.startsRun = true;
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, orch::unansweredEstimate(r).promptTokens);
  TEST_ASSERT_EQUAL_UINT32(16000, orch::bodyOutputCap("{\"model\":\"m\",\"max_output_tokens\":16000}"));
  TEST_ASSERT_EQUAL_UINT32(800, orch::bodyOutputCap("{\"max_tokens\": 800,\"x\":1}"));
  TEST_ASSERT_EQUAL_UINT32(0, orch::bodyOutputCap("{\"model\":\"m\"}"));
  TEST_ASSERT_TRUE(orch::bodyStartsRun("/v1/responses", "{\"background\":true}"));
  TEST_ASSERT_FALSE(orch::bodyStartsRun("/v1/responses", "{\"stream\":false}"));
  TEST_ASSERT_TRUE(orch::bodyStartsRun("/v1/conversations", "{}"));
  TEST_ASSERT_FALSE(orch::bodyStartsRun("/v1/chat/completions", "{}"));
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
  RUN_TEST(test_a_head_borrowing_the_custom_adapter_is_filed_on_its_provider);
  RUN_TEST(test_an_anthropic_turn_with_no_answer_interrupts_its_session);
  RUN_TEST(test_a_caption_with_no_answer_is_counted);
  RUN_TEST(test_caption_attempts_are_filed_on_their_own_providers);
  RUN_TEST(test_a_search_with_no_answer_is_marked_billed);
  RUN_TEST(test_a_request_held_to_its_deadline_counts_output_too);
  RUN_TEST(test_the_held_open_estimate_rule);
  RUN_TEST(test_refused_or_unsent_requests_count_nothing);
  return UNITY_END();
}
