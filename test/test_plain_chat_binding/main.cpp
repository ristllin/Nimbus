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
// Lane E2-CHAT (the owner's daily chat):
//   - a photo part reaches the request as one image block in its own turn only, and only a head
//     that takes images gets the part; the next turn shows its stub;
//   - a context skill rides the pinned block whole; a too-large one is left out of it, listed in
//     the index and named once per turn to the device (which tells the owner one line);
//   - the turn's usage is handed over once per turn, in the provider's counts;
//   - /compact runs the driver now and answers on the chat; /budget's view is in tokens;
//   - the progress line is bound on the hooks (a turn with it installed still answers once).
#include <unity.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/chat_photos.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/texts.h"
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
  int failExists = 0;   // the next N exists() calls answer false (lane E2-STORE)
  int appendsLeft = -1;  // >= 0: appends that still land, then every append is refused
  bool present() override { return card; }
  bool mayWrite() override { return true; }
  uint32_t largestInternal() override { return 64 * 1024; }
  uint32_t nowMs() override { return 0; }
  void lock() override {}
  void unlock() override {}
  bool exists(const std::string& p) override {
    if (failExists > 0) return --failExists, false;
    return files.count(p) || dirs.count(p);
  }
  bool isDir(const std::string& p) override { return dirs.count(p) > 0; }
  bool mkdir(const std::string& p) override { return dirs.insert(p).second; }
  size_t append(const std::string& p, const char* d, size_t n) override {
    if (appendsLeft == 0) return 0;
    if (appendsLeft > 0) --appendsLeft;
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
    d.outside = outside;
    d.recentFiles = recentFiles;
    d.photo = [this](const std::string& ref, plain::PhotoBytes& o) {
      if (plain::photoBytesFrom(photos, ref, o).ok()) return plain::Status::success();
      return plain::photoBytesFrom(stored, ref, o);   // the card's copy of an earlier photo
    };
    if (!skillFiles.empty()) d.skills = [this](const nimbus::orch::Principal&) { return plain::chatSkillsOf(skillFiles); };
    d.skillRefused = [this](const std::string& id, size_t) { refused.push_back(id); };
    d.usage = [this](const plain::TurnOutcome& o) { usage.push_back(plain::turnSpendOf(o, "anthropic", "claude-haiku-5-5")); };
    if (progress)
      d.progress.send = [this](const std::string&, const std::string& t, int64_t& id) {
        id = 0;
        progressLines.push_back(t);
        return true;
      };
    d.noCard = [this] { return noCardFitted; };
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }
  agent::HttpTransport& model_() { return model; }
  plain::Status say(const std::string& text, std::vector<uint64_t> src = {}) {
    plain::ChatMessage m;
    m.text = text;
    m.src = std::move(src);
    m.parts = plain::photoParts(photos, "anthropic", "claude-haiku-5-5");
    out = plain::TurnOutcome{};
    return binding->turn(m, out);
  }

  agent::Platform platform;
  std::string provider;   // "" = the binding's default (anthropic)
  std::function<bool(const std::string&)> outside;   // lane E2-TOOLS
  std::function<nimbus::orch::tx::RecentFilesBlock::Source(const nimbus::orch::Principal&)> recentFiles;
  bool noCardFitted = false;   // the device has no card at all (EngineDeps::noCard)
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  Card card;
  tx::CardTxFiles files;
  nimbus::orch::ToolRegistry reg;
  nimbus::orch::SeenIndex seen;
  std::vector<std::string> sent;
  nimbus::orch::TurnImages* photos = nullptr;     // the message's photo set (null: none)
  nimbus::orch::TurnImages* stored = nullptr;     // earlier photos the card still holds (null: none)
  std::vector<plain::SkillFile> skillFiles;
  std::vector<std::string> refused;
  std::vector<nimbus::orch::TurnSpend> usage;
  bool progress = false;
  std::vector<std::string> progressLines;
  std::unique_ptr<plain::ChatBinding> binding;
  plain::TurnOutcome out;
};

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) n++;
  return n;
}

