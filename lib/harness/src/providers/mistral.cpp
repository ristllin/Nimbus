#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "nimbus/harness/output_caps.h"   // max_tokens per round from the output-cap table
#include "nimbus/harness/prompt_layout.h"  // static / summary / per-turn parts
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"  // 429 -> the quota window that refused (CUM-460)
#include "nimbus/orch/connectors_wire.h"  // connector provenance (CUM-536)
#include "nimbus/orch/head_loop.h"    // runHeadLoop - the portable ReAct controller
#include "nimbus/orch/orch_schema.h"  // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/turn.h"         // leadingJsonObject - the shared whole-reply rule
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - per-round usage
#include "nimbus/orch/transcript.h"   // Transcript - the canonical turn record (Stage 2)
#include "nimbus/orch/reply_text.h"
#include "wire.h"

// Mistral - the PORTABLE wire half of the pre-split
// src/agent/adapters/mistral_adapter.cpp (Stage H). The head turn keeps the
// proven Conversations-API shape with the CANONICAL orch schema (single
// source); the sub-session keeps the synchronous chat/completions +
// result-cache pattern. The hand-rolled SSE/chunked reader was deliberately
// never ported - HTTP/1.0 + Connection: close (in the device transport) gives
// an un-chunked body ArduinoJson can filter-read directly.

namespace agent {
namespace providers {

// Every Mistral request is non-streamed: nothing arrives until the model (and any
// connector the Conversations API runs server-side inside the POST) has finished.
// So the deadline is the whole round, and Mistral gets the same 120 s ceiling as
// every other provider (the one time policy, providers.h). It ran at 60 s since
// the first release, while OpenAI and Anthropic moved to 120 s after exactly this
// failure: a slow round read as a dead network, was retried for another 60 s and
// only then failed over. tg_poll is not watchdog-subscribed, so a bounded wait is
// safe at either value.
static const uint32_t MISTRAL_TIMEOUT_MS = kHeadRoundCeilingMs;
static const char* kMistralHost = "api.mistral.ai";   // mirrors agent_config.h MISTRAL_HOST

// Mistral's Conversations API reserves its built-in connector function names;
// a user tool reusing one 422s the whole request. Rename reserved collisions on
// the wire with a "reg_" prefix (reversible - no registry tool is named reg_*),
// and invert before dispatch. Kept tiny + here so the collision list lives next
// to the wire that hits it. Exposed for the wire test. The reserved set IS the
// provider built-in tool set (one list, shared with connector provenance).
static bool mistralReserved(const std::string& n) {
  return nimbus::orch::isProviderBuiltinTool(n);
}
std::string mistralSafeName(const std::string& n) {  // decl in providers.h
  return mistralReserved(n) ? ("reg_" + n) : n;
}
std::string mistralUnsafeName(const std::string& n) {  // decl in providers.h
  if (n.rfind("reg_", 0) == 0 && mistralReserved(n.substr(4))) return n.substr(4);
  return n;
}

using wire::exchange;
using wire::makeDoc;
using wire::serializeBody;

// A Conversations message.output `content` is a plain string when a schema
// (response_format) pins the output, but an ARRAY of chunks for free-text output
// (the sub-agent case, no schema). Extract the TEXT chunks (a code_interpreter
// run mixes in `tool_file` chunks - those are captured separately, see below).
static std::string mistralOutputContent(ArduinoJson::JsonObjectConst o) {
  ArduinoJson::JsonVariantConst c = o["content"];
  if (c.is<const char*>()) return std::string(c.as<const char*>() ? c.as<const char*>() : "");
  std::string out;
  if (c.is<ArduinoJson::JsonArrayConst>())
    for (ArduinoJson::JsonObjectConst chunk : c.as<ArduinoJson::JsonArrayConst>())
      if (strcmp(chunk["type"] | "", "text") == 0) out += (const char*)(chunk["text"] | "");
  return out;
}

// Provider-generated file reference (v4.1 code_interpreter capture). A sub-agent
// that runs the Mistral `code_interpreter` and writes a file emits it as a
// `tool_file` chunk inside a message.output `content` ARRAY - VERIFIED live
// 2026-08-08:
//   {"type":"tool_file","tool":"code_interpreter",
//    "file_id":"<uuid>","file_name":"chart.png","file_type":"png"}
// The `file_id` is a durable Files-API object (GET /v1/files/<id>/content streams
// the bytes with the Mistral bearer - see src/agent/adapters/provider_file_fetch),
// so the download key is the id, not the ~1 h signed `file_url` the tool.execution
// entry also carries. The device streams each file to SD and registers it; the
// bytes NEVER ride env.reply. (⚠ As of 2026-08-08 the Conversations API returns
// HTTP 500 when the emitted artifact is a PDF/CSV - images/txt succeed; this
// capture path is type-agnostic and lands a PDF the instant that is fixed.)
struct MistralFileRef {
  std::string id;    // file_id (download key)
  std::string name;  // file_name (carries the extension)
  std::string type;  // file_type ("pdf" / "png" / ...)
};

// Collect the tool_file references from a Conversations response's outputs[]
// (bounded to the ResultEnvelope's artifact capacity). The dispatch filter keeps
// outputs[].content whole, so the tool_file chunks are retained without a schema.
// Returns how many refs were DROPPED past the cap so the caller can say so -
// silence there read as "covered everything" when it hadn't (prism v4.1 #7).
static int mistralParseFileRefs(const JsonDocument& doc,
                                std::vector<MistralFileRef>& out) {
  int dropped = 0;
  for (ArduinoJson::JsonObjectConst o : doc["outputs"].as<ArduinoJson::JsonArrayConst>()) {
    ArduinoJson::JsonVariantConst c = o["content"];
    if (!c.is<ArduinoJson::JsonArrayConst>()) continue;
    for (ArduinoJson::JsonObjectConst chunk : c.as<ArduinoJson::JsonArrayConst>()) {
      if (strcmp(chunk["type"] | "", "tool_file") != 0) continue;
      const char* id = chunk["file_id"] | "";
      if (!id[0]) continue;
      if ((int)out.size() >= nimbus::orch::kMaxArtifacts) { dropped++; continue; }
      MistralFileRef fr;
      fr.id = id;
      fr.name = chunk["file_name"] | "";
      fr.type = chunk["file_type"] | "";
      out.push_back(std::move(fr));
    }
  }
  return dropped;
}

// One HTTPS exchange with api.mistral.ai (filter-parsed JSON reply).
// `quotaHeaders`, when set, receives the x-ratelimit-* headers of an error response.
static int mistralRequest(const ProviderDeps& pd, const char* method, const std::string& path,
                          std::string body, JsonDocument& doc, const JsonDocument& filter,
                          uint32_t timeoutMs = MISTRAL_TIMEOUT_MS,
                          HeaderList* quotaHeaders = nullptr) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Authorization", "Bearer " + (pd.key ? pd.key("mistral") : std::string())},
      {"Content-Type", "application/json"},
  };
  // Cumulo head: reroute through /router/mistral with the cumulo key (no-op on a
  // direct pd).
  wire::UpstreamReq req{kMistralHost, 443, true, path, std::move(headers)};
  wire::applyRouter(pd, "mistral", req);
  return exchange(pd, req.host.c_str(), req.port, req.tls, method, req.path,
                  std::move(req.headers), std::move(body), timeoutMs, doc, filter, quotaHeaders);
}

