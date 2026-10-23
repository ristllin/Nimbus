#include <ArduinoJson.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/output_caps.h"   // the output-cap table (max_tokens, effort, retry)
#include "nimbus/harness/prompt_layout.h"  // static / summary / per-turn parts, cache points
#include "nimbus/harness/providers.h"
#include "nimbus/orch/budget.h"       // kBudgetBytesPerToken - the fold trigger scale
#include "nimbus/orch/connectors_wire.h"  // isFileSkill - the outputs hint on file tasks
#include "nimbus/orch/compact.h"      // modelCtxTokens, ORCH_COMPACT_PROMPT (the fold round)
#include "nimbus/orch/head_loop.h"    // runHeadLoop - the portable ReAct controller
#include "nimbus/orch/orch_schema.h"  // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/seam_meter.h"   // anthropicSessionUsage - a finished sub's spend
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - per-round usage
#include "nimbus/orch/transcript.h"   // Transcript - the canonical turn record (Stage 2)
#include "image_parts.h"   // R1: the turn's photos as image parts of the seeded user message
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
// env/agent creation writes the antEnvId/antAgentMap caches (NVS via closures,
// with a boot-lifetime RAM copy - see AntIds below).
// Verified live 2026-10-10 (CUM-481, host harness `nimbus-lab sub`): beta header
// managed-agents-2026-04-01, the environment/agent/session bodies below, the
// session `agent` string shorthand, and the typed event poll all current.

namespace agent {
namespace providers {

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
static const char* kBetaManagedAgents = "managed-agents-2026-04-01";
// A session's output files are listed with BOTH the Files API and the Managed
// Agents beta (the latter is what makes `scope_id` a known filter).
static const char* kBetaSessionFiles = "files-api-2025-04-14,managed-agents-2026-04-01";
// Where a managed-agent sub saves the files it builds for the owner, the line a
// file task carries, and how many polls a done session may wait for a named file
// to be listed (see antListOutputs).
static const char kAntOutputsDir[] = "/mnt/session/outputs/";
static const char kAntOutputsHint[] =
    "\n\nSave each file you make for the owner under /mnt/session/outputs/ and name its "
    "path in your final message.";
static const int kAntOutputPolls = 2;

static int antRequest(const ProviderDeps& pd, const char* method, const std::string& path,
                      std::string body, JsonDocument& doc, const JsonDocument& filter,
                      uint32_t timeoutMs = kHeadRoundCeilingMs,
                      const char* beta = kBetaManagedAgents,
                      const BodySplices* splices = nullptr) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"x-api-key", pd.key ? pd.key("anthropic") : std::string()},
      {"anthropic-version", kAnthropicVer},
      {"anthropic-beta", beta},
      {"Content-Type", "application/json"},
  };
  // Cumulo head: reroute through /router/anthropic with the cumulo key (no-op on
  // a direct pd, so the direct wire and its tests are unchanged).
  wire::UpstreamReq req{kAnthropicHost, 443, true, path, std::move(headers)};
  wire::applyRouter(pd, "anthropic", req);
  return exchange(pd, req.host.c_str(), req.port, req.tls, method, req.path,
                  std::move(req.headers), std::move(body), timeoutMs, doc, filter, nullptr, splices);
}

// The head's beta header: the managed-agents beta every request carries, plus the
// Files API beta when a photo of the turn rides as a file reference (R1).
static const char* kBetaHeadWithFiles = "managed-agents-2026-04-01,files-api-2025-04-14";

// ---- Managed Agents sub-sessions --------------------------------------------

// The longest provider reason a log line carries (CUM-481): enough for an
// Anthropic "<type>: <message>" pair, and short enough that the longest line
// ("anthropic: create session HTTP 400: " + excerpt) still fits hlog's 192-byte
// buffer whole instead of being cut mid-reason.
static const size_t kErrExcerptMax = 150;

static bool is2xx(int code) { return code >= 200 && code < 300; }

// Ask a response filter to keep the API's error envelope
// {"type":"error","error":{"type":...,"message":...}}.
static void keepErrorEnvelope(JsonDocument& filter) {
  filter["error"]["type"] = true;
  filter["error"]["message"] = true;
}

// `text` as one printable-ASCII log fragment of at most `max` chars: control
// bytes and non-ASCII become '?', so provider or model text can never forge a
// second log line.
static std::string logSafe(std::string text, size_t max) {
  for (char& c : text)
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
  if (text.size() > max) text.resize(max);
  return text;
}

// "<error.type>: <error.message>" from an error reply, log-safe, at most
// kErrExcerptMax chars; "" when the body carried neither. The managed-agents
// error envelope names a field or a limit, not a value (and the device log
// sink still masks registered keys and Bearer tokens on top of this).
static std::string antErrExcerpt(const JsonDocument& doc) {
  std::string out = doc["error"]["type"] | "";
  const char* msg = doc["error"]["message"] | "";
  if (msg[0]) out += (out.empty() ? "" : ": ") + std::string(msg);
  return logSafe(out, kErrExcerptMax);
}

// Log one failed call and say why, in the device log's one-line shape.
static void logCallFail(const char* what, int code, const JsonDocument& doc) {
  if (code <= 0) { hlog::logf("anthropic: %s: no response", what); return; }
  const std::string why = is2xx(code) ? std::string("reply carried no id") : antErrExcerpt(doc);
  hlog::logf("anthropic: %s HTTP %d%s%s", what, code, why.empty() ? "" : ": ", why.c_str());
}

// CUM-474: ONE status -> FabricErr rule for every call of the spawn chain
// (environment, agent, session, first turn), so a 429 on any of them reaches
// the owner as a rate limit naming its window, a refused request reads as a bad
// request (retrying cannot help), and only a 5xx reads as a provider fault.
static FabricErr chainErr(const Directive& d, const char* what, int code,
                          const JsonDocument& doc) {
  logCallFail(what, code, doc);
  if (code <= 0) return FabricErr::Network;
  if (is2xx(code) || code >= 500) return FabricErr::RemoteFail;
  if (code == 429) return wire::rateLimited(d, rateLimitFromText(antErrExcerpt(doc)));
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code == 404) return FabricErr::NotFound;
  return FabricErr::BadRequest;
}

// ---- managed-agent id cache (CUM-481) ---------------------------------------
// The shared environment id and the per-model agent map ("model=agentid;...")
// persist in NVS through the ant* closures, whose setters are void. On a full
// NVS partition (CUM-479) a write is silently dropped, and the old code then
// re-read the environment id it had just "saved" to build the session body -
// sending "environment_id":"" (HTTP 400 "environment_id: value is required") -
// and re-created a provider-side environment + agent on every spawn.
//
// Now the ids live in RAM for the whole boot: loaded from NVS once, then every
// lookup reads RAM and every change goes to RAM first and is written through to
// NVS and read back. A write that did not land is logged when it happens; the
// RAM copy keeps serving it until restart. (The NVS storage itself is untouched.)
// The copy is keyed by a fingerprint of the credential it was made under, so a
// key change drops the RAM-only ids and re-reads NVS. The persisted ids are not
// keyed (unchanged from before): ids saved under an older key are reused until
// the provider answers 404, which clears them.
struct AntIds {
  bool        loaded = false;
  size_t      owner = 0;       // std::hash of the key the ids belong to
  std::string envId;
  std::string agentMap;
};
static AntIds& antIdStore() { static AntIds ids; return ids; }

void antForgetIds() { antIdStore() = AntIds(); }

static AntIds& antIds(const ProviderDeps& pd) {
  AntIds& ids = antIdStore();
  const size_t owner = std::hash<std::string>()(pd.key ? pd.key("anthropic") : std::string());
  if (ids.loaded && ids.owner == owner) return ids;
  ids = AntIds();
  ids.loaded = true;
  ids.owner = owner;
  ids.envId = s(pd.antEnvId);
  ids.agentMap = s(pd.antAgentMap);
  return ids;
}

