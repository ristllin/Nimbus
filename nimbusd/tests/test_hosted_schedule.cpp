// test_hosted_schedule - engine 2's schedule tool on the hosted process (lane RB-U1b; PRD sections 4
// and 6), through nimbusd/src/hosted_loops.h on a real POSIX volume and the plain rig.
//   (a) the head sets a reminder through `schedule`; the routines tick fires it at its time; the
//       owner's chat gets the text byte for byte, his transcript the routine note, and the scripted
//       provider sees no request after the turn that set it (no model call for a reminder);
//   (b) loops.json beside loops2.json: loops.json is v5.1.1's format (it loads in loadLoops and holds
//       no engine-2 field), the entry's kind is in loops2.json; a restarted instance lists it;
//   (c) a turn no person started may set a reminder and list, but not create a routine; a call
//       with no turn recorded reads as such a turn (fail closed);
//   (d) engine 1's four tools are still registered with their words (loop.create, wakeup.set).
// Offline: scripted provider, the test clock (NIMBUSD_TEST_CLOCK=1), UTC, no keys.
#include <ArduinoJson.h>

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "../../test/support/scripted_provider/scripted_provider.h"
#include "../../test/support/tx_battery/mem_files.h"
#include "../../test/support/tx_battery/stub_wire.h"
#include "hosted_loops.h"
#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/unattended.h"
#include "plain_rig.h"
#include "posix_fs.h"
#include "test_util.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace orch = nimbus::orch;
namespace sch = nimbus::orch::schedule;

namespace {

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string freshRoot(const std::string& name) {
  const char* t = std::getenv("TMPDIR");
  const std::string root = std::string(t && *t ? t : "/tmp") + "/nimbusd-schedule-" + name + "-" + std::to_string(::getpid());
  ndtest::rmTree(root);
  nimbusd::fsutil::mkdirs(root);
  return root;
}

std::string fileAt(const std::string& path) {
  std::string out;
  return nimbusd::fsutil::readFile(path, out) ? out : std::string();
}

// "YYYY-MM-DD HH:MM" in UTC, `s` seconds from now on the test clock.
std::string whenIn(uint64_t s) {
  const time_t t = (time_t)(nimbusd::vclock::epoch() + s);
  struct tm g{};
  gmtime_r(&t, &g);
  char b[20];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min);
  return b;
}

std::string jsonStr(const std::string& s) {
  JsonDocument d;
  d.set(s);
  std::string out;
  serializeJson(d, out);
  return out;
}

// One hosted owner chat on engine 2 with the schedule tool attached and the routines table.
struct Host {
  explicit Host(const std::string& r) : root(r) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [] { return nimbusd::vclock::epoch(); };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    channel.send = [this](const std::string& chat, const std::string& text) {
      delivered.push_back(chat + "|" + text);
      return true;
    };
    channel.isAllowed = [](const std::string&) { return true; };
    channel.firstAllowedChat = [] { return std::string("2001"); };
    nimbusd::HostedLoopHooks hooks;
    hooks.world = [] { return orch::deliveryWorld("tok", "2001", ""); };
    hooks.fire = [this](const orch::LoopFireRequest& req) { return loops.fireE2(req, binding()); };
    loops.begin(root, hooks);
    nimbusd::PlainRigBindings b{&platform, &model, &files, txb::batteryWire(plain::Dialect::AnthropicMessages), nullptr,
                                nullptr};
    b.dispatch = loops.scheduleDispatch();
    rig.reset(new nimbusd::PlainRig(config(), b));
    loops.attachSchedule(rig->registry());
    loops.registerTools(rig->registry());
    booted = rig->boot();
  }

  static nimbusd::PlainRigConfig config() {
    nimbusd::PlainRigConfig c;
    c.chatKey = "2001";
    c.key = [] { return std::string("sk-fake-schedule"); };
    c.noCompaction = "test_hosted_schedule: the schedule tool's own cases (the hosted compaction is RB-I2's)";
    return c;
  }

  sch::FireBinding binding() {
    sch::FireBinding b;
    b.owner.chatKey = "2001";
    b.owner.chatId = "2001";
    b.owner.deliver = [this](const plain::DeliverInput& in, plain::DeliverOutcome& out) {
      return plain::deliverTurn(plain::DeliverDeps{&channel, nullptr, nullptr}, in, out);
    };
    b.owner.append = [this](const std::string& note, uint64_t at) {
      notes.push_back(note);
      return plain::appendOwnerNote(*rig->engine(), note, at);
    };
    b.files = &files;
    b.run = [this](const plain::RoutineFire&, bool, plain::FireReport&) {
      ++routineRuns;
      return plain::Status::success();
    };
    return b;
  }

  std::string root;
  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  txb::MemFiles files;
  agent::Channel channel;
  nimbusd::HostedLoops loops;
  std::unique_ptr<nimbusd::PlainRig> rig;
  plain::Status booted;
  std::vector<std::string> delivered, notes;
  int routineRuns = 0;
};

