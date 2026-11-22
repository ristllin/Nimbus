// Engine 2's schedule tool, the loops2 sidecar and the fires (schedule_tool.h, lane RB-U1b; PRD
// sections 4 and 6).
//
// Class rules proven here, each over a generated class rather than one instance:
//   - loops.json stays v5.1.1's bytes: a v5.1.1 file round-trips byte for byte, every record the
//     tool writes carries exactly v5.1.1's fields, and the new fields live only in loops2.json;
//   - the sidecar round-trips; an orphan entry is ignored and dropped on the next write; an id with
//     no entry is an old record (default tools, no paths);
//   - write order: create and update write the sidecar first, delete loops.json first; a refused
//     write changes nothing that reads wrong;
//   - a reminder's fire delivers its text byte for byte with zero provider requests;
//   - the lifecycle (create, list, move to 08:15, one record, cancel) over 10 seeds: an update
//     changes only the named id and every other record stays byte-identical;
//   - the state file survives a cut at every step of its write;
//   - nothing the owner sent is cut: a text over 2,048 B is refused with the limit named;
//   - a turn no person started may list and set a reminder, nothing else;
//   - a routine fire runs with the routine's own ceiling, and a fire after outside content opens
//     the next one tainted while that output is in the state.
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../test_unattended/rig.h"
#include "nimbus/harness/plain/request.h"   // toolDecl("schedule")
#include "nimbus/orch/schedule_tool.h"
#include "nimbus/utf8.h"

using namespace nimbus::orch;
namespace sch = nimbus::orch::schedule;
using Rig = ut::Rig;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kNow = ut::kNoon;   // 2026-11-17 12:00 UTC, Tuesday (offset 0 in these tests)

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The device's table and its two files, in memory, at UTC.
struct MemHost : sch::Host {
  std::vector<LoopRecord> loops;
  std::string loopsFile, sidecarFile;
  bool refuseLoops = false, refuseSidecar = false;
  std::vector<std::string> writes;   // "loops" | "loops2", in order
  sch::Clock clk{true, kNow};
  bool ask = false;
  int maxN = kLoopMaxCount;
  uint32_t ids = 0;
  std::set<std::string> reservedIds;
  std::vector<std::string> forgotten;

  void withTable(const std::function<void(std::vector<LoopRecord>&)>& fn) override { fn(loops); }
  bool saveLoops(const std::vector<LoopRecord>& v) override {
    writes.push_back("loops");
    if (refuseLoops) return false;
    loopsFile = dumpLoops(v);
    return true;
  }
  bool readSidecar(std::string& out) override {
    out = sidecarFile;
    return true;
  }
  bool writeSidecar(const std::string& json) override {
    writes.push_back("loops2");
    if (refuseSidecar) return false;
    sidecarFile = json;
    return true;
  }
  sch::Clock clock() override { return clk; }
  bool localToEpoch(const CivilTime& c, uint64_t& out) override {
    out = (uint64_t)daysFromCivil(c.year, (unsigned)c.month, (unsigned)c.day) * 86400ull + c.hour * 3600ull +
          c.min * 60ull + (uint64_t)c.sec;
    return true;
  }
  CivilTime epochToLocal(uint64_t e) override {
    CivilTime c = civilFromDays((uint32_t)(e / 86400ull));
    c.hour = (int)(e % 86400ull / 3600ull);
    c.min = (int)(e % 3600ull / 60ull);
    c.sec = (int)(e % 60ull);
    return c;
  }
  uint64_t nextRun(const LoopRecord& l, uint64_t now) override {
    if (l.sched.kind == SchedKind::Interval || l.sched.kind == SchedKind::Once) return nextIntervalEpoch(l.sched, now);
    uint64_t e = 0;
    localToEpoch(nextWallClockLocal(l.sched, epochToLocal(now)), e);
    return e;
  }
  std::string newId(const std::vector<LoopRecord>&) override {
    char b[10];
    snprintf(b, sizeof b, "lp%06x", (unsigned)(++ids * 0x1f3u & 0xFFFFFFu));
    return b;
  }
  bool askFirst() override { return ask; }
  bool reserved(const std::string& id) override { return reservedIds.count(id) > 0; }
  int maxCount() override { return maxN; }
  void forgetState(const std::string& id) override { forgotten.push_back(id); }

  std::vector<sch::Extra> extras() const {
    std::vector<sch::Extra> e;
    sch::loadSidecar(sidecarFile, e);
    return e;
  }
  const LoopRecord* find(const std::string& id) const {
    for (const auto& l : loops)
      if (l.id == id) return &l;
    return nullptr;
  }
};

ToolResult run(MemHost& h, const std::string& argsJson, notes::Origin o = notes::Origin::Attended,
               const std::string& chat = "1001") {
  JsonDocument d;
  deserializeJson(d, argsJson);
  sch::CallCtx ctx;
  ctx.origin = o;
  ctx.chatId = chat;
  return sch::call(h, d.as<JsonObjectConst>(), ctx);
}

// The id a create's result names (the first "lp" word).
std::string idIn(const ToolResult& r) {
  const size_t at = r.output.find("lp");
  return at == std::string::npos ? std::string() : r.output.substr(at, 8);
}

std::string recBytes(const LoopRecord& l) { return dumpLoops({l}); }

// A time `s` seconds after kNow, in the tool's when form.
std::string whenIn(MemHost& h, uint64_t s) {
  const CivilTime c = h.epochToLocal(kNow + s);
  char b[20];
  snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.hour, c.min);
  return b;
}

std::string jsonStr(const std::string& s) {
  JsonDocument d;
  d.set(s);
  std::string out;
  serializeJson(d, out);
  return out;
}

