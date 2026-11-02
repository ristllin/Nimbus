#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/compact.h"           // ORCH_COMPACT_PROMPT: the fold asks for prose
#include "nimbus/orch/head_loop.h"         // runHeadLoop - the Z.ai head's tool loop
#include "nimbus/orch/orch_schema.h"       // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - usage extraction
#include "nimbus/orch/seam_meter.h"        // meterSubAnswer - a sub's spend (lane DEF)
#include "nimbus/orch/reply_text.h"
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/turn.h"              // leadingJsonObject
#include "chat_loop.h"
#include "image_parts.h"
#include "loop_common.h"
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
// whole orchestrator. A Z.ai head runs the same deps through the tool loop
// (zaiLoopStep) when the engine passes tools.

namespace agent {
namespace providers {

// Sub-session (customDispatch) exchanges: a LAN model's whole answer, inline.
static const uint32_t CUST_TIMEOUT_MS = 30000;
// HEAD turn exchanges (orchTurnCustom - also the Z.ai and Cumulo-zai heads).
// CUM-495: GLM-5.3 with the ~16 KB head prompt took 37-50 s on the host
// (2026-10-10), so at 30 s every slow-but-working turn died as "no response",
// the engine's same-host retry burned another 30 s, and the turn failed over.
// 120 s matches the OpenAI and Anthropic head ceilings (OAI_TIMEOUT_MS /
// ANT_TIMEOUT_MS); the stuck-turn reaper (loop deadline + 120 s) still bounds it.
// A head with its own budget (pd.customHeadTimeoutMs - the Z.ai heads, CUM-544)
// uses that instead.
static const uint32_t CUST_HEAD_TIMEOUT_MS = kHeadRoundCeilingMs;   // the one time policy
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
                       JsonDocument& doc, const JsonDocument& filter,
                       uint32_t timeoutMs = CUST_TIMEOUT_MS) {
  doc.clear();
  wire::lastExchange() = wire::LastExchange{};   // even when exchange() is never reached
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
                  std::move(headers), std::move(body), timeoutMs, doc, filter);
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
    JsonObject u = msgs.add<JsonObject>(); u["role"] = "user";
    wire::putFieldStr(u["content"], d.instruction ? std::string(d.instruction) : std::string(),
                      "the sub-agent task");
    body = serializeBody(doc);
    JsonObject ci = filter["content"].add<JsonObject>(); ci["type"] = true; ci["text"] = true;
    filter["error"]["message"] = true;
    filter["usage"] = true;   // lane DEF: the run's billed usage (the spend ceiling)
  } else {  // openai / mistral chat-completions
    path = "/v1/chat/completions";
    JsonDocument doc;
    doc["model"] = model;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>(); sys["role"] = "system";
    sys["content"] = std::string("You are an autonomous ") + (d.category ? d.category : "ops") +
                     " agent. Complete the task and reply with the final result only.";
    JsonObject u = msgs.add<JsonObject>(); u["role"] = "user";
    wire::putFieldStr(u["content"], d.instruction ? std::string(d.instruction) : std::string(),
                      "the sub-agent task");
    body = serializeBody(doc);
    JsonObject ch = filter["choices"].add<JsonObject>(); ch["message"]["content"] = true;
    filter["error"]["message"] = true;
    filter["usage"] = true;   // lane DEF: the run's billed usage (the spend ceiling)
  }

  JsonDocument doc;
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  const size_t requestChars = body.size();
  int code = custRequest(pd, "POST", path, std::move(body), anthropic, doc, filter);
  // The sub runs INSIDE this POST: a cut after it went out may have started it.
  if (code <= 0) return wire::subTransportErr(pd, t0, CUST_TIMEOUT_MS);
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code == 429) return wire::rateLimitedByText(d, "custom", doc["error"]["message"] | "");
  if (code != 200)               { hlog::logf("custom: HTTP %d", code); return FabricErr::RemoteFail; }

  std::string reply = anthropic ? nimbus::orch::messagesContentText(doc["content"])
                                : nimbus::orch::chatContentText(doc["choices"][0]["message"]["content"]);
  // Lane DEF: the run billed whatever it returned, so it is metered before any check
  // (a keyless LAN model prices at zero unless the owner set its rates).
  nimbus::orch::meterSubAnswer("custom", model.c_str(),
                               doc["usage"].as<ArduinoJson::JsonVariantConst>(), requestChars,
                               reply.size(), 0);
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
    // Cut UTF-8-safe with the visible 16 KB marker (a raw strncpy split characters).
    wire::ReplyWriter(env.reply, sizeof(env.reply)).append(s_cache.reply);
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

