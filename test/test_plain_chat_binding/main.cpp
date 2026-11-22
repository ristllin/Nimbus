// test_plain_chat_binding - the device's one-chat binding of engine 2 (lane E2-DEV,
// nimbus/harness/plain/chat_binding.h), with a fake platform, a scripted Claude head on the real
// Anthropic wire and the card store over a fake card. Pins the binding, not the engine:
//   - a message reaches runTurn (the owner text is in the one request) and the reply reaches the
//     channel once, as the model wrote it;
//   - the arena is sized for the head (tx::arenaBytesFor of its budget), not the 1,728 KB constant;
//   - the guards' cost is set from the head's price row; a provider with no price row does not
//     boot (money fails closed);
//   - a binding booted again over the same card carries the earlier turns (the restart);
//   - the seen index is rebuilt from the card at boot: a re-served update is known, a new one not;
//   - with no card the chat runs in RAM and still answers.
#include <unity.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC

// A card that keeps files and folders (the store's own rules are test_tx_card_files').
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool card = true;
  bool present() override { return card; }
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
};

struct Device {
  Device() : files(card) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    (void)seen.init([](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
  }
  plain::Status boot(const std::string& model = "claude-haiku-5-5", const std::string& chat = "1001") {
    plain::ChatBindingConfig c;
    c.chatKey = chat;
    c.model = model;
    c.summarizerModel = "claude-haiku-5-5";
    if (!provider.empty()) c.provider = provider;
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model_();
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-binding"); };
    d.seen = &seen;
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }
  agent::HttpTransport& model_() { return model; }
  plain::Status say(const std::string& text, std::vector<uint64_t> src = {}) {
    plain::ChatMessage m;
    m.text = text;
    m.src = std::move(src);
    out = plain::TurnOutcome{};
    return binding->turn(m, out);
  }

  agent::Platform platform;
  std::string provider;   // "" = the binding's default (anthropic)
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  Card card;
  tx::CardTxFiles files;
  nimbus::orch::ToolRegistry reg;
  nimbus::orch::SeenIndex seen;
  std::vector<std::string> sent;
  std::unique_ptr<plain::ChatBinding> binding;
  plain::TurnOutcome out;
};

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

void setUp() {}
void tearDown() {}

static void test_a_message_reaches_the_turn_and_the_reply_the_channel_once() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Hello from engine two."));
  const plain::Status st = dev.say("hello from the bench");
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
  TEST_ASSERT_EQUAL(1, (int)dev.model.requests().size());
  TEST_ASSERT_TRUE(has(dev.model.last().body, "hello from the bench"));
  TEST_ASSERT_TRUE(has(dev.model.last().body, "claude-haiku-5-5"));
  TEST_ASSERT_EQUAL(1, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING("1001|Hello from engine two.", dev.sent[0].c_str());
  TEST_ASSERT_TRUE(dev.out.delivered);
  TEST_ASSERT_EQUAL(1u, dev.binding->sentParts());
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(dev.model.verify(why), why.c_str());
}

static void test_the_arena_and_the_cost_follow_the_head() {
  for (const char* model : {"claude-haiku-5-5", "claude-sonnet-5-5"}) {
    Device dev;
    TEST_ASSERT_TRUE_MESSAGE(dev.boot(model).ok(), model);
    tx::HeadBudget b;
    TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{"anthropic", model, false}, tx::kDefaultProfile, b).ok());
    TEST_ASSERT_EQUAL_MESSAGE(tx::arenaBytesFor(b), dev.binding->arena()->capacity(), model);
    TEST_ASSERT_TRUE(dev.binding->arena()->capacity() < tx::kActiveChatArenaBytes);
    TEST_ASSERT_TRUE_MESSAGE(dev.binding->cost().warmRequestMicroUsd > 0 && dev.binding->cost().coldRequestMicroUsd > 0,
                             model);
  }
  Device none;
  none.provider = "unpriced";   // a provider with no price row at all
  TEST_ASSERT_FALSE(none.boot("claude-haiku-5-5").ok());
  TEST_ASSERT_NULL(none.binding->engine());
  TEST_ASSERT_TRUE(none.card.files.empty());   // nothing stored for a head that may not run
}

static void test_a_rebooted_binding_carries_the_earlier_turns() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Noted, the cat is Biscuit."));
  dev.model.push(sp::say("Your cat is Biscuit."));
  TEST_ASSERT_TRUE(dev.say("my cat is called Biscuit").ok());
  dev.binding.reset();   // the restart: RAM gone, the card stays
  dev.ms += 60000;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.say("what is my cat called?").ok());
  TEST_ASSERT_EQUAL(2, (int)dev.model.requests().size());
  const std::string& body = dev.model.requests()[1].body;
  TEST_ASSERT_TRUE(has(body, "my cat is called Biscuit") && has(body, "Noted, the cat is Biscuit."));
  TEST_ASSERT_EQUAL(2, (int)dev.sent.size());
}

static void test_the_seen_index_is_rebuilt_from_the_card() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("First."));
  TEST_ASSERT_TRUE(dev.say("first", {501}).ok());
  dev.binding.reset();
  dev.seen.clear();   // the power cycle: the PSRAM index is gone
  TEST_ASSERT_FALSE(dev.seen.seen("1001", 501));
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->allSeen({501}));
  TEST_ASSERT_FALSE(dev.binding->allSeen({502}));
  TEST_ASSERT_FALSE(dev.binding->allSeen({}));
}

static void test_with_no_card_the_chat_runs_in_ram() {
  Device dev;
  dev.card.card = false;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Still here."));
  TEST_ASSERT_TRUE(dev.say("no card today").ok());
  TEST_ASSERT_TRUE(dev.out.delivered);
  TEST_ASSERT_TRUE(!dev.sent.empty());
  TEST_ASSERT_EQUAL_STRING("1001|Still here.", dev.sent[0].c_str());   // the reply first; the engine's one
                                                                     // not-stored line may follow it
  TEST_ASSERT_TRUE(dev.card.files.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_message_reaches_the_turn_and_the_reply_the_channel_once);
  RUN_TEST(test_the_arena_and_the_cost_follow_the_head);
  RUN_TEST(test_a_rebooted_binding_carries_the_earlier_turns);
  RUN_TEST(test_the_seen_index_is_rebuilt_from_the_card);
  RUN_TEST(test_with_no_card_the_chat_runs_in_ram);
  return UNITY_END();
}
