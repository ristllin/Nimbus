// hon_driver - replay one honesty scenario through the real Nimbus harness.
//
//   hon_driver --scenario <file.json> --out <transcript.json> --host <h> --model <id>
//              [--env <dotenv>] [--effort <e>] [--record <dir>] [--system-out <file>]
//              [--rounds <n>] [--max-tokens <n>] [--wire-out <file>]
//
// --wire-out writes every request body the turn sent to the provider (the exact JSON
// on the wire: system, history, per-turn block, tool rounds), for a diagnosis that
// needs more than the turn's input block. Bodies only: headers (and so keys) are
// never kept by the transport.
//
// It builds the host lab's LabRig (the device's TurnEngine, job engine, tool registry,
// memory engines and provider adapters over libcurl), seeds the earlier chat, runs the
// scenario's steps and writes the transcript as JSON. Grading is not done here: the
// Python runner hands the transcript to a judge with a fresh context.
//
// Steps:
//   sub  a sub-agent the job engine dispatches to a scripted adapter, which answers
//        with the recorded envelope (reply, tool counts, tool names). From there on it
//        is the shipping code: the spawn ledger, [FRESH RESULTS], the synthesis turn.
//        `then`: "consume" takes the result without a live turn and seeds the reply
//        the device sent at the time as history; "synthesize" runs the live synthesis
//        turn; "keep" leaves it for the next owner turn.
//   say  an owner message, answered live.
//   row  a message of the earlier chat that comes between steps (seeded, no live turn).
//   file a file on the card before the steps run; {{TODAY}}/{{YESTERDAY}}/{{TOMORROW}}
//        in its text expand to the device's local day strings.
//   pref standing rules already on the device ("texts": [rule, ...]).
//   scheduled an unattended turn (a routine firing): the text is the routine's
//        prompt, the name the routine's display name, loopId its loop id. The
//        turn runs exactly as the device would fire it (the prefs rails read the
//        scheduled flag), and its reply is graded like any other.
// The transcript also carries the final state: files, prefs and memory rows.
// The graded turn is the last live one. Web search is left out of the lab when the
// scenario says so (the device head it replays had none), and the scenario's clock
// replaces the host clock.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lab_options.h"
#include "lab_rig.h"
#include "nimbus/harness/compose.h"
#include "nimbus/orch/file_append.h"   // logDayOf, for the {{TODAY}} tokens
#include "nimbus/orch/journal.h"
#include "nimbus/orch/owner_prefs.h"

namespace {

struct Profile {
  std::string clock;   // the scenario's local time, "YYYY-MM-DD Day HH:MM TZ"
  std::string fw = "5.0.0-eval (host lab)";
};
Profile g_profile;

// The live hardware manifest flags a Freenove board fills (src/agent/orchestrator.cpp).
void deviceManifest(nimbus::orch::Hardware& hw) {
  hw.ring = hw.touch = hw.mic = hw.speaker = true;
  hw.battery = hw.sd = hw.wifi = hw.files = hw.telegram = true;
  hw.ble = false;
}

}  // namespace

namespace agent {

// compose.cpp, compiled under this name by the Makefile (-DcomposeInstructions=...).
std::string composeInstructionsReal(const ComposeInputs& in);

// The lab's compose inputs carry no clock, no firmware line and no hardware manifest;
// a device always has them. Everything else is the shipping function.
std::string composeInstructions(const ComposeInputs& in) {
  ComposeInputs c = in;
  c.now = g_profile.clock;
  c.fw = g_profile.fw;
  deviceManifest(c.hw);
  return composeInstructionsReal(c);
}

}  // namespace agent

namespace {

namespace orch = nimbus::orch;

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool writeFile(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

std::string str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : ""; }

// The journal's store over RAM (the device keeps the same slots in one flash file).
struct RamStore : orch::JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
};

// A provider's sub-agent run, replaced by the envelope the scenario recorded.
struct ScriptedSub : agent::ManagedAgentAdapter {
  struct Run {
    std::string reply, toolNames;
    uint16_t toolRuns = 0, webRuns = 0;
  };
  std::string backend;
  std::vector<Run> queued;              // the next dispatch takes the front one
  std::map<std::string, Run> running;   // by job id

