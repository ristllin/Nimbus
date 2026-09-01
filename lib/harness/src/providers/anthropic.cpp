#include <ArduinoJson.h>

#include <cstring>
#include <string>
#include <vector>

#include "nimbus/harness/providers.h"
#include "nimbus/mem_cap.h"           // utf8CapLen - the ArduinoJson 64 KB string guard
#include "nimbus/orch/budget.h"       // kBudgetBytesPerToken - the fold trigger scale
#include "nimbus/orch/compact.h"      // modelCtxTokens - the model's window
#include "nimbus/orch/gradient.h"     // foldLine - the shared one-line folder
#include "nimbus/orch/head_loop.h"    // runHeadLoop - the portable ReAct controller
#include "nimbus/orch/orch_schema.h"  // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - per-round usage
#include "nimbus/orch/transcript.h"   // Transcript - the canonical turn record (Stage 2)
#include "wire.h"

// Anthropic - the PORTABLE wire half of the pre-split
// src/agent/adapters/anthropic_adapter.cpp (Stage H). Claude Managed Agents
// sub-sessions (the agent runs server-side in a managed cloud sandbox; the
// device creates the agent+environment once, then per job opens a session,
// sends one user.message, and polls the event list) + the Messages-API head
// turn (forced tool-use). Wire logic moved verbatim into std::string space over
// HttpTransport; socket/TLS mechanics live in the device transport.
//
// LIVE-GATED on device: needs the Anthropic key (web UI / NVS) + STA WiFi. The
// env/agent creation writes the antEnvId/antAgentMap caches (NVS via closures).
// VERIFY(2026-06): beta header (managed-agents-2026-04-01) + /v1/{agents,
// environments,sessions} endpoints + model choice list re-verify at implementation.

namespace agent {
namespace providers {

// 120 s for the same reason as OAI_TIMEOUT_MS (see openai.cpp): v1/messages is
// non-streamed, and a long completion can exceed 30 s before the first byte.
static const uint32_t ANT_TIMEOUT_MS = 120000;
// Mirror agent_config.h (the device stays authoritative for its own surfaces).
static const char* kAnthropicHost = "api.anthropic.com";   // ANTHROPIC_HOST
static const char* kAnthropicVer  = "2023-06-01";          // ANTHROPIC_VER
// Built-in agent toolset (bash, read/write/edit, glob/grep, web_fetch/search).
static const char* ANT_TOOLSET = "agent_toolset_20260401";

using wire::exchange;
using wire::makeDoc;
using wire::serializeBody;
using wire::s;

// HTTPS exchange with api.anthropic.com carrying the Managed-Agents beta headers
// (the pre-split antRequest sent the beta header on EVERY anthropic request,
// head turns included - preserved).
static int antRequest(const ProviderDeps& pd, const char* method, const std::string& path,
                      std::string body, JsonDocument& doc, const JsonDocument& filter,
                      uint32_t timeoutMs = ANT_TIMEOUT_MS) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"x-api-key", pd.key ? pd.key("anthropic") : std::string()},
      {"anthropic-version", kAnthropicVer},
      {"anthropic-beta", "managed-agents-2026-04-01"},
      {"Content-Type", "application/json"},
  };
  return exchange(pd, kAnthropicHost, 443, true, method, path, std::move(headers),
                  std::move(body), timeoutMs, doc, filter);
}

// F25: clamp a per-round socket timeout to the turn's remaining wall-clock budget
// (floored so a near-exhausted final round still gets a real answer). budgetMs ==
// UINT32_MAX (no deadline) leaves the provider default untouched.
static inline uint32_t clampRoundMs(uint32_t budgetMs, uint32_t provDefaultMs) {
  const uint32_t kMinRoundMs = 8000;  // a near-empty final round still gets a shot
  if (budgetMs >= provDefaultMs) return provDefaultMs;
  return budgetMs < kMinRoundMs ? kMinRoundMs : budgetMs;
}

// Create the cloud environment once (network egress for web tools); cache in NVS.
static bool ensureEnv(const ProviderDeps& pd) {
  if (!s(pd.antEnvId).empty()) return true;
  if (!pd.key || pd.key("anthropic").empty()) return false;
  std::string body;
  {
    JsonDocument d;
    d["name"] = "nimbus-env";
    JsonObject cfg = d["config"].to<JsonObject>();
    cfg["type"] = "cloud";
    cfg["networking"]["type"] = "unrestricted";
    body = serializeBody(d);
  }
  JsonDocument filter; filter["id"] = true;
  JsonDocument doc;
  int code = antRequest(pd, "POST", "/v1/environments", std::move(body), doc, filter);
  const char* id = doc["id"] | "";
  if (code != 200 || !id[0]) { hlog::logf("anthropic: create env HTTP %d", code); return false; }
  if (pd.setAntEnvId) pd.setAntEnvId(id);
  hlog::logf("anthropic: env %s", id);
  return true;
}