// Owner texts from several domains (ruling 41: no use case named in code; the corpus spans many).
const char* const kCorpus[] = {
    "summarize the overnight build failures and list the flaky suites",
    "check the train times to the coast and say if the 07:42 is late",
    "add up this week's grocery receipts in receipts.csv",
    "remind me to water the tomatoes",
    "look up the weather for the hike on Saturday",
    "review the training log and note the longest run",
    "pay the electricity bill",
    "draft three questions for the parent-teacher meeting",
    "Cafe\xcc\x81 au lait \xe2\x82\xac 3,20 bei der B\xc3\xa4" "ckerei",
    "\xe6\x98\x8e\xe5\xa4\xa9\xe3\x81\xae\xe4\xbc\x9a\xe8\xad\xb0\xe3\x81\xae\xe8\xb3\x87\xe6\x96\x99",
};
constexpr size_t kCorpusN = sizeof(kCorpus) / sizeof(kCorpus[0]);

// A v5.1.1 loops.json: two records, v5.1.1 dumpLoops field order (lastErr only when set).
const char kV511File[] =
    R"([{"id":"lp00a1b2","name":"morning brief","prompt":"summarize what came in overnight","chatId":"",)"
    R"("kind":1,"interval":21600,"minute":450,"weekMask":127,"createdBy":0,"enabled":true,"approved":true,)"
    R"("pauseReason":0,"nextRun":1794985800,"lastRun":1794899400,"lastResult":1,"firesToday":1,"tokensToday":5120,)"
    R"("lastSyncedDay":20774,"consecFails":0,"lastReplyHash":1234567890123,"repeatRun":0},)"
    R"({"id":"lp00c3d4","name":"reminder","prompt":"call the plumber","chatId":"1001","kind":3,"interval":3600,)"
    R"("minute":510,"weekMask":127,"createdBy":0,"enabled":true,"approved":true,"pauseReason":0,)"
    R"("nextRun":1794920400,"lastRun":0,"lastResult":0,"firesToday":0,"tokensToday":0,"lastSyncedDay":0,)"
    R"("consecFails":0,"lastReplyHash":0,"repeatRun":0,"lastErr":"provider timeout"}])";

std::vector<std::string> keysOf(JsonObjectConst o) {
  std::vector<std::string> k;
  for (JsonPairConst p : o) k.push_back(p.key().c_str());
  return k;
}

}  // namespace

// ---- D1: loops.json stays v5.1.1's ----------------------------------------------------------
void test_loops_json_written_by_the_tool_is_v511_bytes() {
  MemHost h;
  TEST_ASSERT_TRUE(loadLoops(kV511File, h.loops));
  TEST_ASSERT_EQUAL_STRING(kV511File, dumpLoops(h.loops).c_str());   // v5.1.1 bytes round-trip unchanged
  const std::string before0 = recBytes(h.loops[0]);
  // The owner's assistant adds a routine and moves the old reminder; the old routine is untouched.
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"routine","every":"weekly mon,thu 18:30","text":"tidy the downloads folder"})").success);
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"update\",\"id\":\"lp00c3d4\",\"when\":" + jsonStr(whenIn(h, 7200)) + "}").success);
  std::vector<LoopRecord> back;
  TEST_ASSERT_TRUE(loadLoops(h.loopsFile, back));   // v5.1.1's loader (orch_loops.cpp unchanged since v5.1.1)
  TEST_ASSERT_EQUAL(3, (int)back.size());
  TEST_ASSERT_EQUAL_STRING(before0.c_str(), recBytes(back[0]).c_str());
  TEST_ASSERT_EQUAL_STRING(h.loopsFile.c_str(), dumpLoops(back).c_str());
  // Every record carries exactly v5.1.1's keys, in its order (lastErr only when set).
  JsonDocument ref, now;
  deserializeJson(ref, kV511File);
  deserializeJson(now, h.loopsFile);
  const std::vector<std::string> v511 = keysOf(ref[1].as<JsonObjectConst>());
  for (JsonObjectConst o : now.as<JsonArrayConst>()) {
    std::vector<std::string> k = keysOf(o);
    if (k.size() + 1 == v511.size()) k.push_back("lastErr");
    TEST_ASSERT_TRUE_MESSAGE(k == v511, "a record holds exactly the v5.1.1 fields");
  }
  // The new fields live in loops2.json, by id.
  for (const char* f : {"\"tools\":", "\"paths\":", "\"holdS\":", "\"outsideAt\":", "\"kind\":\"r"})
    TEST_ASSERT_FALSE_MESSAGE(contains(h.loopsFile, f), f);
  const std::vector<sch::Extra> ex = h.extras();
  TEST_ASSERT_EQUAL(1, (int)ex.size());
  TEST_ASSERT_EQUAL((int)sch::Kind::Routine, (int)ex[0].kind);
  TEST_ASSERT_NOT_NULL(h.find(ex[0].id));
}

void test_sidecar_round_trip_and_orphans_are_dropped_on_the_next_write() {
  std::mt19937 r(7);
  for (int round = 0; round < 20; ++round) {
    std::vector<sch::Extra> in;
    const int n = (int)(r() % 6);
    for (int i = 0; i < n; ++i) {
      sch::Extra e;
      e.id = "lp" + ut::hexOf(r, 6);
      e.kind = r() % 2 ? sch::Kind::Reminder : sch::Kind::Routine;
      for (int t = (int)(r() % 3); t > 0; --t) e.tools.push_back(ut::wordsOf(r, 1));
      for (int p = (int)(r() % 3); p > 0; --p) e.paths.push_back("p/" + ut::textOf(r, 1 + r() % 12));
      e.holdS = r() % 2 ? (uint32_t)(r() % 86400) : 0;
      for (int o = (int)(r() % 3); o > 0; --o) e.outsideAt.push_back(kNow + r() % 100000);
      in.push_back(e);
    }
    std::vector<sch::Extra> out;
    TEST_ASSERT_TRUE(sch::loadSidecar(sch::dumpSidecar(in), out));
    TEST_ASSERT_EQUAL((int)in.size(), (int)out.size());
    for (size_t i = 0; i < in.size(); ++i) {
      TEST_ASSERT_EQUAL_STRING(in[i].id.c_str(), out[i].id.c_str());
      TEST_ASSERT_EQUAL((int)in[i].kind, (int)out[i].kind);
      TEST_ASSERT_TRUE(in[i].tools == out[i].tools);
      TEST_ASSERT_TRUE(in[i].paths == out[i].paths);
      TEST_ASSERT_EQUAL(in[i].holdS, out[i].holdS);
      TEST_ASSERT_TRUE(in[i].outsideAt == out[i].outsideAt);
    }
  }
  std::vector<sch::Extra> none;
  TEST_ASSERT_TRUE(sch::loadSidecar("", none));
  TEST_ASSERT_EQUAL(0, (int)none.size());
  TEST_ASSERT_FALSE(sch::loadSidecar("[1,2]", none));
  TEST_ASSERT_FALSE(sch::loadSidecar(R"({"v":2,"loops":[]})", none));
  // An orphan (its record is gone, for example a delete cut before the sidecar write) is ignored by
  // the listing and dropped by the next write; an entry whose record exists is kept.
  MemHost h;
  const ToolResult a = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"a"})");
  TEST_ASSERT_TRUE(a.success);
  sch::Extra orphan;
  orphan.id = "lpdeadbe";
  orphan.kind = sch::Kind::Routine;
  std::vector<sch::Extra> ex = h.extras();
  ex.push_back(orphan);
  h.sidecarFile = sch::dumpSidecar(ex);
  const ToolResult l = run(h, R"({"op":"list"})");
  TEST_ASSERT_TRUE(l.success);
  TEST_ASSERT_FALSE(contains(l.output, "lpdeadbe"));
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"reminder","when":")" + whenIn(h, 3600) + R"(","text":"b"})").success);
  TEST_ASSERT_FALSE(contains(h.sidecarFile, "lpdeadbe"));
  TEST_ASSERT_TRUE(contains(h.sidecarFile, idIn(a)));
}

