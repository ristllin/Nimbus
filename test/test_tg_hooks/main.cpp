#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

// The Telegram observer seams (src/agent/telegram.h: setInboundObserver,
// setOutboundObserver) that the on-device Telegram archive attaches to. The archive
// must see EVERY inbound update and EVERY outbound send, so these pin the device
// wiring as a class over src/agent/telegram.cpp (comments stripped): a new receive or
// send path that bypasses the seam fails here.
//   - inbound: handleUpdate calls observeInbound first, before the replay guard and
//     before any classification (voice, file, text);
//   - outbound: every wire send of a text goes through send() (observed before it is
//     split) or sendNow() (observed), the reply-queue drain only delivers what send()
//     already observed, and every media upload is observed just before it.

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

// Copies the string or character literal that opens at s[i] into out; returns the
// index of its closing quote.
static size_t copyLiteral(const std::string& s, size_t i, std::string& out) {
  const char q = s[i];
  out += q;
  for (++i; i < s.size(); ++i) {
    out += s[i];
    if (s[i] == '\\' && i + 1 < s.size()) {
      out += s[++i];
      continue;
    }
    if (s[i] == q) break;
  }
  return i;
}

// Drops // and /* */ comments outside string and character literals.
static std::string stripComments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const char n = i + 1 < s.size() ? s[i + 1] : 0;
    if (c == '"' || c == '\'') {
      i = copyLiteral(s, i, out);
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

static std::vector<size_t> findAll(const std::string& hay, const std::string& needle) {
  std::vector<size_t> at;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) at.push_back(p);
  return at;
}

// The body of the function whose definition starts with `sig`.
static std::string bodyOf(const std::string& src, const std::string& sig) {
  const size_t s = src.find(sig);
  TEST_ASSERT_TRUE_MESSAGE(s != std::string::npos, sig.c_str());
  const size_t e = src.find("\n}\n", s);
  return src.substr(s, e - s);
}

static const std::string& code() {
  static const std::string c = stripComments(readFile("src/agent/telegram.cpp"));
  return c;
}

static void test_inbound_observed_first() {
  const std::string h = bodyOf(code(), "static nimbus::orch::ClassifiedUpdate handleUpdate(");
  const size_t obs = h.find("observeInbound(u);");
  TEST_ASSERT_TRUE(obs != std::string::npos);
  for (const char* later : {"g_cursor.admit(", "classifyVoice(", "classifyFile(", "classifyText(",
                            "allowed(chatId)"}) {
    const size_t at = h.find(later);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos && obs < at, later);
  }
  // handleUpdate is the only path from a getUpdates page to a classifier.
  TEST_ASSERT_EQUAL_size_t(2, findAll(code(), "handleUpdate(").size());   // definition + drain
  TEST_ASSERT_EQUAL_size_t(1, findAll(code(), "observeInbound(u);").size());
}

static void test_every_text_send_is_observed() {
  const std::string& c = code();
  // The raw sendMessage POST is reached only through doSendMessage.
  TEST_ASSERT_EQUAL_size_t(3, findAll(c, "doSendMessageRaw(").size());   // definition + html + plain
  // doSendMessage: its definition, sendNow, and the reply-queue drain (which delivers
  // what send() observed before queueing it). Nothing else sends a text directly.
  const std::vector<size_t> calls = findAll(c, "doSendMessage(");
  TEST_ASSERT_EQUAL_size_t(3, calls.size());
  const std::string now = bodyOf(c, "static bool sendNow(const char* chatId, const char* text) {");
  TEST_ASSERT_TRUE(now.find("observeOutbound(chatId, \"text\", text);") < now.find("doSendMessage("));
  TEST_ASSERT_TRUE(c.find("if (!doSendMessage(reply.chatId, reply.text)") != std::string::npos);
  // send() observes the whole text before it is split into numbered parts.
  const std::string snd = bodyOf(c, "bool send(const String& chatId, const String& text, bool block) {");
  const size_t obs = snd.find("observeOutbound(chatId.c_str(), \"text\", text.c_str());");
  TEST_ASSERT_TRUE(obs != std::string::npos);
  TEST_ASSERT_TRUE(obs < snd.find("xQueueSend("));
  TEST_ASSERT_TRUE(obs < snd.find("TG_CHUNK"));
}

static void test_every_media_send_is_observed() {
  const std::string& c = code();
  const std::vector<size_t> posts = findAll(c, "httpmp::post(");
  TEST_ASSERT_EQUAL_size_t(1, posts.size());
  const std::string media = bodyOf(c, "static void drainMedia() {");
  const size_t obs = media.find("observeOutbound(m.chatId, m.kind, m.caption, m.path, m.sd);");
  TEST_ASSERT_TRUE(obs != std::string::npos);
  TEST_ASSERT_TRUE(obs < media.find("httpmp::post("));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_inbound_observed_first);
  RUN_TEST(test_every_text_send_is_observed);
  RUN_TEST(test_every_media_send_is_observed);
  return UNITY_END();
}