// Anthropic pins the model on the AGENT (no per-session model field), so each
// model needs its own agent. Return the cached agent id for `model`, creating +
// caching it on first use. Cache = NVS string map "model=agentid;model2=...;".
static std::string agentForModel(const ProviderDeps& pd, const char* model,
                                 const char* defaultModel) {
  if (!model || !*model) model = defaultModel;
  // The model string keys an NVS map ("model=agentid;..."), and it originates from
  // LLM-chosen spawn directives - reject any name containing the map delimiters
  // '=' / ';' (or other unexpected chars) so a crafted name can't corrupt the map.
  for (const char* p = model; *p; ++p) {
    char ch = *p;
    if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='.'||ch=='_')) {
      hlog::logf("anthropic: unsafe model name '%s' -> default", model);
      model = defaultModel; break;
    }
  }
  std::string map = s(pd.antAgentMap);
  std::string key = std::string(model) + "=";
  size_t p = map.find(key);
  if (p != std::string::npos) {
    size_t st = p + key.length();
    size_t e = map.find(';', st); if (e == std::string::npos) e = map.length();
    return map.substr(st, e - st);
  }
  std::string body;
  {
    JsonDocument d;
    d["name"]   = std::string("Nimbus Agent (") + model + ")";
    d["model"]  = model;
    d["system"] = "You are an autonomous assistant agent running on a managed "
                  "sandbox. Use your tools (bash, web search/fetch, files) to "
                  "complete the task fully, then give the final result concisely.";
    JsonArray tools = d["tools"].to<JsonArray>();
    tools.add<JsonObject>()["type"] = ANT_TOOLSET;
    // Owner-configured connectors ride the managed agent as mcp_servers[] - the
    // session created against this agent inherits them (sub-agents only; the head
    // is one forced tool). ⚠ managed-agents mcp_servers shape is Board-1
    // live-verified before ship.
    if (pd.attachAnthropic) pd.attachAnthropic(d);
    body = serializeBody(d);
  }
  JsonDocument filter; filter["id"] = true;
  JsonDocument doc;
  int code = antRequest(pd, "POST", "/v1/agents", std::move(body), doc, filter);
  const char* id = doc["id"] | "";
  if (code != 200 || !id[0]) { hlog::logf("anthropic: create agent(%s) HTTP %d", model, code); return std::string(); }
  map += key + id + ";";
  if (pd.setAntAgentMap) pd.setAntAgentMap(map);
  hlog::logf("anthropic: agent %s for %s", id, model);
  return std::string(id);
}

// Send a user.message turn to an existing session.
static FabricErr sendTurn(const ProviderDeps& pd, const char* sessionId, const char* text) {
  std::string body;
  {
    JsonDocument d;
    JsonArray evs = d["events"].to<JsonArray>();
    JsonObject ev = evs.add<JsonObject>();
    ev["type"] = "user.message";
    JsonObject c = ev["content"].add<JsonObject>();
    c["type"] = "text"; c["text"] = text;
    body = serializeBody(d);
  }
  JsonDocument filter; filter["data"] = true;   // minimal; we don't need the body
  JsonDocument doc;
  std::string path = std::string("/v1/sessions/") + sessionId + "/events";
  int code = antRequest(pd, "POST", path, std::move(body), doc, filter);
  return (code == 200) ? FabricErr::Ok : FabricErr::RemoteFail;
}

FabricErr antDispatch(const ProviderDeps& pd, const char* defaultModel,
                      const Directive& d, char outJobId[72]) {
  if (!pd.key || pd.key("anthropic").empty()) { hlog::log("anthropic: no API key"); return FabricErr::Auth; }
  if (!ensureEnv(pd)) return FabricErr::RemoteFail;
  std::string agentId = agentForModel(pd, d.model, defaultModel);   // per-model agent (cached)
  if (agentId.empty()) return FabricErr::RemoteFail;

  // Create a session bound to the per-model agent + the shared environment.
  std::string body;
  {
    JsonDocument doc;
    doc["agent"]          = agentId;
    doc["environment_id"] = s(pd.antEnvId);
    body = serializeBody(doc);
  }
  JsonDocument filter; filter["id"] = true;
  JsonDocument doc;
  int code = antRequest(pd, "POST", "/v1/sessions", std::move(body), doc, filter);
  if (code == 404) {
    // Cached env/agents are stale - clear so they recreate next dispatch.
    if (pd.setAntEnvId) pd.setAntEnvId("");
    if (pd.setAntAgentMap) pd.setAntAgentMap("");
    return FabricErr::RemoteFail;
  }
  const char* sid = doc["id"] | "";
  if (code != 200 || !sid[0]) { hlog::logf("anthropic: create session HTTP %d", code); return FabricErr::RemoteFail; }

  char session[64];
  strncpy(session, sid, sizeof(session) - 1);
  session[sizeof(session) - 1] = 0;

  FabricErr e = sendTurn(pd, session, d.instruction);
  if (e != FabricErr::Ok) { hlog::log("anthropic: send turn failed"); return e; }

  snprintf(outJobId, 72, "anthropic:%s", session);
  hlog::logf("anthropic: session %s started", session);
  return FabricErr::Ok;
}