  const char* backendId() const override { return backend.c_str(); }
  agent::Capabilities capabilities() const override { return {}; }
  agent::FabricErr dispatch(const agent::Directive&, char outJobId[72]) override {
    if (queued.empty()) return agent::FabricErr::RemoteFail;
    std::snprintf(outJobId, 72, "%s:run-%u", backend.c_str(), (unsigned)running.size());
    running[outJobId] = queued.front();
    queued.erase(queued.begin());
    return agent::FabricErr::Ok;
  }
  agent::FabricErr poll(const char* jobId, agent::ResultEnvelope& env) override {
    auto it = running.find(jobId);
    if (it == running.end()) return agent::FabricErr::NotFound;
    env.state = orch::JobState::Done;
    std::snprintf(env.backend, sizeof(env.backend), "%s", backend.c_str());
    std::snprintf(env.reply, sizeof(env.reply), "%s", it->second.reply.c_str());
    std::snprintf(env.toolNames, sizeof(env.toolNames), "%s", it->second.toolNames.c_str());
    env.toolRuns = it->second.toolRuns;
    env.webRuns = it->second.webRuns;
    return agent::FabricErr::Ok;
  }
  agent::FabricErr cancel(const char*) override { return agent::FabricErr::Ok; }
};

struct Subs {
  agent::HeavyFabric fabric;
  RamStore store;
  orch::Journal journal;
  std::vector<std::unique_ptr<ScriptedSub>> adapters;
  ScriptedSub& adapterFor(const std::string& backend) {
    for (auto& a : adapters)
      if (a->backend == backend) return *a;
    adapters.emplace_back(new ScriptedSub);
    adapters.back()->backend = backend;
    fabric.registerAdapter(adapters.back().get());
    return *adapters.back();
  }
};

struct Args {
  std::string scenario, out, systemOut, wireOut, env = lab::Env::defaultDotenvPath();
  std::string host = "anthropic", model, effort = "low", record;
  long maxTokens = 0, rounds = 0;
};

bool parseArgs(int argc, char** argv, Args& a) {
  const std::map<std::string, std::string*> text = {
      {"--scenario", &a.scenario}, {"--out", &a.out},     {"--system-out", &a.systemOut},
      {"--env", &a.env},           {"--host", &a.host},   {"--model", &a.model},
      {"--effort", &a.effort},     {"--record", &a.record},   {"--wire-out", &a.wireOut}};
  const std::map<std::string, long*> number = {{"--max-tokens", &a.maxTokens}, {"--rounds", &a.rounds}};
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    if (i + 1 >= argc) return false;
    const std::string v = argv[++i];
    const auto t = text.find(k);
    const auto n = number.find(k);
    if (t != text.end()) *t->second = v;
    else if (n != number.end()) *n->second = std::strtol(v.c_str(), nullptr, 10);
    else return false;
  }
  return !a.scenario.empty() && !a.out.empty() && !a.model.empty();
}

uint32_t nowHours() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

// Scenario files stay static across run days: {{TODAY}} / {{YESTERDAY}} / {{TOMORROW}}
// in a seeded file's text become the device's local day strings (the ledger keys on
// them), and {{DOW}} becomes the weekday name, so a scenario's clock can name the
// real today without a per-run edit.
std::string substituteDays(const std::string& text) {
  std::string out = text;
  const std::pair<const char*, int> tokens[] = {
      {"{{TODAY}}", 0}, {"{{YESTERDAY}}", -1}, {"{{TOMORROW}}", 1}};
  for (const auto& tk : tokens) {
    const std::string day = orch::logDayOf(std::time(nullptr) + tk.second * 86400, true).date;
    size_t pos = 0;
    while ((pos = out.find(tk.first, pos)) != std::string::npos) {
      out.replace(pos, std::strlen(tk.first), day);
      pos += day.size();
    }
  }
  char dow[8] = {0};
  const std::time_t now = std::time(nullptr);
  std::strftime(dow, sizeof(dow), "%a", std::localtime(&now));
  size_t pos = 0;
  while ((pos = out.find("{{DOW}}", pos)) != std::string::npos) {
    out.replace(pos, 7, dow);
    pos += std::strlen(dow);
  }
  return out;
}

