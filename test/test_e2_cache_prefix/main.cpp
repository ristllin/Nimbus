// test_e2_cache_prefix - lane E2-CACHE, rule A (plain/prefix.h): between two requests of one
// conversation the bytes ahead of its last closed turn are identical unless a listed event
// happened (compaction or a pointer move, the tool set, notes or files changed from outside the
// conversation, the provider's cache expired anyway, /clear).
//
// Driven through the device's own binding (plain::ChatBinding over a fake card, the real
// Anthropic wire, a scripted Claude head), so the same source runs on the tree before the lane
// (red: a pinned note the head saves, a file it writes followed by a chat switch, a restart or a
// pause, each rewrote the system part ahead of the whole conversation) and after it (green).
//
// The oracle is the test's own knowledge of what happened between two requests, never the
// engine's event names: an engine that excused a rewrite with a change point it logged itself
// would pass a check that trusted its events.
//   - the property over generated sequences of turns mixed with note saves by the head (pinned
//     and not), file writes, chat switches, restarts, pauses of any length and routines firing in
//     another chat;
//   - counter-tests: each listed event does change the bytes ahead of the conversation (the
//     outside note, the outside file, the tool set, the expired cache, /clear), and the decision
//     names compaction, the stub pointer and a full-arena drop;
//   - the 1-hour lifetime sits on the stable points only, in the order the API takes;
//   - the record: a torn record is rendered afresh, an unreadable card is never taken as absent.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#if __has_include("nimbus/harness/plain/prefix.h")
#include "nimbus/harness/plain/prefix.h"
#define E2_CACHE_TREE 1   // the lane's tree; the tree before it runs the rule-A cases only (red)
#endif
#include "nimbus/harness/plain/request.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC
constexpr const char* kMark = ",\"cache_control\":{\"type\":\"ephemeral\"}";
constexpr uint64_t kExpiredAfterS = 3600 + 60;   // plain::kCacheLongS + plain::kCacheExpiryMarginS
constexpr const char* kMarkLong = ",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}";

// A card that keeps files and folders (test_plain_chat_binding's, with a switch that makes every
// listing and read fail).
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool deaf = false;   // every list and read answers false (the card did not answer)
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
    if (deaf || it == files.end()) return false;
    if (off < it->second.size()) got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    auto it = files.find(p);
    if (deaf || it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& a, const std::string& b) override {
    if (!files.count(a)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    if (deaf) return false;
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (deaf || !dirs.count(dir)) return false;
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

// One request of the conversation under test, with what happened before it.
struct Sent {
  std::string body;    // the raw body as sent
  std::string chat;    // the chat it belongs to
  uint64_t at = 0;     // epoch when it left
  int listed = 0;      // the oracle's count of listed events before this request
};

// The device: one resident chat, a notes store, a file store, a clock.
struct Device {
  Device() : files(card) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    model.respondWith([](const sp::Recorded&, size_t) { return sp::say("ok"); });
    tools();
    pinned["rule-units"] = "metric units";
    store.push_back(tx::RecentFile{"journal/start.md", kEpoch0 - 86400});
  }

  // The head's own tools: note (pinned or not) and write_file change the stores they name.
  void tools() {
    const auto* n = plain::toolDecl("note");
    const auto* w = plain::toolDecl("write_file");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_NOT_NULL(w);
    reg.add(
        n->name, n->description,
        [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
          const std::string key = a["key"] | "", value = a["value"] | "";
          (a["pin"] | false ? pinned : unpinned)[key] = value;
          return nimbus::orch::ToolResult::ok(key + ": " + value);
        },
        plain::declSchema(*n, false));
    reg.add(
        w->name, w->description,
        [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
          const std::string path = a["path"] | "";
          store.push_back(tx::RecentFile{path, now()});
          return nimbus::orch::ToolResult::ok("stored " + path);
        },
        plain::declSchema(*w, false));
  }

  uint64_t now() const { return kEpoch0 + ms / 1000u; }
  void pause(uint32_t s) { ms += s * 1000u; }

  plain::Status boot(const std::string& chat) {
    resident = chat;
    plain::ChatBindingConfig c;
    c.chatKey = chat;
    c.model = "claude-haiku-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [](const std::string&, const std::string&) { return true; };
    d.key = [] { return std::string("sk-fake-cache"); };
    d.pinnedNotes = [this](const nimbus::orch::Principal&) {
      std::string out;
      for (const auto& kv : pinned) out += (out.empty() ? "" : "\n") + kv.first + ": " + kv.second;
      return out;
    };
    d.recentFiles = [this](const nimbus::orch::Principal&) -> tx::RecentFilesBlock::Source {
      return [this](std::vector<tx::RecentFile>& out) {
        out = store;
        return !storeDeaf;
      };
    };
    binding.reset();   // the device lets the resident chat go before the next one boots
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }
  // The resident chat is `chat` (a switch boots it).
  void resident_(const std::string& chat) {
    if (binding && resident == chat) return;
    TEST_ASSERT_TRUE(boot(chat).ok());
  }

  // One message to `chat`, answered by `steps` (a call or two, then text).
  void say(const std::string& chat, const std::string& text, std::vector<sp::Step> steps = {}) {
    resident_(chat);
    for (const sp::Step& s : steps) model.push(s);
    model.push(sp::say("done: " + text));
    const size_t before = model.requests().size();
    plain::ChatMessage m;
    m.text = text;
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(binding->turn(m, out).ok());
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)model.remaining(), "a turn left scripted steps unused");
    for (size_t i = before; i < model.requests().size(); ++i)
      sent.push_back(Sent{model.requests()[i].body, chat, now(), listed});
  }

  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  Card card;
  tx::CardTxFiles files;
  nimbus::orch::ToolRegistry reg;
  std::map<std::string, std::string> pinned, unpinned;
  std::vector<tx::RecentFile> store;
  bool storeDeaf = false;   // the file store does not answer
  std::unique_ptr<plain::ChatBinding> binding;
  std::string resident;
  std::vector<Sent> sent;
  int listed = 0;   // bumped by the test at every listed event it makes
};

