#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "nimbus/harness/providers.h"
#include "nimbus/orch/head_loop.h"    // runHeadLoop - the portable ReAct controller
#include "nimbus/orch/orch_schema.h"  // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - per-round usage
#include "nimbus/orch/transcript.h"   // Transcript - the canonical turn record (Stage 2)
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

// The Conversations API runs any attached connector SERVER-SIDE inside the POST,
// so a connector turn (e.g. creating a Notion page) can far exceed a plain
// completion. 60 s keeps the streaming reader from hitting its deadline (and
// truncating the parse) mid-connector-run; tg_poll is not watchdog-subscribed, so
// a bounded 60 s wait is safe.
static const uint32_t MISTRAL_TIMEOUT_MS = 60000;
static const char* kMistralHost = "api.mistral.ai";   // mirrors agent_config.h MISTRAL_HOST

// Mistral's Conversations API reserves its built-in connector function names;
// a user tool reusing one 422s the whole request. Rename reserved collisions on
// the wire with a "reg_" prefix (reversible - no registry tool is named reg_*),
// and invert before dispatch. Kept tiny + here so the collision list lives next
// to the wire that hits it. Exposed for the wire test.
static bool mistralReserved(const std::string& n) {
  return n == "web_search" || n == "code_interpreter" ||
         n == "image_generation" || n == "document_library" ||
         n == "web_search_premium";
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
static int mistralRequest(const ProviderDeps& pd, const char* method, const std::string& path,
                          std::string body, JsonDocument& doc, const JsonDocument& filter,
                          uint32_t timeoutMs = MISTRAL_TIMEOUT_MS) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Authorization", "Bearer " + (pd.key ? pd.key("mistral") : std::string())},
      {"Content-Type", "application/json"},
  };
  return exchange(pd, kMistralHost, 443, true, method, path, std::move(headers),
                  std::move(body), timeoutMs, doc, filter);
}

// ---- head turn (Conversations API + canonical schema) -----------------------

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
    s["content"] = sysText;
  }
  const auto& e = tr.entries();
  for (const auto& it : e) {
    if (it.kind != Kind::User) continue;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    u["content"] = it.text;
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
    a["content"] = prose;   // "" is valid alongside tool_calls
    JsonArray tc = a["tool_calls"].to<JsonArray>();
    for (size_t ci : calls) {
      JsonObject t = tc.add<JsonObject>();
      t["id"]   = mistralCallId(e[ci].id);
      t["type"] = "function";
      JsonObject fn = t["function"].to<JsonObject>();
      fn["name"]      = mistralSafeName(e[ci].name);
      fn["arguments"] = e[ci].text;
    }
    for (size_t ri : results) {
      JsonObject m = msgs.add<JsonObject>();
      m["role"]         = "tool";
      m["tool_call_id"] = mistralCallId(e[ri].id);
      m["name"]         = mistralSafeName(e[ri].name);
      m["content"]      = e[ri].isError ? (std::string("ERROR: ") + e[ri].text)
                                        : e[ri].text;
    }
  }
}

