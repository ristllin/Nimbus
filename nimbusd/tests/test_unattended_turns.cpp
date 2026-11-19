// test_unattended_turns - a turn nobody asked for cannot change the owner's files or speak
// into the chat with invented content (lane RB-Q2a, PRD section 6, DIAGNOSIS RC-13).
//
// A real hosted turn (the old engine, the real registry with its access rules, the real
// file tools over a real folder) answered by a scripted model, for every class of
// unattended turn nimbusd runs: a routine (a check-in), an owner reminder, the assistant's
// own wakeup, a self-wakeup and a sub-agent report. The model calls EVERY writing,
// sending, spending and settings tool the instance registers, each aimed at the owner's
// files. Expected, per class:
//   (a) every such call answers a plain "Not run: ..." line the model reads in its next
//       request (never a silent drop, never a crash);
//   (b) the bytes of every owner file are unchanged and no file appears (a food CSV, an
//       expenses CSV and a notes file, the same expectations: the policy names no domain);
//   (c) the turn's own reply is still delivered (held delivery).
// Counter-test: the same model in a turn the owner writes changes the files.
// Then: the Sunday reminder routine yields its reminder 10 of 10 whatever the model tries
// first, and a self-wakeup still fires and delivers under read-only tools.
// Offline: a fake transport, no provider traffic, provider keys unset.
#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../../test/support/fake_http.h"
#include "nimbus/orch/self_wakeup.h"
#include "nimbus/orch/unattended_policy.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

