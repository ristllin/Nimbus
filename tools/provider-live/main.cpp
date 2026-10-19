// provider-live - real-API legs for the provider wire (see the Makefile header).
//
//   provider-live <scenario> <host> <model> [--record=<file.jsonl>]
//
// Scenarios (each prints one JSON line and exits non-zero when a check fails):
//   final-cap      a tool turn capped at one round: round 1 is the final round
//                  (tool_choice must stay auto - a forced one 400s on the 5.5 tier)
//   final-stall    "answer in one plain sentence, no tools": a stall, then the final
//                  round (the stalled answer echoed back) or a prose final
//   single-shot    the single shot (no tool loop): orch_turn or prose, both delivered
//   fold           the compaction fold: its own budget, no tool, a prose summary
//   cut-retry      the first attempt capped at 128 tokens (the bench seam): cut,
//                  one retry with headroom and a different body, no failover
//   parity         a three-step tool turn (write, search, answer) on one host
//   utf8           a tool result carrying a torn UTF-8 character: repaired, 200
//   error-surface  an unknown model id: a typed Rejected, no same-host retry, a
//                  switch to the second host given as --then=<host>:<model>
// Keys come by NAME from NIMBUS_ENV_FILE; nothing is printed but usage numbers.
#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/transcript.h"

namespace {

std::map<std::string, std::string> g_env;
void loadEnv(const char* path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    line = line.substr(b);
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
    g_env[k] = v;
  }
}
std::string keyFor(const std::string& host) {
  if (host == "anthropic") return g_env["ANTHROPIC_API_KEY"];
  if (host == "openai") return g_env["OPENAI_API_KEY"];
  if (host == "mistral") return g_env["MISTRAL_API_KEY"];
  if (host == "zai") return g_env["Z_AI_TOKEN"];
  return "";
}

uint32_t nowMs() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// JSON string escape for the report line.
std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

struct Call {
  std::string host, path, body, resp, err;
  int status = 0;
  double seconds = 0;
};

// The host transport: libcurl, with the device's word for a timeout ("no
// response") so the typed Timeout path runs here as it does on the board.
class Curl : public agent::HttpTransport {
 public:
  std::vector<Call> calls;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    Call c;
    c.host = req.host;
    c.path = req.path;
    c.body = req.body;
    CURL* h = curl_easy_init();
    const std::string url = std::string(req.tls ? "https://" : "http://") + req.host + req.path;
    curl_slist* hdrs = nullptr;
    for (const auto& kv : req.headers) hdrs = curl_slist_append(hdrs, (kv.first + ": " + kv.second).c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)req.timeoutMs);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Curl::onWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out.body);
    if (req.method == "POST") {
      curl_easy_setopt(h, CURLOPT_POST, 1L);
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    }
    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(h);
    c.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
      err = rc == CURLE_OPERATION_TIMEDOUT ? "no response" : std::string("connect failed: ") + curl_easy_strerror(rc);
      c.err = err;
      calls.push_back(c);
      out.status = 0;
      return false;
    }
    out.status = (int)code;
    c.status = (int)code;
    c.resp = out.body;
    calls.push_back(c);
    return true;
  }

 private:
  static size_t onWrite(char* p, size_t sz, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
  }
};

std::vector<std::string> g_log;

struct Rig {
  Curl http;
  std::map<std::string, std::string> models;
  agent::providers::ProviderDeps pd;
  agent::HeadTools ht;
  std::vector<std::string> dispatched;
  bool poisonResult = false;

  Rig() {
    pd.http = &http;
    pd.key = [](const char* h) { return keyFor(h); };
    pd.orchModel = [this](const char* h) { return models[h]; };
    pd.toolLoopOn = [] { return true; };
    pd.nowMs = [] { return nowMs(); };
    pd.freeHeap = [] { return (uint32_t)200000; };
    ht.specs.push_back({"memory_write", "Store one fact in long-term memory.",
                        "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}"});
    ht.specs.push_back({"memory_search", "Search long-term memory for facts.",
                        "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}},\"required\":[\"q\"]}"});
    ht.dispatch = [this](const nimbus::orch::HeadToolCall& c) {
      dispatched.push_back(c.name);
      nimbus::orch::HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      if (c.name == "memory_write") r.output = "stored memory m0001";
      else if (poisonResult) r.output = "notes/diet.md (score 3)\n  ...\x82\xAC" "12-month outcomes: the owner's bike is blue.";
      else r.output = "1 match: [m0001] the owner's bike is blue (stored today)";
      return r;
    };
    ht.cfg.maxRounds = 6;
    ht.cfg.deadlineMs = 300000;
    ht.cfg.roundMinHeap = 0;
    ht.cfg.maxToolResultBytes = 4096;
    ht.cfg.maxTotalToolBytes = 24576;
  }
};

