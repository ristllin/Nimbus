#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "nimbus/harness/providers.h"
#include "nimbus/orch/orch_schema.h"       // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - usage extraction
#include "wire.h"

// Custom / proxy - the PORTABLE wire half of the pre-split
// src/agent/adapters/custom_adapter.cpp (Stage H). Synchronous chat-completion
// against a configurable OpenAI/Mistral/Anthropic-compatible endpoint.
// customDispatch() runs the call inline and caches the answer; customPoll()
// returns it.
//   openai | mistral -> POST /v1/chat/completions, Bearer, choices[0].message.content
//   anthropic        -> POST /v1/messages, x-api-key, content[0].text
//
// P9: a base URL of "http://host[:port]/..." uses PLAIN HTTP (default port 80) -
// so a LAN Ollama (http://<mac-ip>:11434/v1, openai wire) works - while
// "https://" / a bare host stays TLS (default 443). An EMPTY key sends no auth
// header (Ollama needs none). LIVE-GATED on device: needs customBase + STA WiFi.
//
// NEW (Stage H): orchTurnCustom - the HEAD on the custom endpoint (single-shot
// chat-completions structured turn), so a fully keyless LAN setup can run the
// whole orchestrator.

namespace agent {
namespace providers {

static const uint32_t CUST_TIMEOUT_MS = 30000;
static const char* kAnthropicVer = "2023-06-01";   // mirrors agent_config.h ANTHROPIC_VER

using wire::exchange;
using wire::serializeBody;
using wire::s;

namespace {
struct CustCache { char id[24]; std::string reply; bool used; };
static CustCache s_cache;
}  // namespace

// Accept "http(s)://host", "host", or "host:port"; strip scheme + any path.
// Returns false when no host could be extracted.
static bool parseBase(const std::string& base, std::string& host, int& port, bool& http) {
  http = base.rfind("http://", 0) == 0;   // plain HTTP only when explicitly http://
  host = base;
  size_t sch = host.find("://"); if (sch != std::string::npos) host = host.substr(sch + 3);
  size_t slash = host.find('/'); if (slash != std::string::npos) host = host.substr(0, slash);
  port = http ? 80 : 443;
  size_t colon = host.find(':');
  if (colon != std::string::npos) {
    port = atoi(host.c_str() + colon + 1);
    host = host.substr(0, colon);
  }
  return !host.empty();
}

// One HTTP(S) exchange with the custom host, body filter-parsed through `filter`.
// The keyless-on-http rule lives here: an empty key sends no auth header, and a
// key is NEVER sent over plain HTTP (the http:// contract is keyless - don't
// leak a bearer token in cleartext on the wire; a key needs TLS).
static int custRequest(const ProviderDeps& pd, const char* method, const std::string& path,
                       std::string body, bool anthropicAuth,
                       JsonDocument& doc, const JsonDocument& filter) {
  doc.clear();
  std::string host; int port; bool http;
  if (!parseBase(s(pd.customBase), host, port, http)) return 0;

  std::string key = s(pd.customKey);
  std::vector<std::pair<std::string, std::string>> headers;
  if (key.length() && !http) {
    if (anthropicAuth) {
      headers.push_back({"x-api-key", key});
      headers.push_back({"anthropic-version", kAnthropicVer});
    } else {
      headers.push_back({"Authorization", "Bearer " + key});
    }
  } else if (key.length() && http) {
    hlog::logf("custom: key set but base is http:// - NOT sending key in cleartext (use https)");
  }
  headers.push_back({"Content-Type", "application/json"});

  // A router with a non-default API base (Cumulo "/router/openai/v1", Z.ai
  // "/api/paas/v4") carries it as customPathPrefix, REPLACING the leading "/v1" of
  // the request path - parseBase above dropped any path from the base URL. So
  // "/v1/chat/completions" becomes "<prefix>/chat/completions".
  const std::string prefix = s(pd.customPathPrefix);
  std::string fullPath = path;
  if (!prefix.empty()) {
    fullPath = (path.rfind("/v1", 0) == 0) ? prefix + path.substr(3) : prefix + path;
  }
  return exchange(pd, host.c_str(), (uint16_t)port, !http, method, fullPath,
                  std::move(headers), std::move(body), CUST_TIMEOUT_MS, doc, filter);
}

FabricErr customDispatch(const ProviderDeps& pd, const Directive& d, char outJobId[72]) {
  // P9: a base URL is required, but an EMPTY key is allowed (a keyless LAN
  // endpoint like Ollama sends no Authorization header). A remote 401/403 still
  // maps to FabricErr::Auth below, so a genuinely-required-but-missing key fails
  // honestly at the wire.
  if (s(pd.customBase).empty()) return FabricErr::Auth;

  std::string conv = s(pd.customConv);
  bool anthropic = (conv == "anthropic");
  std::string model = s(pd.customModel);

  // Build request + response filter per convention.
  std::string body, path;
  JsonDocument filter;
  if (anthropic) {
    path = "/v1/messages";
    JsonDocument doc;
    doc["model"] = model;
    doc["max_tokens"] = 1024;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject u = msgs.add<JsonObject>(); u["role"] = "user"; u["content"] = d.instruction;
    body = serializeBody(doc);
    JsonObject ci = filter["content"].add<JsonObject>(); ci["type"] = true; ci["text"] = true;
    filter["error"]["message"] = true;
  } else {  // openai / mistral chat-completions
    path = "/v1/chat/completions";
    JsonDocument doc;
    doc["model"] = model;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>(); sys["role"] = "system";
    sys["content"] = std::string("You are an autonomous ") + (d.category ? d.category : "ops") +
                     " agent. Complete the task and reply with the final result only.";
    JsonObject u = msgs.add<JsonObject>(); u["role"] = "user"; u["content"] = d.instruction;
    body = serializeBody(doc);
    JsonObject ch = filter["choices"].add<JsonObject>(); ch["message"]["content"] = true;
    filter["error"]["message"] = true;
  }

  JsonDocument doc;
  int code = custRequest(pd, "POST", path, std::move(body), anthropic, doc, filter);
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code <= 0)                  return FabricErr::Network;
  if (code != 200)               { hlog::logf("custom: HTTP %d", code); return FabricErr::RemoteFail; }

