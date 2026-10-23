// test_tg_receipt - Telegram's answer to a send, read into a delivery receipt
// (nimbus/net/tg_receipt.h). The class: only a 2xx answer that names a positive
// top-level result.message_id is a receipt; every other answer says why there is none.
#include <unity.h>

#include <string>

#include "nimbus/net/tg_receipt.h"

using nimbus::net::parseTgSendAnswer;
using nimbus::net::TgReceipt;
using nimbus::net::tgSendLogLine;

void setUp() {}
void tearDown() {}

static void test_an_accepted_audio_is_a_receipt() {
  const TgReceipt r = parseTgSendAnswer(
      true, "",
      R"({"ok":true,"result":{"message_id":812,"from":{"id":1,"is_bot":true},"chat":{"id":42},)"
      R"("date":1792400000,"audio":{"duration":31,"mime_type":"audio/mpeg","file_size":215433}}})");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_INT64(812, r.messageId);
  TEST_ASSERT_EQUAL_STRING("", r.error.c_str());
  const TgReceipt v = parseTgSendAnswer(true, "", R"({"ok":true,"result":{"message_id":9,"voice":{"duration":4}}})");
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_EQUAL_INT64(9, v.messageId);
  // Ids past 32 bits survive.
  const TgReceipt big = parseTgSendAnswer(true, "", R"({"ok":true,"result":{"message_id":5000000000}})");
  TEST_ASSERT_TRUE(big.ok);
  TEST_ASSERT_EQUAL_INT64(5000000000LL, big.messageId);
}

// Counter-tests: every answer without a usable top-level message id is not a receipt.
static void test_no_message_id_is_no_receipt() {
  struct Case { bool transport; const char* err; const char* body; const char* why; };
  const Case cases[] = {
      {true, "", R"({"ok":true,"result":{}})", "Telegram's answer had no message id"},
      {true, "", R"({"ok":true,"result":{"message_id":0}})", "Telegram's answer had no message id"},
      {true, "", R"({"ok":true,"result":{"message_id":-3}})", "Telegram's answer had no message id"},
      {true, "", R"({"ok":true,"result":{"message_id":"812"}})", "Telegram's answer had no message id"},
      // A nested id (the message it replied to) is not this message's id.
      {true, "", R"({"ok":true,"result":{"reply_to_message":{"message_id":5}}})",
       "Telegram's answer had no message id"},
      {true, "", R"({"ok":false,"result":{"message_id":7}})", "Telegram's answer had no message id"},
      {true, "", "", "Telegram's answer could not be read"},
      {true, "", "<html>bad gateway</html>", "Telegram's answer could not be read"},
      {true, "", R"({"ok":true,"result":{"message_id":8)", "Telegram's answer could not be read"},
      {false, "connect failed", "", "connect failed"},
      {false, "", "", "no answer"},
      // A failed transport is never a receipt, whatever the body says.
      {false, "HTTP 0", R"({"ok":true,"result":{"message_id":7}})", "HTTP 0"},
  };
  for (const Case& c : cases) {
    const TgReceipt r = parseTgSendAnswer(c.transport, c.err, c.body);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, c.body);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, r.messageId, c.body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.why, r.error.c_str(), c.body);
  }
}

static void test_telegrams_refusal_names_the_reason() {
  const TgReceipt r = parseTgSendAnswer(false, "HTTP 400",
                                        R"({"ok":false,"error_code":400,"description":"Bad Request: chat not found"})");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL_INT(400, r.errorCode);
  TEST_ASSERT_EQUAL_STRING("Bad Request: chat not found", r.error.c_str());
  // The reason is printable ASCII and bounded: it reaches the log ring and the model.
  std::string desc(300, 'x');
  desc[3] = '\n';
  desc[4] = '\x01';
  const TgReceipt longer = parseTgSendAnswer(false, "HTTP 413", "{\"ok\":false,\"description\":\"" +
                                                                     std::string("a\\nb") + desc + "\"}");
  TEST_ASSERT_TRUE(longer.error.size() <= 120);
  for (char c : longer.error) TEST_ASSERT_TRUE(c >= 0x20 && c < 0x7F);
}

static void test_the_log_line() {
  TgReceipt ok;
  ok.ok = true;
  ok.messageId = 812;
  TEST_ASSERT_EQUAL_STRING("telegram: sendAudio ok (message_id 812, 215433 bytes)",
                           tgSendLogLine("sendAudio", ok, 215433).c_str());
  TgReceipt bad;
  bad.error = "Bad Request: chat not found";
  TEST_ASSERT_EQUAL_STRING("telegram: sendAudio FAIL (Bad Request: chat not found)",
                           tgSendLogLine("sendAudio", bad, 215433).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_accepted_audio_is_a_receipt);
  RUN_TEST(test_no_message_id_is_no_receipt);
  RUN_TEST(test_telegrams_refusal_names_the_reason);
  RUN_TEST(test_the_log_line);
  return UNITY_END();
}