// Write `v` through a void NVS setter and read it back; a write that did not
// land logs one line naming `what`, and the caller's RAM copy carries on.
static void writeThrough(const std::function<void(const std::string&)>& set,
                         const std::function<std::string()>& get, const std::string& v,
                         const char* what) {
  if (set) set(v);
  if (set && s(get) == v) return;
  hlog::logf("anthropic: %s not saved (storage full?) - kept in RAM until restart", what);
}

// "model=agentid;" map lookup, anchored at an entry start so one model name
// that ends with another can never return the wrong agent.
static std::string agentInMap(const std::string& map, const std::string& model) {
  const std::string key = model + "=";
  size_t p = 0;
  while ((p = map.find(key, p)) != std::string::npos) {
    if (p == 0 || map[p - 1] == ';') {
      size_t st = p + key.size();
      size_t e = map.find(';', st);
      return map.substr(st, (e == std::string::npos ? map.size() : e) - st);
    }
    p += key.size();
  }
  return std::string();
}

// True when a session create refused the cached environment or agent itself:
// 404 (deleted, or another account's id) or a 400 naming it archived (an
// archived env/agent can never start a session again). Live-verified bodies
// 2026-10-10: "Environment env_... is archived." / "agent agent_... is
// archived and cannot be used to create a session" / "... not found".
static bool cachedIdsDead(int code, const JsonDocument& doc) {
  if (code == 404) return true;
  if (code != 400) return false;
  const char* msg = doc["error"]["message"] | "";
  return strstr(msg, " is archived") != nullptr;
}

// One create call of the spawn chain: POST `body` to `path` and return the new
// resource's id with the HTTP status; on any failure the id is "" and `err`
// holds the classified error (chainErr logged why).
struct Created {
  std::string id;
  int code = 0;
  bool deadRef = false;   // the call refused a cached id itself (cachedIdsDead)
};
static Created createCall(const ProviderDeps& pd, const Directive& d, const char* what,
                          const char* path, std::string body, FabricErr& err) {
  JsonDocument filter; filter["id"] = true; keepErrorEnvelope(filter);
  JsonDocument doc;
  Created out;
  out.code = antRequest(pd, "POST", path, std::move(body), doc, filter);
  const char* id = doc["id"] | "";
  if (is2xx(out.code) && id[0]) out.id = id;
  else err = chainErr(d, what, out.code, doc);
  out.deadRef = cachedIdsDead(out.code, doc);
  return out;
}

// The cloud environment (network egress for the web tools), created once and
// cached. "" + `err` on failure.
static std::string envFor(const ProviderDeps& pd, const Directive& d, AntIds& ids,
                          FabricErr& err) {
  if (!ids.envId.empty()) return ids.envId;
  std::string body;
  {
    JsonDocument doc;
    doc["name"] = "nimbus-env";
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["type"] = "cloud";
    cfg["networking"]["type"] = "unrestricted";
    body = serializeBody(doc);
  }
  const Created env = createCall(pd, d, "create env", "/v1/environments", std::move(body), err);
  if (env.id.empty()) return std::string();
  ids.envId = env.id;
  hlog::logf("anthropic: env %s", env.id.c_str());
  writeThrough(pd.setAntEnvId, pd.antEnvId, ids.envId, "env id");
  return ids.envId;
}

// A model name from an LLM-chosen spawn directive keys the "model=agentid;" map,
// so anything outside [A-Za-z0-9._-] (the map's '=' / ';' delimiters above all)
// falls back to the default rather than corrupting it.
static const char* safeModel(const char* model, const char* defaultModel) {
  if (!model || !*model) return defaultModel;
  for (const char* p = model; *p; ++p) {
    char ch = *p;
    if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='.'||ch=='_')) {
      hlog::logf("anthropic: unsafe model name '%s' -> default", logSafe(model, 64).c_str());
      return defaultModel;
    }
  }
  return model;
}

// Fit the connector layer's MCP attachments to managed-agents-2026-04-01. Live
// 2026-10-10 the API refused both shapes the connector layer emits: an inline
// `authorization_token` ("unknown field" - credentials ride a vault attached to
// the session) and an mcp_servers entry that no `mcp_toolset` tool references
// ("declared but no mcp_toolset in tools references them"). Either made EVERY
// spawn fail at agent creation for an owner with an Anthropic MCP connector. So
// a server that needs a token is left off (logged) instead of failing the spawn,
// and each remaining server gets its mcp_toolset with always_allow: the device
// has no tool-confirmation path, and MCP toolsets default to always_ask, which
// would park the session in requires_action forever (OpenAI's attach uses
// require_approval "never" for the same reason). Vault credentials: CUM-519.
static void fitMcpServers(JsonDocument& agent) {
  JsonArray servers = agent["mcp_servers"].as<JsonArray>();
  if (servers.isNull()) return;
  for (size_t i = servers.size(); i-- > 0;) {
    if (servers[i]["authorization_token"].isNull()) continue;
    const std::string name = servers[i]["name"] | "";
    hlog::logf("anthropic: connector %s needs a vault credential - left off the sub-agent",
               logSafe(name, 40).c_str());
    servers.remove(i);
  }
  std::vector<std::string> names;
  for (JsonObject srv : servers) names.push_back(srv["name"] | "");
  if (names.empty()) { agent.remove("mcp_servers"); return; }
  JsonArray tools = agent["tools"].as<JsonArray>();
  for (const std::string& name : names) {
    JsonObject t = tools.add<JsonObject>();
    t["type"] = "mcp_toolset";
    t["mcp_server_name"] = name;
    t["default_config"]["permission_policy"]["type"] = "always_allow";
  }
}

// Anthropic pins the model on the AGENT (no per-session model field), so each
// model needs its own agent: the cached id for `model`, created on first use.
// "" + `err` on failure.
static std::string agentFor(const ProviderDeps& pd, const Directive& d, const char* defaultModel,
                            AntIds& ids, FabricErr& err) {
  const char* model = safeModel(d.model, defaultModel);
  std::string cached = agentInMap(ids.agentMap, model);
  if (!cached.empty()) return cached;
  std::string body;
  {
    JsonDocument doc;
    doc["name"]   = std::string("Nimbus Agent (") + model + ")";
    doc["model"]  = model;
    doc["system"] = "You are an autonomous assistant agent running on a managed "
                    "sandbox. Use your tools (bash, web search/fetch, files) to "
                    "complete the task fully, then give the final result concisely.";
    JsonArray tools = doc["tools"].to<JsonArray>();
    tools.add<JsonObject>()["type"] = ANT_TOOLSET;
    // Owner-configured connectors ride the managed agent as mcp_servers[] - the
    // session created against this agent inherits them (sub-agents only; the head
    // is one forced tool) - fitted to the managed-agents shape (fitMcpServers).
    if (pd.attachAnthropic) { pd.attachAnthropic(doc); fitMcpServers(doc); }
    body = serializeBody(doc);
  }
  const Created agent = createCall(pd, d, "create agent", "/v1/agents", std::move(body), err);
  if (agent.id.empty()) return std::string();
  ids.agentMap += std::string(model) + "=" + agent.id + ";";
  hlog::logf("anthropic: agent %s for %s", agent.id.c_str(), model);
  writeThrough(pd.setAntAgentMap, pd.antAgentMap, ids.agentMap, "agent map");
  return agent.id;
}