  std::string reply = anthropic ? std::string((const char*)(doc["content"][0]["text"] | ""))
                                : std::string((const char*)(doc["choices"][0]["message"]["content"] | ""));
  if (reply.empty()) return FabricErr::ParseFail;

  char id[24]; snprintf(id, sizeof(id), "c%08lx", (unsigned long)(pd.nowMs ? pd.nowMs() : 0));
  strncpy(s_cache.id, id, sizeof(s_cache.id) - 1); s_cache.id[sizeof(s_cache.id) - 1] = 0;
  s_cache.reply = reply; s_cache.used = false;
  snprintf(outJobId, 72, "custom:%s", id);
  hlog::logf("custom: completed %s (%u chars, conv=%s)", id, (unsigned)reply.length(), conv.c_str());
  return FabricErr::Ok;
}

FabricErr customPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env) {
  (void)pd;
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  strncpy(env.jobId,   jobId,    sizeof(env.jobId)   - 1); env.jobId[sizeof(env.jobId)   - 1] = 0;
  strncpy(env.backend, "custom", sizeof(env.backend) - 1); env.backend[sizeof(env.backend) - 1] = 0;
  if (strcmp(s_cache.id, colon + 1) == 0) {
    env.state = JobState::Done;
    // Explicit NUL-terminate: strncpy won't if the provider reply fills the buffer.
    strncpy(env.reply, s_cache.reply.c_str(), sizeof(env.reply) - 1); env.reply[sizeof(env.reply) - 1] = 0;
    s_cache.used = true; s_cache.reply.clear(); s_cache.id[0] = 0;
    return FabricErr::Ok;
  }
  return FabricErr::NotFound;
}

