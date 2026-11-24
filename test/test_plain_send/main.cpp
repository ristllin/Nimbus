// test_plain_send - engine 2's send and speak (lane E2-P-OUT1, src/agent/tools_plain_send.h) inside real
// turns: the device's one-chat binding (chat_binding.h) with a scripted Claude head on the real Anthropic
// wire and a fake card, and the tool registered with fake effects standing for engine 1's delivery code
// (files::sendStored, orchestrator::sendToChat, speakOnDevice) and its screen (orchestrator::screenReply).
// Each test pins what was sent, to which chat, through which function:
//   - a stored file and a photo go through sendFile (files.send's lookup and media lane), the photo as
//     photo-only; a photo by ref of this conversation through sendPhotoRef;
//   - a second message goes to an allowed chat through sendText; a chat off the allowlist gets nothing;
//   - Voice replies on: a voice note through sendVoice with Telegram's message id in the result, and speak
//     (behind more) through the speaker; every text passed the screen first and a suppressed one is not sent;
//   - Voice replies off: a voice send arrives as text through sendText, speak says so, and the turn still
//     ends with the model's text in the chat (never silent).
#include <unity.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../../src/agent/tools_plain_send.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace ps = agent::plainsend;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;
namespace tr = agent::plain::texts::tool_results;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC
constexpr char kChat[] = "1001";
constexpr char kOther[] = "2002";   // on the allowlist

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A card that keeps files and folders (test_plain_agents's).
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool present() override { return true; }
  bool mayWrite() override { return true; }
  uint32_t largestInternal() override { return 64 * 1024; }
  uint32_t nowMs() override { return 0; }
  void lock() override {}
  void unlock() override {}
  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool isDir(const std::string& p) override { return dirs.count(p) > 0; }
  bool mkdir(const std::string& p) override { return dirs.insert(p).second; }
  size_t append(const std::string& p, const char* d, size_t n) override {
    files[p].append(d, n);
    return n;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    got = 0;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off < it->second.size()) got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& a, const std::string& b) override {
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (!dirs.count(dir)) return false;
    const std::string pre = dir == "/" ? std::string("/") : dir + "/";
    auto child = [&](const std::string& p) {
      if (p.size() > pre.size() && p.rfind(pre, 0) == 0 && p.find('/', pre.size()) == std::string::npos)
        names.push_back(p.substr(pre.size()));
    };
    for (const auto& kv : files) child(kv.first);
    for (const std::string& d : dirs) child(d);
    return true;
  }
};