FabricErr antPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  const char* sid = colon + 1;
  strncpy(env.jobId,   jobId,       sizeof(env.jobId)   - 1);
  strncpy(env.backend, "anthropic", sizeof(env.backend) - 1);

  // Pull the event list; keep only event type, message text, idle stop_reason,
  // and error message.
  JsonDocument filter;
  JsonObject di = filter["data"].add<JsonObject>();
  di["type"] = true;
  JsonObject ci = di["content"].add<JsonObject>();
  ci["type"] = true; ci["text"] = true;
  di["stop_reason"]["type"] = true;
  di["error"]["message"] = true;

  JsonDocument doc;
  std::string path = std::string("/v1/sessions/") + sid +
                "/events?types[]=agent.message&types[]=session.status_idle&types[]=session.error";
  int code = antRequest(pd, "GET", path, "", doc, filter);
  if (code == 404)                return FabricErr::NotFound;   // session gone/expired
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code <= 0)                  return FabricErr::Network;     // transient
  if (code != 200)               { hlog::logf("anthropic: poll HTTP %d", code); return FabricErr::RemoteFail; }

  bool done = false, errored = false;
  size_t used = 0;
  env.reply[0] = 0;
  for (JsonObject ev : doc["data"].as<JsonArray>()) {
    const char* type = ev["type"] | "";
    if (!strcmp(type, "agent.message")) {
      for (JsonObject c : ev["content"].as<JsonArray>()) {
        if (strcmp(c["type"] | "", "text") != 0) continue;
        const char* t = c["text"] | "";
        size_t len = strlen(t);
        if (used + len >= sizeof(env.reply)) len = sizeof(env.reply) - 1 - used;
        memcpy(env.reply + used, t, len);
        used += len; env.reply[used] = 0;
        if (used >= sizeof(env.reply) - 1) break;
      }
    } else if (!strcmp(type, "session.status_idle")) {
      if (!strcmp(ev["stop_reason"]["type"] | "", "end_turn")) done = true;
    } else if (!strcmp(type, "session.error")) {
      errored = true;
      strncpy(env.error, ev["error"]["message"] | "agent error", sizeof(env.error) - 1);
    }
  }

  if (errored)    env.state = JobState::Error;
  else if (done)  { env.state = JobState::Done; if (!env.reply[0]) strncpy(env.reply, "(done)", sizeof(env.reply)-1); }
  else            env.state = JobState::Running;
  return FabricErr::Ok;
}

FabricErr antAnswer(const ProviderDeps& pd, const char* jobId, const char* userText) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  return sendTurn(pd, colon + 1, userText);
}

FabricErr antCancel(const ProviderDeps& pd, const char* jobId) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  // Interrupt the in-flight run (DELETE alone won't halt a running session).
  std::string body = "{\"events\":[{\"type\":\"user.interrupt\"}]}";
  JsonDocument filter; filter["data"] = true;
  JsonDocument doc;
  std::string path = std::string("/v1/sessions/") + (colon + 1) + "/events";
  int code = antRequest(pd, "POST", path, std::move(body), doc, filter);
  return (code == 200) ? FabricErr::Ok : FabricErr::RemoteFail;
}

// Recursively drop "description" keys from a schema node. Anthropic's strict
// tool-use compiles the schema into a grammar with a hard size budget, and the
// single-source field descriptions push ours past it ("compiled grammar is too
// large"). Descriptions are semantically inert for ENFORCEMENT - and the model
// still reads them via ORCH_FIELD_DOCS in the system prompt (generated from
// the same macros), so nothing is lost and the source stays single.
static void stripDescriptions(ArduinoJson::JsonVariant v) {
  if (v.is<ArduinoJson::JsonObject>()) {
    ArduinoJson::JsonObject o = v.as<ArduinoJson::JsonObject>();
    o.remove("description");
    for (ArduinoJson::JsonPair kv : o) stripDescriptions(kv.value());
  } else if (v.is<ArduinoJson::JsonArray>()) {
    for (ArduinoJson::JsonVariant e : v.as<ArduinoJson::JsonArray>()) stripDescriptions(e);
  }
}