// Send a user.message turn to an existing session. Returns the HTTP status;
// `doc` keeps the error envelope for the caller's log.
static int sendTurn(const ProviderDeps& pd, const char* sessionId, const char* text,
                    JsonDocument& doc) {
  std::string body;
  {
    JsonDocument d;
    JsonArray evs = d["events"].to<JsonArray>();
    JsonObject ev = evs.add<JsonObject>();
    ev["type"] = "user.message";
    JsonObject c = ev["content"].add<JsonObject>();
    c["type"] = "text";
    wire::putFieldStr(c["text"], text ? std::string(text) : std::string(), "the sub-agent task");
    body = serializeBody(d);
  }
  JsonDocument filter; keepErrorEnvelope(filter);   // the success body is not read
  std::string path = std::string("/v1/sessions/") + sessionId + "/events";
  return antRequest(pd, "POST", path, std::move(body), doc, filter);
}

// The session request, built from the ids THIS dispatch resolved (never re-read
// from the cache it just wrote - the CUM-481 bug).
static std::string sessionBody(const std::string& agentId, const std::string& envId) {
  JsonDocument doc;
  doc["agent"]          = agentId;   // string shorthand = the agent's latest version
  doc["environment_id"] = envId;
  return serializeBody(doc);
}

FabricErr antDispatch(const ProviderDeps& pd, const char* defaultModel,
                      const Directive& d, char outJobId[72]) {
  if (!pd.key || pd.key("anthropic").empty()) { hlog::log("anthropic: no API key"); return FabricErr::Auth; }
  AntIds& ids = antIds(pd);
  FabricErr err = FabricErr::RemoteFail;
  const std::string envId = envFor(pd, d, ids, err);
  if (envId.empty()) return err;
  const std::string agentId = agentFor(pd, d, defaultModel, ids, err);   // per-model agent (cached)
  if (agentId.empty()) return err;

  const Created session =
      createCall(pd, d, "create session", "/v1/sessions", sessionBody(agentId, envId), err);
  if (session.deadRef) {
    // The cached env/agent are gone or archived provider-side - forget them
    // (RAM and NVS) so the next dispatch recreates them.
    ids.envId.clear();
    ids.agentMap.clear();
    writeThrough(pd.setAntEnvId, pd.antEnvId, "", "env id reset");
    writeThrough(pd.setAntAgentMap, pd.antAgentMap, "", "agent map reset");
  }
  if (session.id.empty()) return err;
  // The job id must carry the session id WHOLE ("anthropic:<id>" in 72 bytes),
  // or polls and cancel would address a different session than the turn runs.
  if (session.id.size() > 72 - sizeof("anthropic:")) {
    hlog::logf("anthropic: create session: id too long (%u chars)", (unsigned)session.id.size());
    return FabricErr::RemoteFail;
  }

  // The run starts with this turn (the environment, agent and session above run
  // nothing by themselves, so a cut there is "did not start"): a turn cut after it
  // went out may have started the paid run.
  JsonDocument turnDoc;
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  // OA-107: a file task is told where the device collects files from (antPoll
  // lists the session's /mnt/session/outputs/ files and the device downloads them).
  std::string task = d.instruction ? d.instruction : "";
  if (nimbus::orch::isFileSkill(d.skill ? d.skill : "")) task += kAntOutputsHint;
  const int code = sendTurn(pd, session.id.c_str(), task.c_str(), turnDoc);
  if (code <= 0) {
    logCallFail("send turn", code, turnDoc);
    return wire::subTransportErr(pd, t0, kSyncSubCeilingMs);
  }
  if (!is2xx(code)) return chainErr(d, "send turn", code, turnDoc);

  snprintf(outJobId, 72, "anthropic:%s", session.id.c_str());
  hlog::logf("anthropic: session %s started", session.id.c_str());
  return FabricErr::Ok;
}

// What one session.status_idle means for the job, per the documented rule
// (stop polling on any stop_reason except requires_action): end_turn is done;
// requires_action is the session waiting on a client event (not reachable with
// this agent's always-allow toolsets, so it stays Running rather than ending a
// live job); anything else - retries_exhausted, budget_reached, or a reason this
// code does not know yet - ends the job as an error instead of Running forever
// (a Running job has no give-up timer in the job engine).
static void applyIdle(const char* stopReason, bool& done, bool& errored, ResultEnvelope& env) {
  if (!strcmp(stopReason, "end_turn")) { done = true; return; }
  if (!stopReason[0] || !strcmp(stopReason, "requires_action")) return;
  errored = true;
  if (!strcmp(stopReason, "retries_exhausted"))
    strncpy(env.error, "the agent stopped after repeated errors", sizeof(env.error) - 1);
  else
    snprintf(env.error, sizeof(env.error), "the agent stopped early (%s)",
             logSafe(stopReason, 40).c_str());
}

// Text length of one agent.message event (its text blocks summed).
static size_t antMessageLen(JsonObjectConst ev) {
  size_t n = 0;
  for (JsonObjectConst c : ev["content"].as<JsonArrayConst>())
    if (!strcmp(c["type"] | "", "text")) n += strlen(c["text"] | "");
  return n;
}

// Append one agent.message's text blocks to the reply. Messages after the first
// are separated by a blank line. The writer cuts on a character boundary with a
// visible marker when the 16 KB envelope is full (a raw byte cut split characters)
// and keeps every later block for the spill (L19-36), so all of them are fed.
static void antAppendMessage(JsonObjectConst ev, wire::ReplyWriter& w) {
  if (w.used && antMessageLen(ev)) w.append("\n\n", 2);
  for (JsonObjectConst c : ev["content"].as<JsonArrayConst>()) {
    if (strcmp(c["type"] | "", "text") != 0) continue;
    const char* t = c["text"] | "";
    w.append(t, strlen(t));
  }
}

// CUM-514: the artifact a sub-agent's run produced, without its narration.
// Managed Agents buffers each model request's text into ONE agent.message event,
// so the agent's working narration ("I'll start with Boverket's own pages...",
// said before a tool call) arrives as separate, short messages ahead of the
// report - and used to be glued onto the saved artifact with no separator. The
// rule: the artifact is the LONGEST message (a tie goes to the later one) plus
// every message after it (a closing note after the report is kept). A message
// BEFORE it is narration - and dropped - only if it is short (under a quarter of
// the longest, never under kAntNarrationMin, never over kAntNarrationMax); a long
// earlier message is part of the
// work (a report written over several requests) and is kept, in order. A
// one-message run is unchanged. Bounded by the envelope (a longer artifact goes to
// the overflow record whole, L19-36).
static const size_t kAntNarrationMin = 240;
static const size_t kAntNarrationMax = 600;
static void antArtifactText(JsonArrayConst events, char* out, size_t cap, const std::string& tailNote) {
  int at = -1, idx = 0;
  size_t best = 0;
  for (JsonObjectConst ev : events) {
    if (!strcmp(ev["type"] | "", "agent.message")) {
      const size_t n = antMessageLen(ev);
      if (at < 0 || n >= best) { at = idx; best = n; }
    }
    ++idx;
  }
  const size_t quarter = best / 4 > kAntNarrationMin ? best / 4 : kAntNarrationMin;
  const size_t narration = quarter < kAntNarrationMax ? quarter : kAntNarrationMax;
  wire::ReplyWriter w(out, cap);
  idx = 0;
  for (JsonObjectConst ev : events) {
    const bool before = idx++ < at;
    if (strcmp(ev["type"] | "", "agent.message") != 0) continue;
    if (before && antMessageLen(ev) < narration) continue;   // short working note
    antAppendMessage(ev, w);
  }
  if (!tailNote.empty()) w.appendTail(tailNote);   // the one channel for what it reports
}