// A JPEG's first bytes are enough for the wire (it splices the bytes as they are).
const uint8_t kJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x02, 0x03};

void onePhoto(nimbus::orch::TurnImages& set) {
  nimbus::orch::TurnImage img;
  img.ref = "0123456789abcdef";
  img.mime = "image/jpeg";
  img.bytes = kJpeg;
  img.len = sizeof(kJpeg);
  set.items.push_back(img);
}

// read_file as the registry holds it once the file tools are bound (the index shows only then).
void addReadFile(nimbus::orch::ToolRegistry& reg) {
  const auto* d = plain::toolDecl("read_file");
  TEST_ASSERT_NOT_NULL(d);
  reg.add(d->name, d->description,
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) { return nimbus::orch::ToolResult::ok("x"); },
          plain::declSchema(*d, false));
}

std::string skillMd(const std::string& inject, const std::string& body) {
  return "---\ntitle: Test skill\ndesc: when the bench asks\ninject: " + inject + "\n---\n" + body;
}

int sentWith(const std::vector<std::string>& sent, const std::string& needle) {
  int n = 0;
  for (const std::string& s : sent) n += has(s, needle) ? 1 : 0;
  return n;
}

// The q of every whole line of the chat's segments, in file order.
std::vector<uint64_t> storedQs(const Card& card, const std::string& chat) {
  std::vector<uint64_t> qs;
  const std::string pre = "/mem/tx/" + chat + "/seg-";
  for (const auto& kv : card.files) {
    if (kv.first.rfind(pre, 0) != 0) continue;
    const std::string& f = kv.second;
    for (size_t at = f.find("\"q\":"); at != std::string::npos; at = f.find("\"q\":", at + 4)) {
      const size_t lineStart = at == 0 ? 0 : f.rfind('\n', at);
      const size_t start = lineStart == std::string::npos ? 0 : lineStart + 1;
      if (f.find("\"q\":", start) != at) continue;   // only the first "q" of a line
      qs.push_back(std::strtoull(f.c_str() + at + 4, nullptr, 10));
    }
  }
  return qs;
}

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

// Lane E2-TOOLS: a tool the device names as outside content taints the turn (its result is stored as
// outside); the files a person may read reach the request.
static void test_an_outside_tool_taints_the_turn_and_recent_files_reach_the_request() {
  Device dev;
  dev.reg.add("web_search", "Search the web.", [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return nimbus::orch::ToolResult::ok("a page said hello");
  }, R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})");
  dev.reg.add("calc", "Exact decimal arithmetic.", [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return nimbus::orch::ToolResult::ok("4");
  }, R"({"type":"object","properties":{"expression":{"type":"string"}},"required":["expression"]})");
  dev.outside = [](const std::string& tool) { return tool == "web_search"; };
  std::string asked;
  dev.recentFiles = [&asked](const nimbus::orch::Principal& who) -> nimbus::orch::tx::RecentFilesBlock::Source {
    asked = who.ns;
    return [](std::vector<nimbus::orch::tx::RecentFile>& out) {
      out = {{"notes/bench-recent.md", kEpoch0}};
      return true;
    };
  };
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_FALSE(asked.empty());
  dev.model.push(sp::calls({{"calc", R"({"expression":"2+2"})", ""}, {"web_search", R"({"query":"hello"})", ""}}));
  dev.model.push(sp::say("Done."));
  TEST_ASSERT_TRUE(dev.say("search for hello").ok());
  TEST_ASSERT_TRUE_MESSAGE(has(dev.model.requests()[0].body, "notes/bench-recent.md"), "recent files in the request");
  std::string stored;
  for (const auto& kv : dev.card.files) stored += kv.second;
  // One outside result (web_search) and one receipt (calc) on the card.
  size_t outs = 0;
  for (size_t p = stored.find("\"out\""); p != std::string::npos; p = stored.find("\"out\"", p + 1)) ++outs;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)outs, stored.c_str());
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
  dev.noCardFitted = true;   // no card fitted at all: RAM by design, the existing line only
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Still here."));
  TEST_ASSERT_TRUE(dev.say("no card today").ok());
  TEST_ASSERT_TRUE(dev.out.delivered);
  TEST_ASSERT_TRUE(!dev.sent.empty());
  TEST_ASSERT_EQUAL_STRING("1001|Still here.", dev.sent[0].c_str());   // the reply first; the engine's one
                                                                     // not-stored line may follow it
  TEST_ASSERT_TRUE(dev.card.files.empty());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  TEST_ASSERT_EQUAL(0, sentWith(dev.sent, plain::kNotLoadedLine));
}