// ---- multi-turn tool-use loop (ReAct) ---------------------------------------
// The head model calls registry tools (memory.*/session.*/web.search) mid-turn,
// sees each result, and iterates, terminating when it emits the orch_turn tool.
// Anthropic Messages is STATELESS, so we replay a growing messages[] each round -
// routed to the deps allocator (device: PSRAM) so it doesn't eat internal SRAM.
// The portable runHeadLoop owns all the bounds (rounds/deadline/heap/bytes) +
// fail-soft; this closure is just "build request -> send -> parse -> accumulate"
// per round.
// Gradient fold of the replayed conversation (Context Fabric Stage 1, site 1 of
// the compaction research: keep the LATEST rounds verbatim, fold older ones to
// one line - never dumb-trim). Anthropic is the ONLY provider whose request body
// grows round over round (OpenAI/Mistral keep state server-side), so this is
// where the in-turn gradient belongs.
//
// Structure of msgs: [0] = the seeded user turn (PINNED), then repeating PAIRS
// of (assistant with tool_use blocks, user with tool_result blocks). A pair is
// folded WHOLE into one user text line, which is what keeps the API's pairing
// invariant intact (an unanswered tool_use 400s).
//
// ⚠ Stage 2 replaces this JSON-level walk with gradientTrim() over the canonical
// transcript; foldLine() is already the shared line renderer, so the WORDING
// stays identical across that refactor.
static bool antFoldOldRounds(const ProviderDeps& pd, JsonDocument& conv, JsonArray& msgs,
                             size_t triggerBytes, int keepRounds) {
  if (msgs.size() < 4) return false;   // nothing older than the newest pair
  size_t approx = 0;
  for (JsonObject m : msgs) approx += measureJson(m);
  if (triggerBytes && approx <= triggerBytes) return false;

  // Find pairs STRUCTURALLY (assistant-with-tool_use immediately answered by a
  // user-with-tool_result), never by index parity: a previous fold replaces two
  // messages with ONE, so parity arithmetic silently mis-pairs after the first
  // fold. Already-folded lines are plain user text and are simply carried over.
  auto isToolUseAsst = [](JsonObjectConst m) {
    if (strcmp(m["role"] | "", "assistant")) return false;
    for (JsonObjectConst b : m["content"].as<JsonArrayConst>())
      if (!strcmp(b["type"] | "", "tool_use")) return true;
    return false;
  };
  auto isToolResultUser = [](JsonObjectConst m) {
    if (strcmp(m["role"] | "", "user")) return false;
    for (JsonObjectConst b : m["content"].as<JsonArrayConst>())
      if (!strcmp(b["type"] | "", "tool_result")) return true;
    return false;
  };

  std::vector<size_t> pairStart;
  for (size_t i = 1; i + 1 < msgs.size(); i++)
    if (isToolUseAsst(msgs[i]) && isToolResultUser(msgs[i + 1])) { pairStart.push_back(i); i++; }
  const int foldPairs = (int)pairStart.size() - keepRounds;
  if (foldPairs <= 0) return false;

  JsonDocument next = makeDoc(pd);
  JsonArray out = next.to<JsonArray>();
  out.add(msgs[0]);   // the pinned user turn, verbatim

  size_t pi = 0;      // next pair index
  int folded = 0;     // how many pairs folded THIS pass
  // Label continues from the folds already present, so a second pass does not
  // emit another "[earlier round 0]" (prism 2026-08-05: duplicate labels are
  // ambiguous to the model).
  int labelBase = 0;
  for (JsonObjectConst m : msgs)
    if (!strcmp(m["role"] | "", "user") && m["content"].is<const char*>() &&
        std::string(m["content"] | "").rfind("[earlier round ", 0) == 0)
      labelBase++;
  for (size_t i = 1; i < msgs.size();) {
    const bool startsPair = pi < pairStart.size() && pairStart[pi] == i;
    if (!startsPair) { out.add(msgs[i]); i++; continue; }
    pi++;
    if (folded >= foldPairs) {   // newest keepRounds pairs: verbatim
      out.add(msgs[i]);
      out.add(msgs[i + 1]);
      i += 2;
      continue;
    }
    // Fold: tool name(s) from the assistant turn, gist from its results.
    std::string names, gist;
    for (JsonObjectConst b : msgs[i]["content"].as<JsonArrayConst>()) {
      if (strcmp(b["type"] | "", "tool_use")) continue;
      if (!names.empty()) names += ",";
      names += (b["name"] | "");
    }
    size_t gistBytes = 0;
    for (JsonObjectConst b : msgs[i + 1]["content"].as<JsonArrayConst>()) {
      if (strcmp(b["type"] | "", "tool_result")) continue;
      const char* c = b["content"] | "";
      gistBytes += strlen(c);
      if (gist.size() < 200) gist += (gist.empty() ? "" : " | ") + std::string(c);
    }
    JsonObject u = out.add<JsonObject>();
    u["role"] = "user";
    u["content"] =
        nimbus::orch::foldLine("[earlier round " + std::to_string(labelBase + folded) + "] " + names, gist,
                               160) +
        " (" + std::to_string((unsigned)gistBytes) + " B)";
    folded++;
    i += 2;
  }

  conv = next;                 // replace the document…
  msgs = conv.as<JsonArray>(); // …and re-seat the caller's handle
  return true;
}