// L19-37: the sandbox path the run's last agent.message names for its deliverable
// ("/mnt/session/outputs/<file>"), "" when it names none. The managed agent writes a
// long report to its sandbox and then summarises; on 19 Oct the device synthesised
// from the 6 KB summary while the report stayed in the sandbox.
static std::string antSandboxOutput(JsonArrayConst events) {
  static const char kOutputs[] = "/mnt/session/outputs/";
  std::string found;
  for (JsonObjectConst ev : events) {
    if (strcmp(ev["type"] | "", "agent.message") != 0) continue;
    for (JsonObjectConst c : ev["content"].as<JsonArrayConst>()) {
      const char* t = c["text"] | "";
      const char* at = strstr(t, kOutputs);
      if (!at) continue;
      // Printable ASCII only (agent text may carry control bytes or invisible
      // characters): the path rides a log line and the owner's reply.
      size_t n = strlen(kOutputs);
      while ((unsigned char)at[n] > 0x20 && (unsigned char)at[n] < 0x7f && !strchr("`'\")]>,;", at[n]) && n < 160) n++;
      while (n > strlen(kOutputs) && strchr(".:", at[n - 1])) n--;   // a sentence's own full stop
      if (n > strlen(kOutputs)) found.assign(at, n);   // the LAST message that names one wins
    }
  }
  return found;
}

// Provenance from the session's own event list: every agent.tool_use,
// agent.mcp_tool_use and agent.custom_tool_use event is a tool the sub ran; a
// web_search or web_fetch use is a web run. Counted from the full list each poll.
static void antCountRuns(JsonArrayConst events, ResultEnvelope& env) {
  env.toolRuns = 0;
  env.webRuns = 0;
  for (JsonObjectConst ev : events) {
    const char* type = ev["type"] | "";
    if (strcmp(type, "agent.tool_use") && strcmp(type, "agent.mcp_tool_use") &&
        strcmp(type, "agent.custom_tool_use"))
      continue;
    if (env.toolRuns < 0xFFFF) env.toolRuns++;
    const char* nm = ev["name"] | "";
    if ((!strcmp(nm, "web_search") || !strcmp(nm, "web_fetch")) && env.webRuns < 0xFFFF) env.webRuns++;
  }
}

// ---- session output files (OA-107, L19-37) ----------------------------------
// A managed-agent sub builds a file (a PDF, a chart, a CSV) in its sandbox and
// saves it under /mnt/session/outputs/; the Files API keeps every such file,
// scoped to the session (GET /v1/files?scope_id=<session>, verified live
// 2026-10-22 on claude-haiku-5-5: a 1,550 B PDF listed and downloaded). When the
// session is done, antPoll lists them into artifacts[] and the device's
// "anthropic" capture backend (provider_file_fetch) streams each one to the file
// store. The listing lags the idle event by a second or two (measured: empty on
// the first call, listed 2 s later), so a session whose final message names an
// outputs path but lists nothing yet stays Running for up to kAntOutputPolls more
// polls instead of finishing with its file left behind.

// Whether the last agent.message names a path under /mnt/session/outputs/.
static bool antNamesOutputs(JsonArrayConst events) {
  JsonObjectConst last;
  for (JsonObjectConst ev : events)
    if (!strcmp(ev["type"] | "", "agent.message")) last = ev;
  if (last.isNull()) return false;
  for (JsonObjectConst c : last["content"].as<JsonArrayConst>())
    if (strstr(c["text"] | "", kAntOutputsDir)) return true;
  return false;
}

// The session's output files into env.artifacts[] (the capture cap, then an
// honest note for any past it, added to `tail`: the reply's one tail channel). Returns the files listed, kAntListFailed when the
// listing could not be read (a poll later tries again), or kAntListSkipped for a
// session that ran on the router (its files belong to the router account and are
// never this device's to fetch).
static const int kAntListFailed = -1;
static const int kAntListSkipped = -2;
static int antListOutputs(const ProviderDeps& pd, const char* sid, ResultEnvelope& env, std::string& tail) {
  if (pd.routerBase && !pd.routerBase().empty()) return kAntListSkipped;
  JsonDocument filter;
  JsonObject fi = filter["data"].add<JsonObject>();
  fi["id"] = true;
  fi["filename"] = true;
  fi["downloadable"] = true;
  keepErrorEnvelope(filter);
  JsonDocument doc;
  const int code = antRequest(pd, "GET", std::string("/v1/files?scope_id=") + sid, "", doc,
                              filter, kHeadRoundCeilingMs, kBetaSessionFiles);
  if (code != 200) {
    if (code > 0) logCallFail("list output files", code, doc);
    return kAntListFailed;
  }
  int listed = 0, dropped = 0;
  for (JsonObjectConst f : doc["data"].as<JsonArrayConst>()) {
    const char* id = f["id"] | "";
    if (!id[0] || !(f["downloadable"] | true)) continue;
    listed++;
    if (env.artifactCount >= nimbus::orch::kMaxArtifacts) { dropped++; continue; }
    nimbus::orch::Artifact& a = env.artifacts[env.artifactCount++];
    strncpy(a.type, "file", sizeof(a.type) - 1);
    strncpy(a.url, id, sizeof(a.url) - 1);
    strncpy(a.label, f["filename"] | "", sizeof(a.label) - 1);
  }
  if (dropped > 0)
    tail += "\n[note: the run produced " + std::to_string(dropped) + " more file(s) beyond the " +
            std::to_string(nimbus::orch::kMaxArtifacts) + "-file capture limit - they were NOT captured]";
  hlog::logf("anthropic: session %s output files %d", sid, listed);
  return listed;
}

// Polls a done session spent waiting for its named outputs to be listed, keyed by
// session (RAM, a few slots: sub-agents are polled one at a time on one task).
struct AntOutputWait { char sid[64]; int polls; };
static AntOutputWait s_antWait[4];
static int antOutputWaitBump(const char* sid) {
  AntOutputWait* slot = &s_antWait[0];
  for (AntOutputWait& w : s_antWait) {
    if (!strncmp(w.sid, sid, sizeof(w.sid) - 1)) { slot = &w; break; }
    if (w.polls < slot->polls) slot = &w;   // else reuse the least-used slot
  }
  if (strncmp(slot->sid, sid, sizeof(slot->sid) - 1)) {
    strncpy(slot->sid, sid, sizeof(slot->sid) - 1);
    slot->sid[sizeof(slot->sid) - 1] = 0;
    slot->polls = 0;
  }
  return ++slot->polls;
}
// The session finished: its slot is free again (a later run starts from zero).
static void antOutputWaitDone(const char* sid) {
  for (AntOutputWait& w : s_antWait)
    if (!strncmp(w.sid, sid, sizeof(w.sid) - 1)) { w.sid[0] = 0; w.polls = 0; }
}

// Lane DEF: a finished session's billed usage (GET /v1/sessions/<id>). The events poll
// carries none, so without this read an Anthropic sub-agent's spend never reached the
// monthly spend ceiling, the Usage page or the budgets. One GET per finished job (a
// GET passes the paid-call gate). A cut or busy (429) read answers Network, so the job
// is polled again and the read retried; any other failure delivers the result and
// counts the session's estimate (fail-closed, never zero), so a read that keeps failing
// never costs the owner the result.
static FabricErr antSessionUsage(const ProviderDeps& pd, const char* sid, ResultEnvelope& env) {
  JsonDocument filter;
  filter["usage"] = true;
  keepErrorEnvelope(filter);
  JsonDocument doc;
  const int code = antRequest(pd, "GET", std::string("/v1/sessions/") + sid, "", doc, filter);
  if (code <= 0 || code == 429) {
    if (code > 0) logCallFail("session usage", code, doc);
    return FabricErr::Network;
  }
  nimbus::orch::meterSubSearchRuns("anthropic", env.webRuns);   // billed per search, beside the tokens
  if (code != 200) {
    logCallFail("session usage", code, doc);
    env.promptTokens = nimbus::orch::kSessionInputEstimateTokens;
    env.completionTokens = (uint32_t)((strlen(env.reply) + 1) / 2);
    return FabricErr::Ok;
  }
  const nimbus::orch::TokenUsage u =
      nimbus::orch::anthropicSessionUsage(doc["usage"].as<ArduinoJson::JsonVariantConst>());
  env.promptTokens = u.promptTokens;
  env.completionTokens = u.completionTokens;
  env.cacheReadTokens = u.cacheReadTokens;
  env.cacheWriteTokens = u.cacheWriteTokens;
  return FabricErr::Ok;
}