void test_an_id_with_no_entry_is_an_old_record() {
  LoopRecord remind, wake, daily;
  remind.sched.kind = wake.sched.kind = SchedKind::Once;
  wake.createdBy = CreatedBy::Agent;
  daily.sched.kind = SchedKind::Daily;
  TEST_ASSERT_EQUAL((int)sch::Kind::Reminder, (int)sch::kindOf(remind, nullptr));
  TEST_ASSERT_EQUAL((int)sch::Kind::Routine, (int)sch::kindOf(wake, nullptr));
  TEST_ASSERT_EQUAL((int)sch::Kind::Routine, (int)sch::kindOf(daily, nullptr));
  daily.id = "lp000001";
  daily.name = "brief";
  daily.prompt = "summarize";
  const agent::plain::RoutineFire f = sch::routineFire(daily, nullptr, {}, kNow, false);
  TEST_ASSERT_EQUAL_STRING("lp000001", f.def.id.c_str());
  TEST_ASSERT_EQUAL_STRING("summarize", f.def.instruction.c_str());
  TEST_ASSERT_TRUE(f.def.tools.empty());   // empty: the default tools (runRoutine reads it so)
  TEST_ASSERT_TRUE(f.def.paths.empty());
  TEST_ASSERT_EQUAL(kNow, f.fireAt);
  // Counter: an entry names its own tools, paths and expiry; its kind wins over the record's shape.
  sch::Extra e;
  e.id = daily.id;
  e.kind = sch::Kind::Routine;
  e.tools = {"read_file"};
  e.paths = {"logs/"};
  e.holdS = 600;
  const agent::plain::RoutineFire g = sch::routineFire(daily, &e, {}, kNow, true);
  TEST_ASSERT_TRUE(g.def.tools == e.tools);
  TEST_ASSERT_TRUE(g.def.paths == e.paths);
  TEST_ASSERT_EQUAL(600u, g.def.holdS);
  TEST_ASSERT_TRUE(g.grants);
  e.kind = sch::Kind::Reminder;
  TEST_ASSERT_EQUAL((int)sch::Kind::Reminder, (int)sch::kindOf(daily, &e));
}

// ---- write order -----------------------------------------------------------------------------
void test_write_order_and_refused_writes() {
  MemHost h;
  const ToolResult c = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})");
  TEST_ASSERT_TRUE(c.success);
  TEST_ASSERT_TRUE((h.writes == std::vector<std::string>{"loops2", "loops"}));
  const std::string id = idIn(c);
  h.writes.clear();
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"update\",\"id\":\"" + id + "\",\"every\":\"daily 08:15\"}").success);
  TEST_ASSERT_TRUE((h.writes == std::vector<std::string>{"loops2", "loops"}));
  h.writes.clear();
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"delete\",\"id\":\"" + id + "\"}").success);
  TEST_ASSERT_TRUE((h.writes == std::vector<std::string>{"loops", "loops2"}));
  TEST_ASSERT_TRUE(h.loops.empty());
  TEST_ASSERT_TRUE(h.extras().empty());
  // The sidecar refused: nothing is created, loops.json is not written.
  h.writes.clear();
  h.refuseSidecar = true;
  const std::string file0 = h.loopsFile;
  ToolResult r = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"x"})");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(h.loops.empty());
  TEST_ASSERT_EQUAL_STRING(file0.c_str(), h.loopsFile.c_str());
  TEST_ASSERT_TRUE((h.writes == std::vector<std::string>{"loops2"}));
  // loops.json refused after the sidecar landed: the table is unchanged and the entry is an orphan.
  h.refuseSidecar = false;
  h.refuseLoops = true;
  r = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"y"})");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(h.loops.empty());
  TEST_ASSERT_TRUE(run(h, R"({"op":"list"})").success);
  // A delete whose loops.json write is refused deletes nothing.
  h.refuseLoops = false;
  const ToolResult k = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"z"})");
  TEST_ASSERT_TRUE(k.success);
  TEST_ASSERT_EQUAL(1, (int)h.extras().size());   // the orphan from the refused create is gone
  h.refuseLoops = true;
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"delete\",\"id\":\"" + idIn(k) + "\"}").success);
  TEST_ASSERT_EQUAL(1, (int)h.loops.size());
  // Its sidecar refused after loops.json landed: the record is gone; the entry is an orphan, unlisted.
  h.refuseLoops = false;
  h.refuseSidecar = true;
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"delete\",\"id\":\"" + idIn(k) + "\"}").success);
  TEST_ASSERT_TRUE(h.loops.empty());
  h.refuseSidecar = false;
  TEST_ASSERT_FALSE(contains(run(h, R"({"op":"list"})").output, idIn(k)));
}

