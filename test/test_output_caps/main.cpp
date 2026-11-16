// test_output_caps - the output-cap table (output_caps.h) itself: every (adapter, round
// kind) row and model family, the effort each family takes, the same-host step-down
// and the time fit. These assertions moved here verbatim from test_provider_class,
// test_harness_wire_anthropic and test_harness_wire_mistral (lane P0 of the history
// fix, 16 Nov 2026), so the table's owner changes them in one suite; the wire suites
// keep the behavior on the wire (a cut is retried with a different body).
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"

namespace providers = agent::providers;

void setUp() { providers::setTestFirstAttemptCap(0); }
void tearDown() {}

// Every Anthropic model id the device can be pointed at, plus unknowns. Models
// are parameters: a new id joins this list, never a literal inside a test.
static const char* const kClaudeIds[] = {
    "claude-haiku-5-5", "claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-1",
    "claude-mythos-5-1", "claude-opus-5",    "claude-sonnet-5", "claude-fable-5",
    "claude-opus-4-8",  "claude-sonnet-4-6", "claude-haiku-4-5", "claude-3-5-haiku-latest",
    "model-anthropic",  "",
};

// (from test/test_provider_class/main.cpp)
// The table itself: every (adapter, round kind) row, every family. Caps that are
// sent fit a thinking model and the round's time; a capped kind always has a
// retry that differs; the effort field rides only families that accept it.
static void test_cap_table_rows() {
  for (int k = 0; k < providers::kRoundKindCount; k++) {
    const providers::RoundKind kind = (providers::RoundKind)k;
    for (const char* id : kClaudeIds) {
      const providers::OutputCap c = providers::outputCapFor("anthropic", id, kind, 120000);
      TEST_ASSERT_TRUE(c.maxTokens > 3000);          // the old cap cut thinking models
      TEST_ASSERT_TRUE(c.maxTokens * 5 <= 65535);   // one string can never pass the ceiling
      TEST_ASSERT_EQUAL(providers::anthropicTakesEffort(id), *c.effort != 0);
      // Time: at the family's slowest rate the cap finishes inside the round.
      const uint32_t rate = providers::slowestTokensPerSecond("anthropic", id);
      TEST_ASSERT_TRUE((uint64_t)c.maxTokens * 1000 / rate <= 120000);
      TEST_ASSERT_TRUE(c.hasRetry() && c.retryTokens > c.maxTokens);   // L19-44: always more room
    }
    const providers::OutputCap m = providers::outputCapFor("mistral", "mistral-small-latest", kind, 120000);
    TEST_ASSERT_TRUE(m.maxTokens > 0 && m.hasRetry() && m.retryTokens > m.maxTokens);
    const providers::OutputCap o = providers::outputCapFor("openai", "gpt-5.6", kind, 120000);
    TEST_ASSERT_EQUAL(0, (int)o.maxTokens);   // reasoning would starve the answer
  }
  // Haiku 4.5 answers 400 to the effort field (recorded 18 Oct); current families take it.
  TEST_ASSERT_FALSE(providers::anthropicTakesEffort("claude-haiku-4-5"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("claude-haiku-5-5"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("claude-opus-4-5"));
  TEST_ASSERT_FALSE(providers::anthropicTakesEffort("claude-sonnet-4-5-20250929"));
  TEST_ASSERT_TRUE(providers::anthropicTakesEffort("anthropic/claude-sonnet-5-5"));
  // The bench seam: the first attempt's cap is overridden, the retry keeps headroom.
  providers::setTestFirstAttemptCap(256);
  const providers::OutputCap t = providers::outputCapFor("anthropic", "claude-haiku-5-5", providers::RoundKind::ToolRound);
  TEST_ASSERT_EQUAL(256, (int)t.maxTokens);
  TEST_ASSERT_TRUE(t.hasRetry() && t.retryTokens > 256);
  providers::setTestFirstAttemptCap(0);
}

// (from test/test_harness_wire_anthropic/main.cpp)
// L19-44: under the round's time clamp the retry after a cut keeps MORE room than
// the first attempt, for every Claude family (a no-effort id included), every
// capped round kind and every round length - a full 120 s round on claude-opus-5
// used to collapse both caps to 5,500, so the retry was effort-only and a no-effort
// id had none; under 50 s the cap fell below v4.5.13's 3,000.
static void test_cap_table_rows_retry_keeps_more_room_under_the_clamp() {
  static const char* const kFamilies[] = {
      "claude-haiku-5-5", "claude-sonnet-5-5", "claude-sonnet-5", "claude-opus-5-5", "claude-opus-5",
      "claude-fable-5-1", "claude-mythos-5-1", "claude-opus-4-8", "claude-sonnet-4-6",
      "claude-haiku-4-5",          // takes no effort
      "claude-3-5-haiku-latest",   // takes no effort, old id shape
      "model-anthropic",           // unknown: the slowest tier, no effort
  };
  // 8 s is the floor a near-exhausted final round still gets (kMinRoundMs): shorter
  // than the prefill, so the floors alone hold there.
  for (uint32_t roundMs : {8000u, 40000u, 60000u, 120000u}) {
    for (const char* id : kFamilies) {
      for (providers::RoundKind kind : {providers::RoundKind::ToolRound, providers::RoundKind::FinalRound,
                                        providers::RoundKind::SingleShot, providers::RoundKind::Fold}) {
        const providers::OutputCap c = providers::outputCapFor("anthropic", id, kind, roundMs);
        char what[128];
        snprintf(what, sizeof(what), "%s %s at %u ms: %u then %u", id, providers::roundKindSlug(kind),
                 (unsigned)roundMs, (unsigned)c.maxTokens, (unsigned)c.retryTokens);
        TEST_ASSERT_TRUE_MESSAGE(c.hasRetry(), what);
        TEST_ASSERT_TRUE_MESSAGE(c.retryTokens > c.maxTokens, what);
        TEST_ASSERT_TRUE_MESSAGE(c.retryTokens * 3 >= c.maxTokens * 4, what);   // at least a third more
        TEST_ASSERT_TRUE_MESSAGE(c.maxTokens >= 3000, what);                     // never under v4.5.13's cap
      }
    }
  }
  // The full round on opus (the 19 Oct case): 4,125 first, 5,500 on the retry.
  const providers::OutputCap opus =
      providers::outputCapFor("anthropic", "claude-opus-5", providers::RoundKind::ToolRound, 120000);
  TEST_ASSERT_EQUAL(4125, (int)opus.maxTokens);
  TEST_ASSERT_EQUAL(5500, (int)opus.retryTokens);
  // Mistral keeps the same shape.
  for (uint32_t roundMs : {40000u, 60000u, 120000u}) {
    const providers::OutputCap m =
        providers::outputCapFor("mistral", "mistral-large-latest", providers::RoundKind::ToolRound, roundMs);
    TEST_ASSERT_TRUE(m.hasRetry() && m.retryTokens > m.maxTokens && m.maxTokens >= 3000);
  }
}

// (from test/test_harness_wire_mistral/main.cpp)
// The class behind the routine 6,050-token cut: every reasoning model gets an effort it
// accepts. Large 4 refused "none", the refusal dropped the field, and with no field it
// thinks at its default and runs out of room. Recorded live 1 Nov
// (evidence/probes/mistral-effort-matrix.txt): the value each accepts of none and low.
static void test_effort_table_matches_the_recording() {
  struct Row { const char* model; const char* accepts; };
  const Row rows[] = {{"mistral-large-4-0", "low"},      {"mistral-large-4", "low"},
                      {"mistral-medium-latest", "none"}, {"mistral-small-latest", "none"},
                      {"magistral-medium-latest", "none"}, {"mistral-vibe-cli-latest", "none"},
                      {"zai-glm-5-3", "low"},            {"zai-glm-5-3-flash", "low"},
                      {"mistral-large-latest", ""}};
  for (const Row& r : rows) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.accepts, providers::mistralHeadEffort(r.model), r.model);
    for (const providers::RoundKind k : {providers::RoundKind::ToolRound, providers::RoundKind::FinalRound,
                                         providers::RoundKind::SingleShot, providers::RoundKind::Fold}) {
      const providers::OutputCap c = providers::outputCapFor("mistral", r.model, k, providers::kHeadRoundCeilingMs);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(r.accepts, c.effort, r.model);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(r.accepts, c.retryEffort, r.model);
    }
  }
}