namespace {

const char* const kOwner = "owner";
const std::map<std::string, std::string> kOwnerFiles = {
    {"food/log.csv", "date,item,kcal\n2026-11-15,oats,310\n2026-11-15,banana,105\n"},
    {"money/expenses.csv", "date,what,amount\n2026-11-14,groceries,42.10\n"},
    {"notes/shopping.md", "- milk\n- bread\n"},
};

// Every regular file under `root`, path -> bytes.
void snapshot(const std::string& root, const std::string& rel, std::map<std::string, std::string>& out) {
  DIR* d = ::opendir((root + "/" + rel).c_str());
  if (!d) return;
  while (struct dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    const std::string p = rel.empty() ? n : rel + "/" + n;
    struct stat st;
    if (::stat((root + "/" + p).c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      snapshot(root, p, out);
    } else {
      std::string body;
      fsutil::readFile(root + "/" + p, body);
      out[p] = body;
    }
  }
  ::closedir(d);
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// One Responses-API output item: a function call.
std::string call(const std::string& id, const std::string& wireName, const std::string& argsJson) {
  return "{\"type\":\"function_call\",\"call_id\":\"" + id + "\",\"name\":\"" + wireName + "\",\"arguments\":\"" +
         jsonEscape(argsJson) + "\"}";
}
harness_test::Exchange round(const std::vector<std::string>& calls) {
  std::string out;
  for (const std::string& c : calls) out += (out.empty() ? "" : ",") + c;
  return harness_test::Exchange{"", "", 200, "{\"id\":\"r\",\"status\":\"completed\",\"output\":[" + out + "]}"};
}
harness_test::Exchange answer(const std::string& reply) {
  return round({call("fin", "orch_turn", "{\"reply\":\"" + jsonEscape(reply) + "\"}")});
}

std::string wire(const std::string& reg) {
  std::string w = reg;
  for (char& c : w)
    if (c == '.') c = '_';
  return w;
}

// Arguments that aim a call at the owner's files, whatever its shape: every argument name
// the file, send, memory and device tools read, pointing at the food log.
const char* kAimed =
    "{\"project\":\"food\",\"name\":\"log.csv\",\"path\":\"food/log.csv\",\"text\":\"2026-11-15,oats,310\","
    "\"content\":\"invented\",\"old\":\"banana\",\"new\":\"apple\",\"old_text\":\"banana\",\"new_text\":\"apple\","
    "\"rows\":[\"2026-11-15,oats,310\"],\"url\":\"https://example.com/x\",\"chat_id\":\"\",\"query\":\"x\","
    "\"task\":\"x\",\"prompt\":\"x\",\"note\":\"x\",\"when\":\"5m\",\"in\":\"5m\",\"id\":\"x\",\"key\":\"x\","
    "\"value\":\"x\",\"action\":{\"type\":\"lights\",\"value\":\"off\"},\"share\":true}";

struct Probe {
  std::string dir;
  harness_test::FakeHttpTransport tx;
  Config cfg;
  std::unique_ptr<NimbusdRig> rig;
  std::map<std::string, std::string> before;
  std::vector<std::string> effectTools;   // every registered name the policy does not read as Read
  std::vector<std::string> sent;          // every message the instance delivered
  std::vector<std::string> sentByTool;    // every send a sending tool made

  explicit Probe(const std::string& tag) {
    for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                          "CUMULO_API_KEY", "Z_AI_TOKEN"})
      unsetenv(k);
    setenv("CUMULO_API_KEY", "cumulo_sk_test_Q2A", 1);   // a fake key: the transport is scripted
    ndtest::rmTree(ndtest::scratchDir(tag));
    dir = ndtest::scratchDir(tag) + "/data";
    NimbusdRig::Options o;
    o.dataDir = dir;
    o.embeddings = false;
    o.embedDims = 64;
    o.toolLoop = true;
    rig.reset(new NimbusdRig(cfg, o, &tx));
    rig->setDeliver([this](const std::string&, const std::string& text) { sent.push_back(text); });
    // The device's sending tools (a hosted instance registers none): each records what it
    // would have sent, so a send that got past the policy shows. (reply.speak, the device's
    // own speaker, is Ambient: the owner's routine and reminder may speak.)
    for (const char* t : {"reply.telegram", "files.send"})
      rig->registry().add(t, "send a message (test double of the device tool)",
                          [this, t](ArduinoJson::JsonObjectConst, const orch::Principal&) {
                            sentByTool.push_back(t);
                            return orch::ToolResult::ok("sent");
                          });
    for (const auto& f : kOwnerFiles) rig->files().seed(f.first, f.second);
    snapshot(rig->filesDir(), "", before);
    ArduinoJson::JsonDocument none;
    for (const auto& s : rig->registry().toolSpecs())
      if (orch::toolUseFor(s.name, none.as<ArduinoJson::JsonObjectConst>()) != orch::ToolUse::Read)
        effectTools.push_back(s.name);
  }
  ~Probe() {
    rig.reset();
    unsetenv("CUMULO_API_KEY");
  }
  bool filesUnchanged() {
    std::map<std::string, std::string> now;
    snapshot(rig->filesDir(), "", now);
    return now == before;
  }
  // Round 1: every effect tool aimed at the owner's files; then the answer.
  void scriptEveryEffect(const std::string& reply) {
    std::vector<std::string> calls;
    for (size_t i = 0; i < effectTools.size(); ++i)
      calls.push_back(call("e" + std::to_string(i), wire(effectTools[i]), kAimed));
    tx.script.push_back(round(calls));
    tx.script.push_back(answer(reply));
  }
  // How many round-1 results are a plain refusal: the unattended policy's line, or the
  // action rules' line for what only the owner writing now may do (read first).
  int refusalsSeen() const {
    int n = 0;
    for (const auto& r : results())
      if (r.second.find("Not run: ") != std::string::npos ||
          r.second.find("an automatic turn (a routine, a wakeup or a sub-agent results summary) cannot") !=
              std::string::npos)
        ++n;
    return n;
  }
  bool delivered(const std::string& text) const {
    for (const auto& d : sent)
      if (d.find(text) != std::string::npos) return true;
    return false;
  }
  // The result each call of round 1 got, by call id, from the request that followed.
  std::map<std::string, std::string> results() const {
    std::map<std::string, std::string> out;
    if (tx.seen.size() < 2) return out;
    ArduinoJson::JsonDocument d;
    if (deserializeJson(d, tx.seen[1].body, ArduinoJson::DeserializationOption::NestingLimit(64))) return out;
    for (ArduinoJson::JsonObjectConst it : d["input"].as<ArduinoJson::JsonArrayConst>())
      if (std::string(it["type"] | "") == "function_call_output") out[it["call_id"] | ""] = it["output"] | "";
    return out;
  }
};

// The output a request carried for one call id ("" when none).
std::string resultIn(const std::string& body, const std::string& id) {
  ArduinoJson::JsonDocument d;
  if (deserializeJson(d, body, ArduinoJson::DeserializationOption::NestingLimit(64))) return "";
  for (ArduinoJson::JsonObjectConst it : d["input"].as<ArduinoJson::JsonArrayConst>())
    if (std::string(it["type"] | "") == "function_call_output" && std::string(it["call_id"] | "") == id)
      return it["output"] | "";
  return "";
}

std::string wakeNote(const std::string& text) {
  orch::WakeNote n;
  n.kind = orch::WakeKind::Routine;
  n.text = text;
  n.noticedEpoch = 1763452800;
  n.noticedMinute = 23 * 60;
  return orch::dumpWakeNote(n);
}

// Fire one unattended turn of class `c` on the probe's rig.
void fire(Probe& p, orch::TurnClass c) {
  agent::TurnEngine& e = p.rig->engine();
  switch (c) {
    case orch::TurnClass::Routine:
      e.injectScheduledTurn(kOwner, "Check in on the day so far.", "check-in", "lp00a1");
      break;
    case orch::TurnClass::Reminder:
      e.injectScheduledTurn(kOwner, "buy milk", "reminder", "lp00a2", false, true, true);
      break;
    case orch::TurnClass::Wakeup:
      e.injectScheduledTurn(kOwner, "look at the day again", "follow-up", "lp00a3", false, true);
      break;
    case orch::TurnClass::SelfWakeup:
      e.injectScheduledTurn(kOwner, wakeNote("The 08:10 train is cancelled."), "wake", "sw00a4", false, true, false,
                            true);
      break;
    case orch::TurnClass::SubagentReport:
      p.rig->jobs().addFreshResult("job0001", "modelX", "the research found three shops open on Sunday");
      e.maybeConsolidate(kOwner);
      break;
    default:
      break;
  }
}

// What each class may do beyond reading, written here from PRD section 6 (not read from
// the table under test).
bool classMayUse(orch::TurnClass cls, orch::ToolUse u) {
  using U = orch::ToolUse;
  switch (cls) {
    case orch::TurnClass::Routine:        return u == U::Schedule;   // the owner's switch is off
    case orch::TurnClass::Reminder:       return u == U::Ambient;
    case orch::TurnClass::Wakeup:         return u == U::Ambient || u == U::Schedule;
    // its results are outside content: a send carries only a named artifact (none here)
    case orch::TurnClass::SubagentReport: return u == U::Spend || u == U::Scratch;
    default:                              return false;
  }
}

// How many of `tools`, aimed at the owner's files, a turn of `cls` must refuse: by the
// policy, or first by the action rules (what only the owner writing now may do). A fire
// driven straight through the engine leaves the rig's turn chat unset, so a files.send
// naming no chat reads as another chat (the stricter key), as the seam sees it.
int expectedRefusals(orch::TurnClass cls, const std::vector<std::string>& tools) {
  ArduinoJson::JsonDocument aimed;
  deserializeJson(aimed, kAimed);
  const auto args = aimed.as<ArduinoJson::JsonObjectConst>();
  orch::ActionCaller owner;
  owner.role = orch::Role::Admin;
  owner.unattended = true;
  int n = 0;
  for (const std::string& t : tools)
    if (!classMayUse(cls, orch::toolUseFor(t, args)) ||
        !orch::actionRefusal(orch::toolActionKey(t, args, owner), owner).empty())
      ++n;
  return n;
}

// The classes that are offered tools (a self-wakeup is one short request: its own test).
const orch::TurnClass kClasses[] = {orch::TurnClass::Routine, orch::TurnClass::Reminder, orch::TurnClass::Wakeup,
                                    orch::TurnClass::SubagentReport};

}  // namespace