// How a head request asks for the orch_turn shape (CUM-495).
enum class HeadFormat { Schema, Object, None, Prose };

// The chat-completions body for one head attempt. Schema = response_format
// json_schema (strict) carrying the canonical orch schema; Object =
// response_format json_object; None = no response_format. Every format but
// Schema asks for the bare JSON object in words (json_object mode also requires
// the word "JSON" in the messages). False when the schema will not parse or the
// checked serializer refuses the body; `out.err` says which.
struct HeadBody {
  std::string body;
  std::string err;
};
static bool customHeadBody(const ProviderDeps& pd, const std::string& instructions,
                           const std::string& inputs, HeadFormat fmt, HeadBody& out,
                           bool withEffort = true) {
  JsonDocument d;
  d["model"] = s(pd.customModel);
  JsonArray msgs = d["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  wire::putFieldStr(sys["content"],
                    fmt == HeadFormat::Schema || fmt == HeadFormat::Prose
                        ? instructions
                        : instructions + "\nRespond with ONLY the JSON object for your "
                                         "orch_turn - no prose, no code fences.",
                    "the system prompt");
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  wire::putFieldStr(u["content"], inputs, "the user message");
  // CUM-544: a bounded reasoning effort rides EVERY attempt (the 400 retry too:
  // dropping it would put an always-reasoning model back on the slow path).
  const std::string effort = withEffort ? s(pd.customReasoningEffort) : std::string();
  if (!effort.empty()) d["reasoning_effort"] = effort;
  if (fmt == HeadFormat::Object) d["response_format"]["type"] = "json_object";
  if (fmt == HeadFormat::Schema) {
    JsonObject rf = d["response_format"].to<JsonObject>();
    rf["type"] = "json_schema";
    rf["json_schema"]["name"]   = "orch_turn";
    rf["json_schema"]["strict"] = true;
    JsonDocument sd;
    if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
                        DeserializationOption::NestingLimit(16))) {  // schema depth > the default 10
      out.err = "schema parse";
      return false;
    }
    rf["json_schema"]["schema"] = sd;
  }
  return wire::serializeChecked(d, out.body, out.err);
}
// CUM-544: why a head request got no HTTP answer - the shared rule every head
// uses (wire::headTransportError): "timeout: ..." when the request went out and the
// silence used (nearly) the whole budget, so the engine fails over without a
// same-host retry and the owner hears the real reason; else "network" (the retry).
// (Z.ai's ~60 s gateway cut lands past its 50 s budget, so a slow Z.ai turn always
// times out on the device's clock.)
static std::string headTransportError(const ProviderDeps& pd, uint32_t sentAt, uint32_t timeoutMs) {
  return wire::headTransportError(pd, sentAt, timeoutMs);
}

// One head request in its formats (orchTurnCustom): the first format, then once
// with no response_format when the backend refuses it with a 400. `code` is the
// last status (0: no answer). The attempt and the effort choice carry over to a
// resend, so a resend repeats only the request that never left, never the
// answered one before it.
// A 400 that refuses the MODEL (Z.ai "Unknown Model, please check the model
// code.", "Invalid model: ...") is not a response_format problem: retrying without
// the format would only send the same refusal again (lane N's smoke run: four
// Z.ai requests for one wrong model id).
static bool namesUnknownModel(const JsonDocument& doc) {
  std::string m = doc["error"]["message"] | "";
  if (m.empty()) m = doc["message"] | "";
  for (char& c : m) c = (char)tolower((unsigned char)c);
  return m.find("unknown model") != std::string::npos || m.find("invalid model") != std::string::npos ||
         m.find("model not found") != std::string::npos || m.find("does not exist") != std::string::npos;
}

