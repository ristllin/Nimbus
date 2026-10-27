// hon_driver - replay one honesty scenario through the real Nimbus harness.
//
//   hon_driver --scenario <file.json> --out <transcript.json> --host <h> --model <id>
//              [--env <dotenv>] [--effort <e>] [--record <dir>] [--system-out <file>]
//              [--rounds <n>] [--max-tokens <n>]
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
#include "nimbus/orch/journal.h"

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
  std::string scenario, out, systemOut, env = lab::Env::defaultDotenvPath();
  std::string host = "anthropic", model, effort = "low", record;
  long maxTokens = 0, rounds = 0;
};

bool parseArgs(int argc, char** argv, Args& a) {
  const std::map<std::string, std::string*> text = {
      {"--scenario", &a.scenario}, {"--out", &a.out},     {"--system-out", &a.systemOut},
      {"--env", &a.env},           {"--host", &a.host},   {"--model", &a.model},
      {"--effort", &a.effort},     {"--record", &a.record}};
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
  subs.adapterFor(s.provider).queued.push_back(run);
  if (!rig.jobs().enqueueSpawn(s, chat, /*quiet=*/true)) return false;
  for (int i = 0; i < 300 && !rig.jobs().hasFreshResults(); i++) {   // dispatch, then poll (~2 s)
    rig.jobs().pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return rig.jobs().hasFreshResults();
}

bool runSteps(lab::LabRig& rig, Subs& subs, JsonObjectConst sc, JsonDocument& out) {
  const std::string chat = str(sc["chat"]).empty() ? "owner" : str(sc["chat"]);
  JsonArray turns = out["turns"].to<JsonArray>();
  bool ok = true;
  for (JsonObjectConst st : sc["steps"].as<JsonArrayConst>()) {
    const std::string kind = str(st["do"]);
    const size_t logFrom = lab::LabRig::harnessLog().size();
    if (kind == "row") {   // a message the owner saw between steps (no live turn)
      seedRow(rig, chat, str(st["role"]), str(st["text"]));
      continue;
    }
    if (kind == "say") {
      const lab::TurnRecord t = rig.say(chat, str(st["text"]));
      addTurn(turns, "owner", t, logFrom);
      ok = ok && t.ok;
      continue;
    }
    if (kind != "sub") return false;
    if (!runSub(rig, subs, chat, st)) {
      std::fprintf(stderr, "hon_driver: sub %s never finished\n", str(st["name"]).c_str());
      return false;
    }
    const std::string then = str(st["then"]);
    if (then == "synthesize") {
      const lab::TurnRecord t = rig.synthesize(chat);
      addTurn(turns, "synthesis", t, logFrom);
      ok = ok && t.ok;
    } else if (then == "consume") {
      (void)rig.jobs().takeFreshResults();   // the synthesis already happened on the device
      seedRow(rig, chat, "assistant", str(st["sentThen"]));
    }
  }
  return ok;
}

bool writeSystemDump(const lab::LabRig& rig, const std::string& path) {
  JsonDocument d;
  const auto& turns = rig.turns();
  if (!turns.empty()) d["system_prompt"] = turns.back().systemPrompt;
  std::string s;
  serializeJsonPretty(d, s);
  return writeFile(path, s);
}

bool buildOptions(const Args& a, JsonObjectConst sc, lab::LabRig::Options& opt, std::string& err) {
  opt.priority = a.host;
  opt.models[a.host] = a.model;
  opt.devName = str(sc["devName"]).empty() ? "Nimbus" : str(sc["devName"]);
  opt.midTurnFailover = false;   // a failure is attributable to this one host
  opt.embeddings = false;        // the history and the ledger answer, not recall
  opt.recordDir = a.record;
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
  g_profile.clock = str(sc["clock"]);
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
  out["scenario"] = str(sc["id"]);
  out["host"] = a.host;
  out["model"] = a.model;
  out["banner"] = rig.banner();
  std::string text;
  serializeJsonPretty(out, text);
  if (!writeFile(a.out, text)) return 2;
  if (!a.systemOut.empty() && !writeSystemDump(rig, a.systemOut))
    std::fprintf(stderr, "hon_driver: cannot write %s\n", a.systemOut.c_str());
  return ok ? 0 : 3;
}
