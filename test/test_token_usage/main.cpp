#include <unity.h>

#include <ArduinoJson.h>

#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/token_usage_json.h"

using nimbus::orch::TokenUsage;
using nimbus::orch::tokenUsageFromJson;

void setUp() {}
void tearDown() {}

// --- struct arithmetic (the cross-round accumulator) -----------------------

static void test_struct_arithmetic() {
  TokenUsage a;
  TEST_ASSERT_TRUE(a.empty());
  TEST_ASSERT_EQUAL_UINT32(0, a.total());

  a.add(100, 40);
  TEST_ASSERT_FALSE(a.empty());
  TEST_ASSERT_EQUAL_UINT32(100, a.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(40, a.completionTokens);
  TEST_ASSERT_EQUAL_UINT32(140, a.total());

  TokenUsage b;
  b.add(10, 5);
  a += b;  // accumulate a round
  TEST_ASSERT_EQUAL_UINT32(110, a.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(45, a.completionTokens);
  TEST_ASSERT_EQUAL_UINT32(155, a.total());
}

// --- provider usage-object parsing (fixtures) ------------------------------

static TokenUsage parse(const char* json) {
  JsonDocument d;
  deserializeJson(d, json);
  return tokenUsageFromJson(d["usage"].as<JsonObjectConst>());
}

static void test_parse_anthropic() {  // Messages API: input_tokens / output_tokens
  TokenUsage t = parse(R"({"usage":{"input_tokens":1234,"output_tokens":567}})");
  TEST_ASSERT_EQUAL_UINT32(1234, t.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(567, t.completionTokens);
  TEST_ASSERT_EQUAL_UINT32(1801, t.total());
}

static void test_parse_openai_mistral() {  // Responses/Conversations: prompt_/completion_tokens
  TokenUsage t = parse(R"({"usage":{"prompt_tokens":2000,"completion_tokens":300,"total_tokens":2300}})");
  TEST_ASSERT_EQUAL_UINT32(2000, t.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(300, t.completionTokens);
}

static void test_parse_missing_usage() {  // filtered/absent usage -> empty, not a crash
  TokenUsage t = parse(R"({"content":[]})");
  TEST_ASSERT_TRUE(t.empty());
}

// --- the real shape: a multi-round tool loop summing each round ------------

static void test_sum_across_rounds() {
  TokenUsage total;
  total += parse(R"({"usage":{"input_tokens":500,"output_tokens":50}})");
  total += parse(R"({"usage":{"input_tokens":800,"output_tokens":120}})");
  total += parse(R"({"usage":{"prompt_tokens":300,"completion_tokens":30}})");  // mixed convention
  TEST_ASSERT_EQUAL_UINT32(1600, total.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(200, total.completionTokens);
  TEST_ASSERT_EQUAL_UINT32(1800, total.total());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_struct_arithmetic);
  RUN_TEST(test_parse_anthropic);
  RUN_TEST(test_parse_openai_mistral);
  RUN_TEST(test_parse_missing_usage);
  RUN_TEST(test_sum_across_rounds);
  return UNITY_END();
}