// "<what> HTTP <code>[ [rl:<window>]][: <provider message>]". On a 429 the quota
// window that refused rides along as a tag, read from the x-ratelimit-* headers:
// Mistral's body reads the same for a per-minute limit, a spent daily connector
// quota and a plan that allows 0 requests, so only the headers can tell the
// engine's reply which one it was (CUM-460).
static std::string mistralHttpErr(const char* what, int code, const HeaderList& quota,
                                  const std::string& msg) {
  std::string e = std::string(what) + " HTTP " + std::to_string(code);
  const std::string tag = code == 429 ? rateLimitTag(rateLimitFromHeaders(quota)) : std::string();
  if (!tag.empty()) e += " " + tag;
  if (!msg.empty()) e += ": " + wire::oneLine(msg);
  return e;
}

// The provider's error text from EVERY body shape Mistral answers with (all
// recorded live, 18 Oct): {"message":"Invalid model: ..."}; a 422's
// {"message":{"detail":[{"msg":...}]}} (message is an OBJECT); {"detail":"There
// was an error parsing the body"} (detail is a STRING - the invalid-UTF-8 400 that
// logged as a bare "chat HTTP 400"); {"detail":[{"msg":...}]}; and an
// OpenAI-style {"error":{"message":...}}. "" when none carries text.
static std::string mistralDetailText(JsonVariantConst d) {
  if (d.is<const char*>()) return d.as<const char*>();
  if (d.is<JsonArrayConst>()) {
    std::string out;
    for (JsonVariantConst e : d.as<JsonArrayConst>()) {
      const char* m = e["msg"] | "";
      if (*m) out += (out.empty() ? "" : "; ") + std::string(m);
      if (out.size() > 200) break;
    }
    return out;
  }
  return "";
}
static std::string mistralErrText(const JsonDocument& doc) {
  JsonVariantConst msg = doc["message"];
  if (msg.is<const char*>() && *msg.as<const char*>()) return msg.as<const char*>();
  std::string t = mistralDetailText(msg["detail"]);
  if (t.empty()) t = mistralDetailText(doc["detail"]);
  if (t.empty()) t = doc["error"]["message"] | "";
  if (t.empty() && doc["error"].is<const char*>()) t = doc["error"].as<const char*>();
  return t;
}
// The error fields every Mistral filter keeps, whatever their shape.
static void keepMistralError(JsonDocument& filter) {
  filter["message"] = true;
  filter["detail"] = true;
  filter["error"] = true;
  filter["type"] = true;
  filter["code"] = true;
}

// ---- head turn (Conversations API + canonical schema) -----------------------

// Drop every "description" from a schema node: the field docs already ride the
// prompt (ORCH_FIELD_DOCS, from the same macros), so only the shapes are needed.
// (The same reduction the Anthropic wire applies to fit its grammar budget.)
static void mistralStripDescriptions(ArduinoJson::JsonVariant v) {
  if (v.is<ArduinoJson::JsonObject>()) {
    ArduinoJson::JsonObject o = v.as<ArduinoJson::JsonObject>();
    o.remove("description");
    for (ArduinoJson::JsonPair kv : o) mistralStripDescriptions(kv.value());
  } else if (v.is<ArduinoJson::JsonArray>()) {
    for (ArduinoJson::JsonVariant e : v.as<ArduinoJson::JsonArray>()) mistralStripDescriptions(e);
  }
}

// ---- multi-turn tool-use loop (ReAct) ---------------------------------------
// Stage 2 phase 4: the loop runs on STATELESS /v1/chat/completions - messages[]
// is rendered from the device-owned canonical transcript every round (the same
// pattern as the Anthropic and OpenAI loops), so a mid-turn provider switch can
// carry the turn over. The single-shot path (with the Studio built-in
// connectors) stays on /v1/conversations, untouched. The turn's `memory` field
// still carries cross-turn state.

// Chat-completions REQUIRES tool_call ids of exactly 9 alphanumerics. Mistral's
// own ids satisfy it; a FOREIGN id (an Anthropic toolu_… or OpenAI call_… id in
// a transcript carried over by a mid-turn failover) must be normalized - a
// deterministic 9-char digest, applied consistently to the assistant tool_calls
// and their paired tool messages within the render.
std::string mistralCallId(const std::string& id) {
  if (id.size() == 9) {
    bool ok = true;
    for (char c : id)
      if (!isalnum((unsigned char)c)) { ok = false; break; }
    if (ok) return id;
  }
  static const char* kAl = "abcdefghijklmnopqrstuvwxyz0123456789";
  uint32_t h = 2166136261u;                      // FNV-1a
  for (char c : id) { h ^= (uint8_t)c; h *= 16777619u; }
  std::string out(9, 'a');
  for (int i = 0; i < 9; i++) { out[i] = kAl[h % 36]; h /= 36; h = h * 31 + 7; }
  return out;
}

