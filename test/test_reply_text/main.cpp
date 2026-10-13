#include <ArduinoJson.h>
#include <unity.h>

#include <string>

#include "nimbus/orch/reply_text.h"

// reply_text: the answer text of a provider reply in every content shape the
// providers send. The Mistral case is the shape the Cumulo router returned for
// mistral-large-4-0 on the bench (13 Oct 2026); the old string-only readers read ""
// from it and reported a helper that had run as unreadable.

using nimbus::orch::chatContentText;
using nimbus::orch::messagesContentText;

void setUp() {}
void tearDown() {}

static std::string chat(const char* json) {
  JsonDocument d;
  deserializeJson(d, json);
  return chatContentText(d["choices"][0]["message"]["content"]);
}
static std::string messages(const char* json) {
  JsonDocument d;
  deserializeJson(d, json);
  return messagesContentText(d["content"]);
}

static void test_chat_plain_string() {
  TEST_ASSERT_EQUAL_STRING("hello", chat("{\"choices\":[{\"message\":{\"content\":\"hello\"}}]}").c_str());
}

static void test_chat_mistral_thinking_then_text() {
  const char* j =
      "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":null,\"content\":["
      "{\"type\":\"thinking\",\"thinking\":[{\"type\":\"text\",\"text\":\"Let me count the words\"}]},"
      "{\"type\":\"text\",\"text\":\"The Duty sets higher standards \"},"
      "{\"type\":\"text\",\"text\":\"for retail customers.\"}]}}]}";
  TEST_ASSERT_EQUAL_STRING("The Duty sets higher standards for retail customers.", chat(j).c_str());
}

static void test_chat_no_text_chunk_is_empty() {
  TEST_ASSERT_EQUAL_STRING("", chat("{\"choices\":[{\"message\":{\"content\":[{\"type\":\"thinking\",\"thinking\":[]}]}}]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", chat("{\"choices\":[{\"message\":{\"content\":null}}]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", chat("{\"choices\":[]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", chat("{\"choices\":[{\"message\":{\"content\":42}}]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", chat("{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\",\"text\":7}]}}]}").c_str());
}

static void test_messages_thinking_block_first() {
  const char* j = "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"plan\",\"signature\":\"x\"},"
                  "{\"type\":\"text\",\"text\":\"Answer.\"}]}";
  TEST_ASSERT_EQUAL_STRING("Answer.", messages(j).c_str());
}

static void test_messages_text_blocks_joined_tool_use_skipped() {
  const char* j = "{\"content\":[{\"type\":\"text\",\"text\":\"First.\"},"
                  "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"x\",\"input\":{}},"
                  "{\"type\":\"text\",\"text\":\"Second.\"}]}";
  TEST_ASSERT_EQUAL_STRING("First.\nSecond.", messages(j).c_str());
}

static void test_messages_empty_shapes() {
  TEST_ASSERT_EQUAL_STRING("", messages("{\"content\":[]}").c_str());
  TEST_ASSERT_EQUAL_STRING("", messages("{}").c_str());
  TEST_ASSERT_EQUAL_STRING("plain", messages("{\"content\":\"plain\"}").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_chat_plain_string);
  RUN_TEST(test_chat_mistral_thinking_then_text);
  RUN_TEST(test_chat_no_text_chunk_is_empty);
  RUN_TEST(test_messages_thinking_block_first);
  RUN_TEST(test_messages_text_blocks_joined_tool_use_skipped);
  RUN_TEST(test_messages_empty_shapes);
  return UNITY_END();
}
