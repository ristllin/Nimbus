#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/seam_meter.h"
#include "nimbus/orch/usage_ledger.h"

// The seam meter (lane DEF, owner ruling 22 Oct 2026: a hard 20 USD across all
// providers, fail-closed): every paid call outside a turn reports what it cost, the
// provider's reported usage where the answer carries it, a conservative estimate where
// it does not. Recorded answer shapes for each seam; the sub-agents of every provider
// metered through their real dispatch and poll code over a scripted transport.

using namespace nimbus::orch;
using namespace harness_test;
namespace providers = agent::providers;

namespace {

struct Report {
  SpendSeam seam;
  std::string provider;
  uint64_t micro;   // priced by the built-in table, plus any estimate
  TokenUsage use;   // the first slot's usage (empty for an estimate)
  std::string model;
};
std::vector<Report> g_reports;

void sink(SpendSeam seam, const char* provider, const TurnSpend& spend, uint64_t est) {
  static UsageLedger ledger;   // built-in prices only
  Report r{seam, provider ? provider : "", addSatMicro(ledger.priceTurnMicro(spend), est), {}, ""};
  if (spend.n) {
    r.use.promptTokens = (uint32_t)spend.slot[0].in;
    r.use.completionTokens = (uint32_t)spend.slot[0].out;
    r.use.cacheReadTokens = (uint32_t)spend.slot[0].cacheR;
    r.use.cacheWriteTokens = (uint32_t)spend.slot[0].cacheW;
    r.model = spend.slot[0].model;
  }
  g_reports.push_back(r);
}

uint64_t total(SpendSeam seam) {
  uint64_t m = 0;
  for (const Report& r : g_reports)
    if (r.seam == seam) m += r.micro;
  return m;
}

}  // namespace

void setUp() {
  g_reports.clear();
  resetSeamMeterCounts();
  installSeamMeter(&sink);
}
void tearDown() { installSeamMeter(nullptr); }

// ---- text seams: reported usage wins, else the high estimate ------------------------

static void test_reported_usage_is_priced_at_the_models_row() {
  // An OpenAI embeddings answer (recorded shape): 8 prompt tokens.
  meterSeamAnswerBody(SpendSeam::Embed, "openai", "text-embedding-3-small",
                      "{\"object\":\"list\",\"data\":[{\"embedding\":[0.1]}],"
                      "\"usage\":{\"prompt_tokens\":8,\"total_tokens\":8}}",
                      40, 0);
  TEST_ASSERT_EQUAL(1, (int)g_reports.size());
  TEST_ASSERT_EQUAL(8, (int)g_reports[0].use.promptTokens);
  TEST_ASSERT_EQUAL_STRING("text-embedding-3-small", g_reports[0].model.c_str());
  TEST_ASSERT_EQUAL(1, (int)seamMeterReports(SpendSeam::Embed));
  // A Mistral moderation answer: priced at its own row ($0.10 per 1M), never the
  // provider's highest.
  meterSeamAnswerBody(SpendSeam::Moderation, "mistral", "mistral-moderation-latest",
                      "{\"results\":[{\"categories\":{}}],\"usage\":{\"prompt_tokens\":1000000}}",
                      10, 0);
  TEST_ASSERT_EQUAL(100000, (int)total(SpendSeam::Moderation));
}

static void test_no_usage_estimates_two_characters_a_token_and_the_whole_cap() {
  meterSeamAnswerBody(SpendSeam::UrlFetch, "openai", "gpt-5.6-luna", "{\"choices\":[]}", 1001, 60);
  TEST_ASSERT_EQUAL(1, (int)g_reports.size());
  TEST_ASSERT_EQUAL(501, (int)g_reports[0].use.promptTokens);   // rounded up
  TEST_ASSERT_EQUAL(60, (int)g_reports[0].use.completionTokens);
  // Never below a realistic count (real text runs about four characters a token).
  for (size_t chars : {1u, 7u, 400u, 100000u})
    TEST_ASSERT_TRUE(estimateTextUsage(chars, 0).promptTokens >= chars / 4);
  // An answer with no body at all is still one billed request.
  meterSeamAnswerBody(SpendSeam::Embed, "mistral", "mistral-embed", nullptr, 20, 0);
  TEST_ASSERT_EQUAL(2, (int)g_reports.size());
  TEST_ASSERT_EQUAL(10, (int)g_reports[1].use.promptTokens);
}

