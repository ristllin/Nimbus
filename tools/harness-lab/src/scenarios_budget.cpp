#include "scenarios_budget.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../../lib/harness/src/providers/image_parts.h"   // photoTokens - a photo's window estimate
#include "curl_transport.h"
#include "nimbus/harness/output_caps.h"   // mistralHeadEffort: a Mistral head forced to reason
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"             // ORCH_COMPACT_PROMPT, buildCompactInputs
#include "nimbus/orch/token_usage_json.h"
#include "nimbus/orch/turn_images.h"

// The provider leg set PL at each wire's own request budget (lane W of the history fix,
// v5.1.2). Wire-level legs: each builds its request through the device's own provider
// code (the same files the board runs) at the target its host and model have
// (wireBudgetFor), on the real API, and reads the answers back:
//   - round 0 at the target is accepted, and every later round reads the earlier ones from
//     the cache (OpenAI, Anthropic and Z.ai at least 90 %, Mistral at least 50 %), with
//     each round's body prefix shared with the round before it logged, so a miss reads as
//     the wire's (the prefix moved) or the provider's (it did not); no read timeout; the
//     custom leg answers through its one flat string;
//   - one plain-text fold on a digest of the host's fold step, on the model a fold for that
//     head runs (a Large 4 head folds on Mistral Small, RULINGS-v3), its summary
//     carrying the sections the fold prompt asks for;
//   - one photo round with a 3-photo album (LAB_BUDGET_PHOTOS: a directory of JPEGs) sent
//     as each host's image parts, its window estimate logged against the prompt tokens.
// The environment picks the legs:
//   LAB_BUDGET_LEGS     comma list of leg names (default: every leg with a key)
//   LAB_MISTRAL_PROXY   host:port of a local Mistral key proxy (Bearer proxy-fallback)
//   LAB_CUMULO_BASE     the router (default https://app.cumulo-nimbus.ai)
//   LAB_CUMULO_KEY_FILE a file holding the router key for the routed legs (read here only)
//   LAB_BUDGET_SCALE    0.05..1, the share of each target to build (default 1)
//   LAB_BUDGET_PARTS    which parts run: "round,fold,photo" (default all three)
// Nothing here prints a key or a request body.
namespace lab {
namespace budget {

size_t headBudget(LabRig& rig) {
  const std::string& prio = rig.options().priority;
  const std::string host = prio.substr(0, prio.find(','));
  return agent::providers::wireBudgetFor(host, rig.modelFor(host), false).wireBodyBytes;
}

BudgetPin::BudgetPin(uint32_t bytes) : before(agent::providers::testBudgetOverride()) {
  agent::providers::setTestBudgetOverride(bytes);
}
BudgetPin::~BudgetPin() { agent::providers::setTestBudgetOverride(before); }

namespace {

struct Leg {
  const char* name;
  const char* host;    // the budget's host (the upstream on a routed leg)
  const char* model;
  bool routed;
  bool custom;         // the custom wire's single shot (OpenAI's chat completions as the server)
};
const Leg kLegs[] = {
    {"mistral-large", "mistral", "mistral-large-4-0", false, false},
    {"mistral-medium", "mistral", "mistral-medium-latest", false, false},
    {"openai-luna", "openai", "gpt-5.6-luna", false, false},
    {"openai-56", "openai", "gpt-5.6", false, false},
    {"anthropic-sonnet", "anthropic", "claude-sonnet-5-5", false, false},
    {"zai-glm", "zai", "glm-5.3", false, false},
    {"cumulo-openai", "openai", "gpt-5.6", true, false},
    {"cumulo-anthropic", "anthropic", "claude-sonnet-5-5", true, false},
    {"cumulo-mistral", "mistral", "mistral-large-4-0", true, false},
    {"cumulo-zai", "zai", "glm-5.3", true, false},
    {"custom", "custom", "gpt-5.6-luna", false, true},
};

std::string envOr(const char* name, const char* dflt) {
  const char* v = std::getenv(name);
  return v && *v ? std::string(v) : std::string(dflt);
}
bool listed(const std::string& list, const std::string& item) {
  if (list.empty()) return true;
  return ("," + list + ",").find("," + item + ",") != std::string::npos;
}
uint32_t nowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::string readKeyFile(const std::string& path) {
  std::ifstream f(path);
  std::string k((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
  return k;
}

// The lab's transport for these legs: api.mistral.ai through a local key proxy when one is
// named (the proxy holds the key; the request carries its placeholder bearer).
class LegTransport : public agent::HttpTransport {
 public:
  CurlHttpTransport curl;
  std::string mistralProxy;   // "host:port" ("" = direct)
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    if (mistralProxy.empty() || req.host != "api.mistral.ai") return curl.exec(req, out, err);
    agent::HttpRequest p = req;
    const size_t colon = mistralProxy.find(':');
    p.host = mistralProxy.substr(0, colon);
    p.port = colon == std::string::npos ? 80 : (uint16_t)std::atoi(mistralProxy.c_str() + colon + 1);
    p.tls = false;
    for (auto& h : p.headers)
      if (h.first == "Authorization") h.second = "Bearer proxy-fallback";
    return curl.exec(p, out, err);
  }
};

// Every leg's deps over one transport: the device's provider code, pointed at this leg.
agent::providers::ProviderDeps legDeps(const Env& env, const Leg& leg, LegTransport& http, const std::string& routerKey) {
  agent::providers::ProviderDeps pd;
  pd.http = &http;
  const bool proxied = !http.mistralProxy.empty();
  pd.key = [env, proxied](const char* h) {
    if (proxied && std::string(h) == "mistral") return std::string("proxy-fallback");
    return env.providerKey(h);
  };
  const std::string model = leg.model;
  pd.orchModel = [model](const char*) { return model; };
  pd.toolLoopOn = [] { return true; };
  pd.nowMs = [] { return nowMs(); };
  pd.monoMs = [] { return nowMs(); };
  if (leg.routed) {
    const std::string base = envOr("LAB_CUMULO_BASE", "https://app.cumulo-nimbus.ai");
    pd.routerBase = [base] { return base; };
    pd.routerKey = [routerKey] { return routerKey; };
    pd.key = [routerKey](const char*) { return routerKey; };
  }
  if (std::string(leg.host) == "zai") {
    if (leg.routed) {
      agent::providers::configureRouterCompatHead(pd, "zai", model);
    } else {
      pd.customBase = [] { return std::string("api.z.ai"); };
      pd.customPathPrefix = [] { return std::string("/api/paas/v4"); };
      pd.customKey = [env] { return env.providerKey("zai"); };
      pd.customConv = [] { return std::string("openai"); };
      pd.customModel = [model] { return model; };
      agent::providers::applyZaiHeadPolicy(pd);
    }
  }
  if (leg.custom) {
    pd.customBase = [] { return std::string("https://api.openai.com"); };
    pd.customKey = [env] { return env.providerKey("openai"); };
    pd.customConv = [] { return std::string("openai"); };
    pd.customModel = [model] { return model; };
  }
  return pd;
}

bool legRunnable(const Env& env, const Leg& leg, const std::string& routerKey, const std::string& proxy,
                 std::string& why) {
  if (leg.routed && routerKey.empty()) {
    why = "LAB_CUMULO_KEY_FILE names no router key";
    return false;
  }
  if (leg.routed) return true;
  const std::string host = leg.custom ? "openai" : leg.host;
  if (host == "mistral" && !proxy.empty()) return true;
  if (env.providerKey(host).empty()) {
    why = "no " + host + " key";
    return false;
  }
  return true;
}

// Owner-like text: a food and errands log, so the bytes per token are a real
// conversation's, not a repeated letter's.
std::string loggedDay(int i) {
  static const char* const kFoods[] = {"oats with milk", "skyr and honey", "a chicken wrap", "lentil soup",
                                       "salmon and rice", "an apple", "almonds", "a flat white"};
  char b[400];
  std::snprintf(b, sizeof b,
                "2026-11-%02d %02d:%02d I had %s, about %d g, and later %s. Remind me to call the garage about the "
                "MOT on the %dth, and add %d more grams of protein to today's target. ",
                1 + i % 28, 7 + i % 14, (i * 7) % 60, kFoods[i % 8], 80 + (i * 13) % 220, kFoods[(i + 3) % 8],
                1 + (i * 3) % 28, 5 + i % 20);
  return b;
}

// A split prompt (static, history, per-turn) whose round-0 body on this wire is about
// `want` bytes: the history grows a turn pair at a time until the measured body (a dry
// build through a transport that sends nothing) reaches it.
struct Prompt {
  std::string instructions;
  std::string seed;
  size_t body = 0;
};
class DryRun : public agent::HttpTransport {
 public:
  size_t body = 0;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    body = agent::bodyLength(req);
    out.status = 0;
    err = "dry run";
    return false;
  }
};

agent::HeadTools labTools(int& looked) {
  agent::HeadTools ht;
  ht.specs.push_back({"lab_lookup", "Look up the store's record for one grocery keyword. Call it once.",
                      "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}},\"required\":[\"q\"]}"});
  ht.dispatch = [&looked](const nimbus::orch::HeadToolCall& c) {
    nimbus::orch::HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = "record: oats, 389 kcal per 100 g, 16.9 g protein, aisle 4";
    ++looked;
    return r;
  };
  ht.cfg.maxRounds = 4;
  ht.cfg.deadlineMs = 300000;
  ht.cfg.maxToolResultBytes = 8192;
  ht.cfg.maxTotalToolBytes = 65536;
  return ht;
}

// Where a turn's answer and error land.
struct TurnText {
  std::string& out;
  std::string& err;
};
bool runTurn(const Leg& leg, const agent::providers::ProviderDeps& pd, const std::string& ins, const std::string& seed,
             agent::HeadTools* ht, TurnText t) {
  std::string conv;
  std::string& out = t.out;
  std::string& err = t.err;
  const std::string host = leg.host;
  if (leg.custom || host == "zai") return agent::providers::orchTurnCustom(pd, conv, ins, seed, out, err, ht, nullptr);
  if (host == "anthropic") return agent::providers::orchTurnAnthropic(pd, conv, ins, seed, out, err, ht, nullptr);
  if (host == "openai") return agent::providers::orchTurnOpenAI(pd, conv, ins, seed, out, err, ht, nullptr);
  return agent::providers::orchTurnMistral(pd, conv, ins, seed, out, err, ht, nullptr);
}

Prompt promptAt(const Leg& leg, agent::providers::ProviderDeps pd, size_t want, const std::string& nonce) {
  DryRun dry;
  pd.http = &dry;
  Prompt p;
  p.seed = "Call lab_lookup once with q set to oats. After its answer, reply in one short line with the protein figure.";
  agent::layout::PromptParts parts;
  parts.split = true;
  parts.stat = "[RUN " + nonce + "] You are the owner's desk assistant. Answer briefly and use the tools you are given.\n";
  for (int i = 0; i < 60; ++i) parts.stat += loggedDay(1000 + i);
  parts.dyn = "[NOW] 2026-11-16 09:00\n";
  parts.cache.key = "lab-budget-" + nonce;
  int looked = 0;
  agent::HeadTools ht = labTools(looked);
  // Grow in steps, then back off one step if the last one overshot.
  for (int turn = 0; turn < 4000; ++turn) {
    parts.history.push_back({true, loggedDay(turn)});
    parts.history.push_back({false, "Logged. " + loggedDay(turn + 7)});
    p.instructions = agent::layout::join(parts);
    std::string out, err;
    if (leg.custom) {
      // The custom wire sends one flat string under flatPromptBudget(): stop before it.
      if (agent::layout::flat(p.instructions).size() > agent::providers::flatPromptBudget() - 2048) break;
    }
    runTurn(leg, pd, p.instructions, p.seed, leg.custom ? nullptr : &ht, {out, err});
    if (dry.body > want) {
      parts.history.resize(parts.history.size() - 2);
      p.instructions = agent::layout::join(parts);
      break;
    }
    p.body = dry.body;
  }
  return p;
}

// A body without its output cap field: the cap is fitted per round and is not part of the
// prompt the provider caches, so it must not end the shared prefix.
std::string withoutCap(std::string body) {
  for (const char* key : {"\"max_tokens\":", "\"max_output_tokens\":"}) {
    const size_t at = body.find(key);
    if (at == std::string::npos) continue;
    size_t end = at + std::strlen(key);
    while (end < body.size() && std::isdigit((unsigned char)body[end])) ++end;
    if (end < body.size() && body[end] == ',') ++end;
    body.erase(at, end - at);
  }
  return body;
}
// The bytes two request bodies share from their start, the output cap aside.
size_t sharedPrefix(const std::string& a0, const std::string& b0) {
  const std::string a = withoutCap(a0), b = withoutCap(b0);
  const size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  return i;
}

// The cached share of a call's prompt: cache reads over the whole prompt.
struct CallUse {
  int status = 0;
  double seconds = 0;
  uint32_t prompt = 0, cached = 0, output = 0;
  size_t body = 0;
  size_t sharedPrefix = 0;   // bytes of this request's body equal to the previous request's
};
CallUse useOf(const CurlHttpTransport::Call& c) {
  CallUse u;
  u.status = c.status;
  u.seconds = c.seconds;
  u.body = c.reqBody.size();
  JsonDocument d;
  if (deserializeJson(d, c.respBody, DeserializationOption::NestingLimit(64))) return u;
  const nimbus::orch::TokenUsage t = nimbus::orch::tokenUsageFromJson(d["usage"].as<ArduinoJson::JsonObjectConst>());
  u.prompt = t.maxRequestIn;
  u.cached = t.cacheReadTokens;
  u.output = t.completionTokens;
  return u;
}
int cachePercentFloor(const Leg& leg) { return std::string(leg.host) == "mistral" ? 50 : 90; }

std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
  char buf[200];
  std::snprintf(buf, sizeof buf, f, a, b, c, d);
  return buf;
}

void legRound(const Leg& leg, LegTransport& http, const agent::providers::ProviderDeps& pd, double scale,
              const Report& r) {
  const nimbus::orch::RequestBudget b = agent::providers::wireBudgetFor(leg.host, leg.model, leg.routed);
  const size_t want = (size_t)((double)b.wireBodyBytes * scale * 0.97);
  const std::string nonce = std::to_string(nowMs());
  const Prompt p = promptAt(leg, pd, want, nonce);
  r.note(std::string(leg.name) + ": budget " + std::to_string(b.wireBodyBytes) + " B (" + leg.host + "/" + leg.model +
         (leg.routed ? ", routed" : "") + "), round 0 built at " + std::to_string(p.body) + " B");
  const size_t first = http.curl.calls.size();
  int looked = 0;
  agent::HeadTools ht = labTools(looked);
  std::string out, err;
  const bool ok = runTurn(leg, pd, p.instructions, p.seed, leg.custom ? nullptr : &ht, {out, err});
  std::vector<CallUse> use;
  for (size_t i = first; i < http.curl.calls.size(); ++i) {
    use.push_back(useOf(http.curl.calls[i]));
    if (i > first) use.back().sharedPrefix = sharedPrefix(http.curl.calls[i - 1].reqBody, http.curl.calls[i].reqBody);
  }
  for (size_t i = 0; i < use.size(); ++i)
    r.note(std::string(leg.name) + ": request " + std::to_string(i) + " " + std::to_string(use[i].body) + " B -> HTTP " +
           std::to_string(use[i].status) +
           fmt(" in %.1f s, prompt %.0f tokens, cached %.0f, output %.0f", use[i].seconds, use[i].prompt,
               use[i].cached, use[i].output) +
           (i ? ", prefix shared with the round before " + std::to_string(use[i].sharedPrefix) + " B" : ""));
  r.check(ok, std::string(leg.name) + ": the turn answered" + (ok ? "" : ": " + err));
  r.check(!use.empty() && use[0].status == 200, std::string(leg.name) + ": round 0 at its target was accepted");
  bool timedOut = false;
  for (const CallUse& u : use) timedOut = timedOut || u.status == 0;
  r.check(!timedOut, std::string(leg.name) + ": no read timeout");
  if (leg.custom) {
    r.check(use.size() == 1, "custom: one flat single shot answered");
    return;
  }
  r.check(looked >= 1 && use.size() >= 2, std::string(leg.name) + ": the tool ran and a second round went out");
  for (size_t i = 1; i < use.size(); ++i) {
    if (!use[i].prompt) continue;
    const int pct = (int)(100.0 * use[i].cached / use[i].prompt);
    r.check(pct >= cachePercentFloor(leg), std::string(leg.name) + ": round " + std::to_string(i) + " read " +
                                               std::to_string(pct) + "% from the cache (at least " +
                                               std::to_string(cachePercentFloor(leg)) + "%)");
  }
}

// The model a fold for this head runs: a Mistral head forced to reason (Large 4: it refuses
// effort "none") cannot write a fold inside its 60 s fit at its 50 tok/s floor, so its fold
// runs on Mistral Small (RULINGS-v3, answer 5; the fold ladder pins it). Every other
// head folds on itself.
std::string foldModelFor(const Leg& leg) {
  const bool forced = std::string(leg.host) == "mistral" &&
                      std::strcmp(agent::providers::mistralHeadEffort(leg.model), "low") == 0;
  return forced ? "mistral-small-latest" : leg.model;
}

// The fold: one plain-text request on a digest of the host's fold step.
size_t foldStepFor(const Leg& leg) {
  const size_t step =   // the budget legs run the tool loop
      nimbus::orch::headContextBudget(leg.host, leg.model, true, nimbus::orch::BudgetOverrides()).foldStepBytes;
  return std::min(step, nimbus::orch::foldDigestCap(0) - 4096);
}
int sectionsNamed(const std::string& summary) {
  std::string low;
  for (char c : summary) low += (char)std::tolower((unsigned char)c);
  int n = 0;
  for (const char* k : {"decision", "pending", "fact", "intent", "device"})
    if (low.find(k) != std::string::npos) ++n;
  return n;
}
void legFold(const Leg& leg, LegTransport& http, agent::providers::ProviderDeps pd, double scale, const Report& r) {
  const std::string model = foldModelFor(leg);
  pd.orchModel = [model](const char*) { return model; };
  const size_t step = (size_t)((double)foldStepFor(leg) * scale);
  std::string digest;
  for (int i = 0; digest.size() < step; ++i) digest += "- owner: " + loggedDay(i) + "\n- assistant: Logged.\n";
  digest.resize(step);
  const std::string inputs = nimbus::orch::buildCompactInputs("", digest);
  const size_t first = http.curl.calls.size();
  std::string out, err;
  const bool ok = runTurn(leg, pd, nimbus::orch::ORCH_COMPACT_PROMPT, inputs, nullptr, {out, err});
  std::vector<CallUse> use;
  for (size_t i = first; i < http.curl.calls.size(); ++i) use.push_back(useOf(http.curl.calls[i]));
  for (const CallUse& u : use)
    r.note(std::string(leg.name) + ": fold on " + model + " " + std::to_string(u.body) + " B -> HTTP " +
           std::to_string(u.status) +
           fmt(" in %.1f s, prompt %.0f tokens, output %.0f", u.seconds, u.prompt, u.output));
  r.check(ok && !out.empty(), std::string(leg.name) + ": the plain fold on a " + std::to_string(step) +
                                  " B digest answered" + (ok ? "" : ": " + err));
  r.check(sectionsNamed(out) >= 2, std::string(leg.name) + ": the summary carries the fold's sections (" +
                                       std::to_string(sectionsNamed(out)) + " of 5 named)");
}

// The photo round: a 3-photo album the host sees as its own image parts.
std::vector<std::vector<uint8_t>> loadAlbum(std::string& why) {
  std::vector<std::vector<uint8_t>> out;
  const std::string dir = envOr("LAB_BUDGET_PHOTOS", "");
  if (dir.empty()) {
    why = "LAB_BUDGET_PHOTOS is not set (a directory of JPEGs)";
    return out;
  }
  std::vector<std::string> names;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 4 && n.compare(n.size() - 4, 4, ".jpg") == 0) names.push_back(n);
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  for (size_t i = 0; i < names.size() && out.size() < 3; ++i) {
    std::ifstream f(dir + "/" + names[i], std::ios::binary);
    out.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (out.size() < 3) why = "fewer than 3 JPEGs in LAB_BUDGET_PHOTOS";
  return out;
}
void legPhoto(const Leg& leg, LegTransport& http, const agent::providers::ProviderDeps& pd,
              const std::vector<std::vector<uint8_t>>& album, const Report& r) {
  nimbus::orch::TurnImages set;
  set.nonce = "lb" + std::to_string(nowMs() % 100000);
  set.uploadOnce = false;
  std::string text = "[USER]\n";
  for (size_t i = 0; i < album.size(); ++i) {
    nimbus::orch::TurnImage it;
    char ref[17];
    std::snprintf(ref, sizeof ref, "%016zx", (size_t)0xb0d6e700 + i);
    it.ref = ref;
    it.mime = "image/jpeg";
    it.bytes = album[i].data();
    it.len = album[i].size();
    it.k = (int)i + 1;
    it.n = (int)album.size();
    it.tag = "[image " + std::to_string(i + 1) + "/" + std::to_string(album.size()) + " ref=" + ref + "]";
    it.segment = it.tag + " (attached: the photo itself is in this message)";
    text += it.segment + "\n";
    set.items.push_back(it);
  }
  text += "What does each photo show? One short line each.";
  set.transcribe = [](nimbus::orch::TurnImage& it) { return it.tag + " (not read: this leg has no captioner)"; };
  const nimbus::orch::ActiveTurnImages active(&set);
  const size_t first = http.curl.calls.size();
  int looked = 0;
  agent::HeadTools ht = labTools(looked);
  std::string out, err;
  const bool ok = runTurn(leg, pd, "You are the owner's desk assistant. Answer briefly.", text,
                          leg.custom ? nullptr : &ht, {out, err});
  const std::string host = leg.host;
  const agent::providers::imageparts::Dialect d =
      host == "anthropic" ? agent::providers::imageparts::Dialect::Anthropic
      : host == "openai"  ? agent::providers::imageparts::Dialect::OpenAIResponses
                          : agent::providers::imageparts::Dialect::MistralChat;
  const uint32_t estimate = agent::providers::imageparts::photoTokens(&set, true, false, d);
  CallUse u;
  size_t parts = 0;
  if (http.curl.calls.size() > first) {
    u = useOf(http.curl.calls[first]);
    const std::string& body = http.curl.calls[first].reqBody;
    for (const char* k : {"\"type\":\"image\"", "\"type\":\"input_image\"", "\"type\":\"image_url\""})
      for (size_t at = body.find(k); at != std::string::npos; at = body.find(k, at + 1)) ++parts;
  }
  r.note(std::string(leg.name) + ": photo round " + std::to_string(u.body) + " B -> HTTP " + std::to_string(u.status) +
         ", " + std::to_string(parts) + " image parts, estimate " + std::to_string(estimate) +
         " photo tokens, provider counted " + std::to_string(u.prompt) + " prompt tokens; reply: " + out.substr(0, 160));
  r.check(ok, std::string(leg.name) + ": the photo round answered" + (ok ? "" : ": " + err));
  const bool blind = host == "zai" || leg.custom;
  r.check(blind ? parts == 0 : parts == album.size(),
          std::string(leg.name) + (blind ? ": a text-only head reads the transcriptions"
                                         : ": every photo went as the host's own image part"));
}

}  // namespace

void run(LabRig& rig, const Report& r) {
  const Env& env = rig.env();
  const std::string legs = envOr("LAB_BUDGET_LEGS", "");
  const std::string partsList = envOr("LAB_BUDGET_PARTS", "round,fold,photo");
  double scale = std::atof(envOr("LAB_BUDGET_SCALE", "1").c_str());
  if (scale < 0.05 || scale > 1) scale = 1;
  const std::string proxy = envOr("LAB_MISTRAL_PROXY", "");
  const std::string keyFile = envOr("LAB_CUMULO_KEY_FILE", "");
  const std::string routerKey = keyFile.empty() ? std::string() : readKeyFile(keyFile);
  std::string albumWhy;
  const std::vector<std::vector<uint8_t>> album =
      listed(partsList, "photo") ? loadAlbum(albumWhy) : std::vector<std::vector<uint8_t>>();
  int ran = 0;
  for (const Leg& leg : kLegs) {
    if (!listed(legs, leg.name)) continue;
    std::string why;
    if (!legRunnable(env, leg, routerKey, proxy, why)) {
      r.note(std::string(leg.name) + ": skipped (" + why + ")");
      continue;
    }
    ++ran;
    LegTransport http;
    http.mistralProxy = proxy;
    http.curl.recordBodies = true;
    const agent::providers::ProviderDeps pd = legDeps(env, leg, http, routerKey);
    if (listed(partsList, "round")) legRound(leg, http, pd, scale, r);
    if (listed(partsList, "fold")) legFold(leg, http, pd, scale, r);
    if (listed(partsList, "photo")) {
      if (album.size() == 3) legPhoto(leg, http, pd, album, r);
      else r.note(std::string(leg.name) + ": photo round skipped (" + albumWhy + ")");
    }
  }
  if (!ran) r.skip("no leg had a key (set the provider keys, LAB_MISTRAL_PROXY or LAB_CUMULO_KEY_FILE)");
}

}  // namespace budget
}  // namespace lab