// (a), (b), (c) for every class nimbusd runs.
static void testEveryClassCannotWriteOrSend(ndtest::Ctx& c) {
  std::printf("\n-- every unattended class: every effect tool refused, owner files untouched --\n");
  for (orch::TurnClass cls : kClasses) {
    const std::string slug = orch::turnClassSlug(cls);
    Probe p(std::string("q2a-") + slug);
    c.ok(p.effectTools.size() >= 10, slug + ": the instance registers writing and sending tools to try (" +
                                         std::to_string(p.effectTools.size()) + ")");
    p.scriptEveryEffect("All good, here is the summary.");
    fire(p, cls);
    const int refusable = expectedRefusals(cls, p.effectTools);
    c.ok(p.tx.seen.size() == 2, slug + ": two requests, the effect round and the answer (" +
                                    std::to_string(p.tx.seen.size()) + ")");
    c.eqi(p.refusalsSeen(), refusable, slug + ": every refused call reads a plain Not run line in the next request");
    c.ok(p.filesUnchanged(), slug + ": every owner file byte-identical, no file added or removed");
    for (const auto& f : kOwnerFiles)
      c.eq(p.rig->files().get(f.first), f.second, slug + ": " + f.first + " unchanged");
    c.ok(p.delivered("All good, here is the summary."), slug + ": the turn's own reply is delivered");
    // Send is open to the sub-agent report alone (its own chat; another chat is the owner's).
    const bool mayReport = cls == orch::TurnClass::SubagentReport;
    c.ok(mayReport || (!p.delivered("invented") && p.sentByTool.empty()),
         slug + ": nothing the model aimed at a chat was sent");
    c.ok(!p.rig->engine().inScheduledTurn() &&
             p.rig->engine().unattendedClass() == orch::TurnClass::Attended,
         slug + ": the class ends with the turn");
  }
}