void seedRow(lab::LabRig& rig, const std::string& chat, const std::string& role, const std::string& text) {
  lab::LabRig::Row row;
  row.chat = chat;
  row.role = role == "assistant" ? "assistant" : "user";
  row.text = text;
  row.tsHours = nowHours();
  rig.addRow(row);
}

// The harness log lines a turn wrote (the claim check's "claims: ..." among them).
void addLog(JsonObject o, size_t from) {
  JsonArray log = o["claims_log"].to<JsonArray>();
  const auto& lines = lab::LabRig::harnessLog();
  for (size_t i = from; i < lines.size(); i++)
    if (lines[i].find("claims:") != std::string::npos) log.add(lines[i]);
}

void addTurn(JsonArray turns, const char* kind, const lab::TurnRecord& t, size_t logFrom) {
  JsonObject o = turns.add<JsonObject>();
  o["kind"] = kind;
  o["owner"] = t.userText;
  o["reply"] = t.reply;
  o["ok"] = t.ok;
  o["seconds"] = t.seconds;
  o["tokens_in"] = t.tokensIn;
  o["tokens_out"] = t.tokensOut;
  o["cache_read"] = t.cacheRead;
  o["cache_write"] = t.cacheWrite;
  o["inputs"] = t.turnInputs;   // the per-turn block: [SPAWN LEDGER], [FRESH RESULTS], [USER]
  JsonArray sent = o["deliveries"].to<JsonArray>();
  for (const auto& d : t.deliveries) sent.add(d);
  JsonArray calls = o["tool_calls"].to<JsonArray>();
  for (const auto& c : t.toolCalls) calls.add(c);
  JsonArray results = o["tool_results"].to<JsonArray>();
  for (const auto& r : t.toolResults) results.add(r);
  addLog(o, logFrom);
}