// ---- D2: a reminder's fire, no model call ------------------------------------------------------
void test_a_reminder_fire_delivers_its_text_byte_for_byte_with_no_provider_request() {
  std::mt19937 r(11);
  const size_t lens[] = {1, 7, 64, 300, 1000, 2047, 2048};
  for (size_t n : lens) {
    Rig rig;
    TEST_ASSERT_TRUE(rig.boot().ok());
    MemHost h;
    std::string text = ut::textOf(r, n);
    while (text.size() > n || !nimbus::utf8Valid(text)) text.pop_back();
    if (text.empty() || text.back() == ' ') text += "x";
    const ToolResult c = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600) +
                                    "\",\"text\":" + jsonStr(text) + "}");
    TEST_ASSERT_TRUE_MESSAGE(c.success, c.error.c_str());
    TEST_ASSERT_EQUAL_STRING(text.c_str(), h.loops[0].prompt.c_str());
    sch::FireBinding b;
    b.owner = rig.sink();
    b.files = &rig.card;
    int routines = 0;
    b.run = [&](const agent::plain::RoutineFire&, bool, agent::plain::FireReport&) {
      ++routines;
      return agent::plain::Status::success();
    };
    LoopFireRequest req;
    req.id = h.loops[0].id;
    req.chatId = "1001";
    req.prompt = h.loops[0].prompt;
    const FireOutcome o = sch::fire(h, req, b, kNow + 600);
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_TRUE(o.answered);
    TEST_ASSERT_EQUAL(0, routines);
    TEST_ASSERT_EQUAL(0, (int)rig.ownerModel.requests().size());
    TEST_ASSERT_EQUAL(0, (int)rig.routineModel.requests().size());
    std::string got;
    for (const ut::Sent& s : rig.sent) got += s.text;
    TEST_ASSERT_EQUAL_STRING(text.c_str(), got.c_str());
    TEST_ASSERT_EQUAL(1, (int)rig.ownerNotes().size());   // the routine note form, in his transcript
    TEST_ASSERT_TRUE(contains(rig.ownerNotes()[0], "[routine "));
    TEST_ASSERT_TRUE(contains(rig.ownerNotes()[0], text));
  }
}

// Counter: a routine's fire goes to the routine runner, with its state.
void test_counter_a_routine_fire_runs_the_routine() {
  MemHost h;
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})").success);
  txb::MemFiles card;
  sch::FireBinding b;
  b.files = &card;
  int routines = 0;
  b.run = [&](const agent::plain::RoutineFire& f, bool tainted, agent::plain::FireReport& rep) {
    ++routines;
    TEST_ASSERT_EQUAL_STRING("brief", f.def.instruction.c_str());
    TEST_ASSERT_FALSE(tainted);
    rep.result = agent::plain::FireResult::Sent;
    rep.output = agent::plain::RoutineOutput{f.fireAt, "all quiet"};
    return agent::plain::Status::success();
  };
  LoopFireRequest req;
  req.id = h.loops[0].id;
  const FireOutcome o = sch::fire(h, req, b, kNow);
  TEST_ASSERT_EQUAL(1, routines);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_STRING("all quiet", o.detail.c_str());
  std::vector<agent::plain::RoutineOutput> st;
  TEST_ASSERT_TRUE(sch::loadState(card, req.id, st));
  TEST_ASSERT_EQUAL(1, (int)st.size());
  TEST_ASSERT_EQUAL_STRING("all quiet", st[0].text.c_str());
  TEST_ASSERT_TRUE(card.files.count("/mem/routines/" + req.id + ".state") == 1);
}

// ---- D3: the lifecycle of refloop scenario 13, 10 seeds ------------------------------------------
void test_lifecycle_over_10_seeds_changes_only_the_named_id() {
  for (uint32_t seed = 0; seed < 10; ++seed) {
    std::mt19937 r(seed * 7919u + 1u);
    MemHost h;
    // Other entries first: reminders and routines from the corpus.
    const int others = 1 + (int)(r() % 5);
    for (int i = 0; i < others; ++i) {
      const std::string t = kCorpus[r() % kCorpusN];
      const bool rem = r() % 2;
      const std::string args =
          rem ? "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600 + 3600 * (r() % 48)) +
                    "\",\"text\":" + jsonStr(t) + "}"
              : "{\"op\":\"create\",\"kind\":\"routine\",\"every\":\"daily " + std::to_string(10 + r() % 10) +
                    ":30\",\"text\":" + jsonStr(t) + "}";
      TEST_ASSERT_TRUE_MESSAGE(run(h, args).success, args.c_str());
    }
    const std::string text = kCorpus[r() % kCorpusN];
    const ToolResult c = run(h, "{\"op\":\"create\",\"kind\":\"routine\",\"every\":\"daily 07:00\",\"text\":" +
                                    jsonStr(text) + "}");
    TEST_ASSERT_TRUE(c.success);
    const std::string id = idIn(c);
    TEST_ASSERT_NOT_NULL(h.find(id));
    std::map<std::string, std::string> before;
    for (const auto& l : h.loops)
      if (l.id != id) before[l.id] = recBytes(l);
    const std::string sideBefore = h.sidecarFile;
    // List: it is there once, with its cadence and its whole text.
    ToolResult l = run(h, R"({"op":"list"})");
    TEST_ASSERT_TRUE(l.success);
    TEST_ASSERT_TRUE(contains(l.output, id + " routine daily 07:00"));
    TEST_ASSERT_TRUE(contains(l.output, text));
    // Move it to 08:15: one record changes, the rest stay byte-identical.
    const ToolResult u = run(h, "{\"op\":\"update\",\"id\":\"" + id + "\",\"every\":\"daily 08:15\"}");
    TEST_ASSERT_TRUE_MESSAGE(u.success, u.error.c_str());
    TEST_ASSERT_EQUAL(others + 1, (int)h.loops.size());
    int named = 0;
    for (const auto& rec : h.loops) {
      if (rec.id == id) {
        ++named;
        TEST_ASSERT_EQUAL(8 * 60 + 15, rec.sched.minuteOfDay);
        TEST_ASSERT_EQUAL_STRING(text.c_str(), rec.prompt.c_str());
        continue;
      }
      TEST_ASSERT_EQUAL_STRING(before[rec.id].c_str(), recBytes(rec).c_str());
    }
    TEST_ASSERT_EQUAL(1, named);
    TEST_ASSERT_EQUAL_STRING(sideBefore.c_str(), h.sidecarFile.c_str());
    l = run(h, R"({"op":"list"})");
    TEST_ASSERT_TRUE(contains(l.output, id + " routine daily 08:15"));
    TEST_ASSERT_FALSE(contains(l.output, id + " routine daily 07:00"));
    // Cancel it: gone from both files, the rest unchanged.
    TEST_ASSERT_TRUE(run(h, "{\"op\":\"delete\",\"id\":\"" + id + "\"}").success);
    TEST_ASSERT_NULL(h.find(id));
    TEST_ASSERT_NULL(sch::findExtra(h.extras(), id));
    for (const auto& rec : h.loops) TEST_ASSERT_EQUAL_STRING(before[rec.id].c_str(), recBytes(rec).c_str());
    TEST_ASSERT_FALSE(contains(run(h, R"({"op":"list"})").output, id));
    TEST_ASSERT_TRUE(std::find(h.forgotten.begin(), h.forgotten.end(), id) != h.forgotten.end());
  }
}