FabricErr antPoll(const ProviderDeps& pd, const char* jobId, ResultEnvelope& env) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  const char* sid = colon + 1;
  strncpy(env.jobId,   jobId,       sizeof(env.jobId)   - 1);
  strncpy(env.backend, "anthropic", sizeof(env.backend) - 1);

  // Pull the event list; keep only event type, message text, idle stop_reason,
  // and error message (plus the top-level error envelope of a refused poll).
  JsonDocument filter;
  JsonObject di = filter["data"].add<JsonObject>();
  di["type"] = true;
  JsonObject ci = di["content"].add<JsonObject>();
  ci["type"] = true; ci["text"] = true;
  di["stop_reason"]["type"] = true;
  di["error"]["message"] = true;
  di["name"] = true;   // agent.*tool_use: which tool ran (provenance)
  keepErrorEnvelope(filter);

  JsonDocument doc;
  std::string path = std::string("/v1/sessions/") + sid +
                "/events?types[]=agent.message&types[]=session.status_idle&types[]=session.error"
                "&types[]=agent.tool_use&types[]=agent.mcp_tool_use&types[]=agent.custom_tool_use";
  int code = antRequest(pd, "GET", path, "", doc, filter);
  if (code == 404)                return FabricErr::NotFound;   // session gone/expired
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code <= 0)                  return FabricErr::Network;     // transient
  if (code != 200)               { logCallFail("poll", code, doc); return FabricErr::RemoteFail; }

  bool done = false, errored = false;
  for (JsonObject ev : doc["data"].as<JsonArray>()) {
    const char* type = ev["type"] | "";
    if (!strcmp(type, "session.status_idle")) {
      applyIdle(ev["stop_reason"]["type"] | "", done, errored, env);
    } else if (!strcmp(type, "session.error")) {
      errored = true;
      strncpy(env.error, ev["error"]["message"] | "agent error", sizeof(env.error) - 1);
    }
  }
  // OA-107 / L19-37 capture: a finished run lists its output files into artifacts[]
  // first. A listing that failed, or a named file not listed yet (the listing lags
  // the idle event): poll again, a bounded number of times, before finishing.
  std::string tail;   // notes that must reach the head whole (the reply's tail channel)
  if (done && !errored) {
    const int listed = antListOutputs(pd, sid, env, tail);
    const bool unsure = listed == kAntListFailed ||
                        (listed == 0 && antNamesOutputs(doc["data"].as<JsonArrayConst>()));
    if (unsure && antOutputWaitBump(sid) <= kAntOutputPolls) done = false;
    else antOutputWaitDone(sid);
  }
  // L19-37: a finished run whose deliverable stayed in its sandbox (no file was
  // captured into artifacts[]) says so, so its summary is never taken as the report.
  if (done && !errored && env.artifactCount == 0) {
    const std::string path = antSandboxOutput(doc["data"].as<JsonArrayConst>());
    if (!path.empty()) {
      tail += "\n[report left in the sandbox, not retrieved: " + path +
              " - this reply is the agent's summary, not the report]";
      hlog::logf("anthropic: %s left its report in the sandbox (%s), not retrieved", sid, path.c_str());
    }
  }
  antArtifactText(doc["data"].as<JsonArrayConst>(), env.reply, sizeof(env.reply), tail);
  antCountRuns(doc["data"].as<JsonArrayConst>(), env);

  if (errored)    env.state = JobState::Error;
  else if (done)  { env.state = JobState::Done; if (!env.reply[0]) strncpy(env.reply, "(done)", sizeof(env.reply)-1); }
  else            env.state = JobState::Running;
  if (errored || done) return antSessionUsage(pd, sid, env);
  return FabricErr::Ok;
}

FabricErr antAnswer(const ProviderDeps& pd, const char* jobId, const char* userText) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  JsonDocument doc;
  int code = sendTurn(pd, colon + 1, userText, doc);
  if (is2xx(code)) return FabricErr::Ok;
  logCallFail("answer", code, doc);
  return FabricErr::RemoteFail;
}

FabricErr antCancel(const ProviderDeps& pd, const char* jobId) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  // Interrupt the in-flight run (DELETE alone won't halt a running session).
  std::string body = "{\"events\":[{\"type\":\"user.interrupt\"}]}";
  JsonDocument filter; keepErrorEnvelope(filter);
  JsonDocument doc;
  std::string path = std::string("/v1/sessions/") + (colon + 1) + "/events";
  int code = antRequest(pd, "POST", path, std::move(body), doc, filter);
  if (is2xx(code)) return FabricErr::Ok;
  logCallFail("cancel", code, doc);
  return FabricErr::RemoteFail;
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
static void renderAntMessages(const nimbus::orch::Transcript& tr, ArduinoJson::JsonArray msgs,
                              bool showingImages) {
  namespace orch = nimbus::orch;
  using Kind = orch::TranscriptItem::Kind;
  const auto& e = tr.entries();
  // The seeded user turn (pinned; always first). A request that does not show the
  // turn's photos reads their machine transcriptions in place of their labels (R1).
  for (const auto& it : e) {
    if (it.kind != Kind::User) continue;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    wire::putFieldStr(u["content"], imageparts::userText(tr, it.text, showingImages), "the user message");
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
      wire::putModelStr(t["text"], prose, "replayed prose");
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
        wire::putModelStr(trr["content"], e[ri].text, e[ri].name.c_str());
        if (e[ri].isError) trr["is_error"] = true;
      }
    }
  }
}

// The stalled round, answered: a prose-only (stalled) round is never echoed on
// tool rounds (prefill rule), but on the forced final round after a stall the
// prose IS the answer the owner is waiting for. It rides back as an assistant
// message followed by a user nudge (so the request still ends on a user turn), and
// the final round keeps that answer instead of writing a different one (18 Oct: a
// 305 kcal estimate thrown away for a 345 kcal one written blind).
static void antEchoStall(const nimbus::orch::Transcript& tr, JsonArray msgs) {
  using Kind = nimbus::orch::TranscriptItem::Kind;
  int last = -1;
  bool calls = false;
  std::string prose;
  for (const auto& it : tr.entries()) {
    if (it.kind == Kind::User) continue;
    if (it.round > last) { last = it.round; calls = false; prose.clear(); }
    if (it.kind == Kind::ToolUse) calls = true;
    if (it.kind == Kind::AssistantText && !it.text.empty()) prose += (prose.empty() ? "" : "\n") + it.text;
  }
  if (calls || prose.empty()) return;
  JsonObject a = msgs.add<JsonObject>();
  a["role"] = "assistant";
  JsonObject t = a["content"].add<JsonObject>();
  t["type"] = "text";
  wire::putModelStr(t["text"], prose, "the stalled answer");
  JsonObject u = msgs.add<JsonObject>();
  u["role"] = "user";
  u["content"] = "Now call orch_turn with your final complete turn, keeping the answer you gave above.";
}