// Lane E2-STORE, findings S1 and S5 through the binding.
static void boot_with_one_turn(Device& dev) {
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Noted, the cat is Biscuit."));
  TEST_ASSERT_TRUE(dev.say("my cat is called Biscuit").ok());
  dev.binding.reset();   // the restart
  dev.ms += 60000;
  dev.sent.clear();
}

// One exists() that fails at boot: the chat is not opened empty; the turn that follows loads it
// first, so the model sees the whole conversation and nothing went to the wrong place.
static void test_a_failed_exists_at_boot_is_not_an_empty_conversation() {
  Device dev;
  boot_with_one_turn(dev);
  const std::map<std::string, std::string> before = dev.card.files;
  dev.card.failExists = 1;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  TEST_ASSERT_TRUE(dev.card.files == before);   // no line, no head file written anywhere
  dev.model.push(sp::say("Your cat is Biscuit."));
  TEST_ASSERT_TRUE(dev.say("what is my cat called?").ok());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  TEST_ASSERT_TRUE(has(dev.model.last().body, "my cat is called Biscuit"));
  TEST_ASSERT_EQUAL(0, sentWith(dev.sent, plain::kNotLoadedLine));
  const std::vector<uint64_t> qs = storedQs(dev.card, "1001");
  for (size_t i = 1; i < qs.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(qs[i] > qs[i - 1], "q only rises in file order");
}

// The card is lost at boot: every outage turn tells the owner, the card's return reloads the
// conversation before the next turn, the RAM turns follow it once, and a restart keeps them.
static void test_an_outage_at_boot_reloads_and_stores_the_ram_turns_once() {
  Device dev;
  boot_with_one_turn(dev);
  dev.card.card = false;   // fitted, but lost (sdLost, FAULT sd, a bad seat)
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  dev.model.push(sp::say("Rex, noted."));
  TEST_ASSERT_TRUE(dev.say("my dog is called Rex").ok());
  TEST_ASSERT_FALSE(has(dev.model.last().body, "Biscuit"));
  TEST_ASSERT_EQUAL(1, sentWith(dev.sent, plain::kNotLoadedLine));
  TEST_ASSERT_EQUAL(0, sentWith(dev.sent, "it is kept until the device restarts"));   // not the no-card line
  dev.model.push(sp::say("Goldie, noted."));
  TEST_ASSERT_TRUE(dev.say("my fish is called Goldie").ok());
  TEST_ASSERT_EQUAL(2, sentWith(dev.sent, plain::kNotLoadedLine));   // on every outage turn
  dev.card.card = true;   // the card answers again
  dev.model.push(sp::say("Biscuit, Rex and Goldie."));
  TEST_ASSERT_TRUE(dev.say("name all my pets").ok());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  const std::string body = dev.model.last().body;
  TEST_ASSERT_TRUE(has(body, "my cat is called Biscuit"));
  TEST_ASSERT_TRUE(body.find("my cat is called Biscuit") < body.find("my dog is called Rex"));
  TEST_ASSERT_TRUE(body.find("my dog is called Rex") < body.find("my fish is called Goldie"));
  TEST_ASSERT_EQUAL(1, count(body, "my dog is called Rex"));
  TEST_ASSERT_EQUAL(2, sentWith(dev.sent, plain::kNotLoadedLine));   // none once loaded
  std::string seg;
  for (const auto& kv : dev.card.files)
    if (kv.first.find("/seg-") != std::string::npos) seg += kv.second;
  TEST_ASSERT_EQUAL(1, count(seg, "my dog is called Rex"));
  TEST_ASSERT_EQUAL(1, count(seg, "my fish is called Goldie"));
  TEST_ASSERT_EQUAL(1, count(seg, "name all my pets"));
  const std::vector<uint64_t> qs = storedQs(dev.card, "1001");
  TEST_ASSERT_TRUE(qs.size() >= 12);   // four turns of at least three lines each
  for (size_t i = 1; i < qs.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(qs[i] > qs[i - 1], "q only rises in file order");
  // A restart: the whole conversation, the outage turns included, each once.
  dev.binding.reset();
  dev.ms += 60000;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  dev.model.push(sp::say("Still Biscuit, Rex and Goldie."));
  TEST_ASSERT_TRUE(dev.say("again?").ok());
  const std::string again = dev.model.last().body;
  TEST_ASSERT_EQUAL(1, count(again, "my cat is called Biscuit"));
  TEST_ASSERT_EQUAL(1, count(again, "my dog is called Rex"));
  TEST_ASSERT_EQUAL(1, count(again, "my fish is called Goldie"));
  TEST_ASSERT_EQUAL(1, count(again, "name all my pets"));
}

// A card that keeps refusing: the chat stays in RAM, every turn is kept there and told, nothing
// is written; a truly new chat (folder absent, parent listed) still opens empty.
static void test_a_new_chat_opens_empty_and_an_outage_keeps_ram() {
  Device fresh;
  TEST_ASSERT_TRUE(fresh.boot().ok());
  TEST_ASSERT_FALSE(fresh.binding->unloaded());
  Device dev;
  dev.card.card = false;
  TEST_ASSERT_TRUE(dev.boot().ok());
  for (int i = 0; i < 3; ++i) {
    dev.model.push(sp::say("Kept."));
    TEST_ASSERT_TRUE(dev.say("turn " + std::to_string(i)).ok());
  }
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  TEST_ASSERT_EQUAL(3, sentWith(dev.sent, plain::kNotLoadedLine));
  TEST_ASSERT_TRUE(has(dev.model.last().body, "turn 0") && has(dev.model.last().body, "turn 1"));
  TEST_ASSERT_TRUE(dev.card.files.empty());
}

// The card comes back but refuses part way through the merge: what landed stays, the rest stays
// in RAM, the next turn resumes after it; each line lands once, in order.
static void test_a_merge_cut_part_way_resumes_without_a_second_copy() {
  Device dev;
  boot_with_one_turn(dev);
  dev.card.card = false;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Rex, noted."));
  TEST_ASSERT_TRUE(dev.say("my dog is called Rex").ok());
  dev.model.push(sp::say("Goldie, noted."));
  TEST_ASSERT_TRUE(dev.say("my fish is called Goldie").ok());
  dev.card.card = true;
  dev.card.appendsLeft = 2;   // two RAM lines land, then the card refuses
  dev.model.push(sp::say("Still held."));
  TEST_ASSERT_TRUE(dev.say("are you there?").ok());
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  TEST_ASSERT_EQUAL(3, sentWith(dev.sent, plain::kNotLoadedLine));
  dev.card.appendsLeft = -1;
  dev.model.push(sp::say("Biscuit, Rex and Goldie."));
  TEST_ASSERT_TRUE(dev.say("name all my pets").ok());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  std::string seg;
  for (const auto& kv : dev.card.files)
    if (kv.first.find("/seg-") != std::string::npos) seg += kv.second;
  for (const char* t : {"my cat is called Biscuit", "my dog is called Rex", "my fish is called Goldie", "are you there?",
                        "name all my pets"})
    TEST_ASSERT_EQUAL_MESSAGE(1, count(seg, t), t);
  TEST_ASSERT_TRUE(seg.find("my dog is called Rex") < seg.find("my fish is called Goldie"));
  TEST_ASSERT_TRUE(seg.find("my fish is called Goldie") < seg.find("are you there?"));
  const std::vector<uint64_t> qs = storedQs(dev.card, "1001");
  for (size_t i = 1; i < qs.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(qs[i] > qs[i - 1], "q only rises in file order");
  const std::string body = dev.model.last().body;
  TEST_ASSERT_EQUAL(1, count(body, "my dog is called Rex"));
  TEST_ASSERT_TRUE(has(body, "my cat is called Biscuit") && has(body, "are you there?"));
}

// Lane E2-INTEG: E2-STORE's reload with E2-VERB's verbatim history. A result stored before the
// outage (bulk, on the card) and one held in RAM during it are both in the request after the
// reload, whole, each once.
static void test_after_a_reload_earlier_results_are_shown_whole() {
  std::string before, during;
  for (int i = 0; i < 400; ++i) before += "BEFORE-ROW-" + std::to_string(i) + ";";
  for (int i = 0; i < 400; ++i) during += "DURING-ROW-" + std::to_string(i) + ";";
  TEST_ASSERT_TRUE(before.size() > 4096 && during.size() > 4096);
  Device dev;
  int calls = 0;
  dev.reg.add("calc", "Exact decimal arithmetic.", [&](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    const bool first = calls == 0;
    ++calls;
    return nimbus::orch::ToolResult::ok(first ? before : during);
  }, R"({"type":"object","properties":{"expression":{"type":"string"}},"required":["expression"]})");
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::calls({{"calc", R"({"expression":"1+1"})", ""}}));
  dev.model.push(sp::say("Fetched the first."));
  const plain::Status first = dev.say("fetch the first text");
  TEST_ASSERT_TRUE_MESSAGE(first.ok(), first.where);
  dev.binding.reset();   // the restart
  dev.ms += 60000;
  dev.card.card = false;   // fitted, but lost at boot
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  dev.model.push(sp::calls({{"calc", R"({"expression":"2+2"})", ""}}));
  dev.model.push(sp::say("Fetched the second."));
  TEST_ASSERT_TRUE(dev.say("fetch the second text").ok());
  dev.card.card = true;
  dev.model.push(sp::say("Both texts are here."));
  TEST_ASSERT_TRUE(dev.say("compare both texts").ok());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  const std::string body = dev.model.last().body;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(body, before), "the stored result, whole");
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)count(body, during), "the RAM-held result, whole");
  TEST_ASSERT_TRUE(body.find(before) < body.find(during));
}