// Counter-test: the same model in a turn the owner writes reaches the files (the probe can
// see a change), so the green above is the policy, not a blind rig.
static void testOwnerTurnStillWrites(ndtest::Ctx& c) {
  std::printf("\n-- counter-test: the owner's own turn --\n");
  Probe p("q2a-owner");
  p.tx.script.push_back(round({call("w1", "files_append",
                                    "{\"project\":\"food\",\"name\":\"log.csv\",\"text\":\"2026-11-16,skyr,120\"}")}));
  p.tx.script.push_back(answer("Added."));
  p.rig->say(kOwner, "add skyr 120 to my food log");
  c.ok(!p.filesUnchanged(), "an attended append changes the file");
  c.ok(p.rig->files().get("food/log.csv").find("skyr") != std::string::npos, "the row is in the food log");
  c.eqi(p.refusalsSeen(), 0, "no policy refusal on a person's turn");
}

// The Sunday reminder routine yields its reminder every time, whatever the model tries
// first: a refused send, a read of the list, or the reply straight away.
static void testSundayReminderTenOfTen(ndtest::Ctx& c) {
  std::printf("\n-- the Sunday reminder routine, 10 fires --\n");
  int yielded = 0;
  for (int k = 0; k < 10; ++k) {
    Probe p("q2a-sunday-" + std::to_string(k));
    const std::string reminder = "Sunday reminder: the shopping list has milk and bread.";
    if (k % 3 == 0)
      p.tx.script.push_back(round({call("s1", "reply_telegram", "{\"text\":\"" + reminder + "\"}"),
                                   call("w1", "files_append",
                                        "{\"project\":\"notes\",\"name\":\"shopping.md\",\"text\":\"- eggs\"}")}));
    else if (k % 3 == 1)
      p.tx.script.push_back(round({call("r1", "files_read", "{\"project\":\"notes\",\"name\":\"shopping.md\"}")}));
    p.tx.script.push_back(answer(reminder));
    p.rig->engine().injectScheduledTurn(kOwner, "It is Sunday: remind me to do the shopping.", "sunday-shopping",
                                        "lp5d01");
    const bool ok = p.delivered(reminder) && p.filesUnchanged();
    if (k % 3 == 1)   // the read ran: its result (the list) is in the answer's request
      c.ok(p.tx.seen.size() == 2 && p.tx.seen[1].body.find("- milk") != std::string::npos,
           "fire " + std::to_string(k) + ": the routine read the list");
    if (k % 3 == 0)
      c.ok(p.refusalsSeen() == 2 && p.sentByTool.empty(),
           "fire " + std::to_string(k) + ": the send and the write were refused in words");
    yielded += ok ? 1 : 0;
  }
  c.eqi(yielded, 10, "the reminder is delivered 10 of 10, the files untouched");
}