struct CustomHead {
  const ProviderDeps* pd;
  const std::string* instructions;
  const std::string* inputs;
  JsonDocument* doc;
  const JsonDocument* filter;
  HeadFormat first;
  uint32_t timeoutMs;
  int code = 0;
  int attempt = 0;
  bool withEffort = true;
};
static nimbus::orch::HeadStep runCustomHead(CustomHead& h) {
  const ProviderDeps& pd = *h.pd;
  wire::lastExchange() = wire::LastExchange{};   // a step that fails before the wire inherits nothing
  nimbus::orch::HeadStep st;
  uint32_t sentAt = 0;
  for (; h.attempt < 2; ++h.attempt) {
    sentAt = pd.nowMs ? pd.nowMs() : 0;
    const bool withFormat = (h.attempt == 0);
    HeadBody hb;
    // The fold's prose has no format to drop, and a retry asking for JSON would undo it.
    const HeadFormat fmt = withFormat || h.first == HeadFormat::Prose ? h.first : HeadFormat::None;
    if (!customHeadBody(pd, *h.instructions, *h.inputs, fmt, hb, h.withEffort)) {
      st.ok = false;
      st.error = hb.err;
      return st;
    }
    h.code = custRequest(pd, "POST", "/v1/chat/completions", std::move(hb.body),
                         /*anthropicAuth=*/false, *h.doc, *h.filter, h.timeoutMs);
    if (h.code != 400 || !withFormat || namesUnknownModel(*h.doc)) break;
    // Schema-incapable backend: drop response_format and lean on the nudge + the
    // engine's tolerant parser. A model that refuses the effort field names it; the
    // retry then goes without it rather than failing the same way twice (CUM-544).
    hlog::logf("orchTurn(custom): 400 with response_format - retrying schema-less");
    h.withEffort = std::string((*h.doc)["error"]["message"] | "").find("reasoning") == std::string::npos;
  }
  st.ok = h.code > 0;   // any HTTP answer ends the step; its status is read by the caller
  if (!st.ok) st.error = headTransportError(pd, sentAt, h.timeoutMs);
  return st;
}

// ---- the Z.ai head's tool loop -----------------------------------------------
// VN reality check, 1 Nov: GLM "answered" a memory question "Done." because this head
// was single-shot on the device and on nimbusd alike. The real API takes the loop the
// same way as Mistral's (probed 1 Nov, evidence/probes/zai-*): dotted tool names, foreign
// call ids, tool_choice "required". It differs in one place: "any" is a 400, and a named
// tool_choice is ignored by glm-5.3-flash, so the forced final round offers only orch_turn.
static std::string zaiSameName(const std::string& s2) { return s2; }
static const ChatDialect kZaiDialect{zaiSameName, zaiSameName, zaiSameName};

struct ZaiRound {
  const nimbus::orch::Transcript& tr;
  const layout::PromptParts& parts;
  const HeadTools& ht;
  bool allowTools;
  uint32_t maxTokens;
};
static bool zaiLoopBody(const ProviderDeps& pd, const ZaiRound& a, std::string& body, std::string& err) {
  JsonDocument req = wire::makeDoc(pd);
  req["model"] = s(pd.customModel);
  if (a.maxTokens) req["max_tokens"] = a.maxTokens;
  const std::string effort = s(pd.customReasoningEffort);
  if (!effort.empty()) req["reasoning_effort"] = effort;
  JsonArray msgs = req["messages"].to<JsonArray>();
  const layout::PromptParts& p = a.parts;
  const std::string sysText = p.stat + p.shelf;
  renderChatMessages(ChatRender{a.tr, sysText, /*showingImages=*/false, p.split ? &p : nullptr, /*sees=*/false, kZaiDialect},
                     msgs);
  seedChatUserMessage(msgs, p, nullptr);
  if (!putChatTools(pd, req["tools"].to<JsonArray>(), &a.ht, kZaiDialect, /*onlyFinal=*/!a.allowTools, err)) return false;
  if (a.allowTools) {
    req["tool_choice"] = "required";
  } else {
    JsonObject tc = req["tool_choice"].to<JsonObject>();
    tc["type"] = "function";
    tc["function"]["name"] = "orch_turn";
  }
  if (!wire::serializeChecked(req, body, err)) return false;
  return bodyWithinBudget(body, err);
}