// Render the Anthropic messages[] array from the CANONICAL transcript (Context
// Fabric Stage 2 phase 2). The transcript is the device-owned record the
// controller fills (user seed + per-round prose/calls/results); each round's
// request body is re-rendered from it, replacing the old private msgs[]
// accumulator. Wire rules preserved exactly:
//   - the seeded user turn is msgs[0], string content;
//   - an assistant message appears ONLY for rounds that carried tool_use (the
//     prefill rule - a stalled text-only round is never echoed, because a
//     trailing assistant message + forced tool_choice 400s);
//   - every tool_use is answered by a tool_result in the following user message
//     (the API validates the pairing);
//   - tool_result content is clipped UTF-8-safe under ArduinoJson's 65535-byte
//     string ceiling (a longer string stores as NULL and 400s the request).
static void renderAntMessages(const nimbus::orch::Transcript& tr, ArduinoJson::JsonArray msgs) {
  namespace orch = nimbus::orch;
  using Kind = orch::TranscriptItem::Kind;
  const auto& e = tr.entries();
  // The seeded user turn (pinned; always first).
  for (const auto& it : e) {
    if (it.kind != Kind::User) continue;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    u["content"] = it.text;
    break;
  }
  // Walk rounds in order. Entries are appended in round order by construction.
  int round = -1;
  size_t i = 0;
  while (i < e.size()) {
    if (e[i].kind == Kind::User) { i++; continue; }
    round = e[i].round;
    // Collect this round's prose / calls / results.
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
    if (calls.empty()) continue;   // prefill rule: prose-only rounds are not echoed
    JsonObject a = msgs.add<JsonObject>();
    a["role"] = "assistant";
    JsonArray ac = a["content"].to<JsonArray>();
    if (!prose.empty()) {
      JsonObject t = ac.add<JsonObject>();
      t["type"] = "text";
      t["text"] = prose;
    }
    for (size_t ci : calls) {
      JsonObject tu = ac.add<JsonObject>();
      tu["type"] = "tool_use";
      tu["id"]   = e[ci].id;
      tu["name"] = e[ci].name;
      JsonDocument args;
      if (deserializeJson(args, e[ci].text) == DeserializationError::Ok)
        tu["input"] = args;
      else
        tu["input"].to<JsonObject>();
    }
    if (!results.empty()) {
      JsonObject u = msgs.add<JsonObject>();
      u["role"] = "user";
      JsonArray uc = u["content"].to<JsonArray>();
      for (size_t ri : results) {
        JsonObject trr = uc.add<JsonObject>();
        trr["type"]        = "tool_result";
        trr["tool_use_id"] = e[ri].id;
        const std::string& outp = e[ri].text;
        if (outp.size() > 65500) {
          int keep = nimbus::utf8CapLen(outp.c_str(), (int)outp.size(), 65500);
          trr["content"] = outp.substr(0, (size_t)keep) + "\xE2\x80\xA6";
        } else {
          trr["content"] = outp;
        }
        if (e[ri].isError) trr["is_error"] = true;
      }
    }
  }
}