// One boot of the device with send and speak registered over recording effects.
struct Device {
  Device() : files(card) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    ps::SendDeps d;
    d.inTurn = [this] { return inTurn; };
    d.turnChat = [this] { return inTurn ? std::string(kChat) : std::string(); };
    d.target = [this](const std::string& asked, const orch::Principal&, bool) {
      const std::string c = asked.empty() ? std::string(kChat) : asked;
      return allowed.count(c) ? c : std::string();
    };
    d.voiceOn = [this] { return voiceOn; };
    d.speakerMuted = [] { return false; };
    // The screen: the owner's output rule (here: "darn" becomes "d*rn") and the outbound screen (a text
    // with "forbidden" is suppressed).
    d.screen = [this](const std::string& chat, std::string& text) {
      screened.push_back(chat + "|" + text);
      if (has(text, "forbidden")) return false;
      for (size_t k; (k = text.find("darn")) != std::string::npos;) text.replace(k, 4, "d*rn");
      return true;
    };
    d.sendFile = [this](const std::string& project, const std::string& name, const std::string& chat,
                        const orch::Principal&, bool photoOnly, const std::string& caption) {
      effects.push_back(std::string("sendFile|") + chat + "|" + project + "/" + name + "|" +
                        (photoOnly ? "photo" : "any") + "|" + caption);
      return name == "missing.txt" ? ps::FileSent::NotFound : ps::FileSent::Queued;
    };
    d.sendPhotoRef = [this](const std::string& ref, const std::string& chat, const std::string&) {
      effects.push_back("sendPhotoRef|" + chat + "|" + ref);
      return ps::FileSent::Queued;
    };
    d.sendText = [this](const std::string& chat, const std::string& text) {
      effects.push_back("sendText|" + chat + "|" + text);
      return true;
    };
    d.sendVoice = [this](const std::string& chat, const std::string& text) {
      effects.push_back("sendVoice|" + chat + "|" + text);
      ps::VoiceSent v;
      v.delivered = true;
      v.notesSent = 1;
      v.receipt = "777";
      return v;
    };
    d.speak = [this](const std::string& text, std::string&) {
      effects.push_back("speak|" + text);
      return true;
    };
    TEST_ASSERT_TRUE(ps::registerSendTools(reg, d));
    orch::registerMore(reg);
  }

  void boot() {
    plain::ChatBindingConfig c;
    c.chatKey = kChat;
    c.model = "claude-haiku-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-binding"); };
    binding.reset(new plain::ChatBinding(c, d));
    TEST_ASSERT_TRUE(binding->boot().ok());
  }

  // One person's message as one turn: the tool call the head makes, then its closing text.
  void turn(const std::string& tool, const std::string& args, const std::string& closing = "Done.") {
    model.push(sp::call(tool, args));
    model.push(sp::say(closing));
    plain::ChatMessage m;
    m.text = "please";
    out = plain::TurnOutcome{};
    inTurn = true;
    const plain::Status st = binding->turn(m, out);
    inTurn = false;
    TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
  }

  // The tool result the head read in the turn's second request.
  bool resultSays(const std::string& words) const { return has(model.last().body, words); }

  uint32_t ms = 1000;
  Card card;
  agent::Platform platform;
  sp::ScriptedProvider model;
  tx::CardTxFiles files;
  orch::ToolRegistry reg;
  std::unique_ptr<plain::ChatBinding> binding;
  bool inTurn = false;
  bool voiceOn = true;
  std::set<std::string> allowed{kChat, kOther};
  std::vector<std::string> sent, effects, screened;
  plain::TurnOutcome out;
};

}  // namespace

void setUp() {}
void tearDown() {}

// D1: a stored file goes through files.send's code to this chat, its caption screened.
static void test_a_file_is_sent_through_files_send() {
  Device dev;
  dev.boot();
  dev.turn("send", R"({"kind":"file","path":"reports/march.csv"})", "Sent the report.");
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());
  TEST_ASSERT_EQUAL_STRING("sendFile|1001|reports/march.csv|any|reports/march.csv", dev.effects[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1001|reports/march.csv", dev.screened[0].c_str());
  TEST_ASSERT_TRUE_MESSAGE(dev.resultSays(std::string(tr::kSendQueued) + kChat), dev.model.last().body.c_str());
  TEST_ASSERT_EQUAL_STRING("1001|Sent the report.", dev.sent.back().c_str());
  dev.effects.clear();
  dev.turn("send", R"({"kind":"file","path":"reports/missing.txt"})");
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSendNotFound));
}

// D1: a photo from the store goes photo-only; a photo of this conversation by its ref to this chat.
static void test_a_photo_is_sent_as_a_photo() {
  Device dev;
  dev.boot();
  dev.turn("send", R"({"kind":"photo","path":"pics/cat.jpg","caption":"darn cute"})");
  TEST_ASSERT_EQUAL_STRING("sendFile|1001|pics/cat.jpg|photo|d*rn cute", dev.effects[0].c_str());
  dev.turn("send", R"({"kind":"photo","ref":"p0123456789abcdef"})");
  TEST_ASSERT_EQUAL_STRING("sendPhotoRef|1001|p0123456789abcdef", dev.effects[1].c_str());
  TEST_ASSERT_TRUE(dev.resultSays(std::string(tr::kSendQueued) + kChat));
  dev.turn("send", R"({"kind":"photo","ref":"p0123456789abcdef","chat":"2002"})");   // another chat: never by ref
  TEST_ASSERT_EQUAL(2, (int)dev.effects.size());
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSendNoChat));
}