// One sub-agent through the job engine and the scripted adapter, to a parked result.
bool runSub(lab::LabRig& rig, Subs& subs, const std::string& chat, JsonObjectConst st) {
  ScriptedSub::Run run;
  run.reply = str(st["reply"]);
  run.toolNames = str(st["toolNames"]);
  run.toolRuns = st["toolRuns"] | 0;
  run.webRuns = st["webRuns"] | 0;
  orch::Spawn s;
  s.provider = str(st["provider"]);
  s.model = str(st["model"]);
  s.task = str(st["task"]);
  s.name = str(st["name"]);
  s.category = "research";
  const bool scriptedFail = st["fail"] | false;
  if (!scriptedFail)   // fail=true: the provider refuses the dispatch (spawn failure)
    subs.adapterFor(s.provider).queued.push_back(run);
  if (!rig.jobs().enqueueSpawn(s, chat, /*quiet=*/true)) return false;
  // A scripted failure is the recorded reality, not a driver error: the refusal
  // lands in the spawn ledger on the first pumps, and the scenario carries on.
  const int polls = scriptedFail ? 5 : 300;
  for (int i = 0; i < polls && !rig.jobs().hasFreshResults(); i++) {   // dispatch, then poll (~2 s)
    rig.jobs().pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return scriptedFail || rig.jobs().hasFreshResults();
}

// The seed steps (row, file, pref): state on the device before a live turn runs.
// Returns true when the step was handled, so runSteps can skip to the next one.
bool seedStep(lab::LabRig& rig, const std::string& chat, const std::string& kind, JsonObjectConst st) {
  if (kind == "row") {   // a message the owner saw between steps (no live turn)
    seedRow(rig, chat, str(st["role"]), str(st["text"]));
    return true;
  }
  if (kind == "file") {  // a file on the card before the steps run ({{TODAY}} & co. expand)
    rig.files().seed(str(st["path"]), substituteDays(str(st["text"])));
    return true;
  }
  if (kind == "pref") {  // standing rules already on the device
    for (JsonVariantConst v : st["texts"].as<JsonArrayConst>()) {
      std::string id;
      rig.prefs().store().add(str(v), orch::PrefOrigin::Seed, nowHours(), /*pending=*/false, &id);
    }
    return true;
  }
  return false;
}

// The sub step: dispatch the scripted sub-agent, then synthesize or consume per
// the scenario's "then". Returns false only on a hard driver error.
struct TurnCtx { JsonArray turns; size_t logFrom; };

bool runSubStep(lab::LabRig& rig, Subs& subs, const std::string& chat, JsonObjectConst st, const TurnCtx& ctx,
                bool& ok) {
  if (!runSub(rig, subs, chat, st)) {
    std::fprintf(stderr, "hon_driver: sub %s never finished\n", str(st["name"]).c_str());
    return false;
  }
  const std::string then = str(st["then"]);
  if (then == "synthesize") {
    const lab::TurnRecord t = rig.synthesize(chat);
    addTurn(ctx.turns, "synthesis", t, ctx.logFrom);
    ok = ok && t.ok;
  } else if (then == "consume") {
    // the synthesis already happened on the device (scoped take, as the turn does)
    (void)rig.jobs().takeFreshResultsFor(rig.jobs().freshScopeFor(chat), nullptr, nullptr, nullptr);
    seedRow(rig, chat, "assistant", str(st["sentThen"]));
  }
  return true;
}

bool runSteps(lab::LabRig& rig, Subs& subs, JsonObjectConst sc, JsonDocument& out) {
  const std::string chat = str(sc["chat"]).empty() ? "owner" : str(sc["chat"]);
  JsonArray turns = out["turns"].to<JsonArray>();
  bool ok = true;
  for (JsonObjectConst st : sc["steps"].as<JsonArrayConst>()) {
    const std::string kind = str(st["do"]);
    const size_t logFrom = lab::LabRig::harnessLog().size();
    if (seedStep(rig, chat, kind, st)) continue;
    if (kind == "scheduled") {  // an unattended turn: a routine firing as the device does
      rig.scheduled(chat, str(st["text"]), str(st["name"]), /*quietOk=*/false, str(st["loopId"]));
      const lab::TurnRecord& t = rig.lastTurn();
      addTurn(turns, "scheduled", t, logFrom);
      // A scheduled turn that said nothing (suppressed by a rail, or cancelled by
      // the owner's standing rule) is a legitimate outcome, not a driver error;
      // only a reply that failed mid-turn fails the run.
      ok = ok && (t.ok || t.reply.empty());
      continue;
    }
    if (kind == "say") {
      const lab::TurnRecord t = rig.say(chat, str(st["text"]));
      addTurn(turns, "owner", t, logFrom);
      ok = ok && t.ok;
      continue;
    }
    if (kind != "sub") return false;
    if (!runSubStep(rig, subs, chat, st, TurnCtx{turns, logFrom}, ok)) return false;
  }
  return ok;
}

// The final on-device state, so a runner can assert on what actually landed (the
// ledger's rows, the standing rules, the memory rows) instead of only on the reply.
void dumpState(lab::LabRig& rig, JsonObject out) {
  JsonObject files = out["files"].to<JsonObject>();
  for (const auto& kv : rig.files().all()) files[kv.first.c_str()] = kv.second;
  JsonArray prefs = out["prefs"].to<JsonArray>();
  for (const auto& e : rig.prefs().store().entries()) {
    JsonObject p = prefs.add<JsonObject>();
    p["id"] = e.idStr();
    p["text"] = e.text;
    p["pending"] = e.pending;
    p["moved"] = e.moved;
    p["evicted"] = e.evicted;
  }
  JsonArray mem = out["memory"].to<JsonArray>();
  for (const auto& m : rig.episodic().query(orch::MsgQuery())) {
    JsonObject r = mem.add<JsonObject>();
    r["id"] = m.id;
    r["role"] = m.role;
    r["kind"] = (int)m.kind;
    r["ts"] = m.tsHours;
    r["text"] = m.text;
  }
  // The long-term (vector) store: what a memory write LANDED as. `memory` above is the episodic
  // chat log, which always holds the owner's own message, so it cannot show a write.
  JsonArray durable = out["durable_memory"].to<JsonArray>();
  for (const auto& e : rig.vectors().getAll()) {
    JsonObject r = durable.add<JsonObject>();
    r["id"] = e.id;
    r["content"] = e.content;
    r["importance"] = e.importance;
    r["permanent"] = e.permanentFlag;
    r["source"] = e.source;
  }
  // The turn fields (mem_write[] / mem_query[]) the device dispatched, with their results.
  JsonArray fields = out["mem_field_calls"].to<JsonArray>();
  for (const auto& c : rig.fieldCalls()) fields.add(c);
}

bool writeSystemDump(const lab::LabRig& rig, const std::string& path) {
  JsonDocument d;
  const auto& turns = rig.turns();
  if (!turns.empty()) d["system_prompt"] = turns.back().systemPrompt;
  std::string s;
  serializeJsonPretty(d, s);
  return writeFile(path, s);
}

// The provider requests as sent, one object per exchange (host, path, status, body).
bool writeWireDump(lab::LabRig& rig, const std::string& path) {
  JsonDocument d;
  JsonArray calls = d.to<JsonArray>();
  for (const auto& c : rig.http().calls) {
    JsonObject o = calls.add<JsonObject>();
    o["host"] = c.host;
    o["path"] = c.path;
    o["status"] = c.status;
    o["request"] = c.reqBody;
  }
  std::string s;
  serializeJsonPretty(d, s);
  return writeFile(path, s);
}

bool buildOptions(const Args& a, JsonObjectConst sc, lab::LabRig::Options& opt, std::string& err) {
  opt.priority = a.host;
  opt.models[a.host] = a.model;
  opt.devName = str(sc["devName"]).empty() ? "Nimbus" : str(sc["devName"]);
  opt.midTurnFailover = sc["failover"] | false;   // default: a failure is attributable to this one host
  opt.embeddings = false;        // the history and the ledger answer, not recall
  opt.embedLocal = sc["localMemory"] | false;   // a scenario that asserts a memory write LANDED opts in
  opt.recordDir = a.record;
  for (JsonVariantConst f : sc["faults"].as<JsonArrayConst>())   // injected provider faults
    if (!lab::applyFault(str(f), opt, err)) return false;
  if (!lab::applyEffort(a.effort, opt, err)) return false;
  if (a.maxTokens > 0 && !lab::applyMaxTokens(std::to_string(a.maxTokens), opt, err)) return false;
  if (a.rounds > 0 && !lab::applyRounds(std::to_string(a.rounds), opt, err)) return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parseArgs(argc, argv, a)) {
    std::fputs("usage: hon_driver --scenario f --out f --model id [--host h] [--env f] [--effort e]\n", stderr);
    return 2;
  }
  JsonDocument sc;
  if (deserializeJson(sc, readFile(a.scenario))) {
    std::fprintf(stderr, "hon_driver: cannot parse %s\n", a.scenario.c_str());
    return 2;
  }
  g_profile.clock = substituteDays(str(sc["clock"]));   // {{TODAY}}/{{DOW}} name the real today
  lab::Env env;
  env.loadDotenv(a.env);
  if (!(sc["webSearch"] | false)) env.drop("TAVILY_API_KEY");
  lab::LabRig::Options opt;
  std::string err;
  if (!buildOptions(a, sc.as<JsonObjectConst>(), opt, err)) {
    std::fprintf(stderr, "hon_driver: %s\n", err.c_str());
    return 2;
  }
  if (env.providerKey(a.host).empty()) {
    std::fprintf(stderr, "hon_driver: no key for host %s\n", a.host.c_str());
    return 2;
  }
  lab::LabRig rig(env, opt);
  Subs subs;
  subs.journal.begin(&subs.store);
  rig.attachSubAgents(&subs.fabric, &subs.journal);
  const std::string chat = str(sc["chat"]).empty() ? "owner" : str(sc["chat"]);
  for (JsonObjectConst r : sc["history"].as<JsonArrayConst>()) seedRow(rig, chat, str(r["role"]), str(r["text"]));
  JsonDocument out;
  const bool ok = runSteps(rig, subs, sc.as<JsonObjectConst>(), out);
  dumpState(rig, out.as<JsonObject>());
  out["scenario"] = str(sc["id"]);
  out["host"] = a.host;
  out["model"] = a.model;
  out["banner"] = rig.banner();
  std::string text;
  serializeJsonPretty(out, text);
  if (!writeFile(a.out, text)) return 2;
  if (!a.systemOut.empty() && !writeSystemDump(rig, a.systemOut))
    std::fprintf(stderr, "hon_driver: cannot write %s\n", a.systemOut.c_str());
  if (!a.wireOut.empty() && !writeWireDump(rig, a.wireOut))
    std::fprintf(stderr, "hon_driver: cannot write %s\n", a.wireOut.c_str());
  return ok ? 0 : 3;
}