static void test_a_photo_reaches_its_own_turn_once_as_an_image() {
  Device dev;
  nimbus::orch::TurnImages set;
  onePhoto(set);
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.photos = &set;
  dev.model.push(sp::say("A small test picture."));
  TEST_ASSERT_TRUE(dev.say("[image 1/1 ref=0123456789abcdef] what is this?").ok());
  const std::string& first = dev.model.last().body;
  TEST_ASSERT_EQUAL(1, (int)count(first, "\"type\":\"image\""));
  TEST_ASSERT_EQUAL(1, (int)count(first, "\"media_type\":\"image/jpeg\""));
  dev.photos = nullptr;   // the set is freed with its turn
  dev.model.push(sp::say("It was a small test picture."));
  TEST_ASSERT_TRUE(dev.say("what was in the photo?").ok());
  const std::string& second = dev.model.last().body;
  TEST_ASSERT_EQUAL(0, (int)count(second, "\"type\":\"image\""));
  TEST_ASSERT_TRUE(has(second, "[photo p0123456789abcdef"));   // its stub, and the head's own answer
  TEST_ASSERT_TRUE(has(second, "A small test picture."));
  TEST_ASSERT_EQUAL(2, (int)dev.sent.size());
}

static void test_the_next_message_soon_after_still_sees_the_photo_then_its_stub() {
  Device dev;
  nimbus::orch::TurnImages set;
  onePhoto(set);
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.photos = &set;
  dev.model.push(sp::say("A red triangle."));
  TEST_ASSERT_TRUE(dev.say("[image 1/1 ref=0123456789abcdef] what is this?").ok());
  dev.photos = nullptr;
  dev.stored = &set;      // the card keeps the photo
  dev.ms += 60000;        // the follow-up a minute later: inside the cold gap
  dev.model.push(sp::say("The background is yellow."));
  TEST_ASSERT_TRUE(dev.say("what color was the background?").ok());
  const std::string second = dev.model.last().body;
  TEST_ASSERT_EQUAL(1, (int)count(second, "\"type\":\"image\""));   // carried once, in its own message
  dev.ms += 600000;       // ten minutes later: out of the carry, its stub only
  dev.model.push(sp::say("It was a triangle."));
  TEST_ASSERT_TRUE(dev.say("and the shape?").ok());
  const std::string third = dev.model.last().body;
  TEST_ASSERT_EQUAL(0, (int)count(third, "\"type\":\"image\""));
  TEST_ASSERT_TRUE(has(third, "[photo p0123456789abcdef"));
}