// (from test/test_harness_wire_mistral/main.cpp)
// The same-host step-down table (ruling 3 Nov 09:15Z): only a head that cannot turn
// its reasoning off gets a same-provider non-reasoning round before another provider
// is asked. Large 4 refuses effort "none" (the recording above); every other Mistral
// head runs at "none", Z.ai has no non-reasoning GLM, and every current Claude line
// takes effort.
static void test_the_same_host_step_down_table() {
  const struct { const char* host; const char* model; const char* alt; } rows[] = {
      {"mistral", "mistral-large-4-0", "mistral-medium-latest"},
      {"mistral", "mistral-large-4", "mistral-medium-latest"},
      {"mistral", "mistral/mistral-large-4-0", "mistral-medium-latest"},   // a router pick
      {"mistral", "mistral-medium-latest", ""},   // already runs at effort "none"
      {"mistral", "mistral-small-latest", ""},
      {"mistral", "mistral-large-latest", ""},
      {"zai", "glm-5.3", ""},                    // both GLM models reason
      {"zai", "glm-5.3-flash", ""},
      {"anthropic", "claude-opus-5", ""},         // every Claude line takes effort
      {"openai", "gpt-5.6-sol", ""},
  };
  for (const auto& r : rows)
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.alt, providers::sameHostNonReasoningModel(r.host, r.model),
                                     r.model);
}

