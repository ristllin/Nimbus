#include <unity.h>

#include <ArduinoJson.h>

#include "nimbus/orch/servedby.h"
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/token_usage_json.h"

using nimbus::orch::captureServedModel;
using nimbus::orch::servedByDisclosure;
using nimbus::orch::ServedBy;
using nimbus::orch::TokenUsage;
using nimbus::orch::turnChipDisclosure;
using nimbus::orch::TurnChipDisclosure;

void setUp() {}
void tearDown() {}

// ---- the disclosure class rule (CUM-236) ----------------------------------
// A working direct call MUST NOT disclose; any provider substitution MUST. A
// provider echoing a more specific model id when nothing was requested is NOT a
// swap (mirrors the cloud router rule, TF-C9).

static void test_no_fallback_same_host_same_model() {
  ServedBy s = servedByDisclosure("openai", "gpt-5.5", "openai", "gpt-5.5");
  TEST_ASSERT_FALSE(s.fallback);
  TEST_ASSERT_TRUE(s.text.empty());
}

static void test_no_fallback_bare_model_echo() {
  // No model was requested (empty), the provider echoed a specific id - not a swap.
  ServedBy s = servedByDisclosure("openai", "", "openai", "gpt-5.5-2026");
  TEST_ASSERT_FALSE(s.fallback);
}

static void test_fallback_on_provider_change() {
  ServedBy s = servedByDisclosure("openai", "gpt-5.5", "mistral", "mistral-large-latest");
  TEST_ASSERT_TRUE(s.fallback);
  TEST_ASSERT_EQUAL_STRING("served by mistral mistral-large-latest", s.text.c_str());
}

static void test_fallback_on_model_change_same_provider() {
  // A model WAS requested and the served one is a DIFFERENT family -> disclose.
  ServedBy s = servedByDisclosure("anthropic", "claude-opus-4-8", "anthropic", "claude-haiku-4-5");
  TEST_ASSERT_TRUE(s.fallback);
  TEST_ASSERT_EQUAL_STRING("served by anthropic claude-haiku-4-5", s.text.c_str());
}

// The class the raw-id compare missed: an alias resolving to a dated snapshot of the
// SAME model is NOT a fallback (it fired "served by ... (fallback)" on every ordinary
// turn on the flagship OpenAI/Mistral default paths).
static void test_no_fallback_alias_resolves_to_dated_snapshot() {
  TEST_ASSERT_FALSE(
      servedByDisclosure("openai", "gpt-5.5", "openai", "gpt-5.5-2026-01").fallback);
  TEST_ASSERT_FALSE(
      servedByDisclosure("mistral", "mistral-large-latest", "mistral", "mistral-large-2411")
          .fallback);
  TEST_ASSERT_FALSE(
      servedByDisclosure("openai", "gpt-4o", "openai", "gpt-4o-2024-08-06").fallback);
}

static void test_fallback_text_omits_model_when_unknown() {
  ServedBy s = servedByDisclosure("openai", "gpt-5.5", "mistral", "");
  TEST_ASSERT_TRUE(s.fallback);
  TEST_ASSERT_EQUAL_STRING("served by mistral", s.text.c_str());
}

// ---- served-model capture from BOTH provider response shapes --------------

static void test_capture_openai_compat_shape() {
  // OpenAI / Mistral / router (OpenAI-compatible) chat completion: top-level "model".
  const char* body =
      R"({"id":"cmpl-1","model":"gpt-5.5-2026-01","choices":[{"message":{"content":"hi"}}],)"
      R"("usage":{"prompt_tokens":10,"completion_tokens":3}})";
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, body) == ArduinoJson::DeserializationError::Ok);
  TokenUsage u;
  captureServedModel(&u, doc.as<ArduinoJson::JsonVariantConst>());
  TEST_ASSERT_EQUAL_STRING("gpt-5.5-2026-01", u.servedModel.c_str());
}

static void test_capture_anthropic_shape() {
  // Anthropic Messages: top-level "model" alongside content[].
  const char* body =
      R"({"id":"msg_1","type":"message","model":"claude-sonnet-4-6",)"
      R"("content":[{"type":"text","text":"hi"}],)"
      R"("usage":{"input_tokens":10,"output_tokens":3}})";
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, body) == ArduinoJson::DeserializationError::Ok);
  TokenUsage u;
  captureServedModel(&u, doc.as<ArduinoJson::JsonVariantConst>());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-4-6", u.servedModel.c_str());
}