static void test_only_a_head_that_takes_images_gets_the_part() {
  nimbus::orch::TurnImages set;
  onePhoto(set);
  TEST_ASSERT_EQUAL(1, (int)plain::photoParts(&set, "anthropic", "claude-haiku-5-5").size());
  TEST_ASSERT_EQUAL_STRING("p0123456789abcdef", plain::photoParts(&set, "anthropic", "claude-sonnet-5-5")[0].ref.c_str());
  TEST_ASSERT_EQUAL(0, (int)plain::photoParts(&set, "zai", "glm-5.3").size());
  TEST_ASSERT_EQUAL(0, (int)plain::photoParts(&set, "mistral", "codestral-latest").size());
  TEST_ASSERT_EQUAL(0, (int)plain::photoParts(nullptr, "anthropic", "claude-haiku-5-5").size());
  plain::PhotoBytes px;
  TEST_ASSERT_TRUE(plain::photoBytesFrom(&set, "p0123456789abcdef", px).ok());
  TEST_ASSERT_EQUAL(sizeof(kJpeg), px.len);
  TEST_ASSERT_FALSE(plain::photoBytesFrom(&set, "pffffffffffffffff", px).ok());
}

static void test_a_context_skill_rides_the_fixed_part_and_a_too_large_one_is_refused_once() {
  Device dev;
  addReadFile(dev.reg);
  dev.skillFiles.push_back({"bench-ctx", skillMd("context", "The bench word is ZEPHYR-41.\n\nAlways say it.")});
  dev.skillFiles.push_back({"bench-big", skillMd("context", std::string(5000, 'w'))});
  dev.skillFiles.push_back({"bench-spawn", skillMd("spawn", "Read only when asked.")});
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("ZEPHYR-41"));
  TEST_ASSERT_TRUE(dev.say("what is the bench word?").ok());
  const std::string& body = dev.model.last().body;
  TEST_ASSERT_TRUE(has(body, "skills/bench-ctx.md:\\nThe bench word is ZEPHYR-41.\\nAlways say it."));
  TEST_ASSERT_FALSE(has(body, "wwwwwwwwww"));                          // not cut: left out whole
  TEST_ASSERT_TRUE(has(body, "skills/bench-big.md: when the bench asks"));   // listed instead
  TEST_ASSERT_TRUE(has(body, "skills/bench-spawn.md: when the bench asks"));
  TEST_ASSERT_FALSE(has(body, "Read only when asked."));
  TEST_ASSERT_EQUAL(1, (int)dev.refused.size());
  TEST_ASSERT_EQUAL_STRING("bench-big", dev.refused[0].c_str());
}