// Render the chat-completions messages[] from the canonical transcript:
// system (per-round instructions), the user seed, then per tool-carrying round
// an assistant message ({content?, tool_calls[]}) answered by role:"tool"
// messages. Prose-only rounds are not replayed. Registry names ride the wire
// through mistralSafeName (the reserved-name rename, inverted on dispatch).
static void renderMistralMessages(const nimbus::orch::Transcript& tr,
                                  ArduinoJson::JsonArray msgs,
                                  const std::string& sysText) {
  namespace orch = nimbus::orch;
  using Kind = orch::TranscriptItem::Kind;
  {
    JsonObject s = msgs.add<JsonObject>();
    s["role"] = "system";
    wire::putFieldStr(s["content"], sysText, "the system prompt");
  }
  const auto& e = tr.entries();
  for (const auto& it : e) {
    if (it.kind != Kind::User) continue;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    wire::putFieldStr(u["content"], it.text, "the user message");
    break;
  }
  size_t i = 0;
  while (i < e.size()) {
    if (e[i].kind == Kind::User) { i++; continue; }
    const int round = e[i].round;
    std::string prose;
    std::vector<size_t> calls, results;
    for (; i < e.size() && e[i].kind != Kind::User && e[i].round == round; i++) {
      switch (e[i].kind) {
        case Kind::AssistantText:
          if (!prose.empty()) prose += "\n";
          prose += e[i].text;
          break;
        case Kind::ToolUse:    calls.push_back(i);   break;
        case Kind::ToolResult: results.push_back(i); break;
        default: break;
      }
    }
    if (calls.empty()) continue;
    JsonObject a = msgs.add<JsonObject>();
    a["role"] = "assistant";
    wire::putModelStr(a["content"], prose, "replayed prose");   // "" is valid alongside tool_calls
    JsonArray tc = a["tool_calls"].to<JsonArray>();
    for (size_t ci : calls) {
      JsonObject t = tc.add<JsonObject>();
      t["id"]   = mistralCallId(e[ci].id);
      t["type"] = "function";
      JsonObject fn = t["function"].to<JsonObject>();
      fn["name"]      = mistralSafeName(e[ci].name);
      wire::putModelStr(fn["arguments"], e[ci].text, e[ci].name.c_str());
    }
    for (size_t ri : results) {
      JsonObject m = msgs.add<JsonObject>();
      m["role"]         = "tool";
      m["tool_call_id"] = mistralCallId(e[ri].id);
      m["name"]         = mistralSafeName(e[ri].name);
      wire::putModelStr(m["content"],
                        e[ri].isError ? (std::string("ERROR: ") + e[ri].text) : e[ri].text,
                        e[ri].name.c_str());
    }
  }
}

// One loop request body. Tools re-sent every round (stateless): orch_turn (the
// terminal) + the registry specs. NO Studio built-ins on loop turns (they forbid
// tool_choice forcing - verified live 2026-07-11); the registry's web.search
// covers it. max_tokens comes from the output-cap table: an unbounded round's
// latency had no ceiling but the socket clock.
struct MisLoopReq {
  const layout::PromptParts* round;   // this round's prompt parts (prompt_layout.h)
  const HeadTools* ht;
  bool allowTools;
  uint32_t maxTokens;
  const char* effort;   // reasoning_effort ("" = the field is not sent)
};

// CUM-616: a model the table says reasons but that refused reasoning_effort (400
// code 3051 "reasoning_effort is not enabled for this model"; Mistral documents a 422
// for a bad value) is remembered for this boot, so later rounds do not pay the
// refusal again. Bounded; RAM only.
namespace {
char g_misNoEffort[4][48] = {};
bool misEffortRefusedBefore(const std::string& model) {
  for (const auto& m : g_misNoEffort)
    if (m[0] && model == m) return true;
  return false;
}
void misRememberNoEffort(const std::string& model) {
  if (misEffortRefusedBefore(model)) return;
  static size_t next = 0;
  snprintf(g_misNoEffort[next++ % 4], sizeof(g_misNoEffort[0]), "%s", model.c_str());
}
// A refusal of the reasoning_effort field itself: retried once without it.
bool misRefusedEffort(int code, const std::string& text) {
  return (code == 400 || code == 422) &&
         (text.find("reasoning_effort") != std::string::npos || text.find("3051") != std::string::npos);
}
}  // namespace
static bool misLoopBody(const ProviderDeps& pd, const nimbus::orch::Transcript& tr, const MisLoopReq& a,
                        std::string& body, std::string& err) {
  JsonDocument req = makeDoc(pd);
  req["model"] = pd.orchModel ? pd.orchModel("mistral") : std::string();
  if (a.maxTokens) req["max_tokens"] = a.maxTokens;
  if (a.effort && *a.effort) req["reasoning_effort"] = a.effort;
  {
    JsonArray msgs = req["messages"].to<JsonArray>();
    // A split prompt (prompt_layout.h): the system message is the static block
    // plus the summary (a prefix Mistral reads from its cache), and the per-turn
    // block leads the user seed.
    const layout::PromptParts& p = *a.round;
    renderMistralMessages(tr, msgs, p.stat + p.summary);
    if (p.split)
      for (JsonObject m : msgs)
        if (!strcmp(m["role"] | "", "user")) {
          wire::putFieldStr(m["content"], layout::userWithDynamic(p, m["content"] | ""), "the user message");
          break;
        }
  }
  JsonArray tools = req["tools"].to<JsonArray>();
  {
    JsonObject ot = tools.add<JsonObject>();
    ot["type"] = "function";
    JsonObject fn = ot["function"].to<JsonObject>();
    fn["name"]        = "orch_turn";
    fn["description"] = "Return your COMPLETE orchestrator turn. This is your FINAL "
                        "answer and ENDS the turn - call it once you have everything.";
    JsonDocument sd = makeDoc(pd);
    if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY, DeserializationOption::NestingLimit(16))) {
      err = "schema parse";
      return false;
    }
    // F6c: shapes only - the field docs ride the prompt, and the 10 KB of
    // descriptions rode every round of every Mistral loop turn.
    mistralStripDescriptions(sd.as<ArduinoJson::JsonVariant>());
    fn["parameters"] = sd;
  }
  for (const auto& sp : a.ht->specs) {
    JsonObject t = tools.add<JsonObject>();
    t["type"] = "function";
    JsonObject fn = t["function"].to<JsonObject>();
    // Reserved-name rename kept on chat-completions for one consistent wire
    // (mistralUnsafeName inverts it before dispatch).
    fn["name"] = mistralSafeName(sp.name);
    fn["description"] = sp.description;
    JsonDocument ssd = makeDoc(pd);
    if (deserializeJson(ssd, sp.schemaJson, DeserializationOption::NestingLimit(16)) ==
        DeserializationError::Ok)
      fn["parameters"] = ssd;
    else
      fn["parameters"].to<JsonObject>();
  }
  if (a.allowTools) {
    // "any": chat-completions' force-a-tool value (the Conversations API's
    // "required" dialect does NOT apply here). The model must call SOME tool
    // every round; orch_turn is the only exit.
    req["tool_choice"] = "any";
  } else {
    // Forced final: chat-completions DOES accept the named-function object
    // (the Conversations API 422'd on it - that limitation was conversations-
    // specific). Pin the terminal directly.
    JsonObject tc = req["tool_choice"].to<JsonObject>();
    tc["type"] = "function";
    tc["function"]["name"] = "orch_turn";
  }
  return wire::serializeChecked(req, body, err);
}

