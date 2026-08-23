#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/logring.h"

using core::LogRing;

void setUp() {}
void tearDown() {}

// Layer 1: an exactly-registered secret (the device's own key) is masked
// wherever it appears, even inside a provider error body echoed to the log.
static void test_registered_secret_masked() {
  std::vector<std::string> secrets = {"sk-abcdef0123456789", "6789:AAtelegramBotToken"};
  std::string body =
      "tts: mistral HTTP 401: {\"error\":\"invalid key sk-abcdef0123456789\"}";
  std::string out = LogRing::redact(body, secrets);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)out.find("sk-abcdef0123456789"));
  TEST_ASSERT_TRUE(out.find("***") != std::string::npos);
  // the bot token embedded in a poll-error URL path is masked too
  std::string tg = "multipart: https://api.telegram.org/bot6789:AAtelegramBotToken/sendMessage -> HTTP 400";
  std::string tgo = LogRing::redact(tg, secrets);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)tgo.find("AAtelegramBotToken"));
}

// A secret shorter than 4 chars is ignored (would mask far too much).
static void test_short_secret_ignored() {
  std::vector<std::string> secrets = {"ab"};
  std::string out = LogRing::redact("value ab here", secrets);
  TEST_ASSERT_EQUAL_STRING("value ab here", out.c_str());
}

// Layer 2: the Bearer heuristic masks an unregistered token in an Authorization
// header echoed by a provider error (the common leak the ring must not keep).
static void test_bearer_backstop() {
  std::vector<std::string> none;
  std::string out = LogRing::redact(
      "vision: HTTP 403: Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.secret.sig denied", none);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)out.find("eyJhbGciOiJIUzI1NiJ9.secret.sig"));
  TEST_ASSERT_TRUE(out.find("Bearer ***") != std::string::npos);
}

// Layer 2: key=value / "key":"value" for credential-ish keys, boundary-guarded.
static void test_key_value_backstop() {
  std::vector<std::string> none;
  std::string a = LogRing::redact("connect api_key=live_9f8e7d6c5b4a body", none);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)a.find("live_9f8e7d6c5b4a"));
  std::string b = LogRing::redact("{\"token\":\"t0p-s3cr3t-value\",\"ok\":false}", none);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)b.find("t0p-s3cr3t-value"));
  // Boundary guard: "monkey=" / "compass=" must NOT be masked (not key/pass).
  std::string c = LogRing::redact("monkey=banana compass=north", none);
  TEST_ASSERT_EQUAL_STRING("monkey=banana compass=north", c.c_str());
}

// Layer 2: URL-embedded user:pass@host credentials are masked.
static void test_url_creds_backstop() {
  std::vector<std::string> none;
  std::string out = LogRing::redact("fetch https://user:hunter2@example.com/x failed", none);
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)out.find("hunter2"));
}

// The ring itself redacts on push and bounds its size.
static void test_ring_push_redacts_and_bounds() {
  LogRing ring(2);
  ring.addSecret("sk-topsecretkey123");
  ring.push("first line api_key=leakme12345");
  ring.push("second sk-topsecretkey123 here");
  ring.push("third line");   // evicts the first (cap 2)
  auto lines = ring.lines();
  TEST_ASSERT_EQUAL_UINT(2, lines.size());
  TEST_ASSERT_EQUAL_INT(std::string::npos, (int)lines[0].find("topsecretkey"));
  for (const auto& l : lines) TEST_ASSERT_EQUAL_INT(std::string::npos, (int)l.find("leakme12345"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_registered_secret_masked);
  RUN_TEST(test_short_secret_ignored);
  RUN_TEST(test_bearer_backstop);
  RUN_TEST(test_key_value_backstop);
  RUN_TEST(test_url_creds_backstop);
  RUN_TEST(test_ring_push_redacts_and_bounds);
  return UNITY_END();
}