// ---- D4: the state file survives a cut ------------------------------------------------------------
void test_the_state_file_survives_a_torn_write() {
  using agent::plain::RoutineOutput;
  const std::string id = "lp0a0b0c";
  const std::vector<RoutineOutput> a = {{kNow, "first"}}, b = {{kNow, "first"}, {kNow + 86400, "second"}};
  txb::MemFiles card;
  TEST_ASSERT_TRUE(sch::saveState(card, id, a));
  std::vector<RoutineOutput> got;
  // A cut at the rename: the old state stays.
  card.killAtRename = (long)card.renames;
  bool killed = false;
  try {
    sch::saveState(card, id, b);
  } catch (const txb::Killed&) {
    killed = true;
  }
  TEST_ASSERT_TRUE(killed);
  card.clearFaults();
  TEST_ASSERT_TRUE(sch::loadState(card, id, got));
  TEST_ASSERT_EQUAL(1, (int)got.size());
  // A torn temp beside the state file: the state file wins.
  card.files[sch::statePath(id) + ".tmp"] = "{\"at\":1,\"te";
  TEST_ASSERT_TRUE(sch::loadState(card, id, got));
  TEST_ASSERT_EQUAL(1, (int)got.size());
  TEST_ASSERT_EQUAL_STRING("first", got[0].text.c_str());
  // A cut after the old file went and before the rename: the whole temp is read.
  card.files.erase(sch::statePath(id));
  card.files[sch::statePath(id) + ".tmp"] = agent::plain::encodeRoutineState(b);
  TEST_ASSERT_TRUE(sch::loadState(card, id, got));
  TEST_ASSERT_EQUAL(2, (int)got.size());
  // A refused write keeps the old file and says so.
  TEST_ASSERT_TRUE(sch::saveState(card, id, a));
  card.refuseWholeWrites = true;
  TEST_ASSERT_FALSE(sch::saveState(card, id, b));
  card.clearFaults();
  TEST_ASSERT_TRUE(sch::loadState(card, id, got));
  TEST_ASSERT_EQUAL(1, (int)got.size());
  // No file: an empty state.
  TEST_ASSERT_TRUE(sch::loadState(card, "lpffffff", got));
  TEST_ASSERT_TRUE(got.empty());
  TEST_ASSERT_EQUAL_STRING("/mem/routines/lp0a0b0c.state", sch::statePath(id).c_str());
}

// ---- D5: nothing the owner sent is cut ------------------------------------------------------------
void test_a_text_over_the_limit_is_refused_never_cut() {
  std::mt19937 r(5);
  for (int k = 0; k < 6; ++k) {
    MemHost h;
    std::string fits = ut::textOf(r, kLoopPromptMax);
    while (fits.size() > (size_t)kLoopPromptMax || !nimbus::utf8Valid(fits)) fits.pop_back();
    while (fits.size() < (size_t)kLoopPromptMax) fits += "a";
    const std::string over = fits + (k % 2 ? "\xc3\xa9" : "b");
    const bool rem = k % 2 == 0;
    const std::string head = rem ? "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 900) + "\",\"text\":"
                                 : std::string("{\"op\":\"create\",\"kind\":\"routine\",\"every\":\"daily 07:00\",\"text\":");
    const ToolResult no = run(h, head + jsonStr(over) + "}");
    TEST_ASSERT_FALSE(no.success);
    TEST_ASSERT_TRUE_MESSAGE(contains(no.error, "2048"), no.error.c_str());
    TEST_ASSERT_TRUE(h.loops.empty());
    const ToolResult yes = run(h, head + jsonStr(fits) + "}");
    TEST_ASSERT_TRUE_MESSAGE(yes.success, yes.error.c_str());
    TEST_ASSERT_EQUAL_STRING(fits.c_str(), h.loops[0].prompt.c_str());
    std::vector<LoopRecord> back;
    TEST_ASSERT_TRUE(loadLoops(h.loopsFile, back));
    TEST_ASSERT_EQUAL_STRING(fits.c_str(), back[0].prompt.c_str());   // whole through v5.1.1's loader
    const ToolResult up = run(h, "{\"op\":\"update\",\"id\":\"" + h.loops[0].id + "\",\"text\":" + jsonStr(over) + "}");
    TEST_ASSERT_FALSE(up.success);
    TEST_ASSERT_TRUE(contains(up.error, "2048"));
    TEST_ASSERT_EQUAL_STRING(fits.c_str(), h.loops[0].prompt.c_str());
  }
}