size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
  return n;
}

std::string stripMarks(std::string s) {
  for (const char* m : {kMarkLong, kMark}) {
    const std::string mark = m;
    for (size_t at = s.find(mark); at != std::string::npos; at = s.find(mark, at)) s.erase(at, mark.size());
  }
  return s;
}

// The bytes of a request ahead of its last closed turn, cache markers left out: through the
// previous turn's point when the messages carry two, else through the start of the messages.
std::string aheadOfClosed(const std::string& body) {
  const size_t msgs = body.find("\"messages\":[");
  TEST_ASSERT_TRUE(msgs != std::string::npos);
  std::vector<size_t> marks;
  for (size_t at = body.find(",\"cache_control\":", msgs); at != std::string::npos; at = body.find(",\"cache_control\":", at + 1))
    marks.push_back(at);
  const size_t end = marks.size() >= 2 ? marks.front() : msgs;
  return stripMarks(body.substr(0, end));
}

// The system part of a request (the "system" array, markers left out).
std::string systemOf(const std::string& body) {
  const size_t a = body.find("\"system\":["), b = body.find("],\"tools\":[");
  TEST_ASSERT_TRUE(a != std::string::npos && b != std::string::npos && a < b);
  return stripMarks(body.substr(a, b - a));
}

// Rule A over `chat`'s requests: each one's bytes ahead of its last closed turn open the next one,
// unless the oracle counted a listed event between them or the provider's cache expired anyway
// (more than an hour and the margin between them). The first broken pair, or "".
std::string ruleA(const Device& dev, const std::string& chat) {
  const Sent* prev = nullptr;
  int n = 0;
  for (const Sent& s : dev.sent) {
    if (s.chat != chat) continue;
    ++n;
    if (prev) {
      const bool excused = s.listed != prev->listed || s.at - prev->at > kExpiredAfterS;
      const std::string ahead = aheadOfClosed(prev->body);
      if (!excused && stripMarks(s.body).compare(0, ahead.size(), ahead) != 0) {
        const std::string a = systemOf(prev->body), b = systemOf(s.body);
        return "request " + std::to_string(n) + " of chat " + chat + " rewrote the bytes ahead of the closed turns" +
               (a != b ? " (the system part changed)" : " (inside the messages)");
      }
    }
    prev = &s;
  }
  return std::string();
}

// One generated day: actions drawn from the list rule A must hold over.
enum class Act { Chat, PinnedNote, UnpinnedNote, WriteFile, Switch, Restart, Pause, RoutineElsewhere };

