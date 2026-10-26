// fc_driver - replay one food-coach scenario through the real Nimbus harness.
//
//   fc_driver --scenario <run.json> --out <transcript.json> --host <h> --model <id>
//             [--env <dotenv>] [--effort <e>] [--max-tokens <n>] [--rounds <n>]
//             [--record <dir>] [--system-out <file>] [--no-embed] [--dev-name <n>]
//
// It builds the host lab's LabRig (the device's TurnEngine, tool registry,
// memory engines and provider adapters over libcurl), seeds the world the
// scenario describes (food log, skills, memory, earlier chat turns), sends the
// owner turns one by one, and writes the transcript as JSON. Grading is not done
// here: the Python runner hands the transcript to a judge.
//
// The lab is not edited. Two things it leaves out of the system prompt (the
// clock, and the owner's stored directive) are supplied by the composeInstructions
// shim below; see the Makefile for how the real function is kept under a new name.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lab_options.h"
#include "lab_rig.h"
#include "nimbus/harness/compose.h"

namespace {

// What the lab's prompt lacks compared with a real device (see the shim).
struct Profile {
  std::string directive;   // the owner's stored directive; "" = the shipped default
  bool clock = true;       // the device always knows the date
  std::string fw = "5.0.0-eval (host lab)";
};
Profile g_profile;

std::string clockNow() {
  const std::time_t t = std::time(nullptr);
  std::tm loc{};
  localtime_r(&t, &loc);
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %a %H:%M %Z", &loc);
  return buf;
}

// The live hardware manifest flags the device fills (src/agent/orchestrator.cpp).
void deviceManifest(nimbus::orch::Hardware& hw) {
  hw.ring = hw.touch = hw.mic = hw.speaker = true;
  hw.battery = hw.sd = hw.wifi = hw.files = hw.telegram = true;
  hw.ble = false;
}

}  // namespace

namespace agent {

// compose.cpp, compiled under this name by the Makefile (-DcomposeInstructions=...).
std::string composeInstructionsReal(const ComposeInputs& in);

// The lab's compose inputs carry no clock, no firmware line, no hardware manifest
// and no stored directive; a device always has the first three, and an owner who
// edited the directive has the last. Everything else is the shipping function.
std::string composeInstructions(const ComposeInputs& in) {
  ComposeInputs c = in;
  if (g_profile.clock) c.now = clockNow();
  c.fw = g_profile.fw;
  if (!g_profile.directive.empty()) c.directive = g_profile.directive;
  deviceManifest(c.hw);
  return composeInstructionsReal(c);
}

}  // namespace agent

