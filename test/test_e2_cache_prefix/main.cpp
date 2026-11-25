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

#include <algorithm>
#include <cstdlib>
#include <functional>
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
#if __has_include("nimbus/harness/plain/engine.h")
#include "nimbus/harness/plain/engine.h"
#endif
#ifndef E2_FIX10_BASE
#define E2_FIX10_TREE 1   // TurnInput::stored exists (built with -DE2_FIX10_BASE on the tree before it: red)
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
  bool refuseAppends = false;   // every append is refused (the transcript's items stay in RAM only)
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
    if (refuseAppends) return 0;
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
    store.push_back(tx::RecentFile{"lists/shopping.md", kEpoch0 - 2 * 3600});
    store.push_back(tx::RecentFile{"log/daily.md", kEpoch0 - 3 * 3600});
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
          if (during) during();
          const std::string key = a["key"] | "", value = a["value"] | "", op = a["op"] | "set";
          if (op == "remove") {
            pinned.erase(key);
            unpinned.erase(key);
            return nimbus::orch::ToolResult::ok("removed " + key);
          }
          (a["pin"] | false ? pinned : unpinned)[key] = value;
          return nimbus::orch::ToolResult::ok("saved: " + key + ": " + value + " (12 of 4096 B)");
        },
        plain::declSchema(*n, false));
    reg.add(
        w->name, w->description,
        [this](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
          if (during) during();
          const std::string path = a["path"] | "";
          if (failFiles) return nimbus::orch::ToolResult::fail("not stored " + path);
          touch(path);
          return nimbus::orch::ToolResult::ok("stored " + path);
        },
        plain::declSchema(*w, false));
    // Lane E2-FIX8: an append, an edit and a delete of an existing file (a write to an existing path is an
    // overwrite). Each stamps the path's write time (the store's createdAt) or takes the path out; with
    // failFiles set each answers an error and changes nothing.
    for (const char* name : {"append_file", "edit_file", "delete_file"}) {
      const auto* d = plain::toolDecl(name);
      TEST_ASSERT_NOT_NULL(d);
      const bool del = std::string(name) == "delete_file";
      reg.add(
          d->name, d->description,
          [this, del](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) {
            if (during) during();
            const std::string path = a["path"] | "";
            if (failFiles || !has(path)) return nimbus::orch::ToolResult::fail("no change to " + path);
            if (del) drop(path);
            else touch(path);
            return nimbus::orch::ToolResult::ok((del ? "deleted " : "changed ") + path);
          },
          plain::declSchema(*d, false));
    }
    const auto* r = plain::toolDecl("read_file");   // a read: shows what is there, never the head's own change
    reg.add(
        r->name, r->description,
        [this](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
          if (during) during();
          std::string out;
          for (const auto& kv : pinned) out += kv.first + ": " + kv.second + " ";
          return nimbus::orch::ToolResult::ok(out);
        },
        plain::declSchema(*r, false));
  }

  bool has(const std::string& path) const {
    return std::any_of(store.begin(), store.end(), [&](const tx::RecentFile& f) { return f.path == path; });
  }
  void drop(const std::string& path) {
    store.erase(std::remove_if(store.begin(), store.end(), [&](const tx::RecentFile& f) { return f.path == path; }),
                store.end());
  }
  // A write to `path` now: created, or its write time stamped (the store keeps one entry per path).
  void touch(const std::string& path) {
    drop(path);
    store.push_back(tx::RecentFile{path, now()});
  }
  // Fills the store past the list's 600 B with files written a day or more ago, oldest last.
  void fill(int n) {
    for (int i = 0; i < n; ++i)
      store.push_back(tx::RecentFile{"archive/old-" + std::to_string(i) + ".md", kEpoch0 - 2 * 86400 - (uint64_t)i * 3600});
  }
  uint64_t now() const { return kEpoch0 + ms / 1000u; }
  // The receive path stores `paths` for the next message to `chat` before its turn opens (a photo, an
  // album, a document); `own` false: stored for another chat's message (not passed with this one).
  void arrive(const std::vector<std::string>& paths, bool own = true) {
    for (const std::string& p : paths) touch(p);
    if (own) arriving.insert(arriving.end(), paths.begin(), paths.end());
  }
  void pause(uint32_t s) { ms += s * 1000u; }

  plain::Status boot(const std::string& chat) {
    resident = chat;
    plain::ChatBindingConfig c;
    c.provider = "anthropic";   // E2-R8a: the head slot has no default
    c.host = "api.anthropic.com";
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
#ifdef E2_FIX10_TREE
    m.stored = arriving;   // lane E2-FIX10: what the receive path stored for this message
#endif
    arriving.clear();
    plain::TurnOutcome out;
    const plain::Status st = binding->turn(m, out);
    if (!tolerateFaults) TEST_ASSERT_TRUE(st.ok());
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
  bool failFiles = false;   // every file call answers an error and changes nothing
  std::vector<std::string> arriving;   // paths stored for the next message (TurnInput::stored)
  std::unique_ptr<plain::ChatBinding> binding;
  std::string resident;
  std::vector<Sent> sent;
  int listed = 0;   // bumped by the test at every listed event it makes
  bool tolerateFaults = false;     // a card fault may end the turn with an error status
  std::function<void()> during;   // runs inside the head's tool call (an edit on the web page meanwhile)
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
enum class Act {
  Chat, PinnedNote, UnpinnedNote, WriteFile, Switch, Restart, Pause, RoutineElsewhere,
  AppendFile, EditFile, OverwriteFile, DeleteFile,   // lane E2-FIX8: changes to files that exist
  PhotoArrives, DocumentArrives                       // lane E2-FIX10: stored for the message that opens the turn
};
constexpr uint32_t kActs = 14;

// A file call's arguments as the tool's schema asks for them.
std::string fileArgs(const std::string& tool, const std::string& path) {
  const std::string p = "{\"path\":\"" + path + "\"";
  if (tool == "append_file") return p + ",\"text\":\"one more line\"}";
  if (tool == "edit_file") return p + ",\"old\":\"a\",\"new\":\"b\"}";
  if (tool == "write_file") return p + ",\"content\":\"x\"}";
  return p + "}";
}

// A file the store holds now, drawn by `r` ("" when it holds none).
std::string existing(const Device& dev, uint32_t r) {
  return dev.store.empty() ? std::string() : dev.store[r % dev.store.size()].path;
}

// The head appends to, edits, overwrites or deletes a file the store holds (writes a new one if none).
void changeExisting(Device& dev, std::mt19937& rng, Act a, const std::string& n) {
  static const char* const kTool[] = {"append_file", "edit_file", "write_file", "delete_file"};
  const std::string path = existing(dev, rng());
  dev.pause(30 + rng() % 120);
  const char* tool = path.empty() ? "write_file" : kTool[(int)a - (int)Act::AppendFile];
  dev.say("1001", std::string(tool) + " " + n, {sp::call(tool, fileArgs(tool, path.empty() ? "notes/f" + n + ".md" : path))});
}

// A message arrives with photos (one to three, an album) or a document the device stored for it.
void arrivalAct(Device& dev, std::mt19937& rng, Act a, const std::string& n) {
  dev.pause(30 + rng() % 600);
  std::vector<std::string> paths;
  if (a == Act::DocumentArrives) paths.push_back("inbox/" + n + "-report.pdf");
  for (uint32_t k = 0, many = 1 + rng() % 3; a == Act::PhotoArrives && k < many; ++k)
    paths.push_back("chat-1001/photo-" + n + std::to_string(k) + ".jpg");
  dev.arrive(paths);
  dev.say("1001", "[media " + n + "]");
}

// The head saves a note, pinned or not.
void noteAct(Device& dev, std::mt19937& rng, bool pin, const std::string& n) {
  const std::string key = (pin ? "pin-" : "fact-") + n, flag = pin ? "true" : "false";
  dev.pause(30 + rng() % 120);
  dev.say("1001", (pin ? "remember pinned " : "remember ") + n,
          {sp::call("note", "{\"op\":\"set\",\"key\":\"" + key + "\",\"value\":\"v" + n + "\",\"kind\":\"fact\",\"pin\":" + flag + "}")});
}

// One step that is not a change to the stores: a question, a write, a switch, a restart, a pause, a
// routine elsewhere.
void chatAct(Device& dev, std::mt19937& rng, Act a, const std::string& n) {
  switch (a) {
    case Act::Chat:
      dev.pause(20 + rng() % 200);
      dev.say("1001", "question " + n);
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
    default:   // RoutineElsewhere: a routine's small request in another chat
      dev.pause(60 + rng() % 600);
      dev.say("3003", "routine fire " + n);
      dev.pause(30);
      dev.say("1001", "after the routine " + n);
      break;
  }
}

std::string runGenerated(uint32_t seed, int steps, std::vector<Act>* only = nullptr) {
  Device dev;
  std::mt19937 rng(seed);
  if (seed % 2) dev.fill(30);   // half the days with a full list: a create pushes a line out, a delete lets one in
  dev.say("1001", "hello");
  for (int i = 0; i < steps; ++i) {
    const Act a = only ? (*only)[i % only->size()] : (Act)(rng() % kActs);
    const std::string n = std::to_string(i);
    if (a == Act::PinnedNote || a == Act::UnpinnedNote) noteAct(dev, rng, a == Act::PinnedNote, n);
    else if (a >= Act::PhotoArrives) arrivalAct(dev, rng, a, n);
    else if (a >= Act::AppendFile) changeExisting(dev, rng, a, n);
    else chatAct(dev, rng, a, n);
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
  const auto* d = plain::toolDecl("list_files");
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
// ---- lane E2-FIX6: what may be taken as the turn's own (red on a4f9c0621) ------------------
// C2/C5: an edit on the web page while a turn with calls runs is never the head's own.
static void test_an_outside_note_added_during_a_tool_turn_reaches_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  dev.during = [&dev] { dev.pinned["rule-web"] = "added on the web page meanwhile"; };
  dev.say("1001", "save a file", {sp::call("write_file", R"({"path":"notes/a.md","content":"x"})")});
  dev.during = nullptr;
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("added on the web page meanwhile") != std::string::npos);
}
static void test_an_outside_rule_removed_during_a_tool_turn_leaves_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("metric units") != std::string::npos);
  dev.during = [&dev] { dev.pinned.erase("rule-units"); };
  dev.say("1001", "remember", {sp::call("note", R"({"op":"set","key":"k1","value":"v1","pin":true})")});
  dev.during = nullptr;
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("metric units") == std::string::npos);
}
static void test_an_outside_file_during_a_tool_turn_reaches_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  dev.during = [&dev] { dev.store.push_back(tx::RecentFile{"inbox/upload.csv", dev.now()}); };
  dev.say("1001", "save", {sp::call("write_file", R"({"path":"notes/b.md","content":"x"})")});
  dev.during = nullptr;
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("inbox/upload.csv") != std::string::npos);
}
// The reviewer's case: a read in the turn shows the outside line; a read is never the head's own.
static void test_an_outside_note_a_read_shows_is_still_not_the_heads_own() {
  Device dev;
  dev.say("1001", "one");
  dev.during = [&dev] { dev.pinned["rule-web"] = "added meanwhile"; };
  dev.say("1001", "read it", {sp::call("read_file", R"({"path":"notes/a.md"})")});
  dev.during = nullptr;
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("added meanwhile") != std::string::npos);
}
// C6: a note the head removes or edits does not stay in the system part as in force.
static void test_a_note_the_head_removes_leaves_the_next_request() {
  Device dev;
  dev.say("1001", "one");
  dev.say("1001", "forget the units rule", {sp::call("note", R"({"op":"remove","key":"rule-units"})")});
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("metric units") == std::string::npos);
}
static void test_a_note_the_head_edits_shows_only_its_new_value() {
  Device dev;
  dev.say("1001", "one");
  dev.say("1001", "change it", {sp::call("note", R"({"op":"set","key":"rule-units","value":"imperial units","pin":true})")});
  dev.pause(30);
  dev.say("1001", "next");
  const std::string sys = systemOf(dev.sent.back().body);
  TEST_ASSERT_TRUE(sys.find("metric units") == std::string::npos);
  TEST_ASSERT_TRUE(sys.find("imperial units") != std::string::npos);
}
// C1/C7: a note saved in a turn whose items the card refused, then a restart: the note shows.
static void test_a_note_saved_while_the_card_refused_the_turn_shows_after_a_restart() {
  Device dev;
  dev.say("1001", "one");
  dev.tolerateFaults = true;
  dev.during = [&dev] { dev.card.refuseAppends = true; };   // the call is stored, its result and the rest are not
  dev.say("1001", "remember", {sp::call("note", R"({"op":"set","key":"k-unstored","value":"kept in notes","pin":true})")});
  dev.during = nullptr;
  dev.card.refuseAppends = false;
  TEST_ASSERT_TRUE_MESSAGE(dev.pinned.count("k-unstored") == 1, "the note call did not run");
  dev.binding.reset();   // a restart: the RAM-only items are gone
  dev.pause(30);
  dev.say("1001", "next");
  TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("kept in notes") != std::string::npos);
}
// The cache benefit stays: a note the head adds and a file it writes do not rewrite the prefix.
static void test_own_additions_still_keep_the_prefix() {
  std::vector<Act> acts{Act::PinnedNote, Act::WriteFile, Act::Chat, Act::Switch, Act::Restart};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(9, 10, &acts).c_str());
}
// C3: a replace cut after the old record left: the temporary copy is read, not taken as absent.
static void test_a_record_left_only_as_its_temporary_copy_is_read() {
  plain::FrozenPrefix f;
  f.valid = true;
  f.blocks.core = "core";
  Card card;
  tx::CardTxFiles files(card);
  for (const char* d : {"/mem", "/mem/tx", "/mem/tx/1001"}) card.dirs.insert(d);
  TEST_ASSERT_TRUE(plain::savePrefix(files, "1001", f).ok());
  card.files["/mem/tx/1001/prefix.txt.tmp"] = card.files["/mem/tx/1001/prefix.txt"];
  card.files.erase("/mem/tx/1001/prefix.txt");
  plain::FrozenPrefix back;
  TEST_ASSERT_TRUE(plain::loadPrefix(files, "1001", back).ok());
  TEST_ASSERT_EQUAL_STRING("core", back.blocks.core.c_str());
  card.files["/mem/tx/1001/prefix.txt.tmp"].resize(10);   // a torn temporary copy: absent
  TEST_ASSERT_EQUAL((int)tx::Err::NotFound, (int)plain::loadPrefix(files, "1001", back).err);
}
// ---- lane E2-FIX8: a change to existing files by the turn's own successful calls (red on 970a707e9) ----
// One turn whose only call is `tool` on `path`, between two plain turns; `full`: the store holds more
// files than the list's 600 B shows. `during` runs inside the call (an outside change meanwhile).
std::string ownChange(const char* tool, const std::string& path, bool full, std::function<void(Device&)> during = nullptr,
                      bool fail = false) {
  Device dev;
  if (full) dev.fill(30);
  dev.say("1001", "one");
  dev.pause(90);
  if (during) dev.during = [&dev, during] { during(dev); };
  dev.failFiles = fail;
  dev.say("1001", std::string("please ") + tool, {sp::call(tool, fileArgs(tool, path))});
  dev.during = nullptr;
  dev.failFiles = false;
  dev.pause(90);
  dev.say("1001", "next");
  return ruleA(dev, "1001");
}
struct OwnCase {
  const char* tool;
  const char* path;
  const char* what;
};
const OwnCase kOwnCases[] = {
    {"write_file", "notes/new.md", "create"},        {"append_file", "log/daily.md", "append"},
    {"edit_file", "lists/shopping.md", "edit"},      {"write_file", "journal/start.md", "overwrite"},
    {"delete_file", "journal/start.md", "delete"},   {"append_file", "journal/start.md", "append to the last listed of the three"},
};
static void test_create_append_edit_overwrite_delete_by_the_heads_own_call_keep_the_prefix() {
  std::string broken;   // every case that rewrote, named
  for (bool full : {false, true})
    for (const OwnCase& c : kOwnCases) {
      const std::string why = ownChange(c.tool, c.path, full);
      if (!why.empty()) broken += std::string(" [") + c.what + (full ? ", full list" : "") + "]";
    }
  TEST_ASSERT_EQUAL_STRING("", broken.c_str());
}
// Several own changes in one turn, and own appends over several turns in a row (the bench's photo case).
static void test_own_appends_turn_after_turn_keep_the_prefix() {
  Device dev;
  dev.fill(30);
  dev.say("1001", "one");
  for (int i = 0; i < 4; ++i) {
    dev.pause(70);
    dev.say("1001", "photo " + std::to_string(i),
            {sp::call("append_file", fileArgs("append_file", "log/daily.md")),
             sp::call("edit_file", fileArgs("edit_file", "lists/shopping.md"))});
  }
  dev.pause(70);
  dev.say("1001", "next");
  TEST_ASSERT_EQUAL_STRING("", ruleA(dev, "1001").c_str());
  std::vector<Act> acts{Act::AppendFile, Act::Chat, Act::DeleteFile, Act::EditFile, Act::OverwriteFile, Act::Restart};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(11, 12, &acts).c_str());
}
// Counter-tests: the same changes made from outside, by a failed call, or to a path the turn did not
// name, render afresh at the next open (rule A breaks with no listed event: the system part moved).
static void test_a_file_changed_from_outside_still_renders() {
  // between turns: the web page appends to a listed file
  {
    Device dev;
    dev.say("1001", "one");
    dev.pause(90);
    dev.touch("log/daily.md");
    dev.say("1001", "two");
    TEST_ASSERT_TRUE(ruleA(dev, "1001").find("system part") != std::string::npos);
  }
  // during a turn whose own call appended to another file
  TEST_ASSERT_TRUE(ownChange("append_file", "log/daily.md", false, [](Device& d) { d.touch("lists/shopping.md"); })
                       .find("system part") != std::string::npos);
  // during a turn whose own call deleted a file: an outside delete of another listed file
  TEST_ASSERT_TRUE(ownChange("delete_file", "journal/start.md", false, [](Device& d) { d.drop("lists/shopping.md"); })
                       .find("system part") != std::string::npos);
  // a full list: the head deletes a file while a file below the list's size is written from outside
  TEST_ASSERT_TRUE(ownChange("delete_file", "journal/start.md", true, [](Device& d) { d.touch("archive/old-25.md"); })
                       .find("system part") != std::string::npos);
  // a full list: the head deletes a file while the oldest file, below the list's size, is deleted from
  // outside: no listed line changes but the one the delete lets in, so nothing is hidden and nothing renders
  TEST_ASSERT_EQUAL_STRING("", ownChange("delete_file", "journal/start.md", true, [](Device& d) { d.drop("archive/old-29.md"); }).c_str());
}
static void test_a_failed_file_call_is_not_the_turns_own() {
  // the call failed; the same path changed meanwhile from outside
  TEST_ASSERT_TRUE(ownChange("append_file", "log/daily.md", false, [](Device& d) { d.touch("log/daily.md"); }, true)
                       .find("system part") != std::string::npos);
  TEST_ASSERT_TRUE(ownChange("delete_file", "journal/start.md", false, [](Device& d) { d.drop("journal/start.md"); }, true)
                       .find("system part") != std::string::npos);
  // a failed call that changed nothing leaves nothing to render
  TEST_ASSERT_EQUAL_STRING("", ownChange("append_file", "log/daily.md", false, nullptr, true).c_str());
}
// ---- lane E2-FIX10: files the device stored for the message that opens the turn (red on 6ac66b2b0) ----
// One message carrying `paths` (stored before its turn opens) between two plain turns; `full`: the
// list is past its 600 B. `before` runs just before that message arrives (an outside change meanwhile).
std::string arrivalTurn(const std::vector<std::string>& paths, bool full, std::function<void(Device&)> before = nullptr) {
  Device dev;
  if (full) dev.fill(30);
  dev.say("1001", "one");
  dev.pause(90);
  if (before) before(dev);
  dev.arrive(paths);
  dev.say("1001", "[image 1/1] (attached: the photo itself is in this message)");
  dev.pause(90);
  dev.say("1001", "next");
  return ruleA(dev, "1001");
}
static void test_a_photo_an_album_or_a_document_that_opens_the_turn_keeps_the_prefix() {
  const std::vector<std::vector<std::string>> cases{
      {"chat-1001/photo-501.jpg"},
      {"chat-1001/photo-502.jpg", "chat-1001/photo-503.jpg", "chat-1001/photo-504.jpg"},
      {"inbox/505-quote.pdf"},
      {"inbox/506.txt"}};   // a long paste kept in the inbox
  std::string broken;
  for (bool full : {false, true})
    for (const auto& c : cases)
      if (!arrivalTurn(c, full).empty()) broken += " [" + c.front() + (full ? ", full list" : "") + "]";
  TEST_ASSERT_EQUAL_STRING("", broken.c_str());
  std::vector<Act> acts{Act::PhotoArrives, Act::Chat, Act::DocumentArrives, Act::PhotoArrives, Act::Restart, Act::AppendFile};
  TEST_ASSERT_EQUAL_STRING("", runGenerated(13, 12, &acts).c_str());
}
// Counter-tests: every outside change still renders afresh (rule A breaks: the system part moved).
static void test_outside_files_beside_an_arriving_photo_still_render() {
  const std::vector<std::string> photo{"chat-1001/photo-601.jpg"};
  auto renders = [](const std::string& why) { return why.find("system part") != std::string::npos; };
  // the web Files panel between turns
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.touch("uploads/plan.pdf"); })));
  // another chat's photo or document, stored for that chat's message
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.arrive({"chat-2002/photo-9.jpg"}, false); })));
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.arrive({"inbox/9-x.pdf"}, false); })));
  // a file a routine wrote, a file another task changed or deleted
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.touch("routines/report.md"); })));
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.touch("log/daily.md"); })));
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, false, [](Device& d) { d.drop("lists/shopping.md"); })));
  // the same with a full list: an outside write, and an outside delete of the bottom listed file
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, true, [](Device& d) { d.touch("archive/old-27.md"); })));
  TEST_ASSERT_TRUE(renders(arrivalTurn(photo, true, [](Device& d) { d.drop("archive/old-0.md"); })));
  // a stored path whose store did not happen (named, but not in the store) beside an outside file
  TEST_ASSERT_TRUE(renders(arrivalTurn({"chat-1001/photo-x.jpg"}, false, [](Device& d) {
    d.touch("uploads/other.pdf");
    d.arriving.push_back("chat-1001/photo-never.jpg");
  })));
  // a message with stored paths whose own item the card refused: nothing moves; after a restart the
  // photo's line shows
  {
    Device dev;
    dev.say("1001", "one");
    dev.pause(90);
    dev.tolerateFaults = true;
    dev.card.refuseAppends = true;
    dev.arrive({"chat-1001/photo-701.jpg"});
    dev.say("1001", "[image 1/1]");
    dev.card.refuseAppends = false;
    dev.binding.reset();
    dev.pause(90);
    dev.say("1001", "next");
    TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("chat-1001/photo-701.jpg") != std::string::npos);
  }
  // the message is stored but a later item of its turn is refused: the open's move is undone, so the next
  // turn (no restart) renders the list with the photo
  {
    Device dev;
    dev.say("1001", "one");
    dev.pause(90);
    dev.tolerateFaults = true;
    dev.arrive({"chat-1001/photo-702.jpg"});
    dev.during = [&dev] { dev.card.refuseAppends = true; };
    dev.say("1001", "[image 1/1] look", {sp::call("read_file", R"({"path":"notes/a.md"})")});   // changes no file
    dev.during = nullptr;
    dev.card.refuseAppends = false;
    dev.pause(90);
    dev.say("1001", "next");
    TEST_ASSERT_TRUE(systemOf(dev.sent.back().body).find("chat-1001/photo-702.jpg") != std::string::npos);
  }
  // paths passed with a turn that is not the person's message (a routine's or a notice's) count for nothing:
  // the device passes them only with the message that carries them (turnStoredPaths), and the engine takes
  // them only from a stored person's item.
}
// [fix6-unit-begin]
// C8: the turn the blocks lean on left out of the request renders them; the own-addition rules.
static void test_a_left_out_turn_the_blocks_lean_on_renders_them() {
  plain::FrozenPrefix f;
  plain::PrefixNow n;
  n.blocks.notes = "Pinned notes:\na: 1";
  plain::renderPrefix(f, n);
  f.ownFrom = 40;
  n.omitted = 39;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::None, (int)plain::decidePrefix(f, n));
  n.omitted = 40;
  TEST_ASSERT_EQUAL((int)plain::PrefixRender::Compaction, (int)plain::decidePrefix(f, n));
  plain::renderPrefix(f, n);
  TEST_ASSERT_EQUAL(0, (int)f.ownFrom);
}
static void test_the_own_addition_rules() {
  const std::vector<std::string> own{"saved: b: 2 (12 of 4096 B)"}, hdr{"Pinned notes:"};
  TEST_ASSERT_TRUE(plain::ownNotesAddition("Pinned notes:\na: 1", "Pinned notes:\na: 1", "Pinned notes:\na: 1\nb: 2", own, hdr));
  TEST_ASSERT_TRUE(plain::ownNotesAddition("", "", "Pinned notes:\nb: 2", own, hdr));
  TEST_ASSERT_FALSE(plain::ownNotesAddition("Pinned notes:\na: 1", "Pinned notes:\na: 1", "Pinned notes:\nb: 2", own, hdr));
  TEST_ASSERT_FALSE(plain::ownNotesAddition("a: 1", "a: 1", "a: 1\nc: 3", own, hdr));   // not this turn's
  TEST_ASSERT_FALSE(plain::ownNotesAddition("a: 1", "a: 1\nb: 2", "a: 1", own, hdr));  // an own addition removed
  // The file list, by path (lane E2-FIX8; replaces E2-FIX6's addition-only rule, whose cases stay here).
  const std::vector<plain::ListedFile> listing{{"x.md (Mon 11 Nov 2026 10:01)", "x.md", 100}, {"y.md (Mon 12 Nov 2026 10:02)", "y.md", 200}};
  TEST_ASSERT_TRUE(plain::ownFilesChange("x.md (Mon 11 Nov 2026 10:01)", "y.md (Mon 12 Nov 2026 10:02)\nx.md (Mon 11 Nov 2026 10:01)", listing, {"y.md"}, 300));
  TEST_ASSERT_TRUE(plain::ownFilesChange("x.md (Mon 11 Nov 2026 10:01)", "y.md (Mon 12 Nov 2026 10:02)", listing, {"y.md"}, 300));   // x fell out of the size
  TEST_ASSERT_FALSE(plain::ownFilesChange("x.md (Mon 11 Nov 2026 10:01)\nz.md (Mon 13 Nov 2026 10:03)", "x.md (Mon 11 Nov 2026 10:01)", listing, {}, 300));   // z is gone
  TEST_ASSERT_FALSE(plain::ownFilesChange("x.md (Mon 11 Nov 2026 10:01)", "y.md (Mon 12 Nov 2026 10:02)\nx.md (Mon 11 Nov 2026 10:01)", listing, {}, 300));   // not this turn's
  // an append moves its entry to the top with a new time; a delete takes it out
  const std::vector<plain::ListedFile> l2{{"a.md (Mon 19 Nov 2026 10:09)", "a.md", 900}, {"b.md (Mon 15 Nov 2026 10:05)", "b.md", 500}, {"c.md (Mon 14 Nov 2026 10:04)", "c.md", 400}};
  TEST_ASSERT_TRUE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\na.md (Mon 13 Nov 2026 10:03)\nc.md (Mon 14 Nov 2026 10:04)", "a.md (Mon 19 Nov 2026 10:09)\nb.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"a.md"}, 800));
  TEST_ASSERT_FALSE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\na.md (Mon 13 Nov 2026 10:03)\nc.md (Mon 14 Nov 2026 10:04)", "a.md (Mon 19 Nov 2026 10:09)\nb.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"b.md"}, 800));
  TEST_ASSERT_TRUE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\nd.md (Mon 14 Nov 2026 10:04)\nc.md (Mon 14 Nov 2026 10:04)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"d.md"}, 800));
  TEST_ASSERT_FALSE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\nd.md (Mon 14 Nov 2026 10:04)\nc.md (Mon 14 Nov 2026 10:04)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"a.md"}, 800));
  // another entry changed beside an own one, or two others swapped: not own
  TEST_ASSERT_FALSE(plain::ownFilesChange("b.md (Mon 11 Nov 2026 10:01)\nc.md (Mon 14 Nov 2026 10:04)", "a.md (Mon 19 Nov 2026 10:09)\nb.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"a.md"}, 800));
  TEST_ASSERT_FALSE(plain::ownFilesChange("c.md (Mon 14 Nov 2026 10:04)\nb.md (Mon 15 Nov 2026 10:05)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {}, 800));
  // came into the bottom after an own delete: only with a write time before the turn opened
  TEST_ASSERT_TRUE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\nd.md (Mon 14 Nov 2026 10:04)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"d.md"}, 800));
  TEST_ASSERT_FALSE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\nd.md (Mon 14 Nov 2026 10:04)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"d.md"}, 400));
  TEST_ASSERT_FALSE(plain::ownFilesChange("b.md (Mon 15 Nov 2026 10:05)\nd.md (Mon 14 Nov 2026 10:04)", "b.md (Mon 15 Nov 2026 10:05)\nc.md (Mon 14 Nov 2026 10:04)", l2, {"d.md"}, 0));
  // a line of another path that merely starts with an own path is not an own entry
  const std::string w = " (Tue 17 Nov 2026 13:02)", w1 = " (Sun 1 Nov 2026 09:00)";
  TEST_ASSERT_TRUE(plain::ownFilesChange("x" + w + "\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));
  TEST_ASSERT_TRUE(plain::ownFilesChange("x" + w1 + "\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));
  TEST_ASSERT_TRUE(plain::ownFilesChange("x\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));   // no time shown
  TEST_ASSERT_FALSE(plain::ownFilesChange("x (b)" + w + "\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));
  TEST_ASSERT_FALSE(plain::ownFilesChange("x (1)\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));   // "x (1)", no time
  TEST_ASSERT_FALSE(plain::ownFilesChange("x.bak" + w + "\nc.md (Mon 14 Nov 2026 10:04)", "c.md (Mon 14 Nov 2026 10:04)", l2, {"x"}, 800));
}
// [fix6-unit-end]

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
  RUN_TEST(test_an_outside_note_added_during_a_tool_turn_reaches_the_next_request);
  RUN_TEST(test_an_outside_rule_removed_during_a_tool_turn_leaves_the_next_request);
  RUN_TEST(test_an_outside_file_during_a_tool_turn_reaches_the_next_request);
  RUN_TEST(test_an_outside_note_a_read_shows_is_still_not_the_heads_own);
  RUN_TEST(test_a_note_the_head_removes_leaves_the_next_request);
  RUN_TEST(test_a_note_the_head_edits_shows_only_its_new_value);
  RUN_TEST(test_a_note_saved_while_the_card_refused_the_turn_shows_after_a_restart);
  RUN_TEST(test_own_additions_still_keep_the_prefix);
  RUN_TEST(test_a_record_left_only_as_its_temporary_copy_is_read);
  RUN_TEST(test_create_append_edit_overwrite_delete_by_the_heads_own_call_keep_the_prefix);
  RUN_TEST(test_own_appends_turn_after_turn_keep_the_prefix);
  RUN_TEST(test_a_file_changed_from_outside_still_renders);
  RUN_TEST(test_a_failed_file_call_is_not_the_turns_own);
  RUN_TEST(test_a_photo_an_album_or_a_document_that_opens_the_turn_keeps_the_prefix);
  RUN_TEST(test_outside_files_beside_an_arriving_photo_still_render);
  // [fix6-unit-begin]
  RUN_TEST(test_a_left_out_turn_the_blocks_lean_on_renders_them);
  RUN_TEST(test_the_own_addition_rules);
  // [fix6-unit-end]
#endif
  return UNITY_END();
}