// An answer that came back as text instead of an orch_turn call IS the answer: glm-5.3
// does this despite tool_choice "required", and the forced round that followed gave back an
// orch_turn with no reply, having already answered (parity eval, 1 Nov). The whole reply as
// one orch_turn object runs as one, and anything else reaches the owner as prose (no device
// action, memory write or spawn rides it, and the reply's claims are still checked).
static void zaiFinishFromText(nimbus::orch::HeadStep& out) {
  if (out.finished || !out.toolCalls.empty() || out.text.empty()) return;
  bool whole = false;
  const std::string obj = nimbus::orch::leadingJsonObject(out.text, &whole);
  out.finished = true;
  out.prose = !whole;
  out.finalTurn = whole ? obj : out.text;
  hlog::logf("orchTurn(zai-loop): final round answered in text (%u B)", (unsigned)out.text.size());
}

// Z.ai ends a round "sensitive" (its content filter) or "network_error" with no usable
// text: not an answer, so the fabric moves the round on. True when the round failed.
static bool zaiRefusedRound(const JsonDocument& resp, nimbus::orch::HeadStep& out) {
  const std::string fr = resp["choices"][0]["finish_reason"] | "";
  if (fr != "sensitive" && fr != "network_error") return false;
  out = nimbus::orch::HeadStep();
  out.ok = false;
  out.error = "the provider stopped the reply (finish_reason=" + fr + ")";
  out.kind = fr == "sensitive" ? nimbus::orch::StepErr::Rejected : nimbus::orch::StepErr::Transport;
  hlog::logf("orchTurn(zai-loop): %s", out.error.c_str());
  return true;
}

// A 200's bookkeeping and reading. True when the round is settled (an answer, a tool round,
// or a refusal); false when it was cut, with `cutWhy`.
static bool zaiRoundDone(const JsonDocument& resp, nimbus::orch::TokenUsage* usage, nimbus::orch::HeadStep& out,
                         std::string& cutWhy) {
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());
  if (zaiRefusedRound(resp, out)) return true;
  if (!readChatRound(resp, out, cutWhy, kZaiDialect)) return false;
  zaiFinishFromText(out);
  return true;
}

static void zaiHttpFailure(int code, const JsonDocument& resp, nimbus::orch::HeadStep& out) {
  const char* em = resp["error"]["message"] | "";
  if (!em[0]) em = resp["message"] | "";
  const std::string e = wire::httpErrLine("chat", code, em);
  wire::httpFailure(out, code, e, e);
  hlog::logf("orchTurn(zai-loop): %s", e.c_str());
}

