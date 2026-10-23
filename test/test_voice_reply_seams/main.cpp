// test_voice_reply_seams - the device seams of the voice reply fix, as a class over the
// sources (comments stripped), so a new path that brings back the 22 Oct failure fails
// here. On 22 Oct a reply.telegram voice:true call played on the speaker (Telegram was
// only its fallback), the speaker driver failed to start and nobody heard of it, the
// text-to-speech clip had been cut at a fixed 25 s window, the tool said "sent to
// Telegram" and held the memory card lock for 25 s. The rules:
//   - a Telegram voice request goes to Telegram and never to the speaker;
//   - every voice upload is synchronous, receipted, and its clip file is removed;
//   - the reply tools run outside the whole-call card lock;
//   - every speaker open is checked and its failure reported;
//   - a clip is used only when it arrived whole;
//   - reply.speak reports "spoken" only after the speaker opened;
//   - reply.telegram's voice result is the receipt (or the reason), never fixed text.
#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Drops // and /* */ comments outside string and character literals.
static std::string code(const char* path) {
  const std::string s = readFile(path);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const char n = i + 1 < s.size() ? s[i + 1] : 0;
    if (c == '"' || c == '\'') {
      out += c;
      for (++i; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        if (s[i] == c) break;
      }
    } else if (c == '/' && n == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      out += '\n';
    } else if (c == '/' && n == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) break;
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

static size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

// From `from` to the first `to` after it (the whole rest when `to` is absent).
static std::string between(const std::string& src, const std::string& from, const std::string& to) {
  const size_t s = src.find(from);
  TEST_ASSERT_TRUE_MESSAGE(s != std::string::npos, from.c_str());
  const size_t e = src.find(to, s + from.size());
  return src.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

static std::string bodyOf(const std::string& src, const std::string& sig) { return between(src, sig, "\n}\n"); }

static bool before(const std::string& s, const char* a, const char* b) {
  const size_t pa = s.find(a), pb = s.find(b);
  return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

static void test_a_telegram_voice_request_never_reaches_the_speaker() {
  const std::string orch = code("src/agent/orchestrator.cpp");
  const std::string send = bodyOf(orch, "bool sendToChat(const String& chatId, const String& text, bool asVoice,");
  TEST_ASSERT_TRUE(send.find("voicereply::deliver(") != std::string::npos);
  TEST_ASSERT_EQUAL_size_t(0, count(send, "g_sinks.speak"));
  TEST_ASSERT_EQUAL_size_t(0, count(send, "speakOnDevice("));
  const std::string voice = code("src/agent/voice_reply.cpp");
  for (const char* speaker : {"sfx::", "speakReply", "speakOnDevice", "spkOpen", "music::"})
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, count(voice, speaker), speaker);
}

static void test_every_voice_upload_is_receipted_and_cleaned_up() {
  const std::string voice = code("src/agent/voice_reply.cpp");
  TEST_ASSERT_EQUAL_size_t(0, count(voice, "sendMedia("));   // the queued send has no receipt
  const std::string note = bodyOf(voice, "bool sendNote(");
  TEST_ASSERT_EQUAL_size_t(1, count(note, "telegram::sendMediaNow("));
  // Removed after the upload whatever its outcome (a delivered clip was archived first).
  const size_t up = note.find("telegram::sendMediaNow(");
  TEST_ASSERT_TRUE(note.find("LittleFS.remove(path);", up) < note.find("if (!r.ok)", up));
  TEST_ASSERT_TRUE(before(note, "if (!clip) {", "telegram::sendMediaNow("));
  TEST_ASSERT_TRUE(between(note, "if (!clip) {", "}").find("LittleFS.remove(path);") != std::string::npos);
  TEST_ASSERT_TRUE(voice.find("planVoiceNotes(") != std::string::npos);
  const std::string tg = code("src/agent/telegram.cpp");
  const std::string upload = bodyOf(tg, "static nimbus::net::TgReceipt uploadMedia(");
  TEST_ASSERT_TRUE(before(upload, "httpmp::post(", "parseTgSendAnswer("));
  TEST_ASSERT_TRUE(before(upload, "parseTgSendAnswer(", "tgSendLogLine("));
}

static void test_the_reply_tools_run_outside_the_card_lock() {
  const std::string mem = code("src/agent/memory_subsystem.cpp");
  const std::string mcp = bodyOf(mem, "std::string handleMcp(const std::string& jsonRpcRequest,");
  TEST_ASSERT_TRUE(before(mcp, "if (locksItself(name)) return g_reg.handleRpc(jsonRpcRequest, who);", "Lock g;"));
  const std::string tools = bodyOf(mem, "static bool locksItself(const std::string& name) {");
  TEST_ASSERT_TRUE(tools.find("\"reply.telegram\"") != std::string::npos);
  TEST_ASSERT_TRUE(tools.find("\"reply.speak\"") != std::string::npos);
  // The history row they write takes the lock for itself.
  const std::string capture = bodyOf(mem, "static String captureMessageAt(");
  TEST_ASSERT_TRUE(capture.find("Lock g;") != std::string::npos);
}

static void test_every_speaker_open_is_checked_and_reported() {
  const std::string music = code("src/sfx/music.cpp");
  TEST_ASSERT_EQUAL_size_t(1, count(music, "solide::audio::spkOpen("));
  TEST_ASSERT_EQUAL_size_t(1, count(music, "if (solide::audio::spkOpen("));
  const std::string open = bodyOf(music, "static bool openSpeaker(uint32_t rate, OpenHook onOpen) {");
  TEST_ASSERT_TRUE(open.find("agent::alogf(") != std::string::npos);
  TEST_ASSERT_TRUE(open.find("onOpen(false") != std::string::npos);
  const std::string sfx = code("src/sfx/sound_fx.cpp");
  TEST_ASSERT_EQUAL_size_t(0, count(sfx, "playWavFile(LittleFS"));   // replies stream through music
  const std::string play = bodyOf(sfx, "void playReply(const Item& it) {");
  TEST_ASSERT_TRUE(play.find("replyOpened") != std::string::npos);
}

static void test_a_clip_is_used_only_when_whole() {
  const std::string tts = code("src/agent/adapters/audio_tts.cpp");
  TEST_ASSERT_TRUE(tts.find("&clean)") != std::string::npos);   // b64decodeToFile's clean flag
  const std::string stream = bodyOf(tts, "TtsResult requestAndStream(");
  TEST_ASSERT_TRUE(before(stream, "if (!nimbus::orch::clipWhole(rd)) {", "wrote %u bytes"));
  const std::string cut = between(stream, "if (!nimbus::orch::clipWhole(rd)) {", "return res;");
  TEST_ASSERT_TRUE(cut.find("LittleFS.remove(outPath);") != std::string::npos);
  TEST_ASSERT_TRUE(cut.find("res.bytes") == std::string::npos);   // a cut clip returns no bytes
  TEST_ASSERT_EQUAL_size_t(1, count(stream, "res.bytes = rd.bytes;"));
  TEST_ASSERT_TRUE(before(stream, "if (!nimbus::orch::clipWhole(rd)) {", "res.bytes = rd.bytes;"));
  TEST_ASSERT_TRUE(tts.find("ttsDeadlineMs(") != std::string::npos);
  TEST_ASSERT_EQUAL_size_t(0, count(tts, "millis() + 25000"));
}

static void test_spoken_only_after_the_speaker_opened() {
  const std::string orch = code("src/agent/orchestrator.cpp");
  const std::string speak = bodyOf(orch, "bool speakOnDevice(const String& text, bool capture, String* why) {");
  TEST_ASSERT_TRUE(before(speak, "::sfx::speakReply(", "::sfx::awaitReplyStart("));
  TEST_ASSERT_TRUE(before(speak, "::sfx::awaitReplyStart(", "return true;"));
}

static void test_the_voice_result_is_the_receipt() {
  const std::string mem = code("src/agent/memory_subsystem.cpp");
  const std::string tool = between(mem, "g_reg.add(\"reply.telegram\"", "g_reg.add(");
  TEST_ASSERT_EQUAL_size_t(0, count(tool, "\"sent to Telegram\""));
  TEST_ASSERT_TRUE(tool.find("if (voice) return nimbus::orch::ToolResult::ok(result);") != std::string::npos);
  TEST_ASSERT_TRUE(tool.find("sendToChat(") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_telegram_voice_request_never_reaches_the_speaker);
  RUN_TEST(test_every_voice_upload_is_receipted_and_cleaned_up);
  RUN_TEST(test_the_reply_tools_run_outside_the_card_lock);
  RUN_TEST(test_every_speaker_open_is_checked_and_reported);
  RUN_TEST(test_a_clip_is_used_only_when_whole);
  RUN_TEST(test_spoken_only_after_the_speaker_opened);
  RUN_TEST(test_the_voice_result_is_the_receipt);
  return UNITY_END();
}