// STEP FACTORY (Stage 2 phase 5): one Anthropic model turn against the shared
// canonical transcript. ⚠ Lifetime contract: `instructions`, `ht`, `usage` and
// `tr` are captured BY REFERENCE - the caller's frame must outlive every call of
// the returned step (true for runAntLoop and for the engine's fabric loop, both
// of which hold them across runHeadLoop). `pd` is copied.
nimbus::orch::HeadStepFn antLoopStep(const ProviderDeps& pd,
                                     const std::string& instructions,
                                     const HeadTools& ht,
                                     nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  const std::string model = pd.orchModel ? pd.orchModel("anthropic") : std::string();
  // Fold trigger - NOT the token window. The old window/4 (=200 KB) never fired:
  // the request body breaks LONG before that, at the DEVICE's contiguous-heap
  // ceiling for the TLS send. MEASURED on Board 1 (2026-08-05): a 6-way fan-out
  // synthesis grew its replay to body=65672 over 12 rounds and failed at
  // intLargest~19 K ("no response"→network), while bodies <=52 KB sent fine. So
  // the transcript must fold well under that - keep the replayed history bounded
  // so system + transcript + tools stays in the ~48 K working range regardless of
  // how many rounds (or sub-agents) a turn involves.
  const size_t foldTriggerBytes = 24576;

  return [&instructions, &ht, usage, &tr, pd, model, foldTriggerBytes](
             bool allowTools,
             const std::vector<orch::HeadToolResult>& prior,
             uint32_t budgetMs,
             const std::string& capReason) -> orch::HeadStep {
    namespace orch = nimbus::orch;
    (void)prior;   // the transcript already carries every result (recorded by the
                   // controller after dispatch, before this step runs)
    orch::HeadStep out;

    // 1) render messages[] from the canonical transcript (fresh doc per round -
    //    no cross-round accumulator to leak or drift).
    JsonDocument conv = makeDoc(pd);
    JsonArray msgs = conv.to<JsonArray>();
    renderAntMessages(tr, msgs);
    if (!allowTools && msgs.size() > 0 &&
        !strcmp(msgs[msgs.size() - 1]["role"] | "", "assistant")) {
      // Forced-final insurance: a trailing assistant message + forced tool_choice
      // is rejected by the API. The renderer always answers tool_use rounds with
      // their results, so this shouldn't trigger - but if it ever does, close the
      // turn with a user nudge so the request stays valid.
      JsonObject u = msgs.add<JsonObject>();
      u["role"] = "user";
      u["content"] = "Now call orch_turn with your final complete turn.";
    }

    // 1b) gradient fold: once the rendered conversation passes the trigger,
    //     collapse everything older than the newest round to one line each.
    //     Below the trigger this is a no-op and the body stays byte-identical
    //     to the pre-Context-Fabric wire (pinned by the wire tests). Re-rendering
    //     + re-folding each round is deterministic on the same transcript state,
    //     so the folded wire is stable across rounds.
    if (antFoldOldRounds(pd, conv, msgs, foldTriggerBytes, /*keepRounds=*/1))
      hlog::logf("orchTurn(ant-loop): gradient fold -> %u msgs", (unsigned)msgs.size());

    // 2) build the request (deps allocator). orch_turn is always offered (the
    //    terminal); the registry tools ride alongside only on tool-allowed rounds.
    //    The forced final round gets extra output headroom - its orch_turn must
    //    carry the FULL turn (incl. the cross-turn memory field) after the loop
    //    consumed context.
    JsonDocument req = makeDoc(pd);
    req["model"]      = model;
    req["max_tokens"] = allowTools ? 3000 : 4096;
    // roundInstructions() appends the final-round notice when a cap forced this
    // tool-less round, so the model reports what it could not finish rather than
    // promising work that will never run.
    // Prompt caching (v4.1.1): system rides as a content-block array so the
    // block can carry cache_control. Inside a tool loop the rounds are seconds
    // apart, so the 5-minute ephemeral cache hits on every round after the
    // first - the ~20 KB system + tools prefix bills at ~0.1x instead of full
    // price. ⚠ roundInstructions APPENDS a final-round notice when a cap forces
    // the last round - that round misses the SYSTEM breakpoint (different text)
    // but still hits the TOOLS one below. Below the provider's 1024-token
    // minimum the marker is ignored harmlessly.
    {
      JsonObject sysBlock = req["system"].add<JsonObject>();
      sysBlock["type"] = "text";
      sysBlock["text"] = wire::roundInstructions(instructions, capReason);
      sysBlock["cache_control"]["type"] = "ephemeral";
    }
    req["messages"]   = msgs;   // deep-copy within the doc's allocator
    JsonArray toolsArr = req["tools"].to<JsonArray>();
    {
      JsonObject ot = toolsArr.add<JsonObject>();
      ot["name"]        = "orch_turn";
      ot["description"] = "Return your COMPLETE orchestrator turn. This is your FINAL "
                          "answer and ENDS the turn - call it once you have everything.";
      JsonDocument sd = makeDoc(pd);
      if (deserializeJson(sd, orch::ORCH_SCHEMA_BODY,
                          DeserializationOption::NestingLimit(16))) {
        out.ok = false; out.error = "schema parse"; return out;
      }
      stripDescriptions(sd.as<ArduinoJson::JsonVariant>());
      ot["input_schema"] = sd;
    }
    if (allowTools) {
      for (const auto& s : ht.specs) {
        JsonObject t = toolsArr.add<JsonObject>();
        t["name"]        = s.name;
        t["description"] = s.description;
        JsonDocument ssd = makeDoc(pd);
        if (deserializeJson(ssd, s.schemaJson,
                            DeserializationOption::NestingLimit(16)) == DeserializationError::Ok)
          t["input_schema"] = ssd;
        else
          t["input_schema"].to<JsonObject>();
      }
      req["tool_choice"]["type"] = "auto";        // may call a tool OR finish (orch_turn)
    } else {
      req["tool_choice"]["type"] = "tool";        // forced final round: answer now
      req["tool_choice"]["name"] = "orch_turn";
    }
    // Cache breakpoint on the LAST tool: caches the entire tools array as one
    // prefix segment (Anthropic caches tools -> system -> messages in that
    // canonical order regardless of JSON field order).
    if (toolsArr.size() > 0)
      toolsArr[toolsArr.size() - 1]["cache_control"]["type"] = "ephemeral";

    // 3) send - the body is serialized ONCE into a contiguous buffer and handed
    //    to the transport whole (single TLS write; see wire::serializeBody) -
    //    and filter the response to the assistant content blocks we echo + read.
    JsonDocument filter;
    JsonObject ci = filter["content"].add<JsonObject>();
    ci["type"] = true; ci["id"] = true; ci["name"] = true; ci["input"] = true; ci["text"] = true;
    filter["stop_reason"] = true;
    filter["error"]["message"] = true;
    filter["usage"]["input_tokens"]  = true;   // per-round token usage (summed below)
    filter["usage"]["output_tokens"] = true;
    filter["usage"]["cache_read_input_tokens"]     = true;   // prompt-cache hits
    filter["usage"]["cache_creation_input_tokens"] = true;   // prompt-cache writes
    filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

    JsonDocument resp = makeDoc(pd);
    int code = antRequest(pd, "POST", "/v1/messages", serializeBody(req), resp, filter,
                          clampRoundMs(budgetMs, ANT_TIMEOUT_MS));
    if (code <= 0) { out.ok = false; out.error = "network"; return out; }
    if (code != 200) {
      std::string e = "messages HTTP " + std::to_string(code);
      const char* em = resp["error"]["message"] | "";
      if (em[0]) e += std::string(": ") + em;
      out.ok = false; out.error = e; hlog::logf("orchTurn(ant-loop): %s", e.c_str());
      return out;
    }
    if (usage) *usage += orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
    orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());

    JsonArrayConst content = resp["content"].as<JsonArrayConst>();
    if (content.isNull() || content.size() == 0) {
      out.ok = false;
      out.error = std::string("empty content (stop_reason=") +
                  (const char*)(resp["stop_reason"] | "?") + ")";
      return out;
    }
    // Extract tool calls; orch_turn is terminal (its input IS the turn).
    // Text blocks are COLLECTED into HeadStep.text (Glass Box A4 - they were
    // skipped on the floor; the content filter already requests them). Observed
    // only: they are still never echoed into msgs beyond the verbatim
    // tool-round echo below (the prefill-conflict rule stands).
    for (JsonObjectConst c : content) {
      const char* ty = c["type"] | "";
      if (strcmp(ty, "text") == 0) {
        const char* txt = c["text"] | "";
        if (txt[0]) {
          if (!out.text.empty()) out.text += "\n";
          out.text += txt;
        }
        continue;
      }
      if (strcmp(ty, "tool_use") != 0) continue;
      const char* nm = c["name"] | "";
      std::string args; serializeJson(c["input"], args);
      if (!strcmp(nm, "orch_turn")) {
        out.finished  = true;
        out.finalTurn = args;
        return out;   // terminal - no echo needed, the loop ends here
      }
      orch::HeadToolCall call;
      call.id       = c["id"] | "";
      call.name     = nm;
      call.argsJson = (args.length() && args != "null") ? args : "{}";
      out.toolCalls.push_back(call);
    }
    if (out.toolCalls.empty()) {
      // Text-only round. Distinguish a real stall from max_tokens truncation (the
      // single-shot path surfaces this too) - a truncated round must NOT be echoed
      // or "recovered": fail with the actual cause so it's debuggable from logs.
      const char* sr = resp["stop_reason"] | "";
      if (!strcmp(sr, "max_tokens")) {
        out.ok = false;
        out.error = "truncated at max_tokens before a tool call";
        hlog::logf("orchTurn(ant-loop): stop_reason=max_tokens, round truncated");
        return out;
      }
      // Genuine stall (prose, no tool call). Do NOT echo the assistant message:
      // the controller will force a tool-less final round, and a trailing assistant
      // message + forced tool_choice is rejected by the API (prefill conflict).
      // Dropping the prose loses nothing load-bearing - the model re-answers on the
      // forced round from the same context.
      return out;
    }
    // Continuing with tool dispatches. No echo here: the controller records this
    // round's prose + tool calls (and, after dispatch, the results) into the
    // canonical transcript, and the next round's request is rendered from it -
    // renderAntMessages() reproduces the assistant tool_use turn + its paired
    // tool_result user turn.
    return out;
  };
}

