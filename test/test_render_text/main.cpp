#include <unity.h>

#include "nimbus/render_text.h"

// UTF-8 -> printable-ASCII transliteration (so LLM smart-quotes/dashes/emoji stop
// rendering as '?'). Bytes are the UTF-8 encodings of the code points named.
// The shared pager (text_page) applies this before wrapping, so on-screen text
// stays clean regardless of what a model emits.
static void test_ascii_sanitize() {
  using nimbus::render::asciiSanitize;
  TEST_ASSERT_EQUAL_STRING("don't",  asciiSanitize("don\xE2\x80\x99t").c_str());     // U+2019 '
  TEST_ASSERT_EQUAL_STRING("a-b",    asciiSanitize("a\xE2\x80\x94""b").c_str());     // U+2014 em-dash
  TEST_ASSERT_EQUAL_STRING("hi...",  asciiSanitize("hi\xE2\x80\xA6").c_str());       // U+2026 ellipsis
  TEST_ASSERT_EQUAL_STRING("\"q\"", asciiSanitize("\xE2\x80\x9Cq\xE2\x80\x9D").c_str()); // U+201C/D
  TEST_ASSERT_EQUAL_STRING("cafe",   asciiSanitize("caf\xC3\xA9").c_str());          // U+00E9 e-acute
  TEST_ASSERT_EQUAL_STRING("naive",  asciiSanitize("na\xC3\xAFve").c_str());         // U+00EF i-umlaut
  TEST_ASSERT_EQUAL_STRING("ok ",    asciiSanitize("ok \xF0\x9F\x98\x80").c_str());  // U+1F600 emoji dropped
  TEST_ASSERT_EQUAL_STRING("Hello, world!", asciiSanitize("Hello, world!").c_str()); // ASCII untouched
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_sanitize);
  return UNITY_END();
}