// ---- speech -------------------------------------------------------------------------

static void test_transcription_prefers_provider_counts_then_the_audio_length() {
  // OpenAI gpt-4o-mini-transcribe reports tokens.
  meterTranscription("openai", "gpt-4o-mini-transcribe",
                     "{\"text\":\"hi\",\"usage\":{\"type\":\"tokens\",\"input_tokens\":1000000,"
                     "\"output_tokens\":0}}",
                     1000);
  TEST_ASSERT_EQUAL(3000000, (int)total(SpendSeam::Stt));   // $3 per 1M audio tokens
  g_reports.clear();
  // whisper-1 reports a duration; Mistral reports audio seconds.
  meterTranscription("openai", "whisper-1", "{\"text\":\"hi\",\"usage\":{\"type\":\"duration\",\"seconds\":60}}", 1);
  TEST_ASSERT_EQUAL(6000, (int)total(SpendSeam::Stt));      // the seam's highest, $0.006 a minute
  g_reports.clear();
  meterTranscription("mistral", "voxtral-mini-latest",
                     "{\"text\":\"hi\",\"usage\":{\"prompt_audio_seconds\":120,\"prompt_tokens\":5}}", 1);
  TEST_ASSERT_EQUAL(4000, (int)total(SpendSeam::Stt));      // two minutes at $0.002
  g_reports.clear();
  // No usage: the audio's length. The mic's 16 kHz PCM is exact (32 bytes a ms); a
  // compressed note counts a second per kilobyte.
  TEST_ASSERT_EQUAL(1000, (int)audioMsEstimate(32000, true));
  TEST_ASSERT_EQUAL(10000, (int)audioMsEstimate(10240, false));
  meterTranscription("openai", "gpt-4o-mini-transcribe", "{\"text\":\"hi\"}", 60000);
  TEST_ASSERT_EQUAL(3000, (int)total(SpendSeam::Stt));
  g_reports.clear();
  // A hostile length saturates (never wraps to a small count); a negative one falls back
  // to the recording's own length.
  meterTranscription("mistral", "voxtral-mini-latest", "{\"usage\":{\"prompt_audio_seconds\":1e300}}", 1);
  TEST_ASSERT_EQUAL_UINT64(sttMicro("voxtral-mini-latest", 4294967000u), total(SpendSeam::Stt));
  g_reports.clear();
  meterTranscription("mistral", "voxtral-mini-latest", "{\"usage\":{\"prompt_audio_seconds\":-5}}", 60000);
  TEST_ASSERT_EQUAL(2000, (int)total(SpendSeam::Stt));
  TEST_ASSERT_EQUAL(1, (int)sttMicro("gpt-4o-mini-transcribe", 1));   // never rounds to free
}

static void test_speech_is_priced_per_character_above_the_published_rate() {
  meterSpeech("openai", "gpt-4o-mini-tts", 1000);
  TEST_ASSERT_EQUAL(25000, (int)total(SpendSeam::Tts));
  // $0.015 a minute at 900 characters a minute is $16.7 per 1M: the estimate is above.
  TEST_ASSERT_TRUE(ttsMicro("gpt-4o-mini-tts", 900) >= 15000);
  TEST_ASSERT_EQUAL(30, (int)ttsMicro("voxtral-mini-tts-latest", 1));   // the seam's highest
}

// Every unit table's "*" row is its highest, so a model with no row of its own never
// prices below a known one (fail-closed).
static void test_unit_tables_fall_back_to_their_highest() {
  struct T { const SeamUnitPrice* t; size_t n; };
  const T tables[] = {{kSttPerMinute, sizeof(kSttPerMinute) / sizeof(kSttPerMinute[0])},
                      {kTtsPerMChars, sizeof(kTtsPerMChars) / sizeof(kTtsPerMChars[0])}};
  for (const T& tb : tables) {
    const uint32_t star = seamUnitPrice(tb.t, tb.n, "some-unknown-model");
    for (size_t i = 0; i < tb.n; ++i) TEST_ASSERT_TRUE(tb.t[i].micro <= star);
  }
}

// ---- images -------------------------------------------------------------------------

