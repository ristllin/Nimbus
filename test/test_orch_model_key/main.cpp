#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_key.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/usage_ledger.h"

// test_orch_model_key - the ONE rule that files a turn's spend and its price under a
// (provider, model) key (owner 2026-10-13: per-model prices). The class guards:
// every cold-start id of every provider slot normalizes idempotently and resolves a
// price (a new slot or a new cold-start id with no price fails here), a dated echo
// lands on its alias, capability and 3-digit suffixes are never stripped.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static std::string norm(const char* raw) {
  char out[kModelKeyCap];
  normalizeModelId(raw, out);
  return out;
}

static std::string key(const char* host, const char* configured, const char* served) {
  char out[kModelKeyCap];
  usageModelKey(host, configured, served, out);
  return out;
}

static std::vector<std::string> csvItems(const std::string& csv) {
  std::vector<std::string> v;
  size_t p = 0;
  while (p <= csv.size()) {
    size_t c = csv.find(',', p);
    if (c == std::string::npos) c = csv.size();
    if (c > p) v.push_back(csv.substr(p, c - p));
    p = c + 1;
  }
  return v;
}

// Every cold-start id of every slot: idempotent, a safe key, and priced by a
// built-in tier (model row or the provider default) - so a cold-boot device never
// meters a model at "not priced".
static void test_every_cold_start_id_is_idempotent_and_priced() {
  int n = 0;
  for (size_t i = 0; i < kProviderSlotCount; ++i) {
    const char* slug = kProviderSlots[i].slug;
    for (const std::string& id : csvItems(staticModelChoices(slug))) {
      const std::string k = key(slug, id.c_str(), "");
      TEST_ASSERT_FALSE_MESSAGE(k.empty(), id.c_str());
      TEST_ASSERT_TRUE_MESSAGE(modelKeySafe(k.c_str()), id.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(k.c_str(), key(slug, k.c_str(), "").c_str(), id.c_str());
      const std::string bare = std::strcmp(slug, "cumulo") == 0 ? k.substr(k.find('/') + 1) : k;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(bare.c_str(), norm(bare.c_str()).c_str(), id.c_str());
      UsageLedger L;
      const EffRate r = L.effectiveRate(slug, k.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.in > 0 && r.out > 0, id.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.approx(), id.c_str());   // built-in = always an estimate
      ++n;
    }
  }
  TEST_ASSERT_TRUE(n >= 16);
}

// The cold-start ids the cloud seed prices carry their OWN row (not the provider
// default): the tiers of gpt-5.6 never share a price.
static void test_cold_start_tiers_have_their_own_rows() {
  const char* ids[] = {"gpt-6-astra", "gpt-5.6", "gpt-5.6-terra", "gpt-5.6-luna", nullptr};
  for (int i = 0; ids[i]; ++i)
    TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice("openai", ids[i]), ids[i]);
  TEST_ASSERT_NOT_EQUAL(builtinModelPrice("openai", "gpt-5.6")->microIn,
                        builtinModelPrice("openai", "gpt-5.6-luna")->microIn);
  TEST_ASSERT_NOT_NULL(builtinModelPrice("anthropic", "claude-haiku-4-5"));
  TEST_ASSERT_NOT_NULL(builtinModelPrice("zai", "glm-5.3-flash"));
  TEST_ASSERT_NOT_NULL(builtinModelPrice("openai", "gpt-4o-mini"));
  TEST_ASSERT_NOT_NULL(builtinModelPrice("openai", "text-embedding-3-small"));
  // Cumulo keys read their upstream's rows.
  TEST_ASSERT_EQUAL_PTR(builtinModelPrice("anthropic", "claude-opus-5"),
                        builtinModelPrice("cumulo", "anthropic/claude-opus-5"));
  TEST_ASSERT_EQUAL_STRING(kBuiltinPricesAsOf, "2026-10");
}

