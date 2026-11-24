// test_schedule_binding - the fires of engine 2's schedule entries over the device's chat binding
// (lane E2-SCHED, nimbus/harness/plain/schedule_binding.h), with a fake platform, a scripted Claude
// head on the real Anthropic wire and the card store over a fake card (the device seam
// orchestrator_plain.cpp builds exactly this FireBinding for the loops tick's fire hook):
//   - a due reminder sends exactly its stored text to the chat and makes no provider request; the
//     chat's transcript gets the routine note;
//   - an engine-1 record in loops.json (no loops2.json entry) fires on engine 2: an owner /remind
//     record as a reminder, an old routine isolated on the small model with the default tools, its
//     output to the chat, its state under /mem/routines; loops.json is byte-identical afterwards and
//     no loops2.json is written for it;
//   - an update changes the named record and no other (the old engine's record stays byte-identical);
//   - /remind's path (CallCtx::owner): the record is the owner's and approved even with "ask me first";
//   - a routine fire with no outbox (no card) is skipped and the reminder still goes.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/schedule_binding.h"
#include "nimbus/harness/schedule_fire.h"
#include "nimbus/orch/schedule_tool.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;
namespace sch = nimbus::orch::schedule;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kEpoch0 = 1794916800ULL;   // Tue 17 Nov 2026 12:00 UTC (outside quiet hours)

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A card that keeps files and folders (test_plain_chat_binding's).
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
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {   // E2-STORE's CardOps
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

// loops.json and loops2.json in memory, at UTC, counting the writes.
struct MemHost : sch::Host {
  std::vector<orch::LoopRecord> loops;
  std::string loopsFile, sidecarFile;
  int loopsWrites = 0, sidecarWrites = 0;
  bool ask = false;
  uint64_t now = kEpoch0;
  uint32_t ids = 0;

  void withTable(const std::function<void(std::vector<orch::LoopRecord>&)>& fn) override { fn(loops); }
  bool saveLoops(const std::vector<orch::LoopRecord>& v) override {
    ++loopsWrites;
    loopsFile = orch::dumpLoops(v);
    return true;
  }
  bool readSidecar(std::string& out) override {
    out = sidecarFile;
    return true;
  }
  bool writeSidecar(const std::string& json) override {
    ++sidecarWrites;
    sidecarFile = json;
    return true;
  }
  sch::Clock clock() override { return sch::Clock{true, now}; }
  bool localToEpoch(const orch::CivilTime& c, uint64_t& out) override {
    out = (uint64_t)orch::daysFromCivil(c.year, (unsigned)c.month, (unsigned)c.day) * 86400ull + c.hour * 3600ull +
          c.min * 60ull + (uint64_t)c.sec;
    return true;
  }
  orch::CivilTime epochToLocal(uint64_t e) override {
    orch::CivilTime c = orch::civilFromDays((uint32_t)(e / 86400ull));
    c.hour = (int)(e % 86400ull / 3600ull);
    c.min = (int)(e % 3600ull / 60ull);
    c.sec = (int)(e % 60ull);
    return c;
  }
  uint64_t nextRun(const orch::LoopRecord& l, uint64_t t) override {
    if (l.sched.kind == orch::SchedKind::Interval || l.sched.kind == orch::SchedKind::Once)
      return orch::nextIntervalEpoch(l.sched, t);
    uint64_t e = 0;
    localToEpoch(orch::nextWallClockLocal(l.sched, epochToLocal(t)), e);
    return e;
  }
  std::string newId(const std::vector<orch::LoopRecord>&) override {
    char b[10];
    snprintf(b, sizeof b, "lp%06x", (unsigned)(++ids * 0x1f3u & 0xFFFFFFu));
    return b;
  }
  bool askFirst() override { return ask; }
  bool reserved(const std::string&) override { return false; }
  int maxCount() override { return orch::kLoopMaxCount; }
};

// One device chat on engine 2, its card, and the routine files beside the transcripts.
struct Device {
  Device() : files(card), routineFiles(card, tx::kCardWriteInternalFloor, sch::kStateDir) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
  }
  void boot() {
    plain::ChatBindingConfig c;
    c.chatKey = "web";
    c.model = "claude-sonnet-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-schedule-binding"); };
    binding.reset(new plain::ChatBinding(c, d));
    TEST_ASSERT_TRUE(binding->boot().ok());
  }
  sch::FireBinding fireBinding(bool withOutbox = true) {
    plain::ScheduleBindingDeps d;
    d.held = withOutbox ? &held : nullptr;
    d.grants = grants;
    d.routineModel = "claude-haiku-5-5";
    d.fired = [this](const plain::FireReport&) { ++fires; };
    return plain::fireBindingFor(*binding, &routineFiles, d);
  }
  orch::FireOutcome fire(const std::string& id, bool withOutbox = true) {
    return sch::fire(host, orch::LoopFireRequest{id, std::string(), "web", std::string()}, fireBinding(withOutbox),
                     platform.nowEpoch());
  }
  std::string transcript() const {
    std::string all;
    for (const auto& kv : card.files)
      if (kv.first.rfind("/mem/tx/web/", 0) == 0) all += kv.second;
    return all;
  }

  Card card;
  tx::CardTxFiles files;
  tx::CardTxFiles routineFiles;
  plain::HeldStore held{plain::heldFileOn(routineFiles, plain::heldPathFor("web"))};
  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  orch::ToolRegistry reg;
  MemHost host;
  std::unique_ptr<plain::ChatBinding> binding;
  std::vector<std::string> sent;
  int fires = 0;
  bool grants = false;   // the owner's "/loop grants on"
};