// Read one chat-completions reply into `out`. Returns false when the reply is cut
// (finish_reason "length" / "model_length", or a tool call whose arguments do not
// parse as JSON - a cut argument string must never be dispatched or become the turn).
static bool misReadRound(const JsonDocument& resp, nimbus::orch::HeadStep& out, std::string& cutWhy) {
  JsonObjectConst m = resp["choices"][0]["message"].as<JsonObjectConst>();
  const std::string fr = resp["choices"][0]["finish_reason"] | "";
  if (fr == "length" || fr == "model_length") cutWhy = fr;
  // A reasoning model's content may be a chunk list (thinking, then text).
  const std::string prose = nimbus::orch::chatContentText(m["content"]);
  if (!prose.empty()) out.text = prose;
  for (JsonObjectConst t : m["tool_calls"].as<JsonArrayConst>()) {
    const char* nm = t["function"]["name"] | "";
    // arguments may arrive as a JSON string OR an object - normalize to string.
    std::string args;
    if (t["function"]["arguments"].is<const char*>())
      args = t["function"]["arguments"].as<const char*>();
    else
      serializeJson(t["function"]["arguments"], args);
    JsonDocument probe;
    if (!args.empty() && args != "null" && deserializeJson(probe, args) != DeserializationError::Ok &&
        cutWhy.empty())
      cutWhy = "arguments";
    if (!strcmp(nm, "orch_turn")) {
      // CUM-506: keep scanning - calls batched after the final orch_turn are
      // side effects too (the controller runs them before accepting it).
      if (!out.finished) {
        out.finished  = true;
        out.finalTurn = args;
      }
      continue;
    }
    nimbus::orch::HeadToolCall call;
    call.id       = t["id"] | "";
    call.name     = mistralUnsafeName(nm);   // invert the reserved-name rename
    call.argsJson = (args.length() && args != "null") ? args : "{}";
    out.toolCalls.push_back(call);
  }
  return cutWhy.empty();
}

// STEP FACTORY (Stage 2 phase 5): one Mistral chat-completions model turn
// against the shared canonical transcript. Same lifetime contract as
// antLoopStep - `ht`/`usage`/`tr` by reference, the split `instructions` copied.
nimbus::orch::HeadStepFn misLoopStep(const ProviderDeps& pd,
                                     const std::string& instructions,
                                     const HeadTools& ht,
                                     nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  const size_t kReplayToolBytes = 24576;   // the measured wire-ceiling bound

  // The static, summary and dynamic parts, split once per turn (prompt_layout.h)
  // from the prompt repaired to valid UTF-8 (wire.h: repaired, never clipped).
  std::string sys;
  wire::putFieldStr(sys, instructions, "the system prompt");
  const layout::PromptParts parts = layout::split(sys);

  return [&ht, usage, &tr, pd, kReplayToolBytes, parts](
             bool allowTools,
             const std::vector<orch::HeadToolResult>& prior,
             uint32_t budgetMs,
             const std::string& capReason) -> orch::HeadStep {
    namespace orch = nimbus::orch;
    (void)prior;   // the transcript already carries every result
    orch::HeadStep out;
    // F25 + the one time policy: this round's socket timeout is the turn's
    // remaining budget, at most the 120 s head ceiling.
    const uint32_t roundMs = wire::clampRoundMs(budgetMs, kHeadRoundCeilingMs);
    if (tr.toolBytes() > kReplayToolBytes) tr.trimToolOutputs(kReplayToolBytes);
    const std::string model = pd.orchModel ? pd.orchModel("mistral") : std::string();
    const OutputCap cap = outputCapFor("mistral", model,
                                       allowTools ? RoundKind::ToolRound : RoundKind::FinalRound, roundMs);
    // Final-round notice when a cap forced this tool-less round (wire.h). It
    // rides the per-turn part, so the system message stays a cached prefix.
    layout::PromptParts round = parts;
    std::string& tail = round.split ? round.dyn : round.stat;
    tail = wire::roundInstructions(tail, capReason);

    JsonDocument filter;
    JsonObject ch = filter["choices"].add<JsonObject>();
    ch["finish_reason"] = true;                      // the completeness gate
    JsonObject msg = ch["message"].to<JsonObject>();
    msg["content"] = true;
    JsonObject tcf = msg["tool_calls"].add<JsonObject>();
    tcf["id"] = true;
    tcf["function"]["name"] = true;
    tcf["function"]["arguments"] = true;
    keepMistralError(filter);   // every error-envelope shape (message, detail, error)
    filter["usage"] = true;   // whole usage object -> per-round token accounting
    filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

    bool effortRetried = false;   // the one retry without reasoning_effort (CUM-616)
    for (int attempt = 0; attempt < 2; ++attempt) {
      out = orch::HeadStep();
      const uint32_t maxTokens = attempt ? cap.retryTokens : cap.maxTokens;
      const char* effort = misEffortRefusedBefore(model) ? "" : (attempt ? cap.retryEffort : cap.effort);
      std::string body;
      if (!misLoopBody(pd, tr, MisLoopReq{&round, &ht, allowTools, maxTokens, effort}, body, out.error)) {
        out.ok = false;
        out.kind = orch::StepErr::BadRequest;
        return out;
      }
      JsonDocument resp = makeDoc(pd);
      HeaderList quota;
      const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
      int code = mistralRequest(pd, "POST", "/v1/chat/completions", std::move(body),
                                resp, filter, roundMs, &quota);
      if (code <= 0) { wire::transportFailure(pd, t0, roundMs, out); return out; }
      if (code != 200 && *effort && !effortRetried &&
          misRefusedEffort(code, mistralErrText(resp) + " " + (const char*)(resp["code"] | ""))) {
        hlog::logf("orchTurn(mistral-loop): %s refused reasoning_effort (HTTP %d) - one retry without it",
                   model.c_str(), code);
        misRememberNoEffort(model);
        effortRetried = true;
        --attempt;   // the same attempt again, without the field
        continue;
      }
      if (code != 200) {
        const std::string em = mistralErrText(resp);
        const std::string e = mistralHttpErr("chat", code, quota, em);
        wire::httpFailure(out, code, e,
                          e + " " + (const char*)(resp["type"] | "") + " " + (const char*)(resp["code"] | ""));
        hlog::logf("orchTurn(mistral-loop): %s", e.c_str());
        return out;
      }
      if (usage) *usage += orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
      orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());
      std::string cutWhy;
      if (misReadRound(resp, out, cutWhy)) return out;   // finished, a tool round, or prose (stall)
      if (attempt == 0 && cap.hasRetry()) {
        hlog::logf("orchTurn(mistral-loop): cut (%s) at %u tokens - one retry with %u",
                   cutWhy.c_str(), (unsigned)maxTokens, (unsigned)cap.retryTokens);
        continue;
      }
      wire::cutFailure(out, cutWhy, maxTokens);
      return out;
    }
    return out;   // not reached: the second attempt always returns
  };
}