// ---- (a) and (b) -------------------------------------------------------------------------------
void testReminderFiresWithNoModelCall(ndtest::Ctx& c) {
  std::printf("\n-- (a) a reminder set by the head fires its text with no model call\n");
  const std::string root = freshRoot("a");
  const std::string text = "Bring the blue folder (r\xc3\xa9sum\xc3\xa9) and the \xe2\x82\xac" "20 for the parking meter.";
  {
    Host h(root);
    c.ok(h.booted.ok(), "the rig boots with schedule attached");
    c.ok(h.rig->registry().has("schedule"), "schedule is registered");
    h.loops.tick(false, nullptr);   // the first sweep: nothing is due
    h.model.push(sp::call("schedule", "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(600) +
                                          "\",\"text\":" + jsonStr(text) + "}"));
    h.model.push(sp::say("Set for ten minutes from now."));
    const nimbusd::PlainTurn t = h.rig->say("remind me in ten minutes about the folder and the parking money");
    c.eq(t.stated, "Set for ten minutes from now.", "the turn answers after the schedule call");
    c.ok(h.model.requests().size() == 2 && has(h.model.requests()[1].body, "stored lp"), "the model read the stored entry");
    const size_t before = h.model.requests().size();
    // (b) the files: loops.json in v5.1.1's format, the kind in loops2.json.
    std::vector<orch::LoopRecord> back;
    c.ok(orch::loadLoops(fileAt(root + "/loops.json"), back) && back.size() == 1, "loops.json loads in loadLoops");
    c.ok(!back.empty() && back[0].prompt == text, "the text is stored whole");
    c.ok(!has(fileAt(root + "/loops.json"), "\"kind\":\"reminder\""), "loops.json holds no engine-2 field");
    c.ok(has(fileAt(root + "/loops2.json"), "\"kind\":\"reminder\""), "loops2.json holds its kind");
    // Its time comes: the tick fires it.
    nimbusd::vclock::advance(700);
    h.loops.tick(false, nullptr);
    c.eqi((int)h.model.requests().size(), (int)before, "no provider request for the reminder's fire");
    c.eqi(h.routineRuns, 0, "no routine ran");
    std::string got;
    for (const std::string& d : h.delivered) got += d.substr(d.find('|') + 1);
    c.ok(h.delivered.size() == 1 && h.delivered[0].rfind("2001|", 0) == 0, "one message to the owner's chat");
    c.eq(got, text, "the owner gets the text byte for byte");
    c.ok(h.notes.size() == 1 && has(h.notes[0], "[routine ") && has(h.notes[0], text),
         "his transcript gets the routine note with the text");
    c.eqi((int)h.loops.count(), 0, "a fired reminder retires");
    std::string why;
    c.ok(h.model.verify(why), "the script was used exactly (" + why + ")");
  }
  ndtest::rmTree(root);
}