static void test_a_turns_usage_is_handed_over_once() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  sp::Step s = sp::say("Counted.");
  s.usage.input = 120;
  s.usage.output = 30;
  s.usage.cacheRead = 900;
  s.usage.cacheWrite = 40;
  dev.model.push(s);
  TEST_ASSERT_TRUE(dev.say("count me").ok());
  TEST_ASSERT_EQUAL(1, (int)dev.usage.size());
  const nimbus::orch::TurnSpend& u = dev.usage[0];
  TEST_ASSERT_EQUAL(1, (int)u.n);
  TEST_ASSERT_EQUAL_STRING("anthropic", u.slot[0].host);
  TEST_ASSERT_EQUAL(120u, u.slot[0].in);
  TEST_ASSERT_EQUAL(30u, u.slot[0].out);
  TEST_ASSERT_EQUAL(900u, u.slot[0].cacheR);
  TEST_ASSERT_EQUAL(40u, u.slot[0].cacheW);
  TEST_ASSERT_EQUAL(1u, u.slot[0].billed);
  dev.model.push(sp::say("Again."));
  TEST_ASSERT_TRUE(dev.say("and again").ok());
  TEST_ASSERT_EQUAL(2, (int)dev.usage.size());   // one per turn, never per part or twice
}

static void test_compact_now_answers_on_the_chat_and_the_budget_view_is_in_tokens() {
  Device dev;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->compactNow().ok());
  TEST_ASSERT_EQUAL(1, (int)dev.sent.size());   // nothing to compact yet: one line, no request
  TEST_ASSERT_EQUAL_STRING((std::string("1001|") + plain::texts::device_notes::kCompactNothing).c_str(), dev.sent[0].c_str());
  TEST_ASSERT_EQUAL(0, (int)dev.model.requests().size());
  plain::BudgetView v;
  TEST_ASSERT_TRUE(dev.binding->budgetView(v).ok());
  TEST_ASSERT_TRUE(v.systemTokens > 0 && v.fixedTokens >= v.systemTokens);
  TEST_ASSERT_TRUE(v.dueTokens > 0 && v.maxRounds > 0);
}

