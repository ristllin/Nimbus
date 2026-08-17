#include <unity.h>

#include <string>

#include "nimbus/tg_updates.h"

using namespace nimbus::tg;

void setUp() {}
void tearDown() {}

static bool parse(const std::string& body, std::vector<Update>& out) {
  bool ok = false, trunc = false;
  bool parsed = parseUpdates(body.data(), body.size(), out, ok, trunc);
  return parsed && ok;
}

// A long message (well past the old 255-char cap) survives intact.
static void test_long_message_not_truncated() {
  std::string big(1200, 'x');
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":42,\"message\":{"
      "\"chat\":{\"id\":123456789},\"from\":{\"first_name\":\"Alex\"},"
      "\"text\":\"" + big + "\"}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT(1, (int)u.size());
  TEST_ASSERT_EQUAL_INT(1200, (int)u[0].text.size());   // full length, not 255
  TEST_ASSERT_EQUAL_STRING("123456789", u[0].chatId.c_str());
  TEST_ASSERT_EQUAL_STRING("Alex", u[0].from.c_str());
  TEST_ASSERT_EQUAL_INT(42, u[0].updateId);
}

// JSON escapes decode correctly: \n -> newline (was the letter 'n'), \" -> quote,
// \uXXXX -> UTF-8, surrogate pairs -> the astral codepoint.
static void test_escapes_decode() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":1,\"message\":{"
      "\"chat\":{\"id\":10},\"text\":\"line1\\nline2 \\\"q\\\" \\u00e9 \\ud83d\\ude80\"}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  const std::string& t = u[0].text;
  TEST_ASSERT_TRUE(t.find("line1\nline2") != std::string::npos);   // real newline
  TEST_ASSERT_TRUE(t.find('n') == std::string::npos ||             // no bare 'n' from \n
                   t.find("line1\nline2") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("\"q\"") != std::string::npos);          // unescaped quotes
  TEST_ASSERT_TRUE(t.find("\xc3\xa9") != std::string::npos);       // é as UTF-8
  TEST_ASSERT_TRUE(t.find("\xf0\x9f\x9a\x80") != std::string::npos); // 🚀 surrogate pair
}

// Multi-update batch: all parsed, in order.
static void test_multi_update_batch() {
  std::string body =
      "{\"ok\":true,\"result\":["
      "{\"update_id\":5,\"message\":{\"chat\":{\"id\":1},\"text\":\"a\"}},"
      "{\"update_id\":6,\"message\":{\"chat\":{\"id\":2},\"text\":\"b\"}},"
      "{\"update_id\":7,\"message\":{\"chat\":{\"id\":1},\"text\":\"c\"}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT(3, (int)u.size());
  TEST_ASSERT_EQUAL_INT(5, u[0].updateId);
  TEST_ASSERT_EQUAL_INT(7, u[2].updateId);
  TEST_ASSERT_EQUAL_STRING("c", u[2].text.c_str());
}

// Negative chat id (group) renders exactly.
static void test_negative_group_chat_id() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":9,\"message\":{"
      "\"chat\":{\"id\":-1001234567890},\"text\":\"hi\"}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("-1001234567890", u[0].chatId.c_str());
}

// Voice note -> attachment with file_id; text empty.
static void test_voice_attachment() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":3,\"message\":{"
      "\"chat\":{\"id\":10},\"voice\":{\"file_id\":\"VOICE123\",\"file_size\":4096}}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT((int)Attachment::Kind::Voice, (int)u[0].attachment.kind);
  TEST_ASSERT_EQUAL_STRING("VOICE123", u[0].attachment.fileId.c_str());
  TEST_ASSERT_EQUAL_UINT32(4096, u[0].attachment.fileSize);
}

// Document with caption -> attachment metadata + caption text flagged.
static void test_document_with_caption() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":4,\"message\":{"
      "\"chat\":{\"id\":10},\"caption\":\"here is the file\","
      "\"document\":{\"file_id\":\"DOC9\",\"file_name\":\"report.pdf\","
      "\"mime_type\":\"application/pdf\",\"file_size\":81234}}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT((int)Attachment::Kind::Document, (int)u[0].attachment.kind);
  TEST_ASSERT_EQUAL_STRING("DOC9", u[0].attachment.fileId.c_str());
  TEST_ASSERT_EQUAL_STRING("report.pdf", u[0].attachment.fileName.c_str());
  TEST_ASSERT_EQUAL_STRING("application/pdf", u[0].attachment.mime.c_str());
  TEST_ASSERT_EQUAL_UINT32(81234, u[0].attachment.fileSize);
  TEST_ASSERT_EQUAL_STRING("here is the file", u[0].text.c_str());
  TEST_ASSERT_TRUE(u[0].textIsCaption);
}