orch::ToolResult call(MemHost& h, const std::string& json, bool owner = false) {
  JsonDocument d;
  deserializeJson(d, json);
  sch::CallCtx ctx;
  ctx.origin = orch::notes::Origin::Attended;
  ctx.chatId = "web";
  ctx.owner = owner;
  return sch::call(h, d.as<JsonObjectConst>(), ctx);
}

std::string idIn(const orch::ToolResult& r) {
  const size_t at = r.output.find("lp");
  return at == std::string::npos ? std::string() : r.output.substr(at, 8);
}

// Records as engine 1 (v5.1.x) wrote them: an owner /remind and an owner's daily routine.
std::vector<orch::LoopRecord> engineOneRecords() {
  orch::LoopRecord remind;
  remind.id = "lp0e1a01";
  remind.name = "reminder";
  remind.prompt = "water the plants on the balcony";
  remind.chatId = "web";
  remind.sched.kind = orch::SchedKind::Once;
  remind.sched.intervalSec = 600;
  remind.createdBy = orch::CreatedBy::Owner;
  remind.nextRun = kEpoch0;
  orch::LoopRecord daily;
  daily.id = "lp0e1a02";
  daily.name = "Morning brief";
  daily.prompt = "Give me a one-line summary of the day ahead.";
  daily.chatId = "web";
  daily.sched.kind = orch::SchedKind::Daily;
  daily.sched.minuteOfDay = 720;
  daily.createdBy = orch::CreatedBy::Owner;
  daily.nextRun = kEpoch0;
  return {remind, daily};
}

// ---- cases ------------------------------------------------------------------------------------------

void test_a_due_reminder_sends_exactly_its_text_and_makes_no_provider_request() {
  Device dev;
  dev.boot();
  const std::string text = "stretch \xe2\x82\xac" "2 both arms, then the neck (r\xc3\xa9sum\xc3\xa9 later)";
  JsonDocument a;
  a["op"] = "create";
  a["kind"] = "reminder";
  a["when"] = "2026-11-17 12:03";
  a["text"] = text;
  std::string json;
  serializeJson(a, json);
  const orch::ToolResult r = call(dev.host, json);
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  const size_t before = dev.model.requests().size();
  const orch::FireOutcome o = dev.fire(idIn(r));
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(o.answered);
  TEST_ASSERT_EQUAL(before, dev.model.requests().size());   // no provider request
  TEST_ASSERT_EQUAL(0, dev.fires);                          // no routine ran
  TEST_ASSERT_EQUAL(1, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING(("web|" + text).c_str(), dev.sent[0].c_str());   // exactly its text
  TEST_ASSERT_TRUE(has(dev.transcript(), "[routine "));     // the note in the chat's transcript
}

void test_counter_a_routine_fire_makes_one_request_on_the_small_model() {
  Device dev;
  dev.boot();
  const orch::ToolResult r = call(dev.host, R"({"op":"create","kind":"routine","every":"daily 12:00","text":"name one prime"})");
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  dev.model.push(sp::say("7"));
  const orch::FireOutcome o = dev.fire(idIn(r));
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL(1, (int)dev.model.requests().size());
  TEST_ASSERT_TRUE(has(dev.model.requests()[0].body, "claude-haiku-5-5"));
  TEST_ASSERT_EQUAL_STRING("web|7", dev.sent.back().c_str());
  TEST_ASSERT_TRUE(has(dev.card.files["/mem/routines/" + idIn(r) + ".state"], "7"));
}

void test_an_engine_one_record_fires_on_engine_2_and_loops_json_is_byte_identical() {
  Device dev;
  dev.boot();
  dev.host.loops = engineOneRecords();
  dev.host.loopsFile = orch::dumpLoops(dev.host.loops);
  const std::string before = dev.host.loopsFile;
  // The old /remind: its text, as a reminder, no request.
  orch::FireOutcome o = dev.fire("lp0e1a01");
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(0, (int)dev.model.requests().size());
  TEST_ASSERT_EQUAL_STRING("web|water the plants on the balcony", dev.sent.back().c_str());
  // The old routine: isolated, the default tools, on the small model.
  dev.model.push(sp::say("A quiet Tuesday."));
  o = dev.fire("lp0e1a02");
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL(1, (int)dev.model.requests().size());
  TEST_ASSERT_TRUE(has(dev.model.requests()[0].body, "Give me a one-line summary of the day ahead."));
  TEST_ASSERT_EQUAL_STRING("web|A quiet Tuesday.", dev.sent.back().c_str());
  TEST_ASSERT_TRUE(has(dev.card.files["/mem/routines/lp0e1a02.state"], "A quiet Tuesday."));
  // Not rewritten: loops.json byte-identical, no loops2.json written for the old records.
  TEST_ASSERT_EQUAL(0, dev.host.loopsWrites);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), dev.host.loopsFile.c_str());
  TEST_ASSERT_EQUAL(0, dev.host.sidecarWrites);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), orch::dumpLoops(dev.host.loops).c_str());
}