std::string runGenerated(uint32_t seed, int steps, std::vector<Act>* only = nullptr) {
  Device dev;
  std::mt19937 rng(seed);
  dev.say("1001", "hello");
  for (int i = 0; i < steps; ++i) {
    const Act a = only ? (*only)[i % only->size()] : (Act)(rng() % 8);
    const std::string n = std::to_string(i);
    switch (a) {
      case Act::Chat:
        dev.pause(20 + rng() % 200);
        dev.say("1001", "question " + n);
        break;
      case Act::PinnedNote:
        dev.pause(30 + rng() % 120);
        dev.say("1001", "remember pinned " + n,
                {sp::call("note", "{\"op\":\"set\",\"key\":\"pin-" + n + "\",\"value\":\"v" + n + "\",\"kind\":\"fact\",\"pin\":true}")});
        break;
      case Act::UnpinnedNote:
        dev.pause(30 + rng() % 120);
        dev.say("1001", "remember " + n,
                {sp::call("note", "{\"op\":\"set\",\"key\":\"fact-" + n + "\",\"value\":\"v" + n + "\",\"kind\":\"fact\",\"pin\":false}")});
        break;
      case Act::WriteFile:
        dev.pause(30 + rng() % 120);
        dev.say("1001", "save file " + n, {sp::call("write_file", "{\"path\":\"notes/f" + n + ".md\",\"content\":\"x\"}")});
        break;
      case Act::Switch:   // the other chat becomes resident, answers, then this one comes back
        dev.pause(10 + rng() % 60);
        dev.say("2002", "telegram " + n);
        dev.pause(10 + rng() % 60);
        dev.say("1001", "back on the web " + n);
        break;
      case Act::Restart:
        dev.pause(5 + rng() % 60);
        dev.binding.reset();
        dev.say("1001", "after a restart " + n);
        break;
      case Act::Pause: {   // any length: seconds to hours
        static const uint32_t kGaps[] = {200, 280, 600, 1200, 2400, 3500, 3700, 7200, 4 * 3600};
        dev.pause(kGaps[rng() % 9]);
        dev.say("1001", "after a pause " + n);
        break;
      }
      case Act::RoutineElsewhere:   // a routine's small request in another chat
        dev.pause(60 + rng() % 600);
        dev.say("3003", "routine fire " + n);
        dev.pause(30);
        dev.say("1001", "after the routine " + n);
        break;
    }
  }
  return ruleA(dev, "1001");
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the checker bites ----------------------------------------------------------------
static void test_the_checker_catches_a_silent_rewrite_and_the_oracle_excuses_a_listed_one() {
  Device dev;
  dev.say("1001", "one");
  dev.pause(30);
  dev.say("1001", "two");
  TEST_ASSERT_EQUAL_STRING("", ruleA(dev, "1001").c_str());
  // The same two requests with one system byte changed in the second.
  Sent bad = dev.sent.back();
  const size_t at = bad.body.find("metric units");
  TEST_ASSERT_TRUE(at != std::string::npos);
  bad.body[at] = 'M';
  dev.sent.back() = bad;
  TEST_ASSERT_TRUE(ruleA(dev, "1001").find("system part") != std::string::npos);
  dev.sent.back().listed = 1;   // a listed event between them excuses it
  TEST_ASSERT_EQUAL_STRING("", ruleA(dev, "1001").c_str());
}

// ---- rule A, cause by cause (each is red on the tree before the lane where marked) -------
static void test_a_pinned_note_the_head_saves_keeps_the_prefix() {   // red before
  std::vector<Act> acts{Act::PinnedNote, Act::Chat, Act::PinnedNote, Act::Chat};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(1, 4, &acts).c_str());
}
static void test_an_unpinned_note_keeps_the_prefix() {
  std::vector<Act> acts{Act::UnpinnedNote, Act::Chat};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(2, 4, &acts).c_str());
}
static void test_a_file_it_writes_then_a_chat_switch_keeps_the_prefix() {   // red before
  std::vector<Act> acts{Act::WriteFile, Act::Switch};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(3, 4, &acts).c_str());
}
static void test_a_file_it_writes_then_a_restart_keeps_the_prefix() {   // red before
  std::vector<Act> acts{Act::WriteFile, Act::Restart};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(4, 4, &acts).c_str());
}
static void test_a_file_it_writes_then_a_pause_keeps_the_prefix() {   // red before
  std::vector<Act> acts{Act::WriteFile, Act::Pause};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(5, 6, &acts).c_str());
}
static void test_a_routine_in_another_chat_keeps_the_prefix() {
  std::vector<Act> acts{Act::WriteFile, Act::RoutineElsewhere};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(6, 4, &acts).c_str());
}