// Photo array -> the LARGEST rendition is chosen.
static void test_photo_picks_largest_within_budget() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":["
      "{\"file_id\":\"small\",\"file_size\":100},"
      "{\"file_id\":\"big\",\"file_size\":9000}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT((int)Attachment::Kind::Photo, (int)u[0].attachment.kind);
  TEST_ASSERT_EQUAL_STRING("big", u[0].attachment.fileId.c_str());
}

// A phone photo's full rendition is megabytes - download, base64-in-PSRAM and
// vision tokens all scale with it, for a description a mid-size copy answers
// just as well. So the pick is "largest WITHIN budget", not "largest".
static void test_photo_skips_renditions_over_budget() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":["
      "{\"file_id\":\"thumb\",\"file_size\":900},"
      "{\"file_id\":\"mid\",\"file_size\":120000},"
      "{\"file_id\":\"huge\",\"file_size\":4000000}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("mid", u[0].attachment.fileId.c_str());
  TEST_ASSERT_EQUAL_UINT32(120000, u[0].attachment.fileSize);
}

// Every rendition over budget: take the SMALLEST rather than refuse the photo -
// a legible description beats "I could not look at that".
static void test_photo_all_over_budget_takes_smallest() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":["
      "{\"file_id\":\"big\",\"file_size\":2000000},"
      "{\"file_id\":\"bigger\",\"file_size\":5000000}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("big", u[0].attachment.fileId.c_str());
}

// Telegram may omit file_size. A sizeless rendition must still be usable.
static void test_photo_without_sizes_still_selected() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":[{\"file_id\":\"nosize\"}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_INT((int)Attachment::Kind::Photo, (int)u[0].attachment.kind);
  TEST_ASSERT_EQUAL_STRING("nosize", u[0].attachment.fileId.c_str());
}

// With NO size on any rendition there is nothing to compare, so take the LAST -
// Telegram lists them ascending, so that is the full-size one. An earlier
// version fell back to "smallest", which pinned every sizeless photo to the
// ~90px thumbnail: technically a photo, legible to nobody.
static void test_photo_all_sizeless_takes_the_last_not_the_thumbnail() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":["
      "{\"file_id\":\"thumb\"},{\"file_id\":\"mid\"},{\"file_id\":\"full\"}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("full", u[0].attachment.fileId.c_str());
}

// A mix: only some renditions report a size. The sized ones drive the choice.
static void test_photo_mixed_sizes_prefers_a_reported_one_within_budget() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":8,\"message\":{"
      "\"chat\":{\"id\":10},\"photo\":["
      "{\"file_id\":\"thumb\",\"file_size\":800},"
      "{\"file_id\":\"nosize\"},"
      "{\"file_id\":\"mid\",\"file_size\":90000}]}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("mid", u[0].attachment.fileId.c_str());
}

// from falls back to username when first_name is absent.
static void test_from_username_fallback() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":2,\"message\":{"
      "\"chat\":{\"id\":10},\"from\":{\"username\":\"royd\"},\"text\":\"hi\"}}]}";
  std::vector<Update> u;
  TEST_ASSERT_TRUE(parse(body, u));
  TEST_ASSERT_EQUAL_STRING("royd", u[0].from.c_str());
}

// not-ok body and empty input never crash.
static void test_not_ok_and_empty() {
  std::vector<Update> u;
  bool ok = true, trunc = false;
  TEST_ASSERT_TRUE(parseUpdates("{\"ok\":false}", 12, u, ok, trunc));
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_INT(0, (int)u.size());
  ok = true;
  TEST_ASSERT_FALSE(parseUpdates("", 0, u, ok, trunc));
}

// A body cut mid-JSON flags truncatedTail (device re-polls narrow).
static void test_truncated_tail_flagged() {
  std::string body =
      "{\"ok\":true,\"result\":[{\"update_id\":5,\"message\":{\"chat\":{\"id\":1},\"text\":\"unterminat";
  std::vector<Update> u;
  bool ok = false, trunc = false;
  parseUpdates(body.data(), body.size(), u, ok, trunc);
  TEST_ASSERT_TRUE(trunc);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_long_message_not_truncated);
  RUN_TEST(test_escapes_decode);
  RUN_TEST(test_multi_update_batch);
  RUN_TEST(test_negative_group_chat_id);
  RUN_TEST(test_voice_attachment);
  RUN_TEST(test_document_with_caption);
  RUN_TEST(test_photo_picks_largest_within_budget);
  RUN_TEST(test_photo_skips_renditions_over_budget);
  RUN_TEST(test_photo_all_over_budget_takes_smallest);
  RUN_TEST(test_photo_without_sizes_still_selected);
  RUN_TEST(test_photo_all_sizeless_takes_the_last_not_the_thumbnail);
  RUN_TEST(test_photo_mixed_sizes_prefers_a_reported_one_within_budget);
  RUN_TEST(test_from_username_fallback);
  RUN_TEST(test_not_ok_and_empty);
  RUN_TEST(test_truncated_tail_flagged);
  return UNITY_END();
}