// (from test/test_harness_wire_mistral/main.cpp)
// The class behind a slow failure: every Mistral head request's deadline sits just past the
// gateway's 60 s close, so a close reads as a timeout (no same-host retry), and a capped
// round's first attempt fits inside it at the model's measured rate.
static void test_mistral_rounds_fit_the_gateway() {
  TEST_ASSERT_TRUE(providers::kMistralRoundMs > 60000 && providers::kMistralRoundMs < 70000);
  for (const char* model : {"mistral-large-4-0", "mistral-small-latest", "mistral-medium-latest", "mistral-large-latest"}) {
    const providers::OutputCap c =
        providers::outputCapFor("mistral", model, providers::RoundKind::ToolRound, providers::kMistralRoundMs);
    const uint32_t seconds = c.maxTokens / providers::slowestTokensPerSecond("mistral", model);
    TEST_ASSERT_TRUE_MESSAGE(seconds <= 60, model);
    TEST_ASSERT_TRUE_MESSAGE(c.retryTokens > c.maxTokens, model);
  }
}

// (from test/test_harness_wire_mistral/main.cpp)
// The class: the table agrees with the recorded /v1/models capabilities.reasoning
// bit for EVERY chat model Mistral lists (a new model with no table row fails here).
static void test_cum616_effort_table_matches_the_recorded_model_list() {
  std::ifstream f("test/support/fixtures/models_2026-10/mistral.json");
  TEST_ASSERT_TRUE(f.good());
  const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, raw));
  int checked = 0;
  for (JsonObjectConst m : doc["data"].as<JsonArrayConst>()) {
    JsonObjectConst cap = m["capabilities"];
    if (!(cap["completion_chat"] | false) || !(cap["function_calling"] | false)) continue;
    const char* id = m["id"] | "";
    TEST_ASSERT_EQUAL_MESSAGE((bool)(cap["reasoning"] | false), providers::mistralTakesEffort(id), id);
    TEST_ASSERT_EQUAL_MESSAGE(providers::mistralTakesEffort(std::string("mistral/") + id),
                              providers::mistralTakesEffort(id), id);
    checked++;
  }
  TEST_ASSERT_TRUE(checked > 10);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cap_table_rows);
  RUN_TEST(test_cap_table_rows_retry_keeps_more_room_under_the_clamp);
  RUN_TEST(test_effort_table_matches_the_recording);
  RUN_TEST(test_the_same_host_step_down_table);
  RUN_TEST(test_mistral_rounds_fit_the_gateway);
  RUN_TEST(test_cum616_effort_table_matches_the_recorded_model_list);
  return UNITY_END();
}