// ---- rule A over generated sequences ----------------------------------------------------
static void test_rule_a_holds_over_generated_days() {
  for (uint32_t seed = 100; seed < 130; ++seed) {
    const std::string why = runGenerated(seed, 14);
    char msg[256];
    std::snprintf(msg, sizeof msg, "seed %u: %s", (unsigned)seed, why.c_str());
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), msg);
  }
}

// ---- counter-tests: each listed event does change the bytes ahead ----------------------
static void test_a_note_changed_from_outside_reaches_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  dev.pause(30);
  dev.pinned["rule-web"] = "set on the web page";   // the owner, on the web page
  dev.say("1001", "two");
  TEST_ASSERT_TRUE(dev.sent.back().body.find("set on the web page") != std::string::npos);
  TEST_ASSERT_TRUE(systemOf(dev.sent[0].body) != systemOf(dev.sent.back().body));
}
static void test_a_file_from_outside_reaches_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  dev.pause(30);
  dev.store.push_back(tx::RecentFile{"inbox/upload.csv", dev.now()});   // the web Files panel
  dev.say("1001", "two");
  TEST_ASSERT_TRUE(dev.sent.back().body.find("inbox/upload.csv") != std::string::npos);
}
static void test_a_note_saved_in_another_chat_reaches_this_one() {
  Device dev;
  dev.say("1001", "one");
  dev.say("2002", "remember", {sp::call("note", R"({"op":"set","key":"tg-pin","value":"from telegram","pin":true})")});
  dev.say("1001", "two");
  TEST_ASSERT_TRUE(dev.sent.back().body.find("from telegram") != std::string::npos);
}
static void test_a_new_tool_changes_the_prefix() {
  Device dev;
  dev.say("1001", "one");
  const auto* d = plain::toolDecl("read_file");
  dev.reg.add(d->name, d->description,
              [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) { return nimbus::orch::ToolResult::ok("x"); },
              plain::declSchema(*d, false));
  dev.pause(30);
  dev.say("1001", "two");
  TEST_ASSERT_TRUE(stripMarks(dev.sent.back().body).compare(0, aheadOfClosed(dev.sent[0].body).size(),
                                                           aheadOfClosed(dev.sent[0].body)) != 0);
}
#ifdef E2_CACHE_TREE
static void test_an_expired_cache_brings_the_blocks_up_to_date() {
  Device dev;
  dev.say("1001", "one", {sp::call("note", R"({"op":"set","key":"own-pin","value":"saved by the head","pin":true})")});
  dev.pause(60);
  dev.say("1001", "two");
  TEST_ASSERT_TRUE_MESSAGE(systemOf(dev.sent.back().body).find("saved by the head") == std::string::npos,
                           "a warm turn re-rendered the head's own note");
  dev.pause(plain::kCacheLongS + plain::kCacheExpiryMarginS + 10);
  dev.say("1001", "three");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("saved by the head") != std::string::npos);
}
static void test_a_file_store_that_does_not_answer_is_not_an_empty_list() {
  Device dev;
  dev.say("1001", "one");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("journal/start.md") != std::string::npos);
  dev.storeDeaf = true;
  dev.pause(30);
  dev.say("1001", "two", {sp::call("write_file", R"({"path":"notes/x.md","content":"x"})")});
  dev.pause(30);
  dev.say("1001", "three");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("journal/start.md") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", ruleA(dev, "1001").c_str());
}
static void test_clear_starts_a_new_record() {
  Device dev;
  dev.say("1001", "one", {sp::call("note", R"({"op":"set","key":"own-pin","value":"saved by the head","pin":true})")});
  dev.say("1001_c1", "after clear");   // /clear's new conversation key
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("saved by the head") != std::string::npos);
}
static void test_the_decision_names_every_listed_event() {
  plain::FrozenPrefix f;
  plain::PrefixNow n;
  n.blocks.core = "core";
  n.blocks.notes = "notes";
  n.digestText = "d";
  n.files = "a.md";
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::First, (int)plain::decidePrefix(f, n));
  plain::renderPrefix(f, n);
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::None, (int)plain::decidePrefix(f, n));
  plain::PrefixNow m = n;
  m.manifestHash = 9;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Connector, (int)plain::decidePrefix(f, m));
  m = n, m.blocks.core = "core2";
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Connector, (int)plain::decidePrefix(f, m));
  m = n, m.digestText = "d2";
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Compaction, (int)plain::decidePrefix(f, m));
  m = n, m.stub = 7;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Compaction, (int)plain::decidePrefix(f, m));
  m = n, m.dropped = 7;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Compaction, (int)plain::decidePrefix(f, m));
  m = n, m.blocks.notes = "notes2";
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Notes, (int)plain::decidePrefix(f, m));
  m = n, m.files = "b.md";
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Files, (int)plain::decidePrefix(f, m));
  // The head's own note was seen at its turn's close: no render while warm, one once expired.
  plain::FrozenPrefix own = f;
  own.seenNotes = "notes2";
  m = n, m.blocks.notes = "notes2", m.now = 10000, m.lastReqAt = 10000 - 600, m.lifetimeS = plain::kCacheLongS;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::None, (int)plain::decidePrefix(own, m));
  m.lastReqAt = 10000 - plain::kCacheLongS - plain::kCacheExpiryMarginS - 1;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Expired, (int)plain::decidePrefix(own, m));
  m.lastReqAt = 0;   // no last request known: never taken as expired
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::None, (int)plain::decidePrefix(own, m));
}