void test_an_update_changes_the_named_record_and_no_other() {
  Device dev;
  dev.host.loops = engineOneRecords();
  const std::string oldBytes0 = orch::dumpLoops({dev.host.loops[0]}), oldBytes1 = orch::dumpLoops({dev.host.loops[1]});
  const orch::ToolResult made = call(dev.host, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"check the train times"})");
  TEST_ASSERT_TRUE(made.success);
  const std::string id = idIn(made);
  const orch::ToolResult up = call(dev.host, "{\"op\":\"update\",\"id\":\"" + id + "\",\"every\":\"daily 08:15\"}");
  TEST_ASSERT_TRUE_MESSAGE(up.success, up.error.c_str());
  TEST_ASSERT_EQUAL(3, (int)dev.host.loops.size());
  TEST_ASSERT_EQUAL_STRING(oldBytes0.c_str(), orch::dumpLoops({dev.host.loops[0]}).c_str());
  TEST_ASSERT_EQUAL_STRING(oldBytes1.c_str(), orch::dumpLoops({dev.host.loops[1]}).c_str());
  TEST_ASSERT_EQUAL(8 * 60 + 15, dev.host.loops[2].sched.minuteOfDay);
  TEST_ASSERT_EQUAL_STRING("check the train times", dev.host.loops[2].prompt.c_str());
  // The other kind of change: the old routine's text, by id, leaves the new one as it was.
  const std::string newBytes = orch::dumpLoops({dev.host.loops[2]});
  const orch::ToolResult up2 = call(dev.host, R"({"op":"update","id":"lp0e1a02","text":"Two lines on the day ahead."})");
  TEST_ASSERT_TRUE_MESSAGE(up2.success, up2.error.c_str());
  TEST_ASSERT_EQUAL_STRING("Two lines on the day ahead.", dev.host.loops[1].prompt.c_str());
  TEST_ASSERT_EQUAL_STRING(oldBytes0.c_str(), orch::dumpLoops({dev.host.loops[0]}).c_str());
  TEST_ASSERT_EQUAL_STRING(newBytes.c_str(), orch::dumpLoops({dev.host.loops[2]}).c_str());
}

void test_remind_makes_the_owners_record_approved_even_with_ask_first() {
  MemHost h;
  h.ask = true;
  const orch::ToolResult mine = call(h, R"({"op":"create","kind":"reminder","when":"2026-11-17 12:30","text":"call the vet"})", true);
  TEST_ASSERT_TRUE(mine.success);
  TEST_ASSERT_TRUE(h.loops[0].approved);
  TEST_ASSERT_TRUE(h.loops[0].createdBy == orch::CreatedBy::Owner);
  // Counter: the model's reminder under "ask me first" waits.
  const orch::ToolResult its = call(h, R"({"op":"create","kind":"reminder","when":"2026-11-17 12:30","text":"call the vet"})");
  TEST_ASSERT_TRUE(its.success);
  TEST_ASSERT_FALSE(h.loops[1].approved);
  TEST_ASSERT_TRUE(h.loops[1].createdBy == orch::CreatedBy::Agent);
  // The owner flag is refused from a turn no person started.
  JsonDocument d;
  deserializeJson(d, R"({"op":"create","kind":"reminder","when":"2026-11-17 12:40","text":"x"})");
  sch::CallCtx ctx;
  ctx.origin = orch::notes::Origin::Unattended;
  ctx.owner = true;
  TEST_ASSERT_TRUE(sch::call(h, d.as<JsonObjectConst>(), ctx).success);
  TEST_ASSERT_TRUE(h.loops[2].createdBy == orch::CreatedBy::Agent);
}