static bool runMistralLoop(const ProviderDeps& pd, std::string& convId,
                           const std::string& instructions, const std::string& inputs,
                           std::string& outJson, std::string& err, const HeadTools& ht,
                           nimbus::orch::TokenUsage* usage) {
  namespace orch = nimbus::orch;
  // The CANONICAL transcript: the controller records, step() renders. Stateless
  // by construction - no conversation to create or track in loop mode.
  orch::Transcript tr;
  tr.addUser(inputs);
  // CUM-477: one same-host resend of a request that never left (see openai.cpp).
  const orch::HeadStepFn inner = misLoopStep(pd, instructions, ht, usage, tr);
  const ResendCtx resend{&pd, &ht.cfg, &inner, "orchTurn(mistral-loop)"};
  orch::HeadStepFn step = resendUnsentOnce(&resend);

  orch::HeadLoopHooks hooks;
  hooks.step     = step;
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m2) { hlog::logf("%s", m2.c_str()); };
  hooks.onText   = ht.onRoundText;   // round prose -> engine
  hooks.spill    = ht.spill;         // clamped-result spill -> results ring
  hooks.transcript = &tr;            // the controller records; step() renders

  orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("orchTurn(mistral-loop): rounds=%d cap=%s ok=%d heap=%u",
             res.rounds, res.capReason.c_str(), (int)res.ok,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the
  // failure return - a failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) return wire::loopFailed(res, outJson, err);   // CUM-621: a cut keeps its partial turn
  outJson = res.finalTurn;
  convId  = "chat";   // stateless marker - the device owns the turn's state
  return true;
}

// ---- the schema-less head shape (a Studio connector is attached) -------------
// Measured live 2026-09-27 (CUM-460): a Conversations call carrying a Studio
// connector AND the strict orch_turn response_format never answers - the stream is
// closed at the 60 s deadline, 3 of 3, also with short instructions - while the
// same call without the response_format returns in 5.5 s with the connector run
// and its real result. So a head turn that carries a Studio connector sends no
// response_format and ASKS for the orch_turn object in its instructions instead
// (the one place this adapter asks for JSON in prose: the structured mode is
// exactly what hangs), then reads the reply leniently - the orch_turn object when
// the whole reply is one, else the whole text as the reply, the way a sub-agent's
// free text is read. Built-ins alone (web_search, ...) keep the strict schema:
// only the Studio connector + schema pair was measured to hang. This is the
// smaller of the two honest fixes; the other (route Studio connectors to a spawned
// sub-agent, as nimbusd does) also needs the device catalog to say so.

// The instruction that replaces the response_format on a schema-less turn: the
// same canonical schema (single source, ~3.8 KB without descriptions), as text,
// so item shapes (a mem_write needs `content`, a device action its `type`) are
// still known to the model. "" if the schema cannot be read.
static std::string mistralSchemaLessNote() {
  JsonDocument sd;
  if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY, DeserializationOption::NestingLimit(16)))
    return std::string();
  mistralStripDescriptions(sd.as<ArduinoJson::JsonVariant>());
  std::string shapes;
  serializeJson(sd, shapes);   // ArduinoJson 7 REPLACES the target string's content
  return "\n\n[OUTPUT FORMAT] No response format is attached to this turn (connector "
         "tools are live). Answer with ONE JSON object for your orch_turn and nothing "
         "outside it; it must match this JSON schema: " + shapes;
}

// A Studio connector ({type:"connector"}) among the attached tools.
static bool mistralCarriesStudio(const JsonDocument& d) {
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>())
    if (strcmp(t["type"] | "", "connector") == 0) return true;
  return false;
}

// Whether this head turn carries a Studio connector. A NEW conversation attaches
// the connectors into its body (`body`); a CONTINUED one carries the set pinned at
// its creation, which the current attach reproduces (the attach reads only the
// saved connector set, and the device resets the Mistral conversation on every
// connector save), probed into a scratch doc that is never sent.
static bool mistralHeadCarriesStudio(const ProviderDeps& pd, const std::string& convId,
                                     const JsonDocument& body) {
  if (convId.empty()) return mistralCarriesStudio(body);
  if (!pd.attachMistral) return false;
  JsonDocument probe;
  pd.attachMistral(probe);
  return mistralCarriesStudio(probe);
}

static const char kSpace[] = " \t\r\n";

// Whether the reply (first non-space at `b`) opens like a JSON object: a brace, or
// a fence whose first line of content starts with one. A fenced table or code is
// prose.
static bool opensLikeJsonObject(const std::string& text, size_t b) {
  if (text[b] == '{') return true;
  if (text.compare(b, 3, "```") != 0) return false;
  const size_t nl = text.find('\n', b);
  const size_t c = nl == std::string::npos ? nl : text.find_first_not_of(kSpace, nl);
  return c != std::string::npos && text[c] == '{';
}