// One loop request body: the rendered conversation, the per-round system text,
// the tools (orch_turn always; the registry specs on tool rounds), tool_choice
// auto on EVERY round (never forced: Sonnet 5.5, Opus 5.5 and Fable 5.1 answer a
// forced tool_choice with HTTP 400 - verified live 18 Oct), and the output cap and
// effort for this attempt.
struct AntLoopReq {
  const std::string* model;
  const layout::PromptParts* parts;   // the turn's prompt, split once (prompt_layout.h)
  const std::string* capReason;
  const HeadTools* ht;
  bool allowTools;
  uint32_t maxTokens;
  const char* effort;
};
static bool antLoopBody(const ProviderDeps& pd, const AntLoopReq& a, JsonArray msgs,
                        std::string& body, std::string& err) {
  JsonDocument req = makeDoc(pd);
  req["model"]      = *a.model;
  req["max_tokens"] = a.maxTokens;
  if (a.effort && *a.effort) req["output_config"]["effort"] = a.effort;
  // roundInstructions() appends the final-round notice when a cap forced this
  // tool-less round, so the model reports what it could not finish rather than
  // promising work that will never run. Prompt caching (prompt_layout.h): system
  // rides as content blocks, one per part, so the static and summary blocks carry
  // cache points; inside a tool loop the rounds are seconds apart, so the
  // 5-minute ephemeral cache hits on every round after the first. The notice
  // rides the per-turn block (the only block of an unsplit prompt), so a final
  // round still hits the static and summary blocks.
  // Cache points (at most 4): the last tool, the static block, the summary block,
  // and the last message.
  {
    layout::PromptParts round = *a.parts;
    std::string& tail = round.split ? round.dyn : round.stat;
    tail = wire::roundInstructions(tail, *a.capReason);
    layout::putAnthropicSystem(req["system"].to<JsonArray>(), round);
  }
  req["messages"] = msgs;   // deep-copy within the doc's allocator
  layout::cacheLastMessage(req["messages"].as<JsonArray>());
  JsonArray toolsArr = req["tools"].to<JsonArray>();
  JsonObject ot = toolsArr.add<JsonObject>();
  ot["name"]        = "orch_turn";
  ot["description"] = "Return your COMPLETE orchestrator turn. This is your FINAL "
                      "answer and ENDS the turn - call it once you have everything.";
  JsonDocument sd = makeDoc(pd);
  if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY, DeserializationOption::NestingLimit(16))) {
    err = "schema parse";
    return false;
  }
  stripDescriptions(sd.as<ArduinoJson::JsonVariant>());
  ot["input_schema"] = sd;
  for (size_t i = 0; a.allowTools && i < a.ht->specs.size(); i++) {
    const auto& spec = a.ht->specs[i];
    JsonObject t = toolsArr.add<JsonObject>();
    t["name"]        = spec.name;
    t["description"] = spec.description;
    JsonDocument ssd = makeDoc(pd);
    if (deserializeJson(ssd, spec.schemaJson, DeserializationOption::NestingLimit(16)) ==
        DeserializationError::Ok)
      t["input_schema"] = ssd;
    else
      t["input_schema"].to<JsonObject>();
  }
  req["tool_choice"]["type"] = "auto";   // may call a tool OR finish (orch_turn) - every round
  // Cache breakpoint on the LAST tool: caches the entire tools array as one prefix
  // segment (Anthropic caches tools -> system -> messages in that canonical order).
  toolsArr[toolsArr.size() - 1]["cache_control"]["type"] = "ephemeral";
  return wire::serializeChecked(req, body, err);
}

// Read one loop response into `out`: the FIRST orch_turn's input is the turn and
// the scan goes on (calls after it are still tool calls - CUM-526); text blocks
// are collected into HeadStep.text (Glass Box A4, observed only). Returns the
// stop_reason.
static std::string antReadRound(const JsonDocument& resp, nimbus::orch::HeadStep& out) {
  for (JsonObjectConst c : resp["content"].as<JsonArrayConst>()) {
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
      if (!out.finished) { out.finished = true; out.finalTurn = args; }
      continue;
    }
    nimbus::orch::HeadToolCall call;
    call.id       = c["id"] | "";
    call.name     = nm;
    call.argsJson = (args.length() && args != "null") ? args : "{}";
    out.toolCalls.push_back(call);
  }
  return resp["stop_reason"] | "";
}