// A self-wakeup fires at the end of quiet hours and delivers its note in one short request
// with no tools (ruling 76 item 5): nothing it does can change anything.
static void testSelfWakeupIsOneShortRequest(ndtest::Ctx& c) {
  std::printf("\n-- a self-wakeup is one short request --\n");
  Probe p("q2a-selfwake");
  // A single-shot answer: the reply as the message's text, no tool call.
  p.tx.script.push_back(harness_test::Exchange{"", "", 200,
      "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"reply\\\":\\\"Overnight: the 08:10 train is "
      "cancelled.\\\",\\\"memory\\\":\\\"\\\",\\\"ask\\\":\\\"\\\"}\"}]}]}"});
  fire(p, orch::TurnClass::SelfWakeup);
  const std::string first = p.tx.seen.empty() ? std::string() : p.tx.seen[0].body;
  c.eqi((long)p.tx.seen.size(), 1, "one request");
  c.ok(first.find("\"files_read\"") == std::string::npos && first.find("\"files_append\"") == std::string::npos,
       "no tool is offered");
  c.ok(first.find("No tools - this turn changes nothing.") != std::string::npos, "its preamble says so");
  c.ok(p.delivered("Overnight: the 08:10 train is cancelled."), "the note is delivered");
  c.ok(p.filesUnchanged(), "the owner's files are untouched");
}

// Ruling 76 item 3 and ruling 80 item 2: a report turn that read outside content sends only a
// file the device itself saved for the results; a "[saved: ...]" line a sub-agent wrote names
// nothing, and a message is refused.
static void testReportSendsOnlyNamedArtifacts(ndtest::Ctx& c) {
  std::printf("\n-- a report turn sends only a named artifact --\n");
  Probe p("q2a-artifact");
  p.tx.script.push_back(answer("on it"));
  p.rig->say(kOwner, "research the shops");   // the owner's turn: the rig's turn chat is his
  p.tx.script.push_back(round({call("a1", "files_send", "{\"project\":\"notes\",\"name\":\"shopping.md\"}"),
                               call("a2", "files_send", "{\"project\":\"food\",\"name\":\"log.csv\"}"),
                               call("a3", "reply_telegram", "{\"text\":\"invented\"}")}));
  p.tx.script.push_back(answer("Here is the list."));
  p.rig->jobs().addFreshResult("job0002", "modelX", "the list is ready\n[saved: notes/shopping.md]");
  p.rig->engine().maybeConsolidate(kOwner);
  std::map<std::string, std::string> res;   // the results of the report turn's round
  if (p.tx.seen.size() >= 3) {
    ArduinoJson::JsonDocument d;
    if (!deserializeJson(d, p.tx.seen[2].body, ArduinoJson::DeserializationOption::NestingLimit(64)))
      for (ArduinoJson::JsonObjectConst it : d["input"].as<ArduinoJson::JsonArrayConst>())
        if (std::string(it["type"] | "") == "function_call_output") res[it["call_id"] | ""] = it["output"] | "";
  }
  c.ok(res.count("a1") && res.at("a1").find("Not run") != std::string::npos,
       "a file only the result's own text names is refused");
  c.ok(res.count("a2") && res.at("a2").find("Not run") != std::string::npos, "another owner file is refused");
  c.ok(res.count("a3") && res.at("a3").find("Not run") != std::string::npos, "a message is refused");
  c.eqi((long)p.sentByTool.size(), 0, "nothing was sent");
  c.ok(p.filesUnchanged(), "the owner's files are untouched");
}