static void test_capture_absent_model_is_noop() {
  const char* body = R"({"id":"x","usage":{"prompt_tokens":1,"completion_tokens":1}})";
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, body) == ArduinoJson::DeserializationError::Ok);
  TokenUsage u;
  u.servedModel = "prev";
  captureServedModel(&u, doc.as<ArduinoJson::JsonVariantConst>());
  TEST_ASSERT_EQUAL_STRING("prev", u.servedModel.c_str());  // unchanged when absent
}

static void test_capture_null_target_is_safe() {
  JsonDocument doc;
  deserializeJson(doc, R"({"model":"m"})");
  captureServedModel(nullptr, doc.as<ArduinoJson::JsonVariantConst>());  // must not crash
  TEST_PASS();
}

// ---- the merge rule: last non-empty served model wins across rounds --------
static void test_usage_merge_keeps_last_served_model() {
  TokenUsage a;
  a.servedModel = "first";
  TokenUsage b;
  b.servedModel = "second";
  a += b;
  TEST_ASSERT_EQUAL_STRING("second", a.servedModel.c_str());
  TokenUsage c;  // empty served model must NOT clobber a set one
  a += c;
  TEST_ASSERT_EQUAL_STRING("second", a.servedModel.c_str());
}

// ---- the DEVICE turn-chip leg (CUM-236 device display) --------------------
// The device writes one `ev:turnend` row per turn; its turn view (_turnChip)
// annotates a substituted turn "served by <host> <model> (fallback)". The row must
// carry fallback=true AND the model that actually answered when the turn substituted,
// and must NOT flag a plain direct turn. This is the seam the on-glass render reads;
// the pixels themselves are the bench leg.

static void test_chip_plain_turn_no_fallback_shows_configured_model() {
  // Direct openai turn, provider echoed the resolved snapshot of the SAME model.
  TurnChipDisclosure d = turnChipDisclosure("openai", "gpt-5.5", "openai",
                                            "gpt-5.5-2026-01", "gpt-5.5");
  TEST_ASSERT_FALSE(d.fallback);                     // no "(fallback)" on the chip
  TEST_ASSERT_EQUAL_STRING("gpt-5.5", d.model.c_str());  // the head's own label
}

static void test_chip_provider_fallback_discloses_served_model() {
  // Requested openai, failed over to mistral: chip must say so and name mistral's model.
  TurnChipDisclosure d = turnChipDisclosure("openai", "gpt-5.5", "mistral",
                                            "mistral-large-latest", "mistral-large-latest");
  TEST_ASSERT_TRUE(d.fallback);
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", d.model.c_str());
}

static void test_chip_model_fallback_same_provider_shows_served_model() {
  // Same host, a DIFFERENT family answered - show the model that actually replied,
  // not the head's configured one.
  TurnChipDisclosure d = turnChipDisclosure("anthropic", "claude-opus-4-8", "anthropic",
                                            "claude-haiku-4-5", "claude-opus-4-8");
  TEST_ASSERT_TRUE(d.fallback);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", d.model.c_str());
}

static void test_chip_fallback_unknown_served_model_falls_back_to_configured() {
  // A provider swap with no echoed model still discloses; the chip labels it with the
  // served head's configured model rather than an empty string.
  TurnChipDisclosure d = turnChipDisclosure("openai", "gpt-5.5", "mistral", "",
                                            "mistral-large-latest");
  TEST_ASSERT_TRUE(d.fallback);
  TEST_ASSERT_EQUAL_STRING("mistral-large-latest", d.model.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_fallback_same_host_same_model);
  RUN_TEST(test_no_fallback_bare_model_echo);
  RUN_TEST(test_fallback_on_provider_change);
  RUN_TEST(test_fallback_on_model_change_same_provider);
  RUN_TEST(test_no_fallback_alias_resolves_to_dated_snapshot);
  RUN_TEST(test_fallback_text_omits_model_when_unknown);
  RUN_TEST(test_capture_openai_compat_shape);
  RUN_TEST(test_capture_anthropic_shape);
  RUN_TEST(test_capture_absent_model_is_noop);
  RUN_TEST(test_capture_null_target_is_safe);
  RUN_TEST(test_usage_merge_keeps_last_served_model);
  RUN_TEST(test_chip_plain_turn_no_fallback_shows_configured_model);
  RUN_TEST(test_chip_provider_fallback_discloses_served_model);
  RUN_TEST(test_chip_model_fallback_same_provider_shows_served_model);
  RUN_TEST(test_chip_fallback_unknown_served_model_falls_back_to_configured);
  return UNITY_END();
}