FabricErr customCancel(const ProviderDeps& pd, const char* jobId) {
  (void)pd;
  const char* colon = strchr(jobId, ':');
  if (colon && strcmp(s_cache.id, colon + 1) == 0) { s_cache.id[0] = 0; s_cache.reply.clear(); }
  return FabricErr::Ok;
}

// ---- NEW: the HEAD on the custom endpoint -----------------------------------
// One single-shot structured turn over the OpenAI chat-completions dialect:
// messages = [system(instructions), user(inputs)], response_format json_schema
// (strict) carrying the canonical orch schema. Backends that don't know
// response_format answer 400 - then retry ONCE schema-less with a JSON-only
// nudge; the engine's tolerant parseTurn + salvage absorb dialect wobble.
// Stateless (convId = "chat" marker). v1 deliberately has NO tool loop -
// `tools` is ignored (the engine's loop wiring passes through unused), so a
// small LAN model can't wedge itself in a forced-tool ReAct loop.
bool orchTurnCustom(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  (void)tools;   // v1: single-shot only (see header note)
  outJson.clear(); err.clear();
  if (s(pd.customBase).empty()) { err = "no custom endpoint"; return false; }
  // The head speaks chat-completions only; an anthropic-convention proxy has no
  // /v1/chat/completions, so refuse honestly and let the engine fail over.
  if (s(pd.customConv) == "anthropic") {
    err = "head-custom supports openai/mistral chat-completions only";
    return false;
  }

  JsonDocument filter;
  JsonObject ch = filter["choices"].add<JsonObject>();
  ch["message"]["content"] = true;
  filter["error"]["message"] = true;   // OpenAI-style error envelope
  filter["message"] = true;            // Mistral-style error envelope
  filter["usage"] = true;              // token accounting (when the backend reports it)
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  JsonDocument doc = wire::makeDoc(pd);  // response doc -> PSRAM (retained turn content)
  int code = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool withSchema = (attempt == 0);
    std::string body;
    {
      JsonDocument d;
      d["model"] = s(pd.customModel);
      JsonArray msgs = d["messages"].to<JsonArray>();
      JsonObject sys = msgs.add<JsonObject>();
      sys["role"] = "system";
      sys["content"] = withSchema
          ? instructions
          : instructions + "\nRespond with ONLY the JSON object for your orch_turn "
                           "- no prose, no code fences.";
      JsonObject u = msgs.add<JsonObject>();
      u["role"] = "user"; u["content"] = inputs;
      if (withSchema) {
        JsonObject rf = d["response_format"].to<JsonObject>();
        rf["type"] = "json_schema";
        rf["json_schema"]["name"]   = "orch_turn";
        rf["json_schema"]["strict"] = true;
        JsonDocument sd;
        DeserializationError se = deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
            DeserializationOption::NestingLimit(16));  // schema depth > the default 10
        if (se) { err = "schema parse"; return false; }
        rf["json_schema"]["schema"] = sd;
      }
      body = serializeBody(d);
    }
    code = custRequest(pd, "POST", "/v1/chat/completions", std::move(body),
                       /*anthropicAuth=*/false, doc, filter);
    if (code == 400 && withSchema) {
      // Schema-incapable backend: drop response_format and lean on the nudge +
      // the engine's tolerant parser.
      hlog::logf("orchTurn(custom): 400 with response_format - retrying schema-less");
      continue;
    }
    break;
  }
  if (code <= 0)   { err = "network"; return false; }
  if (code != 200) {
    err = "chat HTTP " + std::to_string(code);
    const char* em = doc["error"]["message"] | "";
    if (!em[0]) em = doc["message"] | "";
    if (em[0]) err += std::string(": ") + em;
    hlog::logf("orchTurn(custom): %s", err.c_str());
    return false;
  }
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());

  outJson = (const char*)(doc["choices"][0]["message"]["content"] | "");
  if (outJson.empty()) { err = "no message content"; return false; }
  convId = "chat";   // stateless host: marker keeps per-host conv bookkeeping happy
  return true;
}

}  // namespace providers
}  // namespace agent