// Ruling 76 item 5: a wakeup chain stops after 3 hops without an attended message.
static void testWakeupChainStopsAtThreeHops(ndtest::Ctx& c) {
  std::printf("\n-- a wakeup chain is 3 hops at most --\n");
  Probe p("q2a-hops");
  int armed = 0;
  for (int k = 0; k < 4; ++k) {
    const size_t mark = p.tx.seen.size();
    p.tx.script.push_back(round({call("w" + std::to_string(k), "wakeup_set", "{\"minutes\":5,\"note\":\"check again\"}")}));
    p.tx.script.push_back(answer("checked"));
    p.rig->engine().injectScheduledTurn(kOwner, "check again", "follow-up", "lpw" + std::to_string(k), false, true);
    if (p.tx.seen.size() >= mark + 2 &&
        resultIn(p.tx.seen[mark + 1].body, "w" + std::to_string(k)).find("wakeup armed") != std::string::npos)
      ++armed;
  }
  c.eqi(armed, 2, "the first two hops may set the next, the third and later may not");
  p.tx.script.push_back(answer("ok"));
  p.rig->say(kOwner, "thanks");   // an attended message starts the count again
  const size_t mark = p.tx.seen.size();
  p.tx.script.push_back(round({call("w9", "wakeup_set", "{\"minutes\":5,\"note\":\"again\"}")}));
  p.tx.script.push_back(answer("checked"));
  p.rig->engine().injectScheduledTurn(kOwner, "check again", "follow-up", "lpw9", false, true);
  c.ok(p.tx.seen.size() >= mark + 2 && resultIn(p.tx.seen[mark + 1].body, "w9").find("wakeup armed") != std::string::npos,
       "after the owner writes, a wakeup may set the next again");
}

// Ruling 76 item 7: the owner's routine switch. Off (the default): a routine reads, sets its
// follow-up and replies; its spawn is refused. On: its spawn is accepted (inside the run caps).
// Both states: its write is refused and every owner file is byte-identical.
static void testRoutineSwitchBothStates(ndtest::Ctx& c) {
  std::printf("\n-- the owner's routine switch, off and on --\n");
  for (int on = 0; on < 2; ++on) {
    const std::string state = on ? "on" : "off";
    Probe p("q2a-grants-" + state);
    c.ok(!p.rig->routines().routineGrants(), state + ": the switch is off by default");
    p.rig->routines().setRoutineGrants(on == 1);
    c.eqi(p.rig->routines().routineGrants() ? 1 : 0, on, state + ": the switch holds");
    p.tx.script.push_back(round({call("w1", "files_append", kAimed)}));
    p.tx.script.push_back(harness_test::Exchange{"", "", 200,
        "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\","
        "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"brief ready\\\",\\\"session_ops\\\":"
        "[{\\\"op\\\":\\\"spawn\\\",\\\"task\\\":\\\"check the trains\\\",\\\"attach\\\":[\\\"food/log.csv\\\"]}]}\"}]}"});
    fire(p, orch::TurnClass::Routine);
    c.eqi(p.rig->jobs().pendingCount(), on, state + (on ? ": the routine's sub-agent is queued" : ": its spawn is refused"));
    c.ok(p.refusalsSeen() == 1, state + ": its write is refused in words");
    c.ok(p.filesUnchanged(), state + ": every owner file is byte-identical");
    c.ok(p.delivered("brief ready"), state + ": the routine's reply is delivered");
  }
  // Ruling 80 item 5: the owner's command on the hosted twin, as on the device.
  Probe cmd("q2a-grants-cmd");
  cmd.rig->say(kOwner, "/loop grants on");
  c.ok(cmd.rig->routines().routineGrants(), "/loop grants on turns the switch on");
  c.ok(cmd.tx.seen.empty(), "the command costs no model call");
  cmd.rig->say(kOwner, "/loop grants off");
  c.ok(!cmd.rig->routines().routineGrants(), "/loop grants off turns it off");
  // The switch persists beside the wake-up mode (no new key): a restarted instance keeps it.
  Probe q("q2a-grants-persist");
  q.rig->routines().setRoutineGrants(true);
  HostedLoops again;
  HostedLoopHooks none;
  again.begin(q.rig->memDir(), none);
  c.ok(again.routineGrants(), "the switch survives a restart");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== unattended turns (lane RB-Q2a, offline) ===\n");
  testEveryClassCannotWriteOrSend(c);
  testOwnerTurnStillWrites(c);
  testSundayReminderTenOfTen(c);
  testSelfWakeupIsOneShortRequest(c);
  testReportSendsOnlyNamedArtifacts(c);
  testWakeupChainStopsAtThreeHops(c);
  testRoutineSwitchBothStates(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
