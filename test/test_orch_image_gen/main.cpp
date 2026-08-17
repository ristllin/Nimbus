#include <unity.h>

#include <string>

#include "nimbus/orch/image_gen.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- size normalization -----------------------------------------------------
static void test_size_default_and_passthrough() {
  // Blank/unknown collapse to the universally valid square, never a 400.
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("gpt-image-1", "").c_str());
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("gpt-image-1", "999x1").c_str());
  // gpt-image-1 landscape/portrait/auto pass through.
  TEST_ASSERT_EQUAL_STRING("1536x1024", normalizeImageSize("gpt-image-1", "1536x1024").c_str());
  TEST_ASSERT_EQUAL_STRING("1024x1536", normalizeImageSize("gpt-image-1", "1024x1536").c_str());
  TEST_ASSERT_EQUAL_STRING("auto", normalizeImageSize("gpt-image-1", "auto").c_str());
}

static void test_size_model_families_differ() {
  // 512x512 is valid for dall-e-2 but not gpt-image-1/dall-e-3.
  TEST_ASSERT_EQUAL_STRING("512x512", normalizeImageSize("dall-e-2", "512x512").c_str());
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("gpt-image-1", "512x512").c_str());
  // 1792x1024 is dall-e-3-only; gpt-image-1 and dall-e-2 collapse it.
  TEST_ASSERT_EQUAL_STRING("1792x1024", normalizeImageSize("dall-e-3", "1792x1024").c_str());
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("gpt-image-1", "1792x1024").c_str());
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("dall-e-2", "1792x1024").c_str());
  // 1536x1024 is gpt-image-1-only; dall-e-3 collapses it.
  TEST_ASSERT_EQUAL_STRING("1024x1024", normalizeImageSize("dall-e-3", "1536x1024").c_str());
}

// ---- request body -----------------------------------------------------------
static void test_body_defaults_to_gpt_image_1() {
  std::string b = imageGenRequestBody("a red cube", "", "");
  // gpt-image-1 returns base64 NATIVELY, so response_format must NOT be sent (the
  // current API rejects it with 400 "Unknown parameter: 'response_format'").
  TEST_ASSERT_EQUAL(std::string::npos, b.find("response_format"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"model\":\"gpt-image-1\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"size\":\"1024x1024\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"n\":1"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"prompt\":\"a red cube\""));
  // Default quality tier: low (fast/small/cheap for a device streaming to SD).
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"quality\":\"low\""));
  // An explicit quality overrides the default.
  std::string h = imageGenRequestBody("x", "", "", "high");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, h.find("\"quality\":\"high\""));
}

static void test_body_escapes_prompt_and_dalle_no_quality() {
  // A prompt with a quote/backslash must be valid JSON (ArduinoJson escapes it).
  std::string b = imageGenRequestBody("say \"hi\"\\n", "dall-e-2", "512x512");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\\\"hi\\\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"model\":\"dall-e-2\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, b.find("\"size\":\"512x512\""));
  // quality is a gpt-image-1-only field; dall-e-* must not carry it.
  TEST_ASSERT_EQUAL(std::string::npos, b.find("quality"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_size_default_and_passthrough);
  RUN_TEST(test_size_model_families_differ);
  RUN_TEST(test_body_defaults_to_gpt_image_1);
  RUN_TEST(test_body_escapes_prompt_and_dalle_no_quality);
  return UNITY_END();
}
