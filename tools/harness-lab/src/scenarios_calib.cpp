#include "scenarios_calib.h"

#include <ArduinoJson.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nimbus/harness/output_caps.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/request_budget.h"

// The request budget's calibration on the PL provider legs (lane B of the history fix; plan v3
// live legs). Per direct leg, ONE cold request at the head's own budget (request_budget.h), shaped
// as that wire sends a loop round (system, one user and assistant message per history turn, the
// seed; the lab registry's real tools), asking for a long final answer under the new load-aware
// cap. It checks:
//   - calibration: prompt_tokens within -15%/+5% of the target, and the body's bytes per token
//     at or above the dense ratio rSafe (so the tighten-only memo never lowers the budget on this
//     content);
//   - the forced long final: the round (measured here, plus the device link's predicted upload at
//     70 KB/s) finishes inside the host's read window with the cap the new caps give.
// Mistral Medium also sends an identical repeat and an append-only extension at its target and
// records the cached tokens. Routed and custom heads sit on the v5.1.1 floor in v5.1.2: no leg.
//
// Content is fixture data only, never an owner's: the synthetic persona conversation
// (test/support/fixtures/l4_persona_b_episodic_2026-10-10.jsonl) and a generated food log (the
// dense, number-heavy part of a Lumi-shaped day). Keys are read in this process (lab Env).
// NIMBUS_CALIB_LEGS=<host/model,...> picks legs; NIMBUS_CALIB_DRY=1 builds every body and sends
// nothing; NIMBUS_CALIB_MISTRAL_PROXY=<host:port> sends the Mistral legs through a local key
// proxy (plain HTTP, the proxy's placeholder bearer); NIMBUS_CALIB_OUT=<file> saves the report.
namespace lab {
namespace calib {

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;

struct Leg {
  std::string host, model;
};
const Leg kLegs[] = {
    {"mistral", "mistral-large-4-0"}, {"mistral", "mistral-medium-latest"}, {"openai", "gpt-5.6-luna"},
    {"openai", "gpt-5.6"},            {"anthropic", "claude-sonnet-5-5"},   {"anthropic", "claude-haiku-5-5"},
    {"zai", "glm-5.3"},
};

struct Pair {
  std::string user, assistant;
};

// ---- fixture content ------------------------------------------------------------------------
std::string readFirst(const std::vector<std::string>& paths) {
  for (const std::string& p : paths) {
    std::ifstream f(p);
    if (f) return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  return std::string();
}

std::vector<Pair> personaPairs() {
  const std::string rel = "test/support/fixtures/l4_persona_b_episodic_2026-10-10.jsonl";
  const std::string text = readFirst({rel, "../../" + rel});
  std::vector<Pair> out;
  std::string pendingUser;
  size_t at = 0;
  while (at < text.size()) {
    const size_t nl = text.find('\n', at);
    const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() : nl + 1;
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    const std::string role = d["role"] | "", body = d["text"] | "";
    if (role == "user") pendingUser = body;
    else if (role == "assistant" && !pendingUser.empty()) out.push_back({pendingUser, body}), pendingUser.clear();
  }
  return out;
}

// A generated food-log exchange: numbers, units and short lines, as a logging day reads.
Pair foodPair(unsigned i) {
  static const char* const kFoods[] = {"chicken breast", "basmati rice", "greek yogurt", "oat flakes", "banana",
                                       "olive oil",      "lentil soup",  "rye bread",    "cottage cheese", "apple"};
  const unsigned g = 80 + (i * 37) % 220, kcal = 90 + (i * 53) % 400, p = 3 + (i * 7) % 45, f = 1 + (i * 5) % 22;
  char u[160], a[420];
  std::snprintf(u, sizeof u, "log %u g %s at %02u:%02u, and what is my total so far?", g, kFoods[i % 10],
                7 + i % 14, (i * 13) % 60);
  std::snprintf(a, sizeof a,
                "Logged %u g %s at %02u:%02u: %u kcal, protein %u.%u g, fat %u.%u g, carbs %u.%u g. Day so far: "
                "%u kcal of 2,150, protein %u g of 140, fat %u g, carbs %u g, fiber %u g, water %u.%u L. "
                "Remaining: %d kcal; next meal suggestion: %s with a side of %s.",
                g, kFoods[i % 10], 7 + i % 14, (i * 13) % 60, kcal, p, i % 10, f, (i * 3) % 10, (i * 11) % 60,
                i % 10, 400 + (i * 97) % 1700, 20 + (i * 9) % 120, 10 + (i * 4) % 70, 50 + (i * 17) % 200,
                3 + i % 25, 1 + i % 3, (i * 7) % 10, 2150 - (int)(400 + (i * 97) % 1700), kFoods[(i + 3) % 10],
                kFoods[(i + 6) % 10]);
  return {u, a};
}

std::string systemBlock() {
  std::string s = "You are Calib, a desk assistant. House rules for this calibration conversation: answer in plain "
                  "prose, keep the food log exact, never invent a number, and say when a figure is an estimate.\n";
  for (unsigned i = 0; s.size() < 20000; i++) s += "- Rule " + std::to_string(i + 1) + ": " + foodPair(i).assistant + "\n";
  return s;
}

const char* kLongAsk =
    "Do not call any tool. Write the longest, most detailed answer you can: a full written review of every "
    "day in this conversation, meal by meal, with each figure restated and a comment on each one, then a "
    "two-week plan with every day spelled out. Keep writing until you run out of room.";

// ---- one wire's body -------------------------------------------------------------------------
struct Shape {
  std::string system;
  std::vector<Pair> pairs;
  std::string seed;
  bool longFinal = true;
  bool extension = false;   // an append-only next round (the cache leg)
};

// A registry name as every provider takes it ([A-Za-z0-9_-]: "memory.search" becomes
// "memory_search", the head loop's own mapping), the same length so the bytes hold.
std::string wireToolName(const std::string& n) {
  std::string out = n;
  for (char& c : out)
    if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
  return out;
}

void putTools(JsonArray tools, LabRig& rig, const std::string& host) {
  for (const auto& sp : rig.registry().toolSpecs()) {
    JsonDocument schema;
    if (deserializeJson(schema, sp.schemaJson)) deserializeJson(schema, "{\"type\":\"object\"}");
    JsonObject t = tools.add<JsonObject>();
    const std::string name = wireToolName(sp.name);
    if (host == "anthropic") {
      t["name"] = name, t["description"] = sp.description, t["input_schema"] = schema;
    } else if (host == "openai") {
      t["type"] = "function", t["name"] = name, t["description"] = sp.description, t["parameters"] = schema;
    } else {
      t["type"] = "function";
      JsonObject f = t["function"].to<JsonObject>();
      f["name"] = name, f["description"] = sp.description, f["parameters"] = schema;
    }
  }
}

void putMessages(JsonArray msgs, const Shape& s, const std::string& host) {
  const char* assistantRole = "assistant";
  for (const Pair& p : s.pairs) {
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user", u["content"] = p.user;
    JsonObject a = msgs.add<JsonObject>();
    a["role"] = assistantRole, a["content"] = p.assistant;
  }
  JsonObject seed = msgs.add<JsonObject>();
  seed["role"] = "user", seed["content"] = s.seed;
  if (!s.extension) return;
  JsonObject a = msgs.add<JsonObject>();
  a["role"] = assistantRole, a["content"] = "Noted: the day so far is logged above.";
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user", u["content"] = host == "anthropic" ? "And the protein left for today?" : "And the protein left?";
}

std::string bodyFor(LabRig& rig, const Leg& leg, const Shape& s, uint32_t cap, const std::string& key) {
  JsonDocument d;
  d["model"] = leg.model;
  if (leg.host == "anthropic") {
    JsonObject sys = d["system"].to<JsonArray>().add<JsonObject>();
    sys["type"] = "text", sys["text"] = s.system;
    d["max_tokens"] = cap;
    d["output_config"]["effort"] = "medium";
    if (s.longFinal) d["tool_choice"]["type"] = "none";
  } else if (leg.host == "openai") {
    JsonObject dev = d["input"].to<JsonArray>().add<JsonObject>();
    dev["role"] = "developer", dev["content"] = s.system;
    d["store"] = false, d["prompt_cache_key"] = key, d["reasoning"]["effort"] = "medium";
    if (s.longFinal) d["tool_choice"] = "none";
  } else {
    JsonObject sys = d["messages"].to<JsonArray>().add<JsonObject>();
    sys["role"] = "system", sys["content"] = s.system;
    if (cap) d["max_tokens"] = cap;
    if (leg.host == "mistral") {
      d["prompt_cache_key"] = key;
      const char* effort = providers::mistralHeadEffort(leg.model);
      if (*effort) d["reasoning_effort"] = effort;
      d["tool_choice"] = s.longFinal ? "none" : "auto";
    }
  }
  putTools(d["tools"].to<JsonArray>(), rig, leg.host);
  putMessages(leg.host == "openai" ? d["input"].as<JsonArray>()
              : leg.host == "anthropic" ? d["messages"].to<JsonArray>()
                                        : d["messages"].as<JsonArray>(),
              s, leg.host);
  std::string out;
  serializeJson(d, out);
  return out;
}

// Fill the shape with history pairs until the body reaches `targetBytes` (within 1%): persona
// pairs and food pairs alternate, the system starts with a nonce so the round is cold.
Shape fill(LabRig& rig, const Leg& leg, uint32_t targetBytes, const std::string& nonce, uint32_t cap) {
  static const std::vector<Pair> persona = personaPairs();
  Shape s;
  s.system = "[calibration " + nonce + "]\n" + systemBlock();
  s.seed = kLongAsk;
  for (unsigned i = 0; i < 4000; i++) {
    const Pair p = (i % 3 == 0 && !persona.empty()) ? persona[(i / 3) % persona.size()] : foodPair(i);
    s.pairs.push_back(p);
    if (bodyFor(rig, leg, s, cap, nonce).size() < targetBytes) continue;
    s.pairs.pop_back();
    break;
  }
  while (bodyFor(rig, leg, s, cap, nonce).size() + 400 < targetBytes) s.seed = foodPair((unsigned)s.seed.size()).assistant + "\n" + s.seed;
  return s;
}

// ---- the request ------------------------------------------------------------------------------
agent::HttpRequest requestFor(const Env& env, const Leg& leg, const std::string& body) {
  agent::HttpRequest r;
  r.method = "POST";
  r.body = body;
  r.timeoutMs = 180000;   // past every read window: an overrun is measured, not cut
  r.headers = {{"Content-Type", "application/json"}};
  if (leg.host == "anthropic") {
    r.host = "api.anthropic.com", r.path = "/v1/messages";
    r.headers.push_back({"x-api-key", env.providerKey("anthropic")});
    r.headers.push_back({"anthropic-version", "2023-06-01"});
    return r;
  }
  const char* proxy = std::getenv("NIMBUS_CALIB_MISTRAL_PROXY");
  if (leg.host == "mistral" && proxy && *proxy) {
    const std::string hp = proxy;
    const size_t colon = hp.rfind(':');
    r.host = hp.substr(0, colon), r.port = (uint16_t)std::atoi(hp.substr(colon + 1).c_str()), r.tls = false;
    r.path = "/v1/chat/completions";
    r.headers.push_back({"Authorization", "Bearer proxy-fallback"});
    return r;
  }
  r.host = leg.host == "openai" ? "api.openai.com" : leg.host == "zai" ? kLabZaiHost : "api.mistral.ai";
  r.path = leg.host == "openai" ? "/v1/responses" : leg.host == "zai" ? std::string(kLabZaiPath) + "/chat/completions"
                                                                       : "/v1/chat/completions";
  r.headers.push_back({"Authorization", "Bearer " + env.providerKey(leg.host)});
  return r;
}

// An error body's type and message only, with anything key-shaped masked: a provider can
// echo part of a rejected key ("Incorrect API key provided: sk-...abcd"), and this line is
// printed and saved.
std::string maskKeys(std::string s) {
  for (const char* p : {"sk-", "Bearer ", "key "}) {
    for (size_t at = s.find(p); at != std::string::npos; at = s.find(p, at + 1)) {
      size_t end = at + std::strlen(p);
      while (end < s.size() && !std::isspace((unsigned char)s[end]) && s[end] != '"' && s[end] != ',') end++;
      s.replace(at, end - at, std::string(p) + "[masked]");
    }
  }
  return s;
}

std::string errorLine(JsonDocument& d) {
  JsonVariantConst e = d["error"];
  const std::string type = e["type"] | e["code"] | "", msg = e["message"] | d["message"] | d["detail"] | "";
  return maskKeys(type + ": " + msg.substr(0, 240));
}

struct Outcome {
  int status = 0;
  uint32_t ms = 0, prompt = 0, cached = 0, output = 0;
  std::string finish, err;
};

Outcome send(LabRig& rig, const Leg& leg, const std::string& body) {
  Outcome o;
  agent::HttpResponse resp;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = rig.http().exec(requestFor(rig.env(), leg, body), resp, o.err);
  o.ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  o.status = ok ? resp.status : 0;
  JsonDocument d;
  if (!ok || deserializeJson(d, resp.body)) {
    if (ok && o.status >= 400) o.err = "unreadable error body";
    o.err = maskKeys(o.err);
    return o;
  }
  JsonObjectConst u = d["usage"];
  o.prompt = u["input_tokens"] | u["prompt_tokens"] | 0u;
  o.cached = u["cache_read_input_tokens"] | u["input_tokens_details"]["cached_tokens"] |
             u["prompt_tokens_details"]["cached_tokens"] | 0u;
  if (leg.host == "anthropic") o.prompt += o.cached + (u["cache_creation_input_tokens"] | 0u);
  o.output = u["output_tokens"] | u["completion_tokens"] | 0u;
  o.finish = d["stop_reason"] | d["status"] | d["choices"][0]["finish_reason"] | "";
  if (o.status >= 400) o.err = errorLine(d);
  return o;
}

double usdOf(const Leg& leg, const Outcome& o) {
  const orch::BuiltinPrice* p = orch::builtinModelPrice(leg.host.c_str(), leg.model.c_str());
  if (!p) return 0;
  const double cr = p->microCacheR ? p->microCacheR : p->microIn * 0.1;
  return ((double)(o.prompt - o.cached) * p->microIn * 1.25 + (double)o.cached * cr + (double)o.output * p->microOut) / 1e12;
}

// ---- one leg -----------------------------------------------------------------------------------
struct LegReport {
  std::string line;
  double usd = 0;
};

std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
  char buf[256];
  std::snprintf(buf, sizeof buf, f, a, b, c, d);
  return buf;
}

LegReport runLeg(LabRig& rig, const Leg& leg, const Report& r, bool dry) {
  const orch::RequestBudget b = orch::requestBudgetFor(leg.host, leg.model, false, orch::BudgetLink::Device);
  providers::RoundLoad load;
  load.bodyBytes = b.wireBodyBytes;
  load.promptTokens = b.targetTokens;
  const providers::OutputCap cap = providers::outputCapFor(leg.host, leg.model, providers::RoundKind::FinalRound, b.roundMs, load);
  const std::string nonce = leg.model + "-" + std::to_string((unsigned)std::time(nullptr));
  const Shape s = fill(rig, leg, b.wireBodyBytes, nonce, cap.maxTokens);
  const std::string body = bodyFor(rig, leg, s, cap.maxTokens, nonce);
  const std::string what = leg.host + "/" + leg.model;
  r.note(what + fmt(": target %.0f tokens, %.0f B budget, body %.0f B, cap %.0f", b.targetTokens, b.wireBodyBytes,
                    body.size(), cap.maxTokens));
  if (dry) return {what + " dry", 0};
  const Outcome o = send(rig, leg, body);
  if (o.status != 200) {
    r.check(false, what + " answered " + std::to_string(o.status) + " " + o.err);
    return {what + " failed " + std::to_string(o.status), 0};
  }
  const double ratio = o.prompt ? (double)body.size() / o.prompt : 0;
  const uint32_t deviceUp = orch::roundOverheadMs(orch::BudgetLink::Device, body.size(), false, 0, 0) - orch::kBudgetConnectMs;
  const uint32_t devicePaced = deviceUp + orch::predictedPaceMs(body.size());
  r.check(o.prompt >= b.targetTokens * 0.85 && o.prompt <= b.targetTokens * 1.05,
          what + fmt(" prompt %.0f tokens within -15%%/+5%% of the %.0f target", o.prompt, b.targetTokens));
  r.check(ratio * 100 >= b.rSafeX100, what + fmt(" %.2f B/token at or above rSafe %.2f", ratio, b.rSafeX100 / 100.0));
  r.check(o.ms + deviceUp <= b.readWindowMs,
          what + fmt(" long final %.0f ms here + %.0f ms device upload inside the %.0f ms read window", o.ms, deviceUp,
                     b.readWindowMs));
  const std::string line = what + fmt(": prompt %.0f, cached %.0f, out %.0f, %.0f ms", o.prompt, o.cached, o.output, o.ms) +
                           " finish=" + o.finish + fmt(", %.2f B/token, +%.0f ms upload (+%.0f paced)", ratio, deviceUp, devicePaced);
  r.note(line);
  return {line, usdOf(leg, o)};
}

// Mistral Medium's cache at its target: an identical repeat, then an append-only extension.
LegReport mediumCache(LabRig& rig, const Report& r, bool dry) {
  const Leg leg{"mistral", "mistral-medium-latest"};
  const orch::RequestBudget b = orch::requestBudgetFor(leg.host, leg.model, false, orch::BudgetLink::Device);
  const std::string nonce = "medium-cache-" + std::to_string((unsigned)std::time(nullptr));
  Shape s = fill(rig, leg, b.wireBodyBytes, nonce, 64);
  s.longFinal = false;
  s.seed = "What is my protein total so far? One line.";
  LegReport out{"", 0};
  for (int step = 0; step < 3 && !dry; step++) {
    s.extension = step == 2;
    const Outcome o = send(rig, leg, bodyFor(rig, leg, s, 64, nonce));
    const char* name = step == 0 ? "cold" : step == 1 ? "repeat" : "extension";
    out.line += name + fmt(" prompt %.0f cached %.0f %.0f ms; ", o.prompt, o.cached, o.ms);
    out.usd += usdOf(leg, o);
    r.check(o.status == 200, std::string("medium cache ") + name + " answered " + std::to_string(o.status));
  }
  r.note("mistral-medium-latest cache at " + std::to_string(b.wireBodyBytes) + " B: " + out.line);
  return out;
}

bool wanted(const Leg& leg) {
  const char* only = std::getenv("NIMBUS_CALIB_LEGS");
  if (!only || !*only) return true;
  return (std::string(",") + only + ",").find("," + leg.host + "/" + leg.model + ",") != std::string::npos;
}

}  // namespace

void run(LabRig& rig, const Report& r) {
  const bool dry = std::getenv("NIMBUS_CALIB_DRY") && *std::getenv("NIMBUS_CALIB_DRY") == '1';
  const char* proxy = std::getenv("NIMBUS_CALIB_MISTRAL_PROXY");
  std::string report;
  double usd = 0;
  for (const Leg& leg : kLegs) {
    if (!wanted(leg)) continue;
    const bool viaProxy = leg.host == "mistral" && proxy && *proxy;
    if (!dry && !viaProxy && !rig.hostAvailable(leg.host)) {
      r.note(leg.host + "/" + leg.model + ": no key, not run");
      continue;
    }
    const LegReport lr = runLeg(rig, leg, r, dry);
    report += lr.line + "\n";
    usd += lr.usd;
  }
  if (wanted({"mistral", "mistral-medium-latest"})) {
    const LegReport mc = mediumCache(rig, r, dry);
    report += "medium cache: " + mc.line + "\n";
    usd += mc.usd;
  }
  r.note(fmt("estimated spend %.3f USD (built-in prices, cold writes at 1.25x)", usd));
  if (const char* out = std::getenv("NIMBUS_CALIB_OUT")) std::ofstream(out) << report << fmt("spend %.3f USD\n", usd);
}

}  // namespace calib
}  // namespace lab