nimbus::orch::HeadStepFn zaiLoopStep(const ProviderDeps& pd, const std::string& instructions,
                                     const HeadTools& ht, nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  std::string sys;
  wire::putFieldStr(sys, instructions, "the system prompt");
  const layout::PromptParts parts = layout::split(sys);
  return [&ht, usage, &tr, pd, parts](bool allowTools, const std::vector<orch::HeadToolResult>& prior,
                                      uint32_t budgetMs, const std::string& capReason) -> orch::HeadStep {
    (void)prior;   // the transcript already carries every result
    orch::HeadStep out;
    const size_t kReplayToolBytes = 24576;   // the measured wire-ceiling bound (Mistral's)
    const uint32_t ceiling = pd.customHeadTimeoutMs ? pd.customHeadTimeoutMs : kHeadRoundCeilingMs;
    const uint32_t roundMs = wire::clampRoundMs(budgetMs, ceiling);
    if (tr.toolBytes() > kReplayToolBytes) tr.trimToolOutputs(kReplayToolBytes);
    const OutputCap cap = outputCapFor("zai", s(pd.customModel),
                                       allowTools ? RoundKind::ToolRound : RoundKind::FinalRound, roundMs);
    layout::PromptParts round = parts;
    std::string& tail = round.split ? round.dyn : round.stat;
    tail = wire::roundInstructions(tail, capReason);
    JsonDocument filter;
    chatRoundFilter(filter);
    filter["error"]["message"] = true;
    filter["message"] = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
      out = orch::HeadStep();
      const uint32_t maxTokens = attempt ? cap.retryTokens : cap.maxTokens;
      std::string body;
      if (!zaiLoopBody(pd, ZaiRound{tr, round, ht, allowTools, maxTokens}, body, out.error)) {
        out.ok = false;
        out.kind = orch::StepErr::BadRequest;
        return out;
      }
      JsonDocument resp = wire::makeDoc(pd);
      const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
      const int code = custRequest(pd, "POST", "/v1/chat/completions", std::move(body), /*anthropicAuth=*/false, resp,
                                   filter, roundMs);
      if (code <= 0) { wire::transportFailure(pd, t0, roundMs, out); return out; }
      if (code != 200) { zaiHttpFailure(code, resp, out); return out; }
      std::string cutWhy;
      if (zaiRoundDone(resp, usage, out, cutWhy)) return out;
      if (attempt == 0 && cap.hasRetry()) {
        hlog::logf("orchTurn(zai-loop): cut (%s) at %u tokens - one retry with %u", cutWhy.c_str(),
                   (unsigned)maxTokens, (unsigned)cap.retryTokens);
        continue;
      }
      wire::cutFailure(out, cutWhy, maxTokens);
      return out;
    }
    return out;   // not reached: the second attempt always returns
  };
}

struct ZaiTurn {
  const std::string& instructions;
  const std::string& inputs;
  std::string& outJson;
  std::string& err;
};
static bool runZaiLoop(const ProviderDeps& pd, std::string& convId, const ZaiTurn& t, const HeadTools& ht,
                       nimbus::orch::TokenUsage* usage) {
  namespace orch = nimbus::orch;
  orch::Transcript tr;
  tr.addUser(t.inputs);
  const imageparts::LoopImages photos(tr);   // a photo reads as its transcription: GLM here is blind
  const orch::HeadStepFn inner = zaiLoopStep(pd, t.instructions, ht, usage, tr);
  const ResendCtx resend{&pd, &ht.cfg, &inner, "orchTurn(zai-loop)"};
  orch::HeadLoopHooks hooks;
  hooks.step     = resendUnsentOnce(&resend);
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m) { hlog::logf("%s", m.c_str()); };
  hooks.onText   = ht.onRoundText;
  hooks.spill    = ht.spill;
  hooks.transcript = &tr;
  orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("orchTurn(zai-loop): rounds=%d cap=%s ok=%d heap=%u", res.rounds, res.capReason.c_str(), (int)res.ok,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) return wire::loopFailed(res, t.outJson, t.err);
  t.outJson = res.finalTurn;
  convId  = "chat";
  return true;
}