static void test_dated_ids_map_to_their_alias() {
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", norm("claude-haiku-4-5-20251001").c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-4o", norm("gpt-4o-2024-08-06").c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.5", norm("gpt-5.5-2026-01").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large", norm("mistral-large-2411").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large", norm("mistral-large-latest").c_str());
  TEST_ASSERT_EQUAL_STRING("model", norm("model-v2").c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-4", norm("gpt-4-0125").c_str());
  // ONE suffix only.
  TEST_ASSERT_EQUAL_STRING("foo-2411", norm("foo-2411-latest").c_str());
  // A dated alias and its alias file under one key, and one price.
  TEST_ASSERT_EQUAL_STRING(key("anthropic", "claude-haiku-4-5", "").c_str(),
                           key("anthropic", "claude-haiku-4-5-20251001", "").c_str());
  TEST_ASSERT_EQUAL_PTR(builtinModelPrice("anthropic", "claude-haiku-4-5"),
                        builtinModelPrice("anthropic", "claude-haiku-4-5-20251001"));
}

static void test_capability_and_three_digit_suffixes_are_kept() {
  const char* kept[] = {"gpt-5.6-luna", "gpt-5.6-terra", "gpt-4o-mini", "o1-pro",
                        "glm-5.3-flash", "glm-4.5-air", "text-embedding-ada-002",
                        "llama-3.1-405", "claude-haiku-4-5", "gpt-5", nullptr};
  for (int i = 0; kept[i]; ++i) TEST_ASSERT_EQUAL_STRING_MESSAGE(kept[i], norm(kept[i]).c_str(), kept[i]);
  // A capability suffix is a different model: never priced as its base.
  TEST_ASSERT_NULL(builtinModelPrice("openai", "gpt-6-astra-pro"));
  TEST_ASSERT_EQUAL_PTR(builtinModelPrice("openai", "gpt-5.6-luna"),
                        builtinModelPrice("openai", "gpt-5.6-luna-2026-07-09"));
  // The custom slot is never stripped.
  TEST_ASSERT_EQUAL_STRING("llama-3.1-405", key("custom", "llama-3.1-405", "").c_str());
  TEST_ASSERT_EQUAL_STRING("qwen3-latest", key("custom", "qwen3-latest", "").c_str());
  TEST_ASSERT_EQUAL_STRING("llama3.1:8b", key("custom", "llama3.1:8b", "").c_str());
  // No lowercasing.
  TEST_ASSERT_EQUAL_STRING("Mistral-Large", norm("Mistral-Large-latest").c_str());
}

static void test_cumulo_keys() {
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", key("cumulo", "gpt-5.6", "").c_str());          // bare
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5",
                           key("cumulo", "anthropic/claude-opus-5", "").c_str());          // prefixed
  TEST_ASSERT_EQUAL_STRING("", key("cumulo", "mistral/mistral/zai-glm-5-3", "").c_str());   // nested
  // A bare served echo of a substitution infers its upstream.
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-4-5",
                           key("cumulo", "openai/gpt-5.6", "claude-haiku-4-5-20251001").c_str());
  TEST_ASSERT_EQUAL_STRING("zai/glm-5.3", key("cumulo", "gpt-5.6", "glm-5.3").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral/pixtral-12b", key("cumulo", "gpt-5.6", "pixtral-12b-2409").c_str());
  // A prefixed served echo keeps its own upstream.
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5",
                           key("cumulo", "openai/gpt-5.6", "anthropic/claude-opus-5").c_str());
  // The upstream's echo of the configured model (an alias) files under the configured key.
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-4-5",
                           key("cumulo", "anthropic/claude-haiku-4-5", "claude-haiku-4-5-20251001").c_str());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", key("cumulo", "openai/gpt-5.6", "gpt-5.6-2026-07-09").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", inferUpstream("mistral-small"));
  TEST_ASSERT_EQUAL_STRING("mistral", inferUpstream("codestral"));
  TEST_ASSERT_EQUAL_STRING("mistral", inferUpstream("ministral-8b"));
  TEST_ASSERT_EQUAL_STRING("openai", inferUpstream("o3"));
}

static void test_alias_echo_vs_substitution() {
  // Alias echo: same model after normalizing -> the configured key.
  TEST_ASSERT_EQUAL_STRING("gpt-5.5", key("openai", "gpt-5.5", "gpt-5.5-2026-01").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-large",
                           key("mistral", "mistral-large-latest", "mistral-large-2411").c_str());
  // A real substitution -> the served key (the coarse family would merge these).
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", key("openai", "gpt-5.6", "gpt-5.6-luna").c_str());
  // No echo -> configured; no configured model -> the echo.
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", key("openai", "gpt-5.6", "").c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", key("openai", "", "gpt-5.6").c_str());
  TEST_ASSERT_EQUAL_STRING("", key("openai", "", "").c_str());
  TEST_ASSERT_EQUAL_STRING("", key("", "gpt-5.6", "").c_str());
}

static void test_charset_rules_and_other_reserved() {
  TEST_ASSERT_TRUE(modelKeySafe("openai/gpt-5.6"));
  TEST_ASSERT_TRUE(modelKeySafe("ft:gpt-4o:org::abc+1_x"));
  TEST_ASSERT_FALSE(modelKeySafe(""));
  TEST_ASSERT_FALSE(modelKeySafe(kOtherModel));        // reserved, never from input
  TEST_ASSERT_FALSE(modelKeySafe("a|b"));
  TEST_ASSERT_FALSE(modelKeySafe("a;b"));
  TEST_ASSERT_FALSE(modelKeySafe("a b"));
  TEST_ASSERT_FALSE(modelKeySafe("a\"b"));
  TEST_ASSERT_FALSE(modelKeySafe(std::string(48, 'a').c_str()));   // 47 is the max
  TEST_ASSERT_TRUE(modelKeySafe(std::string(47, 'a').c_str()));
  TEST_ASSERT_EQUAL_STRING("", key("openai", "a|b", "").c_str());
  TEST_ASSERT_EQUAL_STRING("", key("custom", "~other", "").c_str());
  TEST_ASSERT_EQUAL_STRING("", key("openai", std::string(60, 'g').c_str(), "").c_str());
  TEST_ASSERT_TRUE(provKeySafe("anthropic"));
  TEST_ASSERT_FALSE(provKeySafe("OpenAI"));
  TEST_ASSERT_FALSE(provKeySafe("a/b"));
  TEST_ASSERT_FALSE(provKeySafe("abcdefghijklmnop"));   // 16 chars
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_cold_start_id_is_idempotent_and_priced);
  RUN_TEST(test_cold_start_tiers_have_their_own_rows);
  RUN_TEST(test_dated_ids_map_to_their_alias);
  RUN_TEST(test_capability_and_three_digit_suffixes_are_kept);
  RUN_TEST(test_cumulo_keys);
  RUN_TEST(test_alias_echo_vs_substitution);
  RUN_TEST(test_charset_rules_and_other_reserved);
  return UNITY_END();
}