// ---- the cache points ------------------------------------------------------------------
static void test_the_stable_points_keep_an_hour_and_the_moving_one_five_minutes() {
  Device dev;
  dev.say("1001", "one");
  dev.say("1001", "two", {sp::call("note", R"({"op":"set","key":"k","value":"v","pin":false})")});
  for (const Sent& s : dev.sent) {
    TEST_ASSERT_TRUE(countOf(s.body, ",\"cache_control\":") <= 4);
    // Longer lifetimes first in request order: no 1-hour mark after a 5-minute one.
    const size_t lastLong = s.body.rfind(kMarkLong), firstShort = s.body.find(kMark);
    TEST_ASSERT_TRUE(firstShort != std::string::npos);
    TEST_ASSERT_TRUE(lastLong == std::string::npos || lastLong < firstShort);
    TEST_ASSERT_EQUAL(1, (int)countOf(s.body, kMark));   // the moving last block only
  }
  TEST_ASSERT_EQUAL(3, (int)countOf(dev.sent.back().body, kMarkLong));   // notes, system end, the previous turn
}

// ---- the record --------------------------------------------------------------------------
static void test_a_torn_record_is_rendered_afresh_and_replaced() {
  Device dev;
  dev.say("1001", "one");
  const std::string path = "/mem/tx/1001/prefix.txt";
  TEST_ASSERT_TRUE(dev.card.files.count(path) == 1);
  dev.card.files[path].resize(dev.card.files[path].size() / 2);   // a cut write
  dev.binding.reset();
  dev.say("1001", "two");
  plain::FrozenPrefix f;
  TEST_ASSERT_TRUE(plain::decodePrefix(dev.card.files[path], f).ok());
}
static void test_an_unreadable_card_is_never_absent() {
  plain::FrozenPrefix f;
  f.valid = true;
  f.blocks.core = "core";
  Card card;
  tx::CardTxFiles files(card);
  card.dirs.insert("/mem");
  card.dirs.insert("/mem/tx");
  card.dirs.insert("/mem/tx/1001");
  TEST_ASSERT_TRUE(plain::savePrefix(files, "1001", f).ok());
  card.deaf = true;
  plain::FrozenPrefix back;
  TEST_ASSERT_EQUAL((int)tx::Err::Io, (int)plain::loadPrefix(files, "1001", back).err);
  card.deaf = false;
  TEST_ASSERT_TRUE(plain::loadPrefix(files, "1001", back).ok());
  TEST_ASSERT_EQUAL_STRING("core", back.blocks.core.c_str());
  card.files.erase("/mem/tx/1001/prefix.txt");
  TEST_ASSERT_EQUAL((int)tx::Err::NotFound, (int)plain::loadPrefix(files, "1001", back).err);
}
static void test_the_record_round_trips_any_bytes() {
  plain::FrozenPrefix f;
  f.valid = true;
  f.blocks.core = "core\nwith lines\n";
  f.blocks.notes = std::string("notes \x01 z 1\n", 13);
  f.blocks.digests = "";
  f.manifestHash = 0xFFFFFFFFu;
  f.digestText = "seen-notes 3\nabc\n";   // a field's own header inside a value
  f.seenNotes = "n";
  f.seenFiles = "f";
  f.stub = 12;
  f.dropped = 34;
  std::string text;
  TEST_ASSERT_TRUE(plain::encodePrefix(f, text).ok());
  plain::FrozenPrefix back;
  TEST_ASSERT_TRUE(plain::decodePrefix(text, back).ok());
  TEST_ASSERT_TRUE(back.blocks.core == f.blocks.core && back.blocks.notes == f.blocks.notes && back.digestText == f.digestText);
  TEST_ASSERT_EQUAL(0xFFFFFFFFu, back.manifestHash);
  TEST_ASSERT_EQUAL(34, (int)back.dropped);
  text[text.size() / 2] ^= 1;
  TEST_ASSERT_EQUAL((int)tx::Err::Torn, (int)plain::decodePrefix(text, back).err);
}