void test_with_no_outbox_a_routine_is_skipped_and_a_reminder_still_goes() {
  Device dev;
  dev.boot();
  dev.host.loops = engineOneRecords();
  const orch::FireOutcome r = dev.fire("lp0e1a02", false);
  TEST_ASSERT_TRUE(r.deferred);
  TEST_ASSERT_EQUAL(0, (int)dev.model.requests().size());
  const orch::FireOutcome m = dev.fire("lp0e1a01", false);
  TEST_ASSERT_TRUE(m.ok);
  TEST_ASSERT_EQUAL_STRING("web|water the plants on the balcony", dev.sent.back().c_str());
}

}  // namespace

// Lane E2-P-MEM, D2: the assistant sets itself a wakeup through schedule (kind wakeup, engine 1's
// wakeup.set): it fires once at `when` as an isolated run on the small model with its text as the
// instruction, under the policy row of a wakeup (it reads; no spend, so no web_search even with the
// owner's grants on, where a routine gets it), and at most four wait at once. Red before the lane:
// "kind: reminder|routine".
void test_a_wakeup_runs_its_text_once_under_the_wakeup_row() {
  Device dev;
  const auto okTool = [](ArduinoJson::JsonObjectConst, const orch::Principal&) { return orch::ToolResult::ok("ok"); };
  const char* schema = R"({"type":"object","properties":{"query":{"type":"string"}}})";
  dev.reg.add("web_search", "Search the web.", okTool, schema);
  dev.reg.add("read_file", "Read a file.", okTool, schema);
  dev.grants = true;
  dev.boot();
  const orch::ToolResult w = call(dev.host,
      R"({"op":"create","kind":"wakeup","when":"2026-11-17 12:30","text":"check whether the backup job finished"})");
  TEST_ASSERT_TRUE_MESSAGE(w.success, w.error.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(w.output, " wakeup 2026-11-17 12:30: check whether the backup job finished"), w.output.c_str());
  const orch::ToolResult r = call(dev.host, R"({"op":"create","kind":"routine","every":"daily 12:00","text":"name one prime"})");
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  dev.model.push(sp::say("The backup finished at 12:10."));
  const orch::FireOutcome o = dev.fire(idIn(w));
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL(1, (int)dev.model.requests().size());
  const std::string& body = dev.model.requests()[0].body;
  TEST_ASSERT_TRUE(has(body, "claude-haiku-5-5"));
  TEST_ASSERT_TRUE(has(body, "check whether the backup job finished"));
  TEST_ASSERT_TRUE_MESSAGE(has(body, "\"read_file\""), body.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(body, "\"web_search\""), "a wakeup may not spend, grants or not");
  TEST_ASSERT_EQUAL_STRING("web|The backup finished at 12:10.", dev.sent.back().c_str());
  // The counter: the owner's routine with grants on is offered web_search.
  dev.model.push(sp::say("7"));
  TEST_ASSERT_TRUE(dev.fire(idIn(r)).ok);
  TEST_ASSERT_TRUE(has(dev.model.requests()[1].body, "\"web_search\""));
  // At most four wakeups wait at once (engine 1's cap).
  for (int i = 0; i < 3; ++i)
    TEST_ASSERT_TRUE(call(dev.host, R"({"op":"create","kind":"wakeup","when":"2026-11-18 09:00","text":"look again"})").success);
  const orch::ToolResult fifth = call(dev.host, R"({"op":"create","kind":"wakeup","when":"2026-11-18 09:00","text":"look again"})");
  TEST_ASSERT_FALSE(fifth.success);
  TEST_ASSERT_EQUAL_STRING("max 4 wakeups", fifth.error.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_due_reminder_sends_exactly_its_text_and_makes_no_provider_request);
  RUN_TEST(test_counter_a_routine_fire_makes_one_request_on_the_small_model);
  RUN_TEST(test_an_engine_one_record_fires_on_engine_2_and_loops_json_is_byte_identical);
  RUN_TEST(test_an_update_changes_the_named_record_and_no_other);
  RUN_TEST(test_remind_makes_the_owners_record_approved_even_with_ask_first);
  RUN_TEST(test_with_no_outbox_a_routine_is_skipped_and_a_reminder_still_goes);
  RUN_TEST(test_a_wakeup_runs_its_text_once_under_the_wakeup_row);
  return UNITY_END();
}