// D1: a second message to an allowed chat through reply.telegram's code; never to a chat off the list.
static void test_a_second_message_goes_to_an_allowed_chat() {
  Device dev;
  dev.boot();
  dev.turn("send", R"({"kind":"text","text":"Dinner at 8, darn it","chat":"2002"})", "Told them.");
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());
  TEST_ASSERT_EQUAL_STRING("sendText|2002|Dinner at 8, d*rn it", dev.effects[0].c_str());
  TEST_ASSERT_TRUE(dev.resultSays(std::string(tr::kSendQueued) + kOther));
  dev.turn("send", R"({"kind":"text","text":"hello","chat":"3003"})");
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSendNoChat));
  dev.turn("send", R"({"kind":"text","text":"a forbidden line"})");   // the outbound screen suppressed it
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSendBlocked));
}

// D1 and D2: Voice replies on: a voice note with Telegram's id, and speech on the speaker (behind more),
// both screened like text.
static void test_voice_on_sends_a_voice_note_and_speaks() {
  Device dev;
  dev.boot();
  dev.turn("send", R"({"kind":"voice","text":"Good darn morning"})", "");
  TEST_ASSERT_EQUAL_STRING("sendVoice|1001|Good d*rn morning", dev.effects[0].c_str());
  TEST_ASSERT_TRUE(dev.resultSays(std::string(tr::kSendVoiceDone) + "777"));
  dev.turn("more", R"({"name":"speak","args":{"text":"darn hot today"}})", "");
  TEST_ASSERT_EQUAL_STRING("speak|d*rn hot today", dev.effects[1].c_str());
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSpeakDone));
  dev.turn("send", R"({"kind":"voice","text":"forbidden words"})", "");
  TEST_ASSERT_EQUAL(2, (int)dev.effects.size());
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSendBlocked));
}

// D2: Voice replies off: the voice send arrives as text, speak says it did not speak, and the turn ends
// with the model's text in the chat.
static void test_voice_off_is_never_silent() {
  Device dev;
  dev.voiceOn = false;
  dev.boot();
  dev.turn("send", R"({"kind":"voice","text":"Your timer is done"})", "");
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());
  TEST_ASSERT_EQUAL_STRING("sendText|1001|Your timer is done", dev.effects[0].c_str());
  TEST_ASSERT_TRUE(dev.resultSays(std::string(tr::kSendVoiceOff) + tr::kSendQueued + kChat));
  const size_t before = dev.sent.size();
  dev.turn("more", R"({"name":"speak","args":{"text":"Your tea is ready"}})", "Your tea is ready.");
  TEST_ASSERT_EQUAL(1, (int)dev.effects.size());   // nothing spoken
  TEST_ASSERT_TRUE(dev.resultSays(tr::kSpeakOff));
  TEST_ASSERT_TRUE(dev.out.delivered);
  TEST_ASSERT_EQUAL((int)before + 1, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING("1001|Your tea is ready.", dev.sent.back().c_str());
}

// Outside a turn (the LAN /mcp path) nothing is sent.
static void test_nothing_is_sent_outside_a_turn() {
  Device dev;
  ArduinoJson::JsonDocument a;
  a["kind"] = "text";
  a["text"] = "hi";
  const orch::ToolResult r =
      dev.reg.dispatch("send", a.as<ArduinoJson::JsonObjectConst>(), orch::principalForRole(kChat, orch::Role::Admin));
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING(tr::kSendNotInTurn, r.error.c_str());
  TEST_ASSERT_TRUE(dev.effects.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_file_is_sent_through_files_send);
  RUN_TEST(test_a_photo_is_sent_as_a_photo);
  RUN_TEST(test_a_second_message_goes_to_an_allowed_chat);
  RUN_TEST(test_voice_on_sends_a_voice_note_and_speaks);
  RUN_TEST(test_voice_off_is_never_silent);
  RUN_TEST(test_nothing_is_sent_outside_a_turn);
  return UNITY_END();
}