#endif


// Lane E2-CMP: a compaction renders the frozen system blocks afresh exactly once (the summary is new in them),
// and from the next turn on the requests are byte-stable again ahead of their closed turns (rule A).
static void test_a_compaction_renders_the_blocks_afresh_once_then_they_hold() {
  Device dev;
  for (int i = 0; i < 9; ++i) {
    dev.pause(30);
    dev.say("1001", "message " + std::to_string(i) + " about the garden");
  }
  const std::string before = systemOf(dev.sent.back().body);
  dev.model.push(sp::say("+F The person writes about the garden."));
  TEST_ASSERT_TRUE(dev.binding->compactNow().ok());
  const size_t first = dev.sent.size();
  for (int i = 0; i < 3; ++i) {
    dev.pause(30);
    dev.say("1001", "after the summary " + std::to_string(i));
  }
  TEST_ASSERT_EQUAL(first + 3, dev.sent.size());
  const std::string after = systemOf(dev.sent[first].body);
  TEST_ASSERT_TRUE_MESSAGE(after != before, "the compaction rendered the blocks afresh");
  TEST_ASSERT_TRUE(after.find("The person writes about the garden.") != std::string::npos);
  for (size_t i = first + 1; i < dev.sent.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(after.c_str(), systemOf(dev.sent[i].body).c_str(), "rendered again after the compaction");
    const std::string ahead = aheadOfClosed(dev.sent[i - 1].body);
    TEST_ASSERT_TRUE_MESSAGE(stripMarks(dev.sent[i].body).compare(0, ahead.size(), ahead) == 0,
                             "the bytes ahead of the closed turns changed after the compaction");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_checker_catches_a_silent_rewrite_and_the_oracle_excuses_a_listed_one);
  RUN_TEST(test_a_pinned_note_the_head_saves_keeps_the_prefix);
  RUN_TEST(test_an_unpinned_note_keeps_the_prefix);
  RUN_TEST(test_a_file_it_writes_then_a_chat_switch_keeps_the_prefix);
  RUN_TEST(test_a_file_it_writes_then_a_restart_keeps_the_prefix);
  RUN_TEST(test_a_file_it_writes_then_a_pause_keeps_the_prefix);
  RUN_TEST(test_a_routine_in_another_chat_keeps_the_prefix);
  RUN_TEST(test_rule_a_holds_over_generated_days);
  RUN_TEST(test_a_note_changed_from_outside_reaches_the_next_request);
  RUN_TEST(test_a_file_from_outside_reaches_the_next_request);
  RUN_TEST(test_a_note_saved_in_another_chat_reaches_this_one);
  RUN_TEST(test_a_new_tool_changes_the_prefix);
#ifdef E2_CACHE_TREE
  RUN_TEST(test_an_expired_cache_brings_the_blocks_up_to_date);
  RUN_TEST(test_a_file_store_that_does_not_answer_is_not_an_empty_list);
  RUN_TEST(test_clear_starts_a_new_record);
  RUN_TEST(test_the_decision_names_every_listed_event);
  RUN_TEST(test_the_stable_points_keep_an_hour_and_the_moving_one_five_minutes);
  RUN_TEST(test_a_torn_record_is_rendered_afresh_and_replaced);
  RUN_TEST(test_an_unreadable_card_is_never_absent);
  RUN_TEST(test_the_record_round_trips_any_bytes);
  RUN_TEST(test_a_compaction_renders_the_blocks_afresh_once_then_they_hold);
#endif
  return UNITY_END();
}