// ---- origins ------------------------------------------------------------------------------------
void test_a_turn_no_person_started_lists_and_sets_reminders_only() {
  for (notes::Origin o : {notes::Origin::Unattended, notes::Origin::Tainted}) {
    MemHost h;
    const ToolResult own = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})");
    TEST_ASSERT_TRUE(own.success);
    const std::string id = idIn(own);
    const std::string table = h.loopsFile, side = h.sidecarFile;
    TEST_ASSERT_TRUE(run(h, R"({"op":"list"})", o).success);
    TEST_ASSERT_FALSE(run(h, R"({"op":"create","kind":"routine","every":"daily 09:00","text":"more"})", o).success);
    TEST_ASSERT_FALSE(run(h, "{\"op\":\"update\",\"id\":\"" + id + "\",\"text\":\"other\"}", o).success);
    TEST_ASSERT_FALSE(run(h, "{\"op\":\"delete\",\"id\":\"" + id + "\"}", o).success);
    TEST_ASSERT_EQUAL_STRING(table.c_str(), h.loopsFile.c_str());
    TEST_ASSERT_EQUAL_STRING(side.c_str(), h.sidecarFile.c_str());
    const ToolResult rem = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600) +
                                      "\",\"text\":\"the bus leaves in ten minutes\"}", o);
    TEST_ASSERT_TRUE_MESSAGE(rem.success, rem.error.c_str());
    TEST_ASSERT_EQUAL(2, (int)h.loops.size());
  }
}

void test_counter_an_attended_turn_does_all_four() {
  MemHost h;
  const ToolResult c = run(h, R"({"op":"create","kind":"routine","every":"every 30 min","text":"check the queue"})");
  TEST_ASSERT_TRUE(c.success);
  TEST_ASSERT_EQUAL(1800u, h.loops[0].sched.intervalSec);
  TEST_ASSERT_TRUE(run(h, R"({"op":"list"})").success);
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"update\",\"id\":\"" + idIn(c) + "\",\"every\":\"every 2 h\"}").success);
  TEST_ASSERT_EQUAL(7200u, h.loops[0].sched.intervalSec);
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"delete\",\"id\":\"" + idIn(c) + "\"}").success);
  TEST_ASSERT_TRUE(h.loops.empty());
}

// ---- update keeps the id and counters; a routine change waits for approval again ------------------
void test_update_keeps_id_and_counters_and_a_routine_change_needs_approval_again() {
  MemHost h;
  const ToolResult c = run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})");
  TEST_ASSERT_TRUE(c.success);
  TEST_ASSERT_FALSE(h.loops[0].approved);   // the assistant's routine waits for the owner
  TEST_ASSERT_TRUE(contains(c.output, "approval"));
  LoopRecord& l = h.loops[0];
  l.approved = true;
  l.firesToday = 3;
  l.tokensToday = 4096;
  l.lastRun = kNow - 100;
  l.lastResult = LastResult::Ok;
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"update\",\"id\":\"" + l.id + "\",\"text\":\"brief, with the weather\"}").success);
  TEST_ASSERT_FALSE(h.loops[0].approved);
  TEST_ASSERT_EQUAL(3, h.loops[0].firesToday);
  TEST_ASSERT_EQUAL(4096u, h.loops[0].tokensToday);
  TEST_ASSERT_EQUAL(kNow - 100, h.loops[0].lastRun);
  TEST_ASSERT_EQUAL_STRING(idIn(c).c_str(), h.loops[0].id.c_str());
  // Counter: a reminder's update keeps its approval (one bounded fire, auto-approved by design).
  const ToolResult rem = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600) + "\",\"text\":\"tea\"}");
  TEST_ASSERT_TRUE(rem.success);
  const LoopRecord* rr = h.find(idIn(rem));
  TEST_ASSERT_TRUE(rr->approved);
  TEST_ASSERT_EQUAL((int)CreatedBy::Agent, (int)rr->createdBy);
  TEST_ASSERT_TRUE(run(h, "{\"op\":\"update\",\"id\":\"" + rr->id + "\",\"when\":\"" + whenIn(h, 1200) + "\"}").success);
  TEST_ASSERT_TRUE(h.find(idIn(rem))->approved);
  TEST_ASSERT_EQUAL(kNow + 1200, h.find(idIn(rem))->nextRun);
  // A kind that does not match the entry, an unknown id and a reserved id are refused.
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"update\",\"id\":\"" + rr->id + "\",\"kind\":\"routine\",\"text\":\"x\"}").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"update","id":"lp999999","text":"x"})").success);
  h.reservedIds.insert(rr->id);
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"delete\",\"id\":\"" + rr->id + "\"}").success);
  TEST_ASSERT_NOT_NULL(h.find(rr->id));
}

void test_ask_first_holds_a_reminder_for_approval() {
  MemHost h;
  h.ask = true;
  const ToolResult r = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600) + "\",\"text\":\"tea\"}");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_FALSE(h.loops[0].approved);
  TEST_ASSERT_TRUE(contains(r.output, "approval"));
}

// ---- bounds ---------------------------------------------------------------------------------------
void test_reminder_bounds_are_named_and_reminders_count_against_the_loop_cap() {
  MemHost h;
  const ToolResult soon = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 60) + "\",\"text\":\"a\"}");
  TEST_ASSERT_FALSE(soon.success);
  TEST_ASSERT_TRUE(contains(soon.error, "7 days"));
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 8 * 86400) + "\",\"text\":\"a\"}").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"create","kind":"reminder","when":"tomorrow 9am","text":"a"})").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"create","kind":"reminder","when":"2026-02-30 09:00","text":"a"})").success);
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"every\":\"daily 07:00\",\"text\":\"a\"}").success);
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"create\",\"kind\":\"routine\",\"when\":\"" + whenIn(h, 600) + "\",\"text\":\"a\"}").success);
  TEST_ASSERT_FALSE(run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600) + "\"}").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"create","kind":"routine","every":"every 2 min","text":"a"})").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"rename"})").success);
  TEST_ASSERT_FALSE(run(h, R"({"op":"create","kind":"alarm","text":"a"})").success);
  TEST_ASSERT_TRUE(h.loops.empty());
  // Eight reminders (more than engine 1's four armed wakeups), then the cap of eight entries.
  for (int i = 0; i < kLoopMaxCount; ++i) {
    const ToolResult ok = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 600 + 60 * i) +
                                     "\",\"text\":\"r" + std::to_string(i) + "\"}");
    TEST_ASSERT_TRUE_MESSAGE(ok.success, ok.error.c_str());
  }
  const ToolResult full = run(h, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(h, 9000) + "\",\"text\":\"x\"}");
  TEST_ASSERT_FALSE(full.success);
  TEST_ASSERT_TRUE(contains(full.error, "8"));
  // No clock: nothing is set for a time.
  MemHost n;
  n.clk.valid = false;
  TEST_ASSERT_FALSE(run(n, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(n, 600) + "\",\"text\":\"a\"}").success);
}