// `json` as an orch_turn with "memory"/"ask" filled in (no longer required by the
// parser since CUM-483, kept for a stable wire shape), or "" when it does not
// parse to an object with a string reply.
static std::string mistralTurnFields(const ProviderDeps& pd, const std::string& json) {
  JsonDocument d = makeDoc(pd);
  if (deserializeJson(d, json, DeserializationOption::NestingLimit(16))) return "";
  if (!d.is<JsonObject>() || !d["reply"].is<const char*>()) return "";
  for (const char* k : {"memory", "ask"})
    if (!d[k].is<const char*>()) d[k] = "";
  return serializeBody(d);
}

// Lenient read of a schema-less head reply. A reply that IS one JSON object (bare
// or fenced) is the turn. A reply that opens like JSON (a brace or a fence) but is
// not a turn passes through untouched for the engine's salvage path, so raw JSON
// never reaches a person. Anything else is prose and only ever the reply - even
// prose that quotes a JSON object: this turn carries connector content (a mail
// body, an issue), and a payload the model merely quotes must never become device
// actions, memory writes or spawns.
static std::string mistralLenientTurn(const ProviderDeps& pd, const std::string& text) {
  // The WHOLE reply must be exactly one object (bare or one fence) - matched to its
  // closing brace, because the JSON parser stops after the first value: a reply
  // that merely STARTS with a turn and goes on (a quoted example, a second fence)
  // is not executed (shared rule: nimbus::orch::leadingJsonObject, CUM-495).
  bool whole = false;
  const std::string obj = nimbus::orch::leadingJsonObject(text, &whole);
  if (whole) {
    const std::string turn = mistralTurnFields(pd, obj);
    return turn.empty() ? text : turn;
  }
  const size_t b = text.find_first_not_of(kSpace);
  if (b == std::string::npos) return "";   // nothing but whitespace
  if (opensLikeJsonObject(text, b)) return text;   // the engine's salvage path
  const size_t e = text.find_last_not_of(kSpace);
  // Visible in /api/log: this turn's reply is the whole answer - no device action,
  // memory write or spawn rode it.
  hlog::logf("orchTurn(mistral): schema-less reply was prose (%u B), delivered as reply only",
             (unsigned)(e - b + 1));
  JsonDocument d = makeDoc(pd);
  d["reply"]  = text.substr(b, e - b + 1);
  d["memory"] = "";
  d["ask"]    = "";
  return serializeBody(d);
}

// The strict orch_turn response_format: the canonical schema (single source) via
// Mistral's response_format json_schema (Nuage's proven envelope shape).
static bool mistralPinTurnSchema(JsonDocument& d) {
  JsonObject rf = d["completion_args"]["response_format"].to<JsonObject>();
  rf["type"] = "json_schema";
  rf["json_schema"]["name"]   = "orch_turn";
  rf["json_schema"]["strict"] = true;
  JsonDocument sd;
  if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
                      DeserializationOption::NestingLimit(16)))  // schema depth > the default 10
    return false;
  rf["json_schema"]["schema"] = sd;
  return true;
}

// The single-shot head request body. `studio` reports the shape: with a Studio
// connector attached there is no response_format (it is what hangs, above).
struct MistralHeadBody {
  std::string body;
  bool studio = false;
  std::string err;   // set when false is returned
};
static bool mistralHeadBody(const ProviderDeps& pd, const std::string& convId,
                            const std::string& instructions, const std::string& inputs,
                            MistralHeadBody& out) {
  JsonDocument d;
  d["store"]  = true;
  wire::putFieldStr(d["inputs"], inputs, "the user message");
  if (convId.empty()) {                // new conversation: pins model + instructions
    d["model"]        = pd.orchModel ? pd.orchModel("mistral") : std::string();
    wire::putFieldStr(d["instructions"], instructions, "the system prompt");
    // Studio-authenticated built-in connectors (web_search, code_interpreter,
    // ...) pin on conversation creation and run server-side on every turn.
    if (pd.attachMistral) pd.attachMistral(d);
  }
  out.studio = mistralHeadCarriesStudio(pd, convId, d);
  if (out.studio && convId.empty()) {
    const std::string note = mistralSchemaLessNote();
    if (note.empty()) { out.err = "schema parse"; return false; }
    wire::putFieldStr(d["instructions"], instructions + note, "the system prompt");
  }
  if (!out.studio && !mistralPinTurnSchema(d)) { out.err = "schema parse"; return false; }
  return wire::serializeChecked(d, out.body, out.err);
}

bool orchTurnMistral(const ProviderDeps& pd, std::string& convId,
                     const std::string& instructions, const std::string& inputs,
                     std::string& outJson, std::string& err,
                     const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  outJson.clear(); err.clear();
  if (!pd.key || pd.key("mistral").empty()) { err = "no Mistral key"; return false; }

  // Multi-turn tool-use loop (opt-in): falls through to the single-shot strict
  // structured turn below when the knob is off or no tools were supplied.
  if (tools && tools->dispatch && pd.toolLoopOn && pd.toolLoopOn())
    return runMistralLoop(pd, convId, instructions, inputs, outJson, err, *tools, usage);

  MistralHeadBody hb;   // hb.studio: a Studio connector rides this turn (the schema-less shape)
  // The Conversations API takes one instructions string: send the layout flat.
  if (!mistralHeadBody(pd, convId, layout::flat(instructions), inputs, hb)) { err = hb.err; return false; }

  JsonDocument filter;
  filter["conversation_id"] = true;
  JsonObject oi = filter["outputs"].add<JsonObject>();
  oi["type"] = true; oi["content"] = true;
  keepMistralError(filter);            // every error-envelope shape
  filter["usage"] = true;              // whole usage object -> token accounting
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  const std::string path = convId.length()
      ? std::string("/v1/conversations/") + convId
      : std::string("/v1/conversations");
  JsonDocument doc = makeDoc(pd);   // response doc -> PSRAM (retained turn content)
  HeaderList quota;
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  int code = mistralRequest(pd, "POST", path, std::move(hb.body), doc, filter,
                            MISTRAL_TIMEOUT_MS, &quota);
  if (code <= 0)   { err = wire::headTransportError(pd, t0, MISTRAL_TIMEOUT_MS); return false; }
  if (code == 404 && convId.length()) { convId = ""; err = "conversation gone"; return false; }
  if (code != 200) {
    err = mistralHttpErr("conversations", code, quota, mistralErrText(doc));
    hlog::logf("orchTurn(mistral): %s", err.c_str());
    return false;
  }
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());

  const char* cid = doc["conversation_id"] | "";
  if (cid[0]) convId = cid;
  for (JsonObjectConst o : doc["outputs"].as<JsonArrayConst>()) {
    if (strcmp(o["type"] | "", "message.output") != 0) continue;
    outJson = mistralOutputContent(o);   // string OR array-of-text-chunks
  }
  if (hb.studio) outJson = mistralLenientTurn(pd, outJson);   // no schema pinned it
  if (outJson.length() == 0) { err = "no message.output"; return false; }
  return true;
}