// One single-shot structured turn over the OpenAI chat-completions dialect:
// messages = [system(instructions), user(inputs)], response_format json_schema
// (strict) carrying the canonical orch schema - or, when pd.customJsonMode() is
// "object", response_format json_object plus the JSON-only nudge (a backend that
// ignores json_schema, like Z.ai GLM). Backends that don't know response_format
// answer 400 - then retry ONCE schema-less with the nudge; the engine's lenient
// parseHeadReply + salvage absorb dialect wobble (prose becomes the reply).
// Stateless (convId = "chat" marker). A LAN head deliberately has NO tool loop -
// `tools` advertises nothing (a small LAN model can't wedge itself in a
// forced-tool ReAct loop); when the engine passes it (the tool loop is on) its
// round floor drives the one resend of a request that never left (CUM-477), the
// same seam every other head's loop runs through. A Z.ai head runs runZaiLoop.
bool orchTurnCustom(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  outJson.clear(); err.clear();
  if (s(pd.customBase).empty()) { err = "no custom endpoint"; return false; }
  // The head speaks chat-completions only; an anthropic-convention proxy has no
  // /v1/chat/completions, so refuse honestly and let the engine fail over.
  if (s(pd.customConv) == "anthropic") {
    err = "head-custom supports openai/mistral chat-completions only";
    return false;
  }
  if (pd.customToolLoop && tools && tools->dispatch && pd.toolLoopOn && pd.toolLoopOn())
    return runZaiLoop(pd, convId, ZaiTurn{instructions, inputs, outJson, err}, *tools, usage);

  JsonDocument filter;
  JsonObject ch = filter["choices"].add<JsonObject>();
  ch["message"]["content"] = true;
  ch["finish_reason"] = true;          // the completeness gate
  filter["error"]["message"] = true;   // OpenAI-style error envelope
  filter["message"] = true;            // Mistral-style error envelope
  filter["usage"] = true;              // token accounting (when the backend reports it)
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  JsonDocument doc = wire::makeDoc(pd);  // response doc -> PSRAM (retained turn content)
  // CUM-890's class: the fold's reply IS the summary, and a format built for an orch_turn
  // lets a model answer with an empty reply field.
  const bool fold = instructions == nimbus::orch::ORCH_COMPACT_PROMPT;
  const HeadFormat first = fold ? HeadFormat::Prose
                                : (pd.customJsonMode && pd.customJsonMode() == "object") ? HeadFormat::Object
                                                                                        : HeadFormat::Schema;
  const uint32_t timeoutMs = pd.customHeadTimeoutMs ? pd.customHeadTimeoutMs : CUST_HEAD_TIMEOUT_MS;
  // One system message: the layout flat (prompt_layout.h). Its order (static,
  // summary, per-turn) still gives a prefix-caching backend a stable prefix.
  const std::string sys = layout::flat(instructions);
  CustomHead h{&pd, &sys, &inputs, &doc, &filter, first, timeoutMs};
  auto run = [&h](bool, const std::vector<nimbus::orch::HeadToolResult>&, uint32_t, const std::string&) {
    return runCustomHead(h);
  };
  // Held by reference: a std::function never allocates for a reference_wrapper, so
  // the turn path allocates nothing for the step.
  const nimbus::orch::HeadStepFn step = std::ref(run);
  nimbus::orch::HeadStep st;
  if (tools) {
    const ResendCtx resend{&pd, &tools->cfg, &step, "orchTurn(custom)"};
    st = resendUnsentOnce(&resend)(false, {}, UINT32_MAX, std::string());
  } else {
    st = step(false, {}, UINT32_MAX, std::string());
  }
  const int code = h.code;
  if (!st.ok) {
    err = st.error;
    if (err != "schema parse")
      hlog::logf("orchTurn(custom): %s (%u s budget)", err.c_str(), (unsigned)(timeoutMs / 1000));
    return false;
  }
  if (code != 200) {
    const char* em = doc["error"]["message"] | "";
    if (!em[0]) em = doc["message"] | "";
    err = wire::httpErrLine("chat", code, em);
    hlog::logf("orchTurn(custom): %s", err.c_str());
    return false;
  }
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());

  // The completeness gate: a reply cut at the output limit is never the turn (a
  // cut JSON object would read as a formatting error, a cut prose reply as whole).
  const char* fr = doc["choices"][0]["finish_reason"] | "";
  if (!strcmp(fr, "length") || !strcmp(fr, "model_length")) {
    err = std::string("cut at the output limit (finish_reason=") + fr + ")";
    hlog::logf("orchTurn(custom): %s", err.c_str());
    return false;
  }
  outJson = nimbus::orch::chatContentText(doc["choices"][0]["message"]["content"]);
  if (outJson.empty()) { err = "no message content"; return false; }
  convId = "chat";   // stateless host: marker keeps per-host conv bookkeeping happy
  return true;
}