void test_parse_when_every_and_the_name() {
  CivilTime c;
  TEST_ASSERT_TRUE(sch::parseWhen("2026-11-23 07:05", c));
  TEST_ASSERT_EQUAL(2026, c.year);
  TEST_ASSERT_EQUAL(11, c.month);
  TEST_ASSERT_EQUAL(23, c.day);
  TEST_ASSERT_EQUAL(7, c.hour);
  TEST_ASSERT_EQUAL(5, c.min);
  TEST_ASSERT_TRUE(sch::parseWhen("2028-02-29 23:59", c));
  for (const char* bad : {"", "2026-11-23", "2026-11-23 7:05", "2026-13-01 07:00", "2026-11-31 07:00",
                          "2027-02-29 07:00", "2026-11-23 24:00", "2026-11-23 07:60", "2026-11-23 07:05 ",
                          "x2026-11-23 07:05", "2026/11/23 07:05"})
    TEST_ASSERT_FALSE_MESSAGE(sch::parseWhen(bad, c), bad);
  SchedSpec s;
  TEST_ASSERT_TRUE(sch::parseEvery("daily 07:00", 300, s));
  TEST_ASSERT_EQUAL((int)SchedKind::Daily, (int)s.kind);
  TEST_ASSERT_EQUAL(420, s.minuteOfDay);
  TEST_ASSERT_TRUE(sch::parseEvery("weekly mon,thu 18:30", 300, s));
  TEST_ASSERT_EQUAL((int)SchedKind::Weekly, (int)s.kind);
  TEST_ASSERT_EQUAL(0x12, s.weekMask);
  TEST_ASSERT_EQUAL_STRING("weekly mon,thu 18:30", sch::everyText(s).c_str());
  TEST_ASSERT_TRUE(sch::parseEvery("every 45 min", 300, s));
  TEST_ASSERT_EQUAL(2700u, s.intervalSec);
  TEST_ASSERT_EQUAL_STRING("every 45 min", sch::everyText(s).c_str());
  TEST_ASSERT_TRUE(sch::parseEvery("every 3 h", 300, s));
  TEST_ASSERT_EQUAL_STRING("every 3 h", sch::everyText(s).c_str());
  for (const char* bad : {"", "daily", "daily 7", "daily 25:00", "weekly 07:00", "weekly fun 07:00",
                          "every 4 min", "every 0 h", "every x min", "hourly", "every 5 days"})
    TEST_ASSERT_FALSE_MESSAGE(sch::parseEvery(bad, 300, s), bad);
  // The name: the first words, at most 24 B, valid UTF-8; the text itself is never cut.
  std::mt19937 r(3);
  for (int i = 0; i < 200; ++i) {
    const std::string t = i < (int)kCorpusN ? std::string(kCorpus[i]) : ut::textOf(r, 1 + r() % 80);
    const std::string n = sch::nameFor(t);
    TEST_ASSERT_TRUE(n.size() <= (size_t)kLoopNameMax);
    TEST_ASSERT_TRUE(nimbus::utf8Valid(n));
    TEST_ASSERT_TRUE(t.rfind(n, 0) == 0 || n.empty());
    TEST_ASSERT_TRUE(t.empty() || !n.empty());
  }
  TEST_ASSERT_EQUAL_STRING("check the train times to", sch::nameFor(kCorpus[1]).c_str());
}

// ---- the declaration -------------------------------------------------------------------------------
void test_the_schema_offers_exactly_the_arguments_read() {
  const auto* d = agent::plain::toolDecl("schedule");
  TEST_ASSERT_NOT_NULL(d);
  JsonDocument s;
  TEST_ASSERT_FALSE(deserializeJson(s, d->schema));
  std::set<std::string> keys;
  for (JsonPairConst p : s["properties"].as<JsonObjectConst>()) keys.insert(p.key().c_str());
  TEST_ASSERT_TRUE((keys == std::set<std::string>{"op", "kind", "id", "when", "text", "every"}));
  // A key the schema does not offer changes nothing (chat_id is not read: the entry delivers where
  // the turn was asked).
  MemHost a, b;
  run(a, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})", notes::Origin::Attended, "77");
  run(b, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief","chat_id":"99","name":"x"})",
      notes::Origin::Attended, "77");
  TEST_ASSERT_EQUAL_STRING(a.loopsFile.c_str(), b.loopsFile.c_str());
  TEST_ASSERT_EQUAL_STRING("77", a.loops[0].chatId.c_str());
}