// STEP FACTORY (Stage 2 phase 5): one Anthropic model turn against the shared
// canonical transcript. ⚠ Lifetime contract: `ht`, `usage` and `tr` are captured
// BY REFERENCE - the caller's frame must outlive every call of the returned step
// (true for runAntLoop and for the engine's fabric loop, both of which hold them
// across runHeadLoop). `pd` and the split `instructions` are copied.
nimbus::orch::HeadStepFn antLoopStep(const ProviderDeps& pd,
                                     const std::string& instructions,
                                     const HeadTools& ht,
                                     nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  const std::string model = pd.orchModel ? pd.orchModel("anthropic") : std::string();
  // Replay budget - NOT the token window. The request body breaks long before the
  // window, at the DEVICE's contiguous-heap ceiling for the TLS send. MEASURED on
  // Board 1 (2026-08-05): a 6-way fan-out synthesis grew its replay to body=65672
  // over 12 rounds and failed at intLargest~19 K ("no response"->network), while
  // bodies <=52 KB sent fine. The tool results ride the transcript under this
  // budget (Transcript::trimToolOutputs, append-only), so system + transcript +
  // tools stays in the ~48 K working range however many rounds a turn takes.
  // Smaller than the OpenAI/Mistral 24 KB: the old fold counted the user seed
  // against its 24 KB trigger, this budget counts tool results only.
  const size_t kReplayToolBytes = 16384;
  // The static, summary and dynamic parts, split once per turn (prompt_layout.h)
  // from the prompt repaired to valid UTF-8 (wire.h: the system prompt is
  // repaired, never clipped; serializeChecked still guards every body).
  std::string sys;
  wire::putFieldStr(sys, instructions, "the system prompt");
  const layout::PromptParts parts = layout::split(sys);

  return [&ht, usage, &tr, pd, model, kReplayToolBytes, parts](
             bool allowTools,
             const std::vector<orch::HeadToolResult>& prior,
             uint32_t budgetMs,
             const std::string& capReason) -> orch::HeadStep {
    namespace orch = nimbus::orch;
    (void)prior;   // the transcript already carries every result (recorded by the
                   // controller after dispatch, before this step runs)
    orch::HeadStep out;
    const uint32_t roundMs = wire::clampRoundMs(budgetMs, kHeadRoundCeilingMs);
    const OutputCap cap = outputCapFor("anthropic", model,
                                       allowTools ? RoundKind::ToolRound : RoundKind::FinalRound, roundMs);

    // 1) render messages[] from the canonical transcript (fresh doc per round -
    //    no cross-round accumulator to leak or drift). Append-only replay
    //    (prompt_layout.h): every message already sent keeps its bytes, so each
    //    round reads the previous round's request from the cache. The newest
    //    results are clipped to the room left under the replay budget instead of
    //    folding older rounds (which rewrote them every round).
    if (tr.toolBytes() > kReplayToolBytes) tr.trimToolOutputs(kReplayToolBytes);
    // R1: the photos ride the seeded user message when this model takes images and
    // no provider refused them this turn; uploaded once (the Files API) where the key
    // is direct, so the later rounds reference them instead of re-sending them.
    orch::TurnImages* imgs = imageparts::shown(tr, "anthropic", model);
    if (imgs && !imgs->uploadTried) {
      imgs->uploadTried = true;
      if (imgs->uploadOnce) imageparts::antUploadImages(pd, *imgs);
    }
    JsonDocument conv = makeDoc(pd);
    JsonArray msgs;
    auto render = [&]() {
      msgs = conv.to<JsonArray>();
      renderAntMessages(tr, msgs, imgs != nullptr);
      if (!allowTools && capReason == "stalled") antEchoStall(tr, msgs);
      if (msgs.size() > 0 && !strcmp(msgs[msgs.size() - 1]["role"] | "", "assistant")) {
        // A trailing assistant message is a prefill, which current models reject.
        // The renderer always answers tool_use rounds with their results, so this
        // shouldn't trigger - but if it ever does, the request stays valid.
        JsonObject u = msgs.add<JsonObject>();
        u["role"] = "user";
        u["content"] = "Now call orch_turn with your final complete turn.";
      }
      // 1b) every string content as one text block, so a message has the same bytes
      //     whether or not it is the last one (the one antLoopBody gives the moving
      //     cache point).
      layout::textAsBlocks(msgs);
      // 1c) the photos before the turn text in the seeded message (never a cache
      //     point: the moving one stays on the last text block, after them).
      if (imgs && msgs.size() > 0) imageparts::putParts(msgs[0], *imgs, imageparts::Dialect::Anthropic);
    };
    render();
    // A string dropped while the conversation was rendered (conv is its own
    // document, copied into req as a null) is refused before the send.
    if (conv.overflowed()) {
      out.ok = false;
      out.kind = orch::StepErr::BadRequest;
      out.error = "request field over 64 KB (dropped while the request was built)";
      hlog::logf("wire: %s", out.error.c_str());
      return out;
    }

    JsonDocument filter;
    JsonObject ci = filter["content"].add<JsonObject>();
    ci["type"] = true; ci["id"] = true; ci["name"] = true; ci["input"] = true; ci["text"] = true;
    filter["stop_reason"] = true;
    filter["stop_details"]["category"] = true;   // a refusal names its category
    filter["error"]["message"] = true;
    filter["error"]["type"] = true;
    filter["usage"]["input_tokens"]  = true;   // per-round token usage (summed below)
    filter["usage"]["output_tokens"] = true;
    filter["usage"]["cache_read_input_tokens"]     = true;   // prompt-cache hits
    filter["usage"]["cache_creation_input_tokens"] = true;   // prompt-cache writes
    filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

    // 2) one request, and at most one retry after a cut with a body that differs
    //    (more room, one effort step down) - never the identical body.
    for (int attempt = 0; attempt < 2; ++attempt) {
      out = orch::HeadStep();
      const uint32_t maxTokens = attempt ? cap.retryTokens : cap.maxTokens;
      const AntLoopReq a{&model, &parts, &capReason, &ht, allowTools, maxTokens,
                         attempt ? cap.retryEffort : cap.effort};
      std::string body;
      BodySplices splices;
      if (!antLoopBody(pd, a, msgs, body, out.error) ||
          (imgs && !imageparts::spliceMarkers(body, *imgs, splices, out.error))) {
        out.ok = false;
        out.kind = orch::StepErr::BadRequest;
        return out;
      }
      // 3) send - the body is serialized ONCE into a contiguous buffer and handed
      //    to the transport whole (single TLS write); the photos are spliced in at
      //    send time.
      JsonDocument resp = makeDoc(pd);
      const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
      const int code = antRequest(pd, "POST", "/v1/messages", std::move(body), resp, filter, roundMs,
                                  imageparts::anyFileRef(imgs) ? kBetaHeadWithFiles : kBetaManagedAgents,
                                  splices.empty() ? nullptr : &splices);
      if (code <= 0) { wire::transportFailure(pd, t0, roundMs, out); return out; }
      if (code != 200 && imgs && imageparts::refusedImages(code, resp["error"]["message"] | "") &&
          imageparts::rejectAndRetry(imgs, "orchTurn(ant-loop)", code)) {
        // The provider refused the photos themselves: this round again, reading their
        // transcriptions (the fallback captioner), and so does the rest of the turn.
        imgs = nullptr;
        render();
        --attempt;
        continue;
      }
      if (code != 200) {
        const std::string e = wire::httpErrLine("messages", code, resp["error"]["message"] | "");
        wire::httpFailure(out, code, e, e + " " + (const char*)(resp["error"]["type"] | ""));
        hlog::logf("orchTurn(ant-loop): %s", e.c_str());
        return out;
      }
      if (usage) *usage += orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
      orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());
      JsonArrayConst content = resp["content"].as<JsonArrayConst>();
      if ((content.isNull() || content.size() == 0) && strcmp(resp["stop_reason"] | "", "max_tokens") != 0) {
        out.ok = false;
        out.kind = orch::StepErr::ServerError;   // an answer with nothing in it: worth one resend
        out.error = std::string("empty content (stop_reason=") +
                    (const char*)(resp["stop_reason"] | "?") + ")";
        return out;
      }
      const std::string sr = antReadRound(resp, out);
      // 4) the completeness gate: a reply cut at max_tokens - wherever it stopped,
      //    a finished orch_turn included - is never run and never delivered as
      //    complete (a half-written memory_write, a reply cut mid-sentence).
      if (sr == "max_tokens") {
        if (attempt == 0 && cap.hasRetry()) {
          hlog::logf("orchTurn(ant-loop): cut at %u tokens - one retry with %u (effort %s)",
                     (unsigned)maxTokens, (unsigned)cap.retryTokens,
                     *cap.retryEffort ? cap.retryEffort : "-");
          continue;
        }
        wire::cutFailure(out, "max_tokens", maxTokens);
        return out;
      }
      if (sr == "refusal") {
        out.ok = false;
        out.kind = orch::StepErr::Rejected;
        out.error = std::string("the model declined the request (refusal: ") +
                    (const char*)(resp["stop_details"]["category"] | "unspecified") + ")";
        hlog::logf("orchTurn(ant-loop): %s", out.error.c_str());
        return out;
      }
      if (sr == "model_context_window_exceeded") {
        out.ok = false;
        out.kind = orch::StepErr::Rejected;
        out.error = "the prompt is too long for the model's context window";
        return out;
      }
      if (out.finished || !out.toolCalls.empty() || out.text.empty()) return out;
      // Text only. On a tool round it is a stall (the controller gives the model
      // one more chance, with the stalled answer echoed back). On the forced final
      // round it IS the answer: delivered as prose (the engine reads it as the
      // reply only - no device action, memory write or spawn rides it).
      if (!allowTools && (sr == "end_turn" || sr == "stop_sequence")) {
        out.finished = true;
        out.prose = true;
        out.finalTurn = out.text;
        hlog::logf("orchTurn(ant-loop): final round answered in prose (%u B)",
                   (unsigned)out.text.size());
      }
      return out;
    }
    return out;   // not reached: the second attempt always returns
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
  const imageparts::LoopImages photos(tr);   // R1: the turn's photos ride the seeded message
  // CUM-477: one same-host resend of a request that never left (see openai.cpp).
  const orch::HeadStepFn inner = antLoopStep(pd, instructions, ht, usage, tr);
  const ResendCtx resend{&pd, &ht.cfg, &inner, "orchTurn(ant-loop)"};
  orch::HeadStepFn step = resendUnsentOnce(&resend);

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
  if (!res.ok) return wire::loopFailed(res, outJson, err);   // CUM-621: a cut keeps its partial turn
  outJson = res.finalTurn;
  convId  = "messages";   // stateless marker (state lives in the turn's memory field)
  return true;
}