void testRestartKeepsTheEntry(ndtest::Ctx& c) {
  std::printf("\n-- (b) a restarted instance lists what engine 2 stored\n");
  const std::string root = freshRoot("b");
  std::string id;
  {
    Host h(root);
    h.model.push(sp::call("schedule", R"({"op":"create","kind":"routine","every":"weekly mon,thu 18:30","text":"tidy the downloads folder"})"));
    h.model.push(sp::say("Done."));
    h.rig->say("every Monday and Thursday evening, tidy my downloads");
    std::vector<orch::LoopRecord> back;
    c.ok(orch::loadLoops(fileAt(root + "/loops.json"), back) && back.size() == 1, "the routine is in loops.json");
    if (!back.empty()) id = back[0].id;
    c.ok(!back.empty() && !back[0].approved, "the assistant's routine waits for the owner's approval");
  }
  Host again(root);
  again.model.push(sp::call("schedule", R"({"op":"list"})"));
  again.model.push(sp::say("One routine."));
  again.rig->say("what is scheduled?");
  c.ok(again.model.requests().size() == 2 && has(again.model.requests()[1].body, id + " routine weekly mon,thu 18:30"),
       "the list after a restart names it with its kind and cadence");
  // Deleting it takes its state file with it (a later entry never inherits it).
  again.loops.setStateFiles(&again.files);
  const std::string state = sch::statePath(id);
  again.files.files[state] = "{\"at\":1,\"text\":\"old\"}\n";
  again.model.push(sp::call("schedule", "{\"op\":\"delete\",\"id\":\"" + id + "\"}"));
  again.model.push(sp::say("Deleted."));
  again.rig->say("stop the downloads routine");
  c.eqi((int)again.loops.count(), 0, "the routine is gone");
  c.ok(again.files.files.count(state) == 0, "its state file is gone");
  c.ok(!has(fileAt(root + "/loops2.json"), id), "its loops2.json entry is gone");
  ndtest::rmTree(root);
}

// ---- (c) origins ------------------------------------------------------------------------------------
void testOrigins(ndtest::Ctx& c) {
  std::printf("\n-- (c) a turn no person started sets reminders and lists only\n");
  const std::string root = freshRoot("c");
  Host h(root);
  const plain::ToolDispatchFn d = h.loops.scheduleDispatch();
  const orch::Principal owner = orch::principalForRole("2001", orch::Role::Admin);
  auto callAs = [&](plain::Trigger tr, const std::string& args) {
    plain::TurnCtx turn;
    turn.trigger = tr;
    turn.who = &owner;
    turn.chatId = "2001";
    plain::ToolCall call;
    call.id = "c1";
    call.name = "schedule";
    call.argsJson = args;
    return d(call, turn);
  };
  const std::string routine = R"({"op":"create","kind":"routine","every":"daily 07:00","text":"brief"})";
  const std::string reminder = "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(900) + "\",\"text\":\"stand up\"}";
  for (plain::Trigger tr : {plain::Trigger::Routine, plain::Trigger::AgentResult, plain::Trigger::Notice, plain::Trigger::Resume}) {
    c.ok(callAs(tr, routine).isError, "an unattended turn cannot create a routine");
    c.ok(!callAs(tr, R"({"op":"list"})").isError, "it may list");
  }
  c.ok(!callAs(plain::Trigger::Routine, reminder).isError, "it may set a reminder");
  c.ok(!callAs(plain::Trigger::Owner, routine).isError, "the owner's own turn creates a routine");
  // No turn recorded (a registry call outside the dispatch binding): unattended, fail closed.
  JsonDocument a;
  deserializeJson(a, routine);
  c.ok(!h.rig->registry().dispatch("schedule", a.as<JsonObjectConst>(), owner).success,
       "a call with no turn recorded cannot create a routine");
  c.eqi((int)h.loops.count(), 2, "one reminder and the owner's routine");
  ndtest::rmTree(root);
}

// ---- (d) engine 1's tools keep their words ---------------------------------------------------------
void testEngineOneToolsUnchanged(ndtest::Ctx& c) {
  std::printf("\n-- (d) loop.create and wakeup.set keep their words\n");
  nimbusd::HostedLoops loops;
  orch::ToolRegistry reg;
  loops.registerTools(reg);
  bool create = false, wake = false;
  for (const auto& t : reg.manifest()) {
    if (t.name == "loop.create") create = has(t.description, "Schedule a recurring or timed task ('local loop')");
    if (t.name == "wakeup.set") wake = has(t.description, "Schedule ONE future wakeup for yourself");
  }
  c.ok(create && wake, "both are registered with their engine-1 descriptions");
  c.ok(!reg.has("schedule"), "registerTools adds no engine-2 tool");
}

}  // namespace

int main() {
  setenv("TZ", "UTC", 1);
  tzset();
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  ndtest::Ctx c;
  std::printf("=== engine 2's schedule tool on the hosted process (lane RB-U1b, offline) ===\n");
  testReminderFiresWithNoModelCall(c);
  testRestartKeepsTheEntry(c);
  testOrigins(c);
  testEngineOneToolsUnchanged(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