// ---- fires: the outside mark, no card, the ceiling ---------------------------------------------------
void test_a_fire_after_outside_content_opens_the_next_fire_tainted() {
  MemHost h;
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"news brief"})").success);
  txb::MemFiles card;
  sch::FireBinding b;
  b.files = &card;
  std::vector<bool> taint;
  bool outside = true;
  b.run = [&](const agent::plain::RoutineFire& f, bool tainted, agent::plain::FireReport& rep) {
    taint.push_back(tainted);
    rep.result = agent::plain::FireResult::Sent;
    rep.output = agent::plain::RoutineOutput{f.fireAt, "brief " + std::to_string(taint.size())};
    if (outside) rep.outcome.end.events.push_back("change:outside");
    return agent::plain::Status::success();
  };
  LoopFireRequest req;
  req.id = h.loops[0].id;
  uint64_t t = kNow;
  sch::fire(h, req, b, t);   // reads outside content
  outside = false;
  for (int i = 0; i < 4; ++i) sch::fire(h, req, b, t += 86400);
  // fire 2 opens tainted (fire 1's output is in the state) and so do 3 and 4; fire 5 does not: the
  // state's three outputs are fires 2 to 4, none from outside content.
  TEST_ASSERT_TRUE((taint == std::vector<bool>{false, true, true, true, false}));
  TEST_ASSERT_TRUE(sch::findExtra(h.extras(), req.id)->outsideAt.empty());
  // Counter: an outside fire with no text (silent) adds nothing to the state and taints nothing.
  MemHost q;
  TEST_ASSERT_TRUE(run(q, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"quiet"})").success);
  txb::MemFiles card2;
  b.files = &card2;
  taint.clear();
  b.run = [&](const agent::plain::RoutineFire&, bool tainted, agent::plain::FireReport& rep) {
    taint.push_back(tainted);
    rep.result = agent::plain::FireResult::Silent;
    rep.outcome.end.events.push_back("change:outside");
    return agent::plain::Status::success();
  };
  req.id = q.loops[0].id;
  sch::fire(q, req, b, kNow);
  sch::fire(q, req, b, kNow + 86400);
  TEST_ASSERT_TRUE((taint == std::vector<bool>{false, false}));
}

void test_a_routine_with_no_card_is_skipped_and_a_reminder_still_goes() {
  MemHost h;
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})").success);
  sch::FireBinding b;   // no files: no card
  int routines = 0;
  b.run = [&](const agent::plain::RoutineFire&, bool, agent::plain::FireReport&) {
    ++routines;
    return agent::plain::Status::success();
  };
  LoopFireRequest req;
  req.id = h.loops[0].id;
  const FireOutcome o = sch::fire(h, req, b, kNow);
  TEST_ASSERT_EQUAL(0, routines);
  TEST_ASSERT_TRUE(o.deferred);
  TEST_ASSERT_EQUAL((int)LastResult::Skipped, (int)h.loops[0].lastResult);
  TEST_ASSERT_EQUAL_STRING("no card", h.loops[0].lastErr.c_str());
  // Counter: a reminder needs no card.
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  MemHost m;
  TEST_ASSERT_TRUE(run(m, "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(m, 600) + "\",\"text\":\"stretch\"}").success);
  sch::FireBinding rb;
  rb.owner = rig.sink();
  req.id = m.loops[0].id;
  req.chatId = "1001";
  TEST_ASSERT_TRUE(sch::fire(m, req, rb, kNow + 600).ok);
  TEST_ASSERT_EQUAL(1, (int)rig.sent.size());
  TEST_ASSERT_EQUAL_STRING("stretch", rig.sent[0].text.c_str());
}

void test_a_routine_fire_keeps_the_routine_ceiling() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.boot().ok());
  rig.routineModel.respondWith([](const harness_test::scripted::Recorded& req, size_t i) {
    harness_test::scripted::Step s =
        req.view.toolChoice == "none" ? harness_test::scripted::say("stopped")
                                      : harness_test::scripted::call("read_file", "{\"path\":\"p" + std::to_string(i) + ".md\"}");
    s.usage.input = 1000;
    s.usage.output = 50;
    return s;
  });
  MemHost h;
  TEST_ASSERT_TRUE(run(h, R"({"op":"create","kind":"routine","every":"daily 07:00","text":"read every file"})").success);
  sch::FireBinding b;
  b.owner = rig.sink();
  b.files = &rig.card;
  b.run = [&](const agent::plain::RoutineFire& f, bool tainted, agent::plain::FireReport& rep) {
    agent::plain::RoutineDeps d = rig.routineDeps();
    d.head.tainted = tainted;
    return agent::plain::runRoutine(d, f, rep);
  };
  LoopFireRequest req;
  req.id = h.loops[0].id;
  const FireOutcome o = sch::fire(h, req, b, rig.epoch);
  TEST_ASSERT_EQUAL(agent::plain::kRoutineRounds + 1, (int)rig.routineModel.requests().size());
  TEST_ASSERT_EQUAL(0, (int)rig.ownerModel.requests().size());
  TEST_ASSERT_TRUE(o.answered);
  // Its tokens are the fire's, for the daily ceilings.
  TEST_ASSERT_TRUE(o.tokens.total() > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_loops_json_written_by_the_tool_is_v511_bytes);
  RUN_TEST(test_sidecar_round_trip_and_orphans_are_dropped_on_the_next_write);
  RUN_TEST(test_an_id_with_no_entry_is_an_old_record);
  RUN_TEST(test_write_order_and_refused_writes);
  RUN_TEST(test_a_reminder_fire_delivers_its_text_byte_for_byte_with_no_provider_request);
  RUN_TEST(test_counter_a_routine_fire_runs_the_routine);
  RUN_TEST(test_lifecycle_over_10_seeds_changes_only_the_named_id);
  RUN_TEST(test_the_state_file_survives_a_torn_write);
  RUN_TEST(test_a_text_over_the_limit_is_refused_never_cut);
  RUN_TEST(test_a_turn_no_person_started_lists_and_sets_reminders_only);
  RUN_TEST(test_counter_an_attended_turn_does_all_four);
  RUN_TEST(test_update_keeps_id_and_counters_and_a_routine_change_needs_approval_again);
  RUN_TEST(test_ask_first_holds_a_reminder_for_approval);
  RUN_TEST(test_reminder_bounds_are_named_and_reminders_count_against_the_loop_cap);
  RUN_TEST(test_parse_when_every_and_the_name);
  RUN_TEST(test_the_schema_offers_exactly_the_arguments_read);
  RUN_TEST(test_a_fire_after_outside_content_opens_the_next_fire_tainted);
  RUN_TEST(test_a_routine_with_no_card_is_skipped_and_a_reminder_still_goes);
  RUN_TEST(test_a_routine_fire_keeps_the_routine_ceiling);
  return UNITY_END();
}