static void test_the_progress_line_is_bound_and_a_short_turn_sends_none() {
  Device dev;
  dev.progress = true;
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Quick."));
  TEST_ASSERT_TRUE(dev.say("quick one").ok());
  TEST_ASSERT_EQUAL(1, (int)dev.sent.size());
  TEST_ASSERT_EQUAL(0, (int)dev.progressLines.size());   // under 20 s: no line
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_message_reaches_the_turn_and_the_reply_the_channel_once);
  RUN_TEST(test_the_arena_and_the_cost_follow_the_head);
  RUN_TEST(test_an_outside_tool_taints_the_turn_and_recent_files_reach_the_request);
  RUN_TEST(test_a_rebooted_binding_carries_the_earlier_turns);
  RUN_TEST(test_the_seen_index_is_rebuilt_from_the_card);
  RUN_TEST(test_with_no_card_the_chat_runs_in_ram);
  RUN_TEST(test_a_photo_reaches_its_own_turn_once_as_an_image);
  RUN_TEST(test_the_next_message_soon_after_still_sees_the_photo_then_its_stub);
  RUN_TEST(test_only_a_head_that_takes_images_gets_the_part);
  RUN_TEST(test_a_context_skill_rides_the_fixed_part_and_a_too_large_one_is_refused_once);
  RUN_TEST(test_a_turns_usage_is_handed_over_once);
  RUN_TEST(test_compact_now_answers_on_the_chat_and_the_budget_view_is_in_tokens);
  RUN_TEST(test_the_progress_line_is_bound_and_a_short_turn_sends_none);
  RUN_TEST(test_a_failed_exists_at_boot_is_not_an_empty_conversation);
  RUN_TEST(test_an_outage_at_boot_reloads_and_stores_the_ram_turns_once);
  RUN_TEST(test_a_new_chat_opens_empty_and_an_outage_keeps_ram);
  RUN_TEST(test_a_merge_cut_part_way_resumes_without_a_second_copy);
  RUN_TEST(test_after_a_reload_earlier_results_are_shown_whole);
  return UNITY_END();
}