static void test_image_usage_wins_else_the_largest_size_at_its_quality() {
  meterImage("openai", "gpt-image-1", "low",
             "{\"input_tokens\":50,\"output_tokens\":272,\"total_tokens\":322}, \"tail\":1}");
  // 50 x $10/M + 272 x $40/M = $0.0005 + $0.01088
  TEST_ASSERT_EQUAL(11380, (int)total(SpendSeam::ImageGen));
  g_reports.clear();
  meterImage("openai", "gpt-image-1", "high", nullptr);
  TEST_ASSERT_EQUAL(250000, (int)total(SpendSeam::ImageGen));
  TEST_ASSERT_EQUAL(16000, (int)imageMicro(nullptr, "low"));         // the default model
  TEST_ASSERT_EQUAL(250000, (int)imageMicro("dall-e-3", "standard"));   // unknown: the highest
}

// ---- routes -------------------------------------------------------------------------

static void test_hand_built_requests_meter_under_their_route() {
  SeamRoute r = seamRouteFor("api.openai.com", "/v1/chat/completions", "gpt-5.6-luna");
  TEST_ASSERT_EQUAL_STRING("openai", r.provider.c_str());
  r = seamRouteFor("app.cumulo-nimbus.ai", "/router/anthropic/v1/messages", "claude-haiku-5-5");
  TEST_ASSERT_EQUAL_STRING("cumulo", r.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-5-5", r.model.c_str());
  r = seamRouteFor("api.mistral.ai", "/v1/chat/completions", "mistral-small-latest");
  TEST_ASSERT_EQUAL_STRING("mistral", r.provider.c_str());
  r = seamRouteFor("api.anthropic.com", "/v1/messages", "claude-haiku-4-5-20251001");
  TEST_ASSERT_EQUAL_STRING("anthropic", r.provider.c_str());
  // An unknown host prices fail-closed (every provider's highest row), never at zero.
  r = seamRouteFor("example.net", "/v1/chat/completions", "m");
  TEST_ASSERT_EQUAL_STRING("cumulo", r.provider.c_str());
  meterSeamAnswerBody(SpendSeam::VisionFallback, r.provider.c_str(), r.model.c_str(),
                      "{\"usage\":{\"prompt_tokens\":1000000}}", 0, 0);
  uint32_t in = 0, out = 0;
  builtinHighestPrice("cumulo", in, out);
  TEST_ASSERT_EQUAL((int)in, (int)total(SpendSeam::VisionFallback));
}

// Every model a seam calls by default has a price row of its own (so its reported
// usage prices at its rate, not its provider's highest).
static void test_every_default_seam_model_has_its_own_price_row() {
  const char* rows[][2] = {{"openai", "text-embedding-3-small"},   {"mistral", "mistral-embed"},
                           {"mistral", "mistral-moderation-latest"}, {"openai", "gpt-image-1"},
                           {"openai", "gpt-4o-mini-transcribe"},     {"openai", "gpt-5.6-luna"},
                           {"mistral", "mistral-small-latest"}};
  for (auto& r : rows) TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice(r[0], r[1]), r[1]);
}

// ---- sub-agents ---------------------------------------------------------------------

// The recorded Anthropic session usage (22 Oct 2026, a claude-haiku-5-5 session): the
// cache traffic stays outside input; a 1-hour write counts twice as input.
static void test_anthropic_session_usage_shape() {
  ArduinoJson::JsonDocument d;
  deserializeJson(d, "{\"input_tokens\":4,\"cache_read_input_tokens\":6028,\"cache_creation\":"
                     "{\"ephemeral_5m_input_tokens\":128,\"ephemeral_1h_input_tokens\":10},"
                     "\"output_tokens\":15}");
  const TokenUsage u = anthropicSessionUsage(d.as<ArduinoJson::JsonVariantConst>());
  TEST_ASSERT_EQUAL(4 + 20, (int)u.promptTokens);
  TEST_ASSERT_EQUAL(6028, (int)u.cacheReadTokens);
  TEST_ASSERT_EQUAL(128, (int)u.cacheWriteTokens);
  TEST_ASSERT_EQUAL(15, (int)u.completionTokens);
  TEST_ASSERT_EQUAL(0, (int)u.maxRequestIn);   // a session's request size is unknown
  ArduinoJson::JsonDocument huge;   // a hostile count saturates, never wraps to a small one
  deserializeJson(huge, "{\"cache_creation\":{\"ephemeral_1h_input_tokens\":18446744073709551615}}");
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, anthropicSessionUsage(huge.as<ArduinoJson::JsonVariantConst>()).promptTokens);
  ArduinoJson::JsonDocument flat;   // the flat cache-write field
  deserializeJson(flat, "{\"input_tokens\":1,\"cache_creation_input_tokens\":7,\"output_tokens\":2}");
  TEST_ASSERT_EQUAL(7, (int)anthropicSessionUsage(flat.as<ArduinoJson::JsonVariantConst>()).cacheWriteTokens);
}