const char* kSystem =
    "You are Nimbus, a small desk assistant under test. Use the registry tools when the "
    "user asks you to remember or look something up. Always finish a turn by calling the "
    "orch_turn tool with your reply in its reply field.";

struct Report {
  std::string scenario, host, model, err, kind, out;
  bool ok = false;
  std::vector<std::pair<std::string, bool>> checks;
  void check(const char* name, bool pass) { checks.push_back({name, pass}); }
  bool allPass() const {
    for (const auto& c : checks)
      if (!c.second) return false;
    return true;
  }
};

// One request of the run as a JSON object: status, time, size, stop reason, the
// provider's error text and its usage (what the spend report reads).
std::string requestJson(const Call& c) {
  JsonDocument keep;   // only what the report reads (a reasoning response is deep and large)
  keep["usage"] = true; keep["stop_reason"] = true; keep["status"] = true; keep["error"] = true;
  keep["message"] = true; keep["detail"] = true; keep["choices"][0]["finish_reason"] = true;
  JsonDocument d;
  std::string usage = "null";
  if (!deserializeJson(d, c.resp, DeserializationOption::Filter(keep), DeserializationOption::NestingLimit(64)) &&
      !d["usage"].isNull()) {
    usage.clear();
    serializeJson(d["usage"], usage);
  }
  std::string stop = d["stop_reason"] | "";
  if (stop.empty()) stop = d["choices"][0]["finish_reason"] | "";
  if (stop.empty()) stop = d["status"] | "";
  std::string msg = d["error"]["message"] | "";
  if (msg.empty() && d["message"].is<const char*>()) msg = d["message"].as<const char*>();
  if (msg.empty() && d["detail"].is<const char*>()) msg = d["detail"].as<const char*>();
  return "{\"host\":" + jstr(c.host) + ",\"status\":" + std::to_string(c.status) +
         ",\"seconds\":" + std::to_string(c.seconds) + ",\"bodyBytes\":" + std::to_string(c.body.size()) +
         ",\"stop\":" + jstr(stop) + ",\"error\":" + jstr(msg.substr(0, 200)) + ",\"transport\":" + jstr(c.err) +
         ",\"usage\":" + usage + "}";
}

// The harness log lines that say what the wire did (one JSON array).
std::string logJson() {
  static const char* const kKeep[] = {"orchTurn", "fabric", "utf8", "wire", "headloop", "provider"};
  std::string o = "[";
  size_t n = 0;
  for (const auto& l : g_log) {
    bool keep = false;
    for (const char* k : kKeep) keep = keep || l.find(k) != std::string::npos;
    if (keep) o += std::string(n++ ? "," : "") + jstr(l.substr(0, 220));
  }
  return o + "]";
}

void print(const Report& r, const Rig& rig, const std::string& recordPath) {
  std::string o = "{\"scenario\":" + jstr(r.scenario) + ",\"host\":" + jstr(r.host) +
                  ",\"model\":" + jstr(r.model) + ",\"ok\":" + (r.ok ? "true" : "false") +
                  ",\"err\":" + jstr(r.err) + ",\"kind\":" + jstr(r.kind) + ",\"out\":" + jstr(r.out.substr(0, 200)) +
                  ",\"requests\":[";
  for (size_t i = 0; i < rig.http.calls.size(); i++) o += std::string(i ? "," : "") + requestJson(rig.http.calls[i]);
  o += "],\"checks\":{";
  for (size_t i = 0; i < r.checks.size(); i++)
    o += std::string(i ? "," : "") + jstr(r.checks[i].first) + ":" + (r.checks[i].second ? "true" : "false");
  o += "},\"log\":" + logJson() + ",\"pass\":" + std::string(r.allPass() ? "true" : "false") + "}";
  std::printf("%s\n", o.c_str());
  if (!recordPath.empty()) {
    std::ofstream f(recordPath, std::ios::app);
    f << o << "\n";
  }
}