namespace {

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

struct Args {
  std::string scenario, out, systemOut, env = lab::Env::defaultDotenvPath();
  std::string host = "anthropic", model, effort = "low", record, devName = "Nimbus-Eval";
  long maxTokens = 0, rounds = 0;
  bool noEmbed = false;
};

bool parseArgs(int argc, char** argv, Args& a) {
  const std::map<std::string, std::string*> text = {
      {"--scenario", &a.scenario}, {"--out", &a.out},         {"--system-out", &a.systemOut},
      {"--env", &a.env},           {"--host", &a.host},       {"--model", &a.model},
      {"--effort", &a.effort},     {"--record", &a.record},   {"--dev-name", &a.devName}};
  const std::map<std::string, long*> number = {{"--max-tokens", &a.maxTokens}, {"--rounds", &a.rounds}};
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    if (k == "--no-embed") { a.noEmbed = true; continue; }
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

std::string str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : ""; }

uint32_t nowHours() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::hours>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

void seedHistory(lab::LabRig& rig, JsonArrayConst rows) {
  for (JsonObjectConst r : rows) {
    lab::LabRig::Row row;
    row.chat = "owner";
    const std::string role = str(r["role"]);
    row.role = role == "assistant" ? "assistant" : "user";
    row.text = str(r["text"]);
    const uint32_t ago = r["hours_ago"] | 0u;
    row.tsHours = nowHours() - ago;
    rig.addRow(row);
  }
}

void seedWorld(lab::LabRig& rig, JsonObjectConst seed) {
  for (JsonObjectConst f : seed["files"].as<JsonArrayConst>())
    rig.files().seed(str(f["path"]), str(f["text"]));
  for (JsonObjectConst s : seed["skills"].as<JsonArrayConst>()) {
    const auto res = rig.skills().ownerSave(str(s["id"]), str(s["md"]));
    if (!res.ok) std::fprintf(stderr, "fc_driver: skill %s refused: %s\n", str(s["id"]).c_str(), res.err.c_str());
  }
  for (JsonVariantConst m : seed["memory"].as<JsonArrayConst>()) {
    JsonDocument d;
    d["content"] = str(m);
    d["importance"] = 0.9;
    d["ttl"] = "permanent";
    std::string args;
    serializeJson(d, args);
    const std::string out = rig.callTool("memory.write", args);
    if (out.find("error") != std::string::npos)
      std::fprintf(stderr, "fc_driver: memory.write: %s\n", out.substr(0, 200).c_str());
  }
  seedHistory(rig, seed["history"].as<JsonArrayConst>());
}

void addTurn(JsonArray turns, const lab::TurnRecord& t) {
  JsonObject o = turns.add<JsonObject>();
  o["owner"] = t.userText;
  o["reply"] = t.reply;
  o["ok"] = t.ok;
  o["seconds"] = t.seconds;
  o["tokens_in"] = t.tokensIn;
  o["tokens_out"] = t.tokensOut;
  o["cache_read"] = t.cacheRead;
  o["cache_write"] = t.cacheWrite;
  JsonArray sent = o["deliveries"].to<JsonArray>();   // every message the owner would receive
  for (const auto& d : t.deliveries) sent.add(d);
  JsonArray calls = o["tool_calls"].to<JsonArray>();
  for (const auto& c : t.toolCalls) calls.add(c);
  JsonArray results = o["tool_results"].to<JsonArray>();
  for (const auto& r : t.toolResults) results.add(r);
  if (!t.ok) {
    JsonArray log = o["harness_log"].to<JsonArray>();
    const auto& lines = lab::LabRig::harnessLog();
    for (size_t i = lines.size() > 8 ? lines.size() - 8 : 0; i < lines.size(); i++) log.add(lines[i]);
  }
}

bool runScenario(lab::LabRig& rig, JsonObjectConst sc, const Args& a, JsonDocument& out) {
  bool allOk = true;
  JsonArray turns = out["turns"].to<JsonArray>();
  for (JsonVariantConst t : sc["turns"].as<JsonArrayConst>()) {
    const lab::TurnRecord rec = rig.say("owner", str(t));
    addTurn(turns, rec);
    allOk = allOk && rec.ok;
  }
  JsonObject files = out["final_files"].to<JsonObject>();
  for (JsonObjectConst f : sc["seed"]["files"].as<JsonArrayConst>())
    files[str(f["path"])] = rig.files().get(str(f["path"]));
  out["host"] = a.host;
  out["model"] = a.model;
  out["banner"] = rig.banner();
  return allOk;
}

bool writeSystemDump(const lab::LabRig& rig, const std::string& path) {
  JsonDocument d;
  const auto& turns = rig.turns();
  if (!turns.empty()) d["system_prompt"] = turns.front().systemPrompt;
  JsonArray inputs = d["turn_inputs"].to<JsonArray>();
  for (const auto& t : turns) inputs.add(t.turnInputs);
  std::string s;
  serializeJsonPretty(d, s);
  return writeFile(path, s);
}

bool buildOptions(const Args& a, lab::LabRig::Options& opt, std::string& err) {
  opt.priority = a.host;
  opt.models[a.host] = a.model;
  opt.devName = a.devName;
  opt.midTurnFailover = false;   // a failure is attributable to this one host
  opt.embeddings = !a.noEmbed;
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
    std::fputs("usage: fc_driver --scenario f --out f --model id [--host h] [--env f] [--effort e]\n", stderr);
    return 2;
  }
  JsonDocument sc;
  if (deserializeJson(sc, readFile(a.scenario))) {
    std::fprintf(stderr, "fc_driver: cannot parse %s\n", a.scenario.c_str());
    return 2;
  }
  g_profile.directive = str(sc["profile"]["directive"]);
  g_profile.clock = sc["profile"]["clock"] | true;

  lab::Env env;
  env.loadDotenv(a.env);
  lab::LabRig::Options opt;
  std::string err;
  if (!buildOptions(a, opt, err)) {
    std::fprintf(stderr, "fc_driver: %s\n", err.c_str());
    return 2;
  }
  if (env.providerKey(a.host).empty()) {
    std::fprintf(stderr, "fc_driver: no key for host %s\n", a.host.c_str());
    return 2;
  }
  lab::LabRig rig(env, opt);
  seedWorld(rig, sc["seed"].as<JsonObjectConst>());
  JsonDocument out;
  const bool ok = runScenario(rig, sc.as<JsonObjectConst>(), a, out);
  std::string text;
  serializeJsonPretty(out, text);
  if (!writeFile(a.out, text)) return 2;
  if (!a.systemOut.empty()) writeSystemDump(rig, a.systemOut);
  return ok ? 0 : 3;
}