// STEP FACTORY (Stage 2 phase 5): one Mistral chat-completions model turn
// against the shared canonical transcript. Same lifetime contract as
// antLoopStep - `instructions`/`ht`/`usage`/`tr` by reference.
nimbus::orch::HeadStepFn misLoopStep(const ProviderDeps& pd,
                                     const std::string& instructions,
                                     const HeadTools& ht,
                                     nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  const size_t kReplayToolBytes = 24576;   // the measured wire-ceiling bound

  return [&instructions, &ht, usage, &tr, pd, kReplayToolBytes](
             bool allowTools,
             const std::vector<orch::HeadToolResult>& prior,
             uint32_t budgetMs,
             const std::string& capReason) -> orch::HeadStep {
    namespace orch = nimbus::orch;
    (void)prior;   // the transcript already carries every result
    orch::HeadStep out;
    // F25: clamp this round's socket timeout to the turn's remaining budget so
    // slow rounds can't stack past the deadline (no-op when budgetMs==UINT32_MAX).
    const uint32_t kMinRoundMs = 8000;
    const uint32_t roundMs = budgetMs >= MISTRAL_TIMEOUT_MS ? MISTRAL_TIMEOUT_MS
                             : (budgetMs < kMinRoundMs ? kMinRoundMs : budgetMs);
    if (tr.toolBytes() > kReplayToolBytes) tr.trimToolOutputs(kReplayToolBytes);

    JsonDocument req = makeDoc(pd);
    req["model"] = pd.orchModel ? pd.orchModel("mistral") : std::string();
    {
      JsonArray msgs = req["messages"].to<JsonArray>();
      // Final-round notice when a cap forced this tool-less round (wire.h).
      renderMistralMessages(tr, msgs, wire::roundInstructions(instructions, capReason));
    }
    // Tools re-sent every round (stateless): orch_turn (the terminal) + the
    // registry specs. NO Studio built-ins on loop turns (they forbid tool_choice
    // forcing - verified live 2026-07-11); the registry's web.search covers it.
    JsonArray tools = req["tools"].to<JsonArray>();
    {
      JsonObject ot = tools.add<JsonObject>();
      ot["type"] = "function";
      JsonObject fn = ot["function"].to<JsonObject>();
      fn["name"]        = "orch_turn";
      fn["description"] = "Return your COMPLETE orchestrator turn. This is your FINAL "
                          "answer and ENDS the turn - call it once you have everything.";
      JsonDocument sd = makeDoc(pd);
      if (deserializeJson(sd, orch::ORCH_SCHEMA_BODY,
                          DeserializationOption::NestingLimit(16))) {
        out.ok = false; out.error = "schema parse"; return out;
      }
      fn["parameters"] = sd;
    }
    for (const auto& s : ht.specs) {
      JsonObject t = tools.add<JsonObject>();
      t["type"] = "function";
      JsonObject fn = t["function"].to<JsonObject>();
      // Reserved-name rename kept on chat-completions for one consistent wire
      // (mistralUnsafeName inverts it before dispatch).
      fn["name"] = mistralSafeName(s.name);
      fn["description"] = s.description;
      JsonDocument ssd = makeDoc(pd);
      if (deserializeJson(ssd, s.schemaJson,
                          DeserializationOption::NestingLimit(16)) == DeserializationError::Ok)
        fn["parameters"] = ssd;
      else
        fn["parameters"].to<JsonObject>();
    }
    if (allowTools) {
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

    JsonDocument filter;
    JsonObject ch = filter["choices"].add<JsonObject>();
    JsonObject msg = ch["message"].to<JsonObject>();
    msg["content"] = true;
    JsonObject tcf = msg["tool_calls"].add<JsonObject>();
    tcf["id"] = true;
    tcf["function"]["name"] = true;
    tcf["function"]["arguments"] = true;
    filter["message"] = true;                        // error envelope (some errors)
    filter["detail"].add<JsonObject>()["msg"] = true; // 422 validation detail
    filter["usage"] = true;   // whole usage object -> per-round token accounting
    filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

    JsonDocument resp = makeDoc(pd);
    int code = mistralRequest(pd, "POST", "/v1/chat/completions", serializeBody(req),
                              resp, filter, roundMs);
    if (code <= 0) { out.ok = false; out.error = "network"; return out; }
    if (code != 200) {
      std::string e = "chat HTTP " + std::to_string(code);
      const char* em = resp["message"] | "";
      if (!em[0]) em = resp["detail"][0]["msg"] | "";   // 422 validation shape
      if (em[0]) e += std::string(": ") + em;
      out.ok = false; out.error = e;
      hlog::logf("orchTurn(mistral-loop): %s", e.c_str());
      return out;
    }
    if (usage) *usage += nimbus::orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
    nimbus::orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());

    JsonObjectConst m = resp["choices"][0]["message"].as<JsonObjectConst>();
    const char* prose = m["content"] | "";
    if (prose[0]) out.text = prose;
    for (JsonObjectConst t : m["tool_calls"].as<JsonArrayConst>()) {
      const char* nm = t["function"]["name"] | "";
      // arguments may arrive as a JSON string OR an object - normalize to string.
      std::string args;
      if (t["function"]["arguments"].is<const char*>())
        args = t["function"]["arguments"].as<const char*>();
      else
        serializeJson(t["function"]["arguments"], args);
      if (!strcmp(nm, "orch_turn")) {
        out.finished  = true;
        out.finalTurn = args;
        return out;
      }
      orch::HeadToolCall call;
      call.id       = t["id"] | "";
      call.name     = mistralUnsafeName(nm);   // invert the reserved-name rename
      call.argsJson = (args.length() && args != "null") ? args : "{}";
      out.toolCalls.push_back(call);
    }
    return out;   // prose only -> controller stall path
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
  orch::HeadStepFn step = misLoopStep(pd, instructions, ht, usage, tr);

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
  if (!res.ok) { err = res.error.empty() ? std::string("loop failed") : res.error; return false; }
  outJson = res.finalTurn;
  convId  = "chat";   // stateless marker - the device owns the turn's state
  return true;
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

  std::string body;
  {
    JsonDocument d;
    d["store"]  = true;
    d["inputs"] = inputs;
    if (convId.length() == 0) {          // new conversation: pins model + instructions
      d["model"]        = pd.orchModel ? pd.orchModel("mistral") : std::string();
      d["instructions"] = instructions;
      // Studio-authenticated built-in connectors (web_search, code_interpreter,
      // ...) pin on conversation creation and run server-side on every turn.
      if (pd.attachMistral) pd.attachMistral(d);
    }
    // Structured output: the canonical orch_turn schema (single source) via
    // Mistral's response_format json_schema (Nuage's proven envelope shape).
    JsonObject rf = d["completion_args"]["response_format"].to<JsonObject>();
    rf["type"] = "json_schema";
    rf["json_schema"]["name"]   = "orch_turn";
    rf["json_schema"]["strict"] = true;
    {
      JsonDocument sd;
      DeserializationError se = deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
          DeserializationOption::NestingLimit(16));  // schema depth > the default 10
      if (se) { err = "schema parse"; return false; }
      rf["json_schema"]["schema"] = sd;
    }
    body = serializeBody(d);
  }

  JsonDocument filter;
  filter["conversation_id"] = true;
  JsonObject oi = filter["outputs"].add<JsonObject>();
  oi["type"] = true; oi["content"] = true;
  filter["message"] = true;            // error envelope
  filter["usage"] = true;              // whole usage object -> token accounting
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  const std::string path = convId.length()
      ? std::string("/v1/conversations/") + convId
      : std::string("/v1/conversations");
  JsonDocument doc = makeDoc(pd);   // response doc -> PSRAM (retained turn content)
  int code = mistralRequest(pd, "POST", path, std::move(body), doc, filter);
  if (code <= 0)   { err = "network"; return false; }
  if (code == 404 && convId.length()) { convId = ""; err = "conversation gone"; return false; }
  if (code != 200) {
    err = "conversations HTTP " + std::to_string(code);
    const char* em = doc["message"] | "";
    if (em[0]) err += std::string(": ") + em;
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
  if (outJson.length() == 0) { err = "no message.output"; return false; }
  return true;
}

// ---- sub-session: Conversations + connectors, streamed, result cache --------
// Sub-agents run over the Conversations API (not chat/completions) so the owner's
// Studio connectors (Notion/Gmail/Drive/GitHub) ride the call and run server-side
// on lab compute - the point of offloading heavy connector work off the device.
// The response streams off the socket (transport execJson), so a fat connector
// write result never resides whole in RAM. Result is cached; poll() serves Done.

static const char* MISTRAL_AGENT_SYS =
    "You are an autonomous assistant agent. Complete the user's task fully and "
    "reply with the final result only - no preamble.";

namespace {
struct Cached {
  char id[24];
  std::string reply;
  std::vector<MistralFileRef> files;   // v4.1: code_interpreter-produced files
  bool used;
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
}  // namespace

FabricErr mistralDispatch(const ProviderDeps& pd, const std::string& model,
                          const Directive& d, char outJobId[72]) {
  if (!pd.key || pd.key("mistral").empty()) return FabricErr::Auth;
  const int slot = freeSlot();
  if (slot < 0) return FabricErr::RemoteFail;   // cache full: journal backs off

  std::string body;
  {
    JsonDocument doc;
    doc["store"]        = true;
    doc["inputs"]       = d.instruction ? d.instruction : "";
    doc["model"]        = (d.model && d.model[0]) ? d.model : model.c_str();
    doc["instructions"] = MISTRAL_AGENT_SYS;
    // Studio connectors ride the conversation and run server-side (auto tool_choice
    // - no forcing, so no 422). Free-text result (no response_format schema).
    if (pd.attachMistral) pd.attachMistral(doc);
    body = serializeBody(doc);
  }

  JsonDocument filter;
  filter["conversation_id"] = true;
  JsonObject oi = filter["outputs"].add<JsonObject>();
  // type + content whole: `content` retains BOTH the free-text chunks (the reply)
  // AND the code_interpreter `tool_file` chunks (the file references) - no filter
  // widening beyond this is needed to keep {file_id,file_name,file_type}.
  oi["type"] = true; oi["content"] = true;
  filter["message"] = true;   // error envelope

  JsonDocument doc = makeDoc(pd);   // response -> PSRAM; execJson streams it off the socket
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  int code = mistralRequest(pd, "POST", "/v1/conversations", std::move(body), doc, filter);
  if (code <= 0) {
    // A Mistral sub runs INSIDE this blocking POST, so a slow sub outlasts our
    // 60 s read deadline. Distinguish that read TIMEOUT (elapsed ~= the whole
    // deadline - the run started, we just couldn't wait for it) from a genuine
    // connection failure (fails fast): the timeout is reported honestly upstream
    // as "started but ran too long", never the misleading "couldn't start".
    const uint32_t elapsed = (pd.nowMs ? pd.nowMs() : 0) - t0;
    return elapsed >= (MISTRAL_TIMEOUT_MS * 9) / 10 ? FabricErr::Timeout
                                                    : FabricErr::Network;
  }
  if (code == 401) return FabricErr::Auth;
  if (code == 429) return FabricErr::RateLimited;
  if (code != 200) {
    hlog::logf("mistral dispatch HTTP %d: %s", code, (const char*)(doc["message"] | ""));
    return FabricErr::RemoteFail;
  }
  std::string reply;
  for (JsonObjectConst o : doc["outputs"].as<JsonArrayConst>()) {
    if (strcmp(o["type"] | "", "message.output") != 0) continue;
    reply = mistralOutputContent(o);   // string OR array-of-text-chunks
  }
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
  // The cached reply may carry a trailing "[note: ... NOT captured]" that the
  // strncpy would clip away on a full buffer - the one channel reporting
  // uncaptured files must survive, so keep the TAIL when it doesn't fit (prism).
  {
    const std::string& r = s_cache[slot].reply;
    const size_t cap = sizeof(env.reply) - 1;
    if (r.size() <= cap) {
      memcpy(env.reply, r.data(), r.size());
      env.reply[r.size()] = 0;
    } else {
      const size_t noteAt = r.rfind("\n[note:");
      size_t from = r.size() - cap;                    // default: keep the tail
      if (noteAt != std::string::npos && r.size() - noteAt <= cap) {
        // Keep as much head as fits, then the whole note.
        const size_t noteLen = r.size() - noteAt;
        const size_t head = cap - noteLen;
        memcpy(env.reply, r.data(), head);
        memcpy(env.reply + head, r.data() + noteAt, noteLen);
        env.reply[cap] = 0;
        from = std::string::npos;                      // done
      }
      if (from != std::string::npos) {
        memcpy(env.reply, r.data() + from, cap);
        env.reply[cap] = 0;
      }
    }
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
  s_cache[slot].id[0] = 0;                    // slot freed after delivery
  s_cache[slot].reply.clear();
  s_cache[slot].files.clear();
  return FabricErr::Ok;
}

FabricErr mistralCancel(const ProviderDeps& pd, const char* jobId) {
  (void)pd;
  const char* colon = strchr(jobId, ':');
  if (colon) {
    const int slot = findSlot(colon + 1);
    if (slot >= 0) {
      s_cache[slot].id[0] = 0;
      s_cache[slot].reply.clear();
      s_cache[slot].files.clear();   // a reused slot must not inherit stale file refs
    }
  }
  return FabricErr::Ok;                       // idempotent, cache-local
}

}  // namespace providers
}  // namespace agent