bool allStatus200(const Rig& rig) {
  for (const auto& c : rig.http.calls)
    if (c.status != 200) return false;
  return !rig.http.calls.empty();
}
bool anyLog(const char* needle) {
  for (const auto& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}
bool bodyHas(const Rig& rig, size_t i, const char* needle) {
  return i < rig.http.calls.size() && rig.http.calls[i].body.find(needle) != std::string::npos;
}

bool fabric(Rig& rig, const std::vector<std::string>& hosts, const std::string& user, Report& r, int* switches) {
  std::string out, err;
  nimbus::orch::TurnSpend spend;   // per-host spend (the fabric files each host once)
  int n = 0;
  const bool ok = agent::providers::runFabricLoop(rig.pd, hosts, kSystem, user, out, err, rig.ht, &spend,
                                                  [&](const std::string&, const std::string&) { n++; });
  r.ok = ok;
  r.out = out;
  r.err = err;
  if (switches) *switches = n;
  return ok;
}

// ---- scenarios ------------------------------------------------------------------

struct Ctx {
  Rig& rig;
  Report& r;
  std::string host, model, thenHost;
};

void scFinalCap(Ctx& c) {
  int sw = 0;
  c.rig.ht.cfg.maxRounds = 1;   // round 1 is the forced final round
  fabric(c.rig, {c.host}, "First search my memory for my bike's color, then remember that I also own a red scooter. "
                          "Then tell me both facts.", c.r, &sw);
  c.r.check("every request 200", allStatus200(c.rig));
  c.r.check("turn ok", c.r.ok);
  c.r.check("no switch", sw == 0);
  c.r.check("reached the final round", anyLog("cap=rounds"));
  c.r.check("no forced tool_choice on the wire", !bodyHas(c.rig, c.rig.http.calls.size() - 1, "\"type\":\"tool\""));
}

void scFinalStall(Ctx& c) {
  int sw = 0;
  fabric(c.rig, {c.host}, "Reply in one plain sentence and do not use any tools: what is two plus two?", c.r, &sw);
  c.r.check("every request 200", allStatus200(c.rig));
  c.r.check("turn ok", c.r.ok);
  c.r.check("reply delivered", !c.r.out.empty());
  c.r.check("no switch", sw == 0);
}

void pointAtZai(Rig& rig, const std::string& model) {
  rig.pd.customBase = [] { return std::string("https://api.z.ai"); };
  rig.pd.customPathPrefix = [] { return std::string("/api/paas/v4"); };
  rig.pd.customKey = [] { return keyFor("zai"); };
  rig.pd.customConv = [] { return std::string("openai"); };
  rig.pd.customModel = [model] { return model; };
  agent::providers::applyZaiHeadPolicy(rig.pd);
}

bool singleShot(Ctx& c, bool fold, std::string& out, std::string& err) {
  std::string conv;
  nimbus::orch::TokenUsage u;
  const std::string inputs = fold ? "[CONVERSATION SINCE]\n[owner] my bike is blue\n[assistant] Noted, blue bike.\n"
                                    "[owner] remind me to oil the chain on Saturday\n[assistant] Saturday reminder set."
                                  : "What is two plus two? Answer briefly.";
  const char* sys = fold ? nimbus::orch::ORCH_COMPACT_PROMPT : kSystem;
  auto& pd = c.rig.pd;
  if (c.host == "anthropic") return agent::providers::orchTurnAnthropic(pd, conv, sys, inputs, out, err, nullptr, &u);
  if (c.host == "openai") return agent::providers::orchTurnOpenAI(pd, conv, sys, inputs, out, err, nullptr, &u);
  if (c.host == "mistral") return agent::providers::orchTurnMistral(pd, conv, sys, inputs, out, err, nullptr, &u);
  pointAtZai(c.rig, c.model);
  return agent::providers::orchTurnCustom(pd, conv, sys, inputs, out, err, nullptr, &u);
}

void scSingle(Ctx& c, bool fold) {
  std::string out, err;
  const bool ok = singleShot(c, fold, out, err);
  c.r.ok = ok;
  c.r.out = out;
  c.r.err = err;
  c.r.check("every request 200", allStatus200(c.rig));
  c.r.check("answer delivered", ok && !out.empty());
  if (c.host == "anthropic") c.r.check("tool_choice never forced", !bodyHas(c.rig, 0, "\"type\":\"tool\""));
  if (fold && c.host == "anthropic") c.r.check("fold sends no tool", !bodyHas(c.rig, 0, "\"tools\""));
}
void scSingleShot(Ctx& c) { scSingle(c, false); }
void scFold(Ctx& c) { scSingle(c, true); }

void scCutRetry(Ctx& c) {
  int sw = 0;
  agent::providers::setTestFirstAttemptCap(128);
  fabric(c.rig, {c.host}, "Write a 300-word paragraph about bicycles, then call orch_turn with it as the reply.", c.r, &sw);
  const auto& calls = c.rig.http.calls;
  c.r.check("first attempt was cut", anyLog("one retry with"));
  c.r.check("retry body differs", calls.size() >= 2 && calls[0].body != calls[1].body);
  c.r.check("no switch", sw == 0);
  c.r.check("turn ok", c.r.ok);
}

void scParity(Ctx& c) {
  int sw = 0;
  fabric(c.rig, {c.host}, "Remember that my bike is blue. Then search my memory for my bike to confirm it is stored, "
                          "and tell me what you found.", c.r, &sw);
  c.r.check("every request 200", allStatus200(c.rig));
  c.r.check("turn ok", c.r.ok);
  c.r.check("both tools ran", c.rig.dispatched.size() >= 2);
  c.r.check("no switch", sw == 0);
}

void scUtf8(Ctx& c) {
  int sw = 0;
  c.rig.poisonResult = true;
  fabric(c.rig, {c.host}, "Call the memory_search tool now with q=\"bike\", then tell me my bike's color.", c.r, &sw);
  c.r.check("every request 200", allStatus200(c.rig));
  c.r.check("turn ok", c.r.ok);
  c.r.check("repair logged", anyLog("utf8: repaired"));
}

void scErrorSurface(Ctx& c) {
  int sw = 0;
  std::vector<std::string> hosts{c.host};
  if (!c.thenHost.empty()) hosts.push_back(c.thenHost);
  fabric(c.rig, hosts, "What is two plus two?", c.r, &sw);
  const auto& last = agent::providers::lastFabricSwitch();
  c.r.kind = nimbus::orch::stepErrSlug(last.kind);
  size_t firstHostCalls = 0;
  for (const auto& call : c.rig.http.calls)
    if (call.host == c.rig.http.calls[0].host) firstHostCalls++;
  c.r.check("one request to the refusing host (no same-host retry)", firstHostCalls == 1);
  c.r.check("typed rejected", last.kind == nimbus::orch::StepErr::Rejected);
  c.r.check("switched", sw == 1);
  c.r.check("turn ok on the second host", c.r.ok);
}

struct Scenario {
  const char* name;
  void (*run)(Ctx&);
};
const Scenario kScenarios[] = {
    {"final-cap", scFinalCap},   {"final-stall", scFinalStall}, {"single-shot", scSingleShot},
    {"fold", scFold},            {"cut-retry", scCutRetry},     {"parity", scParity},
    {"utf8", scUtf8},            {"error-surface", scErrorSurface},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: provider-live <scenario> <host> <model> [--record=f] [--then=host:model]\n");
    return 2;
  }
  const std::string scenario = argv[1], host = argv[2], model = argv[3];
  std::string record, thenHost, thenModel;
  for (int i = 4; i < argc; i++) {
    const std::string a = argv[i];
    if (a.rfind("--record=", 0) == 0) record = a.substr(9);
    if (a.rfind("--then=", 0) == 0) {
      const size_t c = a.find(':', 7);
      thenHost = a.substr(7, c - 7);
      thenModel = c == std::string::npos ? "" : a.substr(c + 1);
    }
  }
  const Scenario* sc = nullptr;
  for (const Scenario& s : kScenarios)
    if (scenario == s.name) sc = &s;
  if (!sc) {
    std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
    return 2;
  }
  const char* envPath = std::getenv("NIMBUS_ENV_FILE");
  loadEnv(envPath ? envPath : "/Users/roydarnell/Projects/nimbus-project/nimbus/.env");
  curl_global_init(CURL_GLOBAL_DEFAULT);
  agent::hlog::setSink(+[](const char* l) { g_log.push_back(l); });
  Rig rig;
  rig.models[host] = model;
  if (!thenHost.empty()) rig.models[thenHost] = thenModel;
  Report r;
  r.scenario = scenario;
  r.host = host;
  r.model = model;
  Ctx ctx{rig, r, host, model, thenHost};
  sc->run(ctx);
  print(r, rig, record);
  curl_global_cleanup();
  return r.allPass() ? 0 : 1;
}
