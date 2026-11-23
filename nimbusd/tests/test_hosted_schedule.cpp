// test_hosted_schedule - engine 2's schedule tool on the hosted process (lane RB-U1b; PRD sections 4
// and 6), through nimbusd/src/hosted_loops.h on a real POSIX volume and the plain rig.
//   (a) the head sets a reminder through `schedule`; the routines tick fires it at its time; the
//       owner's chat gets the text byte for byte, his transcript the routine note, and the scripted
//       provider sees no request after the turn that set it (no model call for a reminder);
//   (b) loops.json beside loops2.json: loops.json is v5.1.1's format (it loads in loadLoops and holds
//       no engine-2 field), the entry's kind is in loops2.json; a restarted instance lists it;
//   (c) a turn no person started may set a reminder and list, but not create a routine; a call
//       with no turn recorded reads as such a turn (fail closed);
//   (d) engine 1's four tools are still registered with their words (loop.create, wakeup.set);
//   (e) lane E2-SCHED: the daemon's engine-2 owner chat (Engine2Host) with the routines table bound:
//       the head sets a reminder and a routine through `schedule`; the tick fires the reminder through
//       Engine2Host::fire with its text and no request, and the routine isolated on the sub-agent model,
//       its output to the owner and its state file under <data>/mem/routines/.
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
#include "engine2_host.h"
#include <algorithm>

#include "hosted_loops.h"
#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/unattended.h"
#include "nimbus/orch/loop_delivery.h"
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

// ---- (e) the daemon's engine-2 owner chat fires the entries (lane E2-SCHED) -------------------------
void testEngine2HostFires(ndtest::Ctx& c) {
  std::printf("\n-- (e) Engine2Host: a reminder with no request, a routine isolated on the sub model\n");
  const std::string root = freshRoot("e");
  orch::setHostedWebChatAlias(true);   // as the hosted composition sets it (rig.h): the web chat is a device channel
  nimbusd::PosixFiles files(root + "/mem/files");
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  std::vector<std::string> sent;
  nimbusd::HostedLoops loops;
  std::unique_ptr<nimbusd::Engine2Host> host;
  nimbusd::HostedLoopHooks hooks;
  hooks.world = [] { return orch::deliveryWorld("", "", ""); };
  hooks.fire = [&host](const orch::LoopFireRequest& r) { return host->fire(r); };
  loops.begin(root, hooks);
  nimbusd::Engine2Options o;
  o.dataDir = root;
  o.files = &files;
  o.head = [] { return std::string("anthropic"); };
  o.model = [] { return std::string("claude-sonnet-5-5"); };
  o.summarizerModel = [] { return std::string("claude-haiku-5-5"); };
  o.key = [] { return std::string("sk-fake-e2sched"); };
  o.utcOffsetMin = [](uint32_t) { return 0; };
  o.deliver = [&sent](const std::string& chat, const std::string& text) { sent.push_back(chat + "|" + text); };
  o.transport = &model;
  o.loops = &loops;
  o.chatId = orch::kHostedWebChatId;
  std::vector<std::string> tags;   // lane E2-FIX7: what reaches the instance's ceiling
  bool paused = false;
  o.spend = [&tags](const orch::TurnSpend& s, const std::string& tag) {
    if (!s.empty()) tags.push_back(tag);
  };
  o.paused = [&paused] { return paused; };
  host.reset(new nimbusd::Engine2Host(o));
  c.eq(host->start(), "", "the engine-2 owner chat starts with the routines table bound");
  c.ok(host->rig() && host->rig()->registry().has("schedule"), "schedule is on its registry");
  const std::string text = "stretch your back";
  model.push(sp::call("schedule", "{\"op\":\"create\",\"kind\":\"reminder\",\"when\":\"" + whenIn(300) +
                                      "\",\"text\":" + jsonStr(text) + "}"));
  model.push(sp::call("schedule", R"({"op":"create","kind":"routine","every":"every 5 min","text":"say the word ORBIT"})"));
  model.push(sp::say("Both set."));
  host->say(orch::kHostedWebChatId, "remind me to stretch in five minutes, and every five minutes say ORBIT", "web");
  std::vector<orch::LoopRecord> back;
  c.ok(orch::loadLoops(fileAt(root + "/loops.json"), back) && back.size() == 2, "both entries are in loops.json");
  std::string routineId;
  for (const orch::LoopRecord& l : back)
    if (l.sched.kind != orch::SchedKind::Once) routineId = l.id;
  c.ok(!routineId.empty() && loops.approve(routineId), "the owner approves the routine (/loop approve)");
  const size_t before = model.requests().size();
  sent.clear();
  loops.tick(false, nullptr);    // the first sweep: nothing is due
  nimbusd::vclock::advance(400);
  loops.tick(false, nullptr);    // one due fire per sweep: the earliest, the reminder
  c.eqi((int)model.requests().size(), (int)before, "the reminder's fire made no provider request");
  c.ok(sent.size() == 1 && sent[0] == std::string(orch::kHostedWebChatId) + "|" + text, "the owner chat gets the reminder's text exactly");
  model.push(sp::say("ORBIT"));
  nimbusd::vclock::advance(120);
  loops.tick(false, nullptr);
  c.eqi((int)model.requests().size(), (int)before + 1, "the routine's fire is one request");
  c.ok(model.requests().size() == before + 1 && has(model.requests()[before].body, "claude-haiku-5-5"),
       "...on the sub-agent model");
  c.ok(model.requests().size() == before + 1 && !has(model.requests()[before].body, "remind me to stretch"),
       "...with none of the owner's conversation");
  // The test clock reads 00:02 UTC: quiet hours. The note waits in the outbox, then goes when they end.
  c.ok(sent.size() == 1, "in quiet hours nothing is sent yet");
  c.ok(has(fileAt(root + "/mem/routines/held-" + std::string(orch::kHostedWebChatId) + ".json"), "ORBIT"),
       "the output waits in the outbox on the volume");
  nimbusd::vclock::advance(9 * 3600);
  host->idle(false);
  c.ok(sent.size() == 2 && sent[1] == std::string(orch::kHostedWebChatId) + "|ORBIT",
       "after quiet hours the idle step sends the output to the owner chat");
  c.ok(has(fileAt(root + "/mem/routines/" + routineId + ".state"), "ORBIT"), "its state file holds the output");
  // Lane E2-FIX7 (M3, M4, M7; red on 88bfbe826): the routine's spend reaches the ceiling, and once it is reached no
  // engine-2 path makes a paid request: the routine waits, a Telegram turn gets the pause line, an idle step runs
  // no compaction.
  c.ok(std::count(tags.begin(), tags.end(), std::string("routine")) == 1, "the routine's spend is counted for the ceiling");
  paused = true;
  const size_t frozenAt = model.requests().size();
  nimbusd::vclock::advance(400);
  loops.tick(false, nullptr);
  c.eqi((int)model.requests().size(), (int)frozenAt, "paused: the due routine waits, no request");
  sent.clear();
  host->say("12345", "hello from telegram", "tg");
  c.eqi((int)model.requests().size(), (int)frozenAt, "paused: a Telegram turn sends no request");
  c.ok(sent.size() == 1 && sent[0] == std::string("12345|") + nimbusd::kPausedLine, "...and the person gets the pause line");
  c.ok(host->requestCompaction(), "a /compact is queued");
  host->idle(false);
  c.eqi((int)model.requests().size(), (int)frozenAt, "paused: the idle step runs no compaction");
  std::string why;
  c.ok(model.verify(why), "the script was used exactly (" + why + ")");
  host.reset();
  ndtest::rmTree(root);
}

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
  testEngine2HostFires(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
