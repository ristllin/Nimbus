// test_tg_send_feedback - a file queued for Telegram never ends in silence. files.send
// answers "queued <project>/<name> for Telegram delivery" and the upload runs later on
// the poll task (drainMedia); when that upload fails, the chat is told once that the
// file could not be sent and is still under Files. The upload is device code, so the
// guard reads src/agent/telegram.cpp itself: the failure branch sits after the
// upload, covers stored (SD) files, goes through the archived direct send, and names
// the file. Counter-cases prove the reader sees a missing or misplaced notice.
#include <unity.h>

#include <cstdio>
#include <string>

void setUp() {}
void tearDown() {}

static std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The body of the function whose definition starts with `sig` (brace matched).
static std::string bodyOf(const std::string& src, const char* sig) {
  const size_t at = src.find(sig);
  if (at == std::string::npos) return "";
  size_t open = src.find('{', at);
  int depth = 0;
  for (size_t i = open; i < src.size(); i++) {
    if (src[i] == '{') depth++;
    if (src[i] == '}' && --depth == 0) return src.substr(open, i - open + 1);
  }
  return "";
}

// Empty when the drain body tells the owner about a failed stored-file upload; else why not.
static std::string feedbackProblem(const std::string& body) {
  const size_t post = body.find("httpmp::post(");
  if (post == std::string::npos) return "no upload in the drain";
  const size_t fail = body.find("if (!ok && m.sd) sendNow(m.chatId,", post);
  if (fail == std::string::npos) return "no notice after a failed stored-file upload";
  const size_t eol = body.find('\n', fail);
  const std::string line = body.substr(fail, eol - fail);
  if (line.find("base") == std::string::npos) return "the notice does not name the file";
  if (line.find("saved under Files") == std::string::npos) return "the notice does not say where the file is";
  return "";
}

static void test_failed_upload_tells_the_owner() {
  const std::string src = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE_MESSAGE(src.size() > 10000, "cannot read src/agent/telegram.cpp (run from repo root)");
  const std::string drain = bodyOf(src, "static void drainMedia() {");
  TEST_ASSERT_TRUE_MESSAGE(!drain.empty(), "drainMedia not found");
  const std::string why = feedbackProblem(drain);
  TEST_ASSERT_TRUE_MESSAGE(why.empty(), why.c_str());
  // sendNow is the archived direct send (its own guard lives in test_tg_hooks).
  TEST_ASSERT_TRUE(bodyOf(src, "static bool sendNow(const char* chatId, const char* text) {")
                       .find("agent::tgarchive::onOutboundText(") != std::string::npos);
}

static void test_the_reader_sees_a_missing_or_misplaced_notice() {
  TEST_ASSERT_FALSE(feedbackProblem("{\n  bool ok = httpmp::post(a);\n}\n").empty());
  TEST_ASSERT_FALSE(feedbackProblem("{\n  if (!ok && m.sd) sendNow(m.chatId, \"x\");\n"
                                    "  bool ok = httpmp::post(a);\n}\n").empty());
  TEST_ASSERT_FALSE(feedbackProblem("{\n  bool ok = httpmp::post(a);\n"
                                    "  if (!ok && m.sd) sendNow(m.chatId, \"Couldn't send it.\");\n}\n").empty());
  TEST_ASSERT_TRUE(feedbackProblem("{\n  bool ok = httpmp::post(a);\n  if (!ok && m.sd) sendNow(m.chatId, "
                                   "(String(\"Couldn't send \") + base + \". It's saved under Files.\").c_str());\n}\n")
                       .empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_failed_upload_tells_the_owner);
  RUN_TEST(test_the_reader_sees_a_missing_or_misplaced_notice);
  return UNITY_END();
}