static void test_anthropic_finished_session_reads_its_usage() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/sessions/sess_1/events", 200,
      "{\"data\":[{\"type\":\"agent.message\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]},"
      "{\"type\":\"session.status_idle\",\"stop_reason\":{\"type\":\"end_turn\"}}]}"});
  d.http.script.push_back({"", "/v1/sessions/sess_1", 200,
      "{\"id\":\"sess_1\",\"usage\":{\"input_tokens\":4,\"cache_read_input_tokens\":6028,"
      "\"cache_creation\":{\"ephemeral_5m_input_tokens\":128},\"output_tokens\":15}}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sess_1", env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  TEST_ASSERT_EQUAL(4, (int)env.promptTokens);
  TEST_ASSERT_EQUAL(6028, (int)env.cacheReadTokens);
  TEST_ASSERT_EQUAL(128, (int)env.cacheWriteTokens);
  TEST_ASSERT_EQUAL(15, (int)env.completionTokens);
  // A running session reads no usage (one request, no session GET).
  FakeProviderDeps run;
  run.http.script.push_back({"", "/events", 200, "{\"data\":[]}"});
  auto rpd = run.contract();
  agent::ResultEnvelope renv{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(rpd, "anthropic:sess_1", renv));
  TEST_ASSERT_EQUAL(1, (int)run.http.seen.size());
  // A cut or busy usage read answers Network: the job is polled again and the read
  // retried (nothing metered yet, so the retry never double-counts).
  for (int status : {0, 429}) {
    FakeProviderDeps cut;
    cut.http.script.push_back(d.http.script[0]);
    Exchange e = transportCut(0);
    if (status) e = Exchange{"", "/v1/sessions/", status, "{}"};
    cut.http.script.push_back(e);
    auto cpd = cut.contract();
    agent::ResultEnvelope cenv{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Network, (int)providers::antPoll(cpd, "anthropic:sess_1", cenv));
  }
  // Any other failed read (the session is gone, the endpoint fails): the result is
  // delivered and the session's estimate counts, never zero.
  for (int status : {404, 503}) {
    FakeProviderDeps gone;
    gone.http.script.push_back(d.http.script[0]);
    gone.http.script.push_back({"", "/v1/sessions/", status, "{}"});
    auto gpd = gone.contract();
    agent::ResultEnvelope genv{};
    TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antPoll(gpd, "anthropic:sess_1", genv));
    TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)genv.state);
    TEST_ASSERT_EQUAL((int)kSessionInputEstimateTokens, (int)genv.promptTokens);
    TEST_ASSERT_EQUAL(1, (int)genv.completionTokens);   // "ok": two characters
  }
}

// An OpenAI sub's terminal poll: its web searches and a code-interpreter container count
// beside the tokens (which the job engine files from the envelope); a running poll
// meters nothing.
static void test_openai_sub_meters_searches_and_its_container() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/responses/r1", 200,
      "{\"status\":\"in_progress\",\"output\":[{\"type\":\"web_search_call\"}]}"});
  d.http.script.push_back({"", "/v1/responses/r1", 200,
      "{\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\"},"
      "{\"type\":\"web_search_call\"},{\"type\":\"code_interpreter_call\"},"
      "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}],"
      "\"usage\":{\"input_tokens\":100,\"output_tokens\":10,"
      "\"input_tokens_details\":{\"cached_tokens\":40}}}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiPoll(pd, "api.openai.com", "k", "openai", "openai:r1", env));
  TEST_ASSERT_EQUAL(0, (int)g_reports.size());
  agent::ResultEnvelope done{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiPoll(pd, "api.openai.com", "k", "openai", "openai:r1", done));
  TEST_ASSERT_EQUAL(40, (int)done.cacheReadTokens);
  TEST_ASSERT_EQUAL((int)(2 * kProviderSearchRunMicro + kProviderContainerMicro), (int)total(SpendSeam::Sub));
}

static void test_mistral_sub_meters_its_usage_and_search_runs() {
  FakeProviderDeps d;
  d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
      "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"tool.execution\",\"name\":\"web_search\"},"
      "{\"type\":\"message.output\",\"content\":\"the result\"}],"
      "\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":100,\"connector_tokens\":500}}"});
  auto pd = d.contract();
  agent::Directive dir;
  dir.instruction = "do it";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::mistralDispatch(pd, "mistral-small-latest", dir, jobId));
  TEST_ASSERT_EQUAL(2, (int)g_reports.size());
  TEST_ASSERT_EQUAL((int)SpendSeam::Sub, (int)g_reports[0].seam);
  TEST_ASSERT_EQUAL_STRING("mistral", g_reports[0].provider.c_str());
  TEST_ASSERT_EQUAL(1500, (int)g_reports[0].use.promptTokens);   // connector traffic included
  TEST_ASSERT_EQUAL(100, (int)g_reports[0].use.completionTokens);
  TEST_ASSERT_EQUAL((int)kProviderSearchRunMicro, (int)g_reports[1].micro);   // one web search
}