// ---- sub-session: Conversations + connectors, streamed, result cache --------
// Sub-agents run over the Conversations API (not chat/completions) so the owner's
// Studio connectors (Notion/Gmail/Drive/GitHub) ride the call and run server-side
// on lab compute - the point of offloading heavy connector work off the device.
// The response streams off the socket (transport execJson), so a fat connector
// write result never resides whole in RAM. Result is cached; poll() serves Done.

// The no-invention sentence is CUM-536: with a calendar connector attached but
// never called, the sub answered "fetch this week's events" with an invented week.
static const char* MISTRAL_AGENT_SYS =
    "You are an autonomous assistant agent. Complete the user's task fully and "
    "reply with the final result only - no preamble. If a tool or connector you "
    "need does not run, fails, or returns nothing, say so plainly; never invent "
    "events, messages, files or other records.";

namespace {
struct Cached {
  char id[24];
  std::string reply;
  std::vector<MistralFileRef> files;   // v4.1: code_interpreter-produced files
  bool used;
  // Connector provenance (CUM-536) and run counts - copied into the ResultEnvelope at poll.
  uint16_t connectorCalls;
  std::string connectorAsked;
  uint16_t toolRuns;
  uint16_t webRuns;
};
static Cached s_cache[6];   // == kAgentMaxJobs; sync jobs vacate in seconds

int freeSlot() {
  for (int i = 0; i < 6; i++) if (!s_cache[i].id[0]) return i;
  return -1;
}
int findSlot(const char* id) {
  for (int i = 0; i < 6; i++) if (!strcmp(s_cache[i].id, id)) return i;
  return -1;
}
// A delivered or cancelled slot is free again; a reused one must not inherit
// stale text, file refs or provenance.
void clearSlot(Cached& c) {
  c.id[0] = 0;
  c.reply.clear();
  c.files.clear();
  c.connectorAsked.clear();
  c.connectorCalls = 0;
  c.toolRuns = 0;
  c.webRuns = 0;
}

// CUM-536 connector provenance, decided from the request before the run: the
// Studio connectors that rode it and the one the bare task (never the chat
// [CONTEXT] tail) or the spawn skill asked for.
struct Provenance {
  std::vector<nimbus::orch::AttachedConnector> attached;   // connectors on the request
  std::string askedId;                  // the wire id the task named ("" = none)
  std::string asked;                    // its display name
};
Provenance provenanceOf(const ProviderDeps& pd, const JsonDocument& req, const Directive& d) {
  Provenance p;
  p.attached = nimbus::orch::mistralAttachedConnectors(req);
  if (p.attached.empty()) return p;
  // The request names a connector only by its wire id, and a custom Studio
  // connector rides by UUID: an identity probe of the same attach (a scratch doc,
  // never sent) adds the device name and type the task would call it by.
  if (pd.attachMistral) {
    JsonDocument probe = makeDoc(pd);
    probe[nimbus::orch::kMistralIdentityProbe] = true;
    pd.attachMistral(probe);
    for (const auto& known : nimbus::orch::mistralAttachedConnectors(probe))
      for (auto& a : p.attached)
        if (a.id == known.id && a.name.empty() && a.type.empty()) {
          a.name = known.name;
          a.type = known.type;
        }
  }
  const char* task = d.task ? d.task : d.instruction;
  p.asked = nimbus::orch::connectorAskedFor(task ? task : "", d.skill ? d.skill : "",
                                            p.attached, &p.askedId);
  return p;
}

// The provider's own run counts for one sub (copied into the ResultEnvelope).
struct RunCounts {
  uint16_t connector = 0;   // runs of the asked connector (CUM-536)
  uint16_t tools = 0;       // every tool.execution entry
  uint16_t web = 0;         // ...that was a web search or fetch
};
bool isWebTool(const char* name) {
  return strstr(name, "web_search") != nullptr || strstr(name, "web_fetch") != nullptr ||
         strstr(name, "news_search") != nullptr;
}
void bump(uint16_t& n) { if (n < 0xFFFF) n++; }

// The reply text of a Conversations response (the last message.output) and the
// run counts from its tool.execution entries.
std::string outputsReply(const JsonDocument& doc, const Provenance& p, RunCounts& runs) {
  std::string reply;
  runs = RunCounts();
  for (JsonObjectConst o : doc["outputs"].as<JsonArrayConst>()) {
    const char* type = o["type"] | "";
    if (strcmp(type, "tool.execution") == 0) {
      const char* nm = o["name"] | "";
      bump(runs.tools);
      if (isWebTool(nm)) bump(runs.web);
      if (nimbus::orch::countsAsConnectorRun(nm, p.askedId, p.attached)) bump(runs.connector);
      continue;
    }
    if (strcmp(type, "message.output") == 0) reply = mistralOutputContent(o);   // string OR chunks
  }
  return reply;
}
}  // namespace