// ---- single-shot head turn and the compaction fold ---------------------------
// One request, no tool loop. A head turn offers the orch_turn tool with
// tool_choice auto (NEVER forced: Sonnet 5.5, Opus 5.5 and Fable 5.1 answer a
// forced tool_choice with HTTP 400 - verified live 18 Oct) and accepts either the
// orch_turn input or a prose answer (the engine reads prose as the reply only: no
// device action, memory write or spawn rides it). The compaction fold (its
// instructions ARE ORCH_COMPACT_PROMPT) offers no tool at all: its reply IS the
// summary, so the request carries no 10 KB schema and the summary comes back as
// text. Both take max_tokens and effort from the output-cap table, and a reply cut
// at the limit is retried once with more room before it fails.
//
// STATELESS BY DESIGN: the turn contract carries all cross-turn state in the
// `memory` field + the World context re-composed each turn, so no server-side
// session is needed. convId is set to a marker so the caller's per-host
// conversation bookkeeping stays consistent.
namespace {
struct AntShot {
  const std::string* model;
  const std::string* instructions;
  const std::string* inputs;
  bool fold;
  uint32_t maxTokens;
  const char* effort;
};

bool antShotBody(const AntShot& a, std::string& body, std::string& err) {
  JsonDocument d;
  d["model"] = *a.model;
  d["max_tokens"] = a.maxTokens;
  if (a.effort && *a.effort) d["output_config"]["effort"] = a.effort;
  // Prompt caching (prompt_layout.h) - same block shape as the loop path. A
  // single-shot turn caches the static and summary blocks for the NEXT turn
  // (hits when the owner replies within the 5-minute TTL; refreshed on every
  // hit); the per-turn block does not. The compaction prompt is unsplit: one
  // block with its own cache point.
  {
    std::string sys;
    wire::putFieldStr(sys, *a.instructions, "the system prompt");
    layout::putAnthropicSystem(d["system"].to<JsonArray>(), layout::split(sys));
  }
  JsonObject m = d["messages"].add<JsonObject>();
  m["role"] = "user";
  wire::putFieldStr(m["content"], *a.inputs, "the user message");
  if (!a.fold) {
    JsonObject tool = d["tools"].add<JsonObject>();
    tool["name"] = "orch_turn";
    tool["description"] = "Return your complete orchestrator turn.";
    // NO strict:true - VERIFIED LIVE (2026-07-06): Anthropic's strict tool-use
    // compiles the schema to a grammar with a hard size budget, and this contract
    // exceeds it even with descriptions stripped ("compiled grammar is too
    // large"). So the schema is ADVISORY; real enforcement is the device
    // validator + tolerant parser.
    JsonDocument sd;   // canonical schema (single source)
    if (deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
                        DeserializationOption::NestingLimit(16))) {   // schema depth > the default 10
      err = "schema parse";
      return false;
    }
    stripDescriptions(sd.as<ArduinoJson::JsonVariant>());   // fit the grammar budget
    tool["input_schema"] = sd;
    d["tool_choice"]["type"] = "auto";   // never forced (see above)
    tool["cache_control"]["type"] = "ephemeral";   // caches the tools prefix segment
  }
  return wire::serializeChecked(d, body, err);
}

// What one single-shot reply holds: the orch_turn input (the turn), else the
// prose answer, and whether it was cut at max_tokens.
struct AntShotReply {
  std::string turn;
  bool cut = false;
  std::string stop;
};
AntShotReply antReadShot(const JsonDocument& doc) {
  AntShotReply r;
  r.stop = doc["stop_reason"] | "";
  r.cut = r.stop == "max_tokens";
  std::string prose;
  for (JsonObjectConst c : doc["content"].as<JsonArrayConst>()) {
    const char* ty = c["type"] | "";
    if (!strcmp(ty, "text")) {
      const char* t = c["text"] | "";
      if (*t) prose += (prose.empty() ? "" : "\n") + std::string(t);
    } else if (!strcmp(ty, "tool_use") && !strcmp(c["name"] | "", "orch_turn") && r.turn.empty()) {
      serializeJson(c["input"], r.turn);   // the tool input IS the turn
    }
  }
  // A complete prose answer is the reply (the engine reads it as prose only).
  if (r.turn.empty() && !r.cut && (r.stop == "end_turn" || r.stop == "stop_sequence")) r.turn = prose;
  return r;
}
}  // namespace

bool orchTurnAnthropic(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err,
                       const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  outJson.clear(); err.clear();
  if (!pd.key || pd.key("anthropic").empty()) { err = "no Anthropic key"; return false; }

  // Multi-turn tool-use loop (opt-in): the model calls registry tools mid-turn and
  // iterates, terminating on the orch_turn tool. Falls through to the single shot
  // below when the knob is off or no tools were supplied.
  if (tools && tools->dispatch && pd.toolLoopOn && pd.toolLoopOn())
    return runAntLoop(pd, convId, instructions, inputs, outJson, err, *tools, usage);

  const std::string model = pd.orchModel ? pd.orchModel("anthropic") : std::string();
  const bool fold = instructions == nimbus::orch::ORCH_COMPACT_PROMPT;
  const OutputCap cap = outputCapFor("anthropic", model, fold ? RoundKind::Fold : RoundKind::SingleShot,
                                     kHeadRoundCeilingMs);
  // Keep the tool_use input UNFILTERED (it is the whole turn object); filter the
  // rest of the response envelope down to what we read.
  JsonDocument filter;
  JsonObject ci = filter["content"].add<JsonObject>();
  ci["type"] = true; ci["name"] = true; ci["input"] = true; ci["text"] = true;
  filter["stop_reason"] = true;
  filter["error"]["message"] = true;
  filter["error"]["type"] = true;
  filter["usage"]["input_tokens"]  = true;   // token usage (single-shot turn)
  filter["usage"]["output_tokens"] = true;
  filter["usage"]["cache_read_input_tokens"]     = true;   // prompt-cache hits
  filter["usage"]["cache_creation_input_tokens"] = true;   // prompt-cache writes
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  // A single shot shows no photo: the turn's photos read as their transcriptions (R1).
  const std::string seen = fold ? inputs : imageparts::blindInputs(inputs);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const AntShot shot{&model, &instructions, &seen, fold,
                       attempt ? cap.retryTokens : cap.maxTokens, attempt ? cap.retryEffort : cap.effort};
    std::string body;
    if (!antShotBody(shot, body, err)) return false;
    JsonDocument doc = makeDoc(pd);   // response doc -> PSRAM (retained turn content)
    const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
    const int code = antRequest(pd, "POST", "/v1/messages", std::move(body), doc, filter);
    if (code <= 0)   { err = wire::headTransportError(pd, t0, kHeadRoundCeilingMs); return false; }
    if (code != 200) {
      err = wire::httpErrLine("messages", code, doc["error"]["message"] | "");
      hlog::logf("orchTurn(ant): %s", err.c_str());
      return false;
    }
    if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
    nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());
    const AntShotReply r = antReadShot(doc);
    if (r.cut && attempt == 0 && cap.hasRetry()) {
      hlog::logf("orchTurn(ant): %s cut at %u tokens - one retry with %u (effort %s)",
                 fold ? "fold" : "turn", (unsigned)cap.maxTokens, (unsigned)cap.retryTokens,
                 *cap.retryEffort ? cap.retryEffort : "-");
      continue;
    }
    if (r.cut) {
      err = "cut at the output limit (" + std::to_string(shot.maxTokens) + " tokens)";
      return false;
    }
    if (r.turn.empty()) {
      // stop_reason explains the miss (a refusal, an empty reply) - surface it
      // instead of a blind "no tool_use".
      err = "no orch_turn tool_use (stop_reason=" + (r.stop.empty() ? std::string("?") : r.stop) + ")";
      return false;
    }
    outJson = r.turn;
    convId = "messages";   // stateless host: marker keeps per-host conv bookkeeping happy
    return true;
  }
  return false;   // not reached: the second attempt always returns
}

}  // namespace providers
}  // namespace agent