static bool runAntLoop(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err, const HeadTools& ht,
                       nimbus::orch::TokenUsage* usage) {
  namespace orch = nimbus::orch;
  // The CANONICAL transcript (Context Fabric Stage 2): the device owns the
  // turn's record. The controller (runHeadLoop) records prose/calls/results into
  // it via hooks.transcript, so when step() runs for round N, rounds 0..N-1 are
  // complete - the request body is rendered fresh from it every round.
  orch::Transcript tr;
  tr.addUser(inputs);
  orch::HeadStepFn step = antLoopStep(pd, instructions, ht, usage, tr);

  orch::HeadLoopHooks hooks;
  hooks.step     = step;
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m) { hlog::logf("%s", m.c_str()); };
  hooks.onText   = ht.onRoundText;   // round prose -> engine (Glass Box A4)
  hooks.spill    = ht.spill;         // clamped-result spill -> results ring
  hooks.transcript = &tr;            // the controller records; step() renders

  orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("orchTurn(ant-loop): rounds=%d cap=%s ok=%d heap=%u",
             res.rounds, res.capReason.c_str(), (int)res.ok,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the
  // failure return - a failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) { err = res.error.empty() ? std::string("loop failed") : res.error; return false; }
  outJson = res.finalTurn;
  convId  = "messages";   // stateless marker (state lives in the turn's memory field)
  return true;
}