static void test_compat_and_custom_subs_meter_their_usage_or_an_estimate() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200,
      "{\"choices\":[{\"message\":{\"content\":\"hi\"}}],"
      "\"usage\":{\"prompt_tokens\":300,\"completion_tokens\":20}}"});
  auto pd = d.contract();
  providers::CompatEndpoint ep;
  ep.host = "api.z.ai";
  ep.basePath = "/api/paas/v4";
  ep.key = "tok";
  ep.model = "glm-5.3-flash";
  ep.backendTag = "zai";
  ep.wire = providers::CompatWire::OpenAIChat;
  agent::Directive dir;
  dir.category = "ops";
  dir.instruction = "write a haiku";
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::openaiCompatDispatch(pd, ep, dir, jobId));
  TEST_ASSERT_EQUAL(1, (int)g_reports.size());
  TEST_ASSERT_EQUAL_STRING("zai", g_reports[0].provider.c_str());
  TEST_ASSERT_EQUAL(300, (int)g_reports[0].use.promptTokens);
  // A custom endpoint that reports no usage: estimated from the request and the reply.
  FakeProviderDeps c;
  c.custBase = "https://llm.example.net/v1";
  c.http.script.push_back({"", "/v1/chat/completions", 200,
      "{\"choices\":[{\"message\":{\"content\":\"0123456789\"}}]}"});
  auto cpd = c.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::customDispatch(cpd, dir, jobId));
  TEST_ASSERT_EQUAL(2, (int)g_reports.size());
  TEST_ASSERT_EQUAL_STRING("custom", g_reports[1].provider.c_str());
  TEST_ASSERT_EQUAL(5, (int)g_reports[1].use.completionTokens);   // ten characters of reply
  TEST_ASSERT_TRUE(g_reports[1].use.promptTokens > 0);
}

// Nothing installed (the lab): reports are counted and dropped, nothing breaks.
static void test_no_sink_counts_and_drops() {
  installSeamMeter(nullptr);
  meterSpeech("openai", "gpt-4o-mini-tts", 10);
  TEST_ASSERT_EQUAL(0, (int)g_reports.size());
  TEST_ASSERT_EQUAL(1, (int)seamMeterReports(SpendSeam::Tts));
  meterSeamMicro(SpendSeam::Tts, "openai", 0);   // a free call reports nothing
  TEST_ASSERT_EQUAL(1, (int)seamMeterReports(SpendSeam::Tts));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reported_usage_is_priced_at_the_models_row);
  RUN_TEST(test_no_usage_estimates_two_characters_a_token_and_the_whole_cap);
  RUN_TEST(test_transcription_prefers_provider_counts_then_the_audio_length);
  RUN_TEST(test_speech_is_priced_per_character_above_the_published_rate);
  RUN_TEST(test_unit_tables_fall_back_to_their_highest);
  RUN_TEST(test_image_usage_wins_else_the_largest_size_at_its_quality);
  RUN_TEST(test_hand_built_requests_meter_under_their_route);
  RUN_TEST(test_every_default_seam_model_has_its_own_price_row);
  RUN_TEST(test_anthropic_session_usage_shape);
  RUN_TEST(test_anthropic_finished_session_reads_its_usage);
  RUN_TEST(test_openai_sub_meters_searches_and_its_container);
  RUN_TEST(test_mistral_sub_meters_its_usage_and_search_runs);
  RUN_TEST(test_compat_and_custom_subs_meter_their_usage_or_an_estimate);
  RUN_TEST(test_no_sink_counts_and_drops);
  return UNITY_END();
}