// ---- web.search on Z.ai's built-in search --------------------------------------
// `pd` is already pointed at Z.ai. Its index ran months behind and leaned on Chinese-language
// sources (live 1 Nov), so the digest keeps each result's date for the head to weigh.
static std::string zaiSearchDigest(const JsonDocument& doc, int maxResults) {
  std::string out = "Answer: " + nimbus::orch::chatContentText(doc["choices"][0]["message"]["content"]) +
                    "\nSources (check the dates, this index can lag):\n";
  int n = 0;
  for (JsonObjectConst r : doc["web_search"].as<JsonArrayConst>()) {
    if (n++ >= maxResults) break;
    out += "- ";
    out += (const char*)(r["title"] | "(untitled)");
    out += " | ";
    out += (const char*)(r["link"] | "");
    const char* date = r["publish_date"] | "";
    if (date[0]) out += std::string(" | ") + date;
    out += "\n";
  }
  return out;
}

websearch::Result zaiWebSearch(const ProviderDeps& pd, const std::string& query, int maxResults) {
  websearch::Result res;
  res.hosted = true;
  const char* const model = "glm-5.3-flash";
  JsonDocument req;
  req["model"] = model;
  req["max_tokens"] = 800;
  req["reasoning_effort"] = kZaiReasoningEffort;
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = "You are a web research tool. Answer the query from the search results, naming the source and "
                   "date of each fact. Answer in the language of the query. Under 150 words.";
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  wire::putFieldStr(u["content"], query, "the search query");
  JsonObject t = req["tools"].to<JsonArray>().add<JsonObject>();
  t["type"] = "web_search";
  t["web_search"]["enable"] = true;
  t["web_search"]["search_result"] = true;
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  JsonObject wf = filter["web_search"].add<JsonObject>();
  wf["title"] = true;
  wf["link"] = true;
  wf["publish_date"] = true;
  filter["error"]["message"] = true;
  filter["usage"] = true;
  JsonDocument doc = wire::makeDoc(pd);
  std::string body = serializeBody(req);
  const size_t requestChars = body.size();
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  const int code = custRequest(pd, "POST", "/v1/chat/completions", std::move(body), /*anthropicAuth=*/false, doc, filter,
                               kZaiHeadTimeoutMs);
  if (code <= 0) {
    res.err = wire::headTransportError(pd, t0, kZaiHeadTimeoutMs);
    res.billed = wire::lastExchange().noResponse;
    return res;
  }
  if (code != 200) {
    res.err = wire::httpErrLine("search", code, doc["error"]["message"] | "");
    return res;
  }
  res.billed = true;
  const std::string answer = nimbus::orch::chatContentText(doc["choices"][0]["message"]["content"]);
  nimbus::orch::meterSubAnswer("zai", model, doc["usage"].as<ArduinoJson::JsonVariantConst>(), requestChars,
                               answer.size(), 1);
  if (answer.empty()) {
    res.err = "the search came back with no text";
    return res;
  }
  res.ok = true;
  res.digest = zaiSearchDigest(doc, maxResults < 1 ? 5 : maxResults);
  return res;
}

// ---- Z.ai (GLM) wire policy (CUM-544) ---------------------------------------

const char* reasoningEffortFor(const std::string& upstream) {
  return upstream == "zai" ? kZaiReasoningEffort : "";
}

void applyZaiHeadPolicy(ProviderDeps& pd) {
  // CUM-495: Z.ai answers 200 but IGNORES response_format json_schema; json_object
  // + the JSON-only nudge returns valid orch_turns.
  pd.customJsonMode        = [] { return std::string("object"); };
  pd.customReasoningEffort = [] { return std::string(kZaiReasoningEffort); };
  pd.customHeadTimeoutMs   = kZaiHeadTimeoutMs;
  pd.customToolLoop        = true;
}

void configureRouterCompatHead(ProviderDeps& pd, const std::string& upstream,
                               const std::string& model) {
  pd.customBase       = pd.routerBase;
  pd.customPathPrefix = [upstream] { return std::string("/router/") + upstream; };
  pd.customKey        = pd.routerKey;
  pd.customConv       = [] { return std::string("openai"); };
  pd.customModel      = [model] { return model; };
  // Every upstream on this path gets json_object: GLM behind the router ignores
  // json_schema exactly like direct Z.ai, and json_object is the portable mode.
  pd.customJsonMode   = [] { return std::string("object"); };
  if (upstream == "zai") applyZaiHeadPolicy(pd);
}

}  // namespace providers
}  // namespace agent
