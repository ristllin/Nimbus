// One model on one provider slot (plain/head_slot.h), lane E2-R8a. Both engines compile it (engine 1's
// provider verify reads a custom endpoint with it), so it is outside the plain_* engine-2 family.
#include "nimbus/harness/plain/head_slot.h"

#include <ArduinoJson.h>

#include <cstdlib>

#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_key.h"   // usageModelKey: the key the ledger files a routed spend under
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"

namespace agent {
namespace plain {

namespace {

namespace caps = nimbus::orch::caps;

// The table's host of a direct slot ("" for a slot no row gives one).
std::string tableHost(caps::Slot slot) {
  const caps::CapsTable& t = caps::builtinCaps();
  for (size_t i = 0; i < t.providerCount; ++i)
    if (t.providers[i].slot == slot) return caps::poolStr(t, t.providers[i].host);
  return std::string();
}

// The wire a slot speaks.
Dialect dialectOf(caps::Slot slot) {
  switch (slot) {
    case caps::Slot::Anthropic:
    case caps::Slot::CustomMessages: return Dialect::AnthropicMessages;
    case caps::Slot::OpenAI: return Dialect::OpenAIResponses;
    default: return Dialect::ChatCompletions;
  }
}

}  // namespace

bool customEndpointOf(const std::string& base, HeadEndpoint& e) {
  std::string rest = base;
  e.tls = true;
  e.port = 443;
  const size_t scheme = rest.find("://");
  if (scheme != std::string::npos) {
    e.tls = rest.compare(0, scheme, "http") != 0;
    e.port = e.tls ? 443 : 80;
    rest = rest.substr(scheme + 3);
  }
  const size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  e.basePath = slash == std::string::npos ? std::string() : rest.substr(slash);
  while (!e.basePath.empty() && e.basePath.back() == '/') e.basePath.pop_back();
  const size_t colon = hostPort.rfind(':');
  if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
    const long port = std::strtol(hostPort.c_str() + colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    e.port = (uint16_t)port;
    hostPort.resize(colon);
  }
  e.host = hostPort;
  return !e.host.empty();
}

namespace {

// The router's host from its base URL or bare host.
std::string hostOf(const std::string& base) {
  HeadEndpoint e;
  return customEndpointOf(base, e) ? e.host : std::string();
}

Status bad(const char* where) { return Status::fail(Err::InvalidArgument, where); }

// A routed head: the selector's upstream wire under the router's host (design section 10).
Status routedSlot(const SlotSettings& s, HeadSlot& out) {
  caps::Slot slot;
  const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(out.model);
  if (!caps::slotFromHost(rr.upstream.c_str(), &slot) || slot == caps::Slot::CustomChat) return bad("plain::headSlotFor.upstream");
  out.dialect = dialectOf(slot);
  out.endpoint.host = hostOf(s.routerHost);
  out.endpoint.routed = true;
  return out.endpoint.host.empty() ? bad("plain::headSlotFor.router") : Status::success();
}

// The owner's endpoint (design section 3.3): its convention's wire, text mode until verified.
Status customSlot(const SlotSettings& s, HeadSlot& out) {
  if (!customEndpointOf(s.customBase, out.endpoint)) return bad("plain::headSlotFor.custom");
  const bool messages = s.customConv == "anthropic";
  out.dialect = messages ? Dialect::AnthropicMessages : s.customTools ? Dialect::ChatCompletions : Dialect::Flat;
  out.endpoint.toolsVerified = !messages && s.customTools;
  return Status::success();
}

}  // namespace

Status headSlotFor(const SlotSettings& s, const std::string& provider, const std::string& model, HeadSlot& out) {
  out = HeadSlot{};
  out.provider = provider;
  out.model = model;
  if (model.empty()) return bad("plain::headSlotFor.model");
  const std::function<std::string(const std::string&)> keyOf = s.key;
  out.endpoint.key = [keyOf, provider] { return keyOf ? keyOf(provider) : std::string(); };
  if (provider == "cumulo") return routedSlot(s, out);
  if (provider == "custom") return customSlot(s, out);
  caps::Slot slot;
  if (!caps::slotFromHost(provider.c_str(), &slot)) return bad("plain::headSlotFor.provider");
  out.dialect = dialectOf(slot);
  out.endpoint.host = slot == caps::Slot::Zai && !s.zaiHost.empty() ? hostOf(s.zaiHost) : tableHost(slot);
  return out.endpoint.host.empty() ? bad("plain::headSlotFor.host") : Status::success();
}

Dialect slotDialect(const std::string& provider, const std::string& model, Dialect head) {
  if (provider != "cumulo") return head;
  caps::Slot slot;
  const std::string up = nimbus::orch::resolveRouterRoute(model).upstream;
  return caps::slotFromHost(up.c_str(), &slot) ? dialectOf(slot) : head;
}

bool slotRatesFor(const std::string& provider, const std::string& model, HeadRates& out) {
  if (headRatesFor(provider, model, out)) return true;
  if (!nimbus::orch::isCustomSlot(provider.c_str()) || nimbus::orch::customEndpointPaid()) return false;
  out = HeadRates{};   // a local endpoint bills nothing
  return true;
}

nimbus::orch::TokenUsage ledgerUsage(const std::string& provider, const std::string& model, nimbus::orch::TokenUsage u) {
  std::string billed = provider;
  if (provider == "cumulo" && nimbus::orch::routedBudgetLift()) {
    // E2-REP1: by the key the ledger files it under (usageModelKey: a router echo naming another
    // upstream than the selector files under the echo), so the two read the same cache shape.
    char key[nimbus::orch::kModelKeyCap];
    const std::string sel =
        nimbus::orch::usageModelKey("cumulo", model.c_str(), u.servedModel.c_str(), key) ? std::string(key) : model;
    billed = nimbus::orch::resolveRouterRoute(sel).upstream;
  }
  if (billed == "anthropic") return u;   // the Excluded shape: the uncached part is the input count
  u.promptTokens += u.cacheReadTokens + u.cacheWriteTokens;
  return u;
}

uint32_t servedWindowOf(const std::string& modelsBody, const std::string& model) {
  JsonDocument doc;
  if (deserializeJson(doc, modelsBody, DeserializationOption::NestingLimit(8))) return 0;
  JsonArrayConst rows = doc["data"].is<JsonArrayConst>() ? doc["data"].as<JsonArrayConst>() : doc["models"].as<JsonArrayConst>();
  JsonObjectConst pick;
  for (JsonObjectConst r : rows)
    if (model == (const char*)(r["id"] | r["name"] | "")) pick = r;
  if (pick.isNull() && rows.size() == 1) pick = rows[0].as<JsonObjectConst>();
  if (pick.isNull()) return 0;
  for (const char* f : {"max_model_len", "context_length", "context_window", "max_context_length", "max_input_tokens"}) {
    const uint32_t v = pick[f] | 0u;
    if (v) return v;
  }
  return 0;
}

std::string toolProbeBody(const std::string& model) {
  JsonDocument doc;
  doc["model"] = model;
  doc["max_tokens"] = 64;
  JsonObject m = doc["messages"].add<JsonObject>();
  m["role"] = "user";
  m["content"] = texts::probe::kToolProbeAsk;
  JsonObject fn = doc["tools"].add<JsonObject>();
  fn["type"] = "function";
  fn["function"]["name"] = texts::probe::kToolProbeName;
  fn["function"]["description"] = texts::probe::kToolProbeDescription;
  fn["function"]["parameters"]["type"] = "object";
  fn["function"]["parameters"]["properties"].to<JsonObject>();
  doc["tool_choice"] = "required";
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool answerCalledTool(const std::string& chatBody) {
  JsonDocument doc;
  if (deserializeJson(doc, chatBody, DeserializationOption::NestingLimit(10))) return false;
  JsonArrayConst calls = doc["choices"][0]["message"]["tool_calls"];
  return calls.size() > 0 && std::string((const char*)(calls[0]["function"]["name"] | "")) == texts::probe::kToolProbeName;
}

CustomVerify customVerify(const HeadSlot& slot, const std::string& key, bool messagesConv, const CustomExchangeFn& exchange) {
  CustomVerify v;
  if (!key.empty() && !slot.endpoint.tls) {   // E2-REP1: a key never rides a cleartext socket (as every codec)
    v.result = 0;
    v.cleartextKey = true;
    return v;
  }
  HttpRequest req;
  req.method = "GET";
  req.host = slot.endpoint.host;
  req.port = slot.endpoint.port;
  req.tls = slot.endpoint.tls;
  const std::string base = slot.endpoint.basePath.empty() ? "/v1" : slot.endpoint.basePath;
  req.path = base + "/models";
  if (messagesConv) {   // E2-REP1: a Messages endpoint's own headers, as the head codec sends them
    req.headers.push_back({"anthropic-version", "2023-06-01"});
    if (!key.empty()) req.headers.push_back({"x-api-key", key});
  } else if (!key.empty()) {
    req.headers.push_back({"Authorization", std::string("Bearer") + ' ' + key});
  }
  std::string body;
  const int code = exchange(req, body);
  if (code == 401 || code == 403) v.result = 0;
  if (code != 200) return v;
  v.result = 1;
  v.window = servedWindowOf(body, slot.model);
  if (messagesConv) return v;   // a Messages endpoint keeps its tools (no chat probe)
  req.method = "POST";
  req.path = base + "/chat/completions";
  req.headers.push_back({"Content-Type", "application/json"});
  req.body = toolProbeBody(slot.model);
  body.clear();
  v.tools = exchange(req, body) == 200 && answerCalledTool(body);
  return v;
}

bool customToolsFrom(const std::string& storeText, const std::string& model) {
  bool tools = false;
  nimbus::orch::forEachStoreRow("custom", storeText, [&](const nimbus::orch::ModelInfo& m) {
    if (m.id != model) return true;
    tools = m.apiCaps && m.hasCap(nimbus::orch::CapTools) && m.ctxTokens >= kCustomToolsMinWindow;
    return false;
  });
  return tools;
}

}  // namespace plain
}  // namespace agent