// One Head-Orchestrator turn on Anthropic via the MESSAGES API with a single
// FORCED TOOL: tools=[{name:"orch_turn", input_schema:ORCH_SCHEMA_BODY}] +
// tool_choice {type:"tool"} makes the tool input BE the structured turn - true
// structured output, replacing the old managed-agents session that asked for
// JSON in prose and substring-scanned the reply (indexOf('{') ... '}').
//
// STATELESS BY DESIGN: the turn contract already carries ALL cross-turn state
// in the `memory` field ("it is ALL you keep across turns and provider
// failovers") + the World context re-composed each turn, so no server-side
// session is needed. convId is set to a marker so the caller's per-host
// conversation bookkeeping stays consistent. The old path's agent/session
// caching (antOrchAgent) is orch-unused now; sub-agent dispatch still uses
// environments/sessions (ensureEnv) unchanged.
bool orchTurnAnthropic(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err,
                       const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  outJson.clear(); err.clear();
  if (!pd.key || pd.key("anthropic").empty()) { err = "no Anthropic key"; return false; }

  // Multi-turn tool-use loop (opt-in): the model calls registry tools mid-turn and
  // iterates, terminating on the orch_turn tool. Falls through to the single-shot
  // forced-tool path below when the knob is off or no tools were supplied.
  if (tools && tools->dispatch && pd.toolLoopOn && pd.toolLoopOn())
    return runAntLoop(pd, convId, instructions, inputs, outJson, err, *tools, usage);

  std::string body;
  {
    JsonDocument d;
    d["model"]      = pd.orchModel ? pd.orchModel("anthropic") : std::string();
    d["max_tokens"] = 3000;              // a full orch_turn is well inside this
    // Prompt caching (v4.1.1) - same block-array shape as the loop path. A
    // single-shot turn caches for the NEXT turn (hits when the owner replies
    // within the 5-minute TTL; refreshed on every hit).
    {
      JsonObject sysBlock = d["system"].add<JsonObject>();
      sysBlock["type"] = "text";
      sysBlock["text"] = instructions;
      sysBlock["cache_control"]["type"] = "ephemeral";
    }
    JsonObject m = d["messages"].add<JsonObject>();
    m["role"] = "user";
    m["content"] = inputs;
    JsonObject tool = d["tools"].add<JsonObject>();
    tool["name"]        = "orch_turn";
    tool["description"] = "Return your complete orchestrator turn.";
    // NO strict:true - VERIFIED LIVE (2026-07-06): Anthropic's strict tool-use
    // compiles the schema to a grammar with a hard size budget, and this
    // contract (5-branch device union + enums + 9 fields) exceeds it even with
    // descriptions stripped ("compiled grammar is too large"). So on Anthropic
    // the schema is ADVISORY (forced tool-use still guarantees a tool_use
    // block); real enforcement is the device validator + tolerant parser.
    // OpenAI + Mistral DO enforce the same schema server-side. Re-try strict
    // here if Anthropic raises the grammar budget.
    {
      JsonDocument sd;                   // canonical schema (single source)
      DeserializationError se = deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
          DeserializationOption::NestingLimit(16));  // schema depth > the default 10
      if (se) { err = "schema parse"; return false; }
      stripDescriptions(sd.as<ArduinoJson::JsonVariant>());  // fit the strict grammar budget
      tool["input_schema"] = sd;
    }
    d["tool_choice"]["type"] = "tool";   // FORCE the orch_turn tool call
    d["tool_choice"]["name"] = "orch_turn";
    // Cache breakpoint on the (single) tool - caches the tools prefix segment.
    tool["cache_control"]["type"] = "ephemeral";
    body = serializeBody(d);
  }

  // Keep the tool_use input UNFILTERED (it is the whole turn object); filter the
  // rest of the response envelope down to what we read.
  JsonDocument filter;
  JsonObject ci = filter["content"].add<JsonObject>();
  ci["type"] = true; ci["name"] = true; ci["input"] = true;
  filter["stop_reason"] = true;
  filter["error"]["message"] = true;
  filter["usage"]["input_tokens"]  = true;   // token usage (single-shot turn)
  filter["usage"]["output_tokens"] = true;
  filter["usage"]["cache_read_input_tokens"]     = true;   // prompt-cache hits
  filter["usage"]["cache_creation_input_tokens"] = true;   // prompt-cache writes
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  JsonDocument doc = makeDoc(pd);   // response doc -> PSRAM (retained turn content)
  int code = antRequest(pd, "POST", "/v1/messages", std::move(body), doc, filter);
  if (code <= 0)   { err = "network"; return false; }
  if (code != 200) {
    err = "messages HTTP " + std::to_string(code);
    const char* em = doc["error"]["message"] | "";
    if (em[0]) err += std::string(": ") + em;
    hlog::logf("orchTurn(ant): %s", err.c_str());
    return false;
  }
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());

  for (JsonObjectConst c : doc["content"].as<JsonArrayConst>()) {
    if (strcmp(c["type"] | "", "tool_use") != 0) continue;
    if (strcmp(c["name"] | "", "orch_turn") != 0) continue;
    serializeJson(c["input"], outJson);           // the tool input IS the turn
    break;
  }
  if (outJson.length() == 0) {
    // stop_reason explains the miss (e.g. "max_tokens": the turn got truncated
    // before the tool call) - surface it instead of a blind "no tool_use".
    err = std::string("no orch_turn tool_use (stop_reason=") +
          (const char*)(doc["stop_reason"] | "?") + ")";
    return false;
  }
  convId = "messages";   // stateless host: marker keeps per-host conv bookkeeping happy
  return true;
}

}  // namespace providers
}  // namespace agent