FabricErr mistralDispatch(const ProviderDeps& pd, const std::string& model,
                          const Directive& d, char outJobId[72]) {
  if (!pd.key || pd.key("mistral").empty()) return FabricErr::Auth;
  const int slot = freeSlot();
  if (slot < 0) return FabricErr::RemoteFail;   // cache full: journal backs off

  std::string body;
  Provenance prov;   // CUM-536: decided from the request, applied to the response
  {
    JsonDocument doc;
    doc["store"]        = true;
    wire::putFieldStr(doc["inputs"], d.instruction ? std::string(d.instruction) : std::string(),
                      "the sub-agent task");
    doc["model"]        = (d.model && d.model[0]) ? d.model : model.c_str();
    doc["instructions"] = MISTRAL_AGENT_SYS;
    // Studio connectors ride the conversation and run server-side (auto tool_choice
    // - no forcing, so no 422). Free-text result (no response_format schema).
    if (pd.attachMistral) pd.attachMistral(doc);
    prov = provenanceOf(pd, doc, d);
    body = serializeBody(doc);
  }

  JsonDocument filter;
  filter["conversation_id"] = true;
  JsonObject oi = filter["outputs"].add<JsonObject>();
  // type + content whole: `content` retains BOTH the free-text chunks (the reply)
  // AND the code_interpreter `tool_file` chunks (the file references) - no filter
  // widening beyond this is needed to keep {file_id,file_name,file_type}.
  oi["type"] = true; oi["content"] = true;
  oi["name"] = true;          // tool.execution: which tool ran (connector provenance)
  keepMistralError(filter);   // every error-envelope shape

  JsonDocument doc = makeDoc(pd);   // response -> PSRAM; execJson streams it off the socket
  HeaderList quota;                 // a 429's x-ratelimit-*: which window refused
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  int code = mistralRequest(pd, "POST", "/v1/conversations", std::move(body), doc, filter,
                            kSyncSubCeilingMs, &quota);
  // A Mistral sub runs INSIDE this blocking POST, so a slow sub outlasts the read
  // deadline (the 120 s sync-sub ceiling) and a peer that cuts it after the request
  // went out may have run it: either is "may have started", never the misleading
  // "couldn't start".
  if (code <= 0) return wire::subTransportErr(pd, t0, kSyncSubCeilingMs);
  if (code == 401) return FabricErr::Auth;
  if (code == 429) {
    // Classified exactly like a head turn's 429 (CUM-465): the headers' window,
    // else whatever the provider's text names. A spent Studio connector day reads
    // the same in the body as a per-minute limit; only the headers tell them apart.
    const std::string err = mistralHttpErr("dispatch", code, quota, mistralErrText(doc));
    hlog::logf("mistral %s", err.c_str());
    return wire::rateLimited(d, rateLimitFromError(err));
  }
  if (code != 200) {
    hlog::logf("mistral dispatch HTTP %d: %s", code, mistralErrText(doc).c_str());
    return FabricErr::RemoteFail;
  }
  RunCounts runs;
  std::string reply = outputsReply(doc, prov, runs);
  // Capture any code_interpreter-produced files (poll -> env.artifacts[]).
  std::vector<MistralFileRef> files;
  const int droppedFiles = mistralParseFileRefs(doc, files);
  // A run may produce only a file with no prose; that is still a success.
  if (reply.empty() && files.empty()) return FabricErr::RemoteFail;
  if (droppedFiles > 0)
    reply += "\n[note: the run produced " + std::to_string(droppedFiles) +
             " more file(s) beyond the " + std::to_string(nimbus::orch::kMaxArtifacts) +
             "-file capture limit - they were NOT captured]";

  // Cache the finished result under a fresh local id; poll() serves it as Done.
  snprintf(s_cache[slot].id, sizeof(s_cache[slot].id), "m%lx",
           (unsigned long)(pd.nowMs ? pd.nowMs() : 0));
  s_cache[slot].reply = std::move(reply);
  s_cache[slot].files = std::move(files);
  s_cache[slot].used  = false;
  s_cache[slot].connectorCalls = runs.connector;
  s_cache[slot].connectorAsked = prov.asked;
  s_cache[slot].toolRuns = runs.tools;
  s_cache[slot].webRuns = runs.web;
  if (!prov.attached.empty())
    hlog::logf("mistral sub: connectors attached=%u calls=%u asked=%s",
               (unsigned)prov.attached.size(), (unsigned)runs.connector,
               prov.asked.empty() ? "-" : prov.asked.c_str());
  snprintf(outJobId, 72, "mistral:%s", s_cache[slot].id);
  return FabricErr::Ok;
}

FabricErr mistralPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env) {
  (void)pd;
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  const int slot = findSlot(colon + 1);
  if (slot < 0) return FabricErr::NotFound;   // reboot lost the cache: expired

  env.state = JobState::Done;
  // The cached reply may carry a trailing "[note: ... NOT captured]": the one
  // channel reporting uncaptured files must survive a full buffer, so the reply
  // text gives way before it (cut UTF-8-safe, with the visible 16 KB marker).
  {
    const std::string& r = s_cache[slot].reply;
    const size_t noteAt = r.rfind("\n[note:");
    const bool hasNote = noteAt != std::string::npos && r.size() - noteAt < sizeof(env.reply) / 2;
    wire::ReplyWriter w(env.reply, sizeof(env.reply));
    w.append(r.data(), hasNote ? noteAt : r.size());
    if (hasNote) w.appendTail(r.substr(noteAt));
  }
  // v4.1: hand any captured file references to the JobEngine as artifacts[]. It
  // fetches each (stream to SD + FileStore register) on tg_poll, one TLS at a
  // time - the bytes never touch env.reply. type="file", url=file_id (the
  // /v1/files/<id>/content download key), label=file_name (carries the ext).
  int nf = 0;
  for (const auto& f : s_cache[slot].files) {
    if (nf >= nimbus::orch::kMaxArtifacts) break;
    nimbus::orch::Artifact& a = env.artifacts[nf];
    snprintf(a.type, sizeof(a.type), "file");
    snprintf(a.url, sizeof(a.url), "%s", f.id.c_str());
    snprintf(a.label, sizeof(a.label), "%s", f.name.c_str());
    nf++;
  }
  env.artifactCount = nf;
  env.connectorCalls = s_cache[slot].connectorCalls;
  env.toolRuns = s_cache[slot].toolRuns;
  env.webRuns = s_cache[slot].webRuns;
  snprintf(env.connectorAsked, sizeof(env.connectorAsked), "%s",
           s_cache[slot].connectorAsked.c_str());
  clearSlot(s_cache[slot]);                   // slot freed after delivery
  return FabricErr::Ok;
}

FabricErr mistralCancel(const ProviderDeps& pd, const char* jobId) {
  (void)pd;
  const char* colon = strchr(jobId, ':');
  if (colon) {
    const int slot = findSlot(colon + 1);
    if (slot >= 0) clearSlot(s_cache[slot]);
  }
  return FabricErr::Ok;                       // idempotent, cache-local
}

}  // namespace providers
}  // namespace agent
