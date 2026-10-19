#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/head_loop.h"    // runHeadLoop - the portable ReAct controller
#include "nimbus/orch/model_catalog.h" // gptGeneration - the reasoning-family gate
#include "nimbus/orch/orch_schema.h"  // ORCH_SCHEMA_BODY - the wire contract
#include "nimbus/orch/token_usage_json.h"  // tokenUsageFromJson - per-round usage
#include "nimbus/orch/transcript.h"   // Transcript - the canonical turn record (Stage 2)
#include "wire.h"

// OpenAI Responses API - the PORTABLE wire half of the pre-split
// src/agent/adapters/openai_adapter.cpp (Stage H). Wire logic moved verbatim
// into std::string space over HttpTransport; the socket/TLS mechanics live in
// the device transport.
//   dispatch: POST /v1/responses {background:true, store:true} -> resp_... (fast)
//   poll:     GET  /v1/responses/{id} -> status + output[].content[].text
//   cancel:   POST /v1/responses/{id}/cancel
// The agent loop runs on OpenAI's side; the device sends/receives small requests.
// Response bodies are filter-parsed with ArduinoJson (only the wanted fields
// are kept), same filters as pre-split.
//
// LIVE-GATED on device: needs the OpenAI key (web UI / NVS) + provisioned STA WiFi.

namespace agent {
namespace providers {

// ⚠ 120 s, not 30 (field 2026-08-11): /v1/responses is NON-STREAMED - nothing
// arrives until the model finishes, and a reasoning model on a substantive
// prompt (the compaction fold, a long synthesis) routinely thinks past 30 s.
// At 30 s every such request died as "no response" while GET /v1/models and
// quick turns passed, so the failure looked like a flaky network instead of a
// deadline. The transport's wall clock covers connect+write+read in one budget.
// The value lives in the one time policy (providers.h).
static const uint32_t OAI_TIMEOUT_MS = kHeadRoundCeilingMs;
static const char* kOpenAIHost = "api.openai.com";  // mirrors agent_config.h OPENAI_HOST

using wire::exchange;
using wire::makeDoc;
using wire::serializeBody;

static std::vector<std::pair<std::string, std::string>> oaiHeaders(const std::string& key) {
  return {{"Authorization", "Bearer " + key}, {"Content-Type", "application/json"}};
}

// One OpenAI Responses (POST /v1/responses) exchange, rerouted through the Cumulo
// router when `pd` carries the override (no-op otherwise). Centralizes the
// applyRouter rewrite so the head loop and the single-shot head share ONE path.
static int oaiResponses(const ProviderDeps& pd, const std::string& key, std::string body,
                        uint32_t timeoutMs, JsonDocument& resp, const JsonDocument& filter) {
  wire::UpstreamReq req{kOpenAIHost, 443, true, "/v1/responses", oaiHeaders(key)};
  wire::applyRouter(pd, "openai", req);
  return exchange(pd, req.host.c_str(), req.port, req.tls, "POST", req.path,
                  std::move(req.headers), std::move(body), timeoutMs, resp, filter);
}

// Glass Box A4 (OpenAI reasoning capture): the Responses `reasoning` parameter and
// the reasoning-summary output items are ONLY valid on the reasoning families
// (o-series, gpt-5 and newer) - sending `reasoning` to a chat model (gpt-4o/gpt-4.1)
// is a hard 400 that would fail EVERY turn. Gate the request on the model name so
// a non-reasoning model behaves exactly as before. Matches "o1"/"o3"/"o4"/"o5"(-*)
// and every "gpt-<N>" generation from 5 up (gpt-5.x, gpt-6-astra, ...): a
// reasoning model under store:false also NEEDS the encrypted reasoning items
// replayed with its function calls, so a new generation left out of this gate
// would 400 on round 2 of every tool-calling turn rather than merely lose its
// summaries. The generation parse is the shared nimbus::orch::gptGeneration.
static bool oaiIsReasoningModel(const std::string& model) {
  // gpt-5+ EXCEPT the -chat variants (gpt-5-chat-latest etc.) - those are the
  // non-reasoning ChatGPT snapshots and hard-400 on the reasoning parameter.
  if (nimbus::orch::gptGeneration(model) >= 5) return model.find("chat") == std::string::npos;
  // o<N> or o<N>-...  (o1, o3-mini, o4-mini, ...). Require a digit after 'o' so
  // "openai"/"omni"-style names never match.
  if (model.size() >= 2 && model[0] == 'o' && model[1] >= '1' && model[1] <= '9') return true;
  return false;
}

static JobState mapStatus(const char* s) {
  if (!s) return JobState::Unknown;
  if (!strcmp(s, "completed"))   return JobState::Done;
  if (!strcmp(s, "failed"))      return JobState::Error;
  if (!strcmp(s, "incomplete"))  return JobState::Error;
  if (!strcmp(s, "cancelled"))   return JobState::Cancelled;
  if (!strcmp(s, "queued"))      return JobState::Queued;
  if (!strcmp(s, "in_progress")) return JobState::Running;
  return JobState::Unknown;
}

// Dispatch a background response.  jobId = "<backend>:<resp_id>".
FabricErr oaiDispatch(const ProviderDeps& pd, const char* host, const std::string& key,
                      const char* model, const char* backend, const Directive& d,
                      char outJobId[72]) {
  if (key.empty()) { hlog::logf("%s: no API key", backend); return FabricErr::Auth; }

  std::string instr = std::string("You are an autonomous ") +
                      (d.category ? d.category : "research") +
                      " agent. Complete the task fully and reply with the final result only.";
  std::string body;
  {
    JsonDocument doc;
    doc["model"]        = model;
    doc["background"]   = true;
    doc["store"]        = true;
    doc["instructions"] = instr;
    wire::putFieldStr(doc["input"], d.instruction ? std::string(d.instruction) : std::string(),
                      "the sub-agent task");
    // Give every OpenAI sub-agent the hosted web_search tool by default (matches
    // Anthropic's always-on sandbox) - a spawned research/ops agent with no web
    // access was the surprise in Roy's session ("I don't have live web access").
    // The tool is harmless if the task doesn't need it; the "web"/"research"/
    // "deep_research" skill hints are now a no-op superset.
    doc["tools"].to<JsonArray>().add<JsonObject>()["type"] = "web_search";
    // Owner-configured connectors/MCP servers (GitHub etc.) ride the sub-agent
    // too - OpenAI executes them server-side within the background response.
    if (pd.attachOpenAI) pd.attachOpenAI(doc);
    body = serializeBody(doc);
  }

  JsonDocument filter; filter["id"] = true;
  filter["error"]["message"] = true;   // a 429's text names its window (CUM-465)
  JsonDocument doc;
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  int code = exchange(pd, host, 443, true, "POST", "/v1/responses", oaiHeaders(key),
                      std::move(body), OAI_TIMEOUT_MS, doc, filter);
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code == 429) return wire::rateLimitedByText(d, backend, doc["error"]["message"] | "");
  // This POST creates the billed background job: a cut after it went out may have.
  if (code <= 0) return wire::subTransportErr(pd, t0, OAI_TIMEOUT_MS);
  if (code != 200)               { hlog::logf("%s: dispatch HTTP %d", backend, code); return FabricErr::RemoteFail; }

  const char* id = doc["id"] | "";
  if (!id[0]) return FabricErr::ParseFail;
  snprintf(outJobId, 72, "%s:%s", backend, id);
  hlog::logf("%s: dispatched %s", backend, id);
  return FabricErr::Ok;
}

// Poll a background response; fills env (state + reply/error on terminal).
FabricErr oaiPoll(const ProviderDeps& pd, const char* host, const std::string& key,
                  const char* backend, const char* jobId, ResultEnvelope& env) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  const char* id = colon + 1;

  strncpy(env.jobId,   jobId,   sizeof(env.jobId)   - 1);
  strncpy(env.backend, backend, sizeof(env.backend) - 1);

  // Filter: status, error.message, usage, and every output[].content[].{type,text}
  // + the container_file_citation annotations (W7b - a code_interpreter sub that
  // wrote a file cites it here; the device downloads it from the container).
  JsonDocument filter;
  filter["status"] = true;
  filter["error"]["message"] = true;
  filter["usage"] = true;   // real sub-session token usage -> spawn attribution
  JsonObject outItem  = filter["output"].add<JsonObject>();    // applies to all output items
  outItem["type"] = true;   // "*_call" items: the tools the sub actually ran (provenance)
  JsonObject contItem = outItem["content"].add<JsonObject>();  // applies to all content items
  contItem["type"] = true;
  contItem["text"] = true;
  JsonObject annItem = contItem["annotations"].add<JsonObject>();
  annItem["type"] = true;
  annItem["container_id"] = true;
  annItem["file_id"] = true;
  annItem["filename"] = true;

  JsonDocument doc;
  std::string path = std::string("/v1/responses/") + id;
  int code = exchange(pd, host, 443, true, "GET", path, oaiHeaders(key), "",
                      OAI_TIMEOUT_MS, doc, filter);
  if (code == 404)                return FabricErr::NotFound;   // expired
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code <= 0)                  return FabricErr::Network;     // transient
  if (code != 200)               { hlog::logf("%s: poll HTTP %d", backend, code); return FabricErr::RemoteFail; }

  env.state = mapStatus(doc["status"] | "");
  // Provenance from OpenAI's own record: every output item whose type ends in
  // "_call" is a tool the sub ran (web_search_call, mcp_call, code_interpreter_call,
  // file_search_call, function_call, ...); web_search_call is a web run.
  env.toolRuns = 0;
  env.webRuns = 0;
  for (JsonObjectConst item : doc["output"].as<JsonArrayConst>()) {
    const std::string ty = item["type"] | "";
    if (ty.size() < 5 || ty.compare(ty.size() - 5, 5, "_call") != 0) continue;
    if (env.toolRuns < 0xFFFF) env.toolRuns++;
    if (ty == "web_search_call" && env.webRuns < 0xFFFF) env.webRuns++;
  }
  // Real billed usage of the background response (present on terminal objects).
  {
    nimbus::orch::TokenUsage u =
        nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
    env.promptTokens     = u.promptTokens;
    env.completionTokens = u.completionTokens;
  }
  if (env.state == JobState::Done) {
    wire::ReplyWriter w(env.reply, sizeof(env.reply));
    // Over-cap file ids, remembered so REPEAT citations of the same uncaptured
    // file don't inflate the count (the model cites one file many times - the
    // note claimed "3 more files" when one was missed). Bounded, ids only.
    int dropped = 0;
    char droppedIds[4][48] = {};
    for (JsonObject item : doc["output"].as<JsonArray>()) {
      for (JsonObject c : item["content"].as<JsonArray>()) {
        // W7b: container_file_citation = a code_interpreter-produced file. Hand
        // it to the JobEngine as artifacts[] (url = "<container_id>/<file_id>",
        // the two halves of the /v1/containers download path; label = filename).
        // Dedup by file_id - the model often cites one file more than once.
        for (JsonObject a : c["annotations"].as<JsonArray>()) {
          if (strcmp(a["type"] | "", "container_file_citation") != 0) continue;
          const char* cid = a["container_id"] | "";
          const char* fid = a["file_id"] | "";
          if (!cid[0] || !fid[0]) continue;
          bool dup = false;
          for (int i = 0; i < env.artifactCount && !dup; i++)
            dup = strstr(env.artifacts[i].url, fid) != nullptr;
          if (dup) continue;
          if (env.artifactCount >= nimbus::orch::kMaxArtifacts) {
            for (int i = 0; i < dropped && i < 4; i++)
              if (strcmp(droppedIds[i], fid) == 0) { dup = true; break; }
            if (dup) continue;                       // already counted this file
            if (dropped < 4) snprintf(droppedIds[dropped], sizeof(droppedIds[0]), "%s", fid);
            dropped++;
            continue;
          }
          nimbus::orch::Artifact& art = env.artifacts[env.artifactCount++];
          snprintf(art.type, sizeof(art.type), "file");
          snprintf(art.url, sizeof(art.url), "%s/%s", cid, fid);
          snprintf(art.label, sizeof(art.label), "%s", (const char*)(a["filename"] | ""));
        }
        if (strcmp(c["type"] | "", "output_text") != 0) continue;
        const char* t = c["text"] | "";
        w.append(t, strlen(t));   // cut UTF-8-safe with a visible marker at 16 KB
      }
    }
    if (dropped > 0) {
      char note[96];
      snprintf(note, sizeof(note),
               "\n[note: %d more generated file(s) beyond the %d-file capture "
               "limit were NOT captured]", dropped, nimbus::orch::kMaxArtifacts);
      // The note is the ONLY channel that reports uncaptured files, so it must
      // never be the thing that gets dropped: the reply gives way instead.
      w.appendTail(note);
    }
    if (!env.reply[0]) strncpy(env.reply, "(completed, no text output)", sizeof(env.reply) - 1);
  } else if (env.state == JobState::Error) {
    const char* msg = doc["error"]["message"] | "agent failed";
    strncpy(env.error, msg, sizeof(env.error) - 1);
  }
  return FabricErr::Ok;
}

FabricErr oaiCancel(const ProviderDeps& pd, const char* host, const std::string& key,
                    const char* jobId) {
  const char* colon = strchr(jobId, ':');
  if (!colon) return FabricErr::BadRequest;
  JsonDocument filter; filter["status"] = true;
  JsonDocument doc;
  std::string path = std::string("/v1/responses/") + (colon + 1) + "/cancel";
  int code = exchange(pd, host, 443, true, "POST", path, oaiHeaders(key), "",
                      OAI_TIMEOUT_MS, doc, filter);
  return (code == 200) ? FabricErr::Ok : FabricErr::RemoteFail;
}

// ---- multi-turn tool-use loop (ReAct) ---------------------------------------
// The head model calls registry tools mid-turn and iterates, terminating when it
// calls the orch_turn FUNCTION tool (its arguments string IS the turn JSON). State
// is SERVER-SIDE via the previous_response_id chain - no growing local history -
// so per-round bodies stay small: instructions + tools (re-sent each round; the
// Responses API does not inherit either) + the round's function_call_output items.
// Strict text.format is dropped in loop mode (it conflicts with free tool choice);
// the schema still rides orch_turn's parameters with strict:true (ENFORCED).
// Render the full Responses input[] from the CANONICAL transcript (Stage 2
// phase 3 - the intentional wire change). The server-side previous_response_id
// chain is GONE (and with it the entire F20 chain-poisoning class): every round
// replays the whole turn - user seed, then per round the provider meta
// (reasoning items, verbatim), the function_call items, and their
// function_call_output answers. Prose/reasoning SUMMARIES are observed-only and
// never replayed (the encrypted reasoning items in `meta` are the replayable
// form a reasoning model requires under store:false).
static void renderOaiInput(const nimbus::orch::Transcript& tr, ArduinoJson::JsonArray in) {
  namespace orch = nimbus::orch;
  using Kind = orch::TranscriptItem::Kind;
  const auto& e = tr.entries();
  for (const auto& it : e) {
    if (it.kind != Kind::User) continue;
    JsonObject m = in.add<JsonObject>();
    m["type"] = "message"; m["role"] = "user";
    wire::putFieldStr(m["content"], it.text, "the user message");
    break;
  }
  size_t i = 0;
  while (i < e.size()) {
    if (e[i].kind == Kind::User) { i++; continue; }
    const int round = e[i].round;
    std::string meta;
    std::vector<size_t> calls, results;
    for (; i < e.size() && e[i].kind != Kind::User && e[i].round == round; i++) {
      if (meta.empty() && !e[i].meta.empty()) meta = e[i].meta;
      if (e[i].kind == Kind::ToolUse)         calls.push_back(i);
      else if (e[i].kind == Kind::ToolResult) results.push_back(i);
    }
    if (calls.empty()) continue;   // prose-only rounds are not replayed
    // Reasoning items first (the API requires each function_call's preceding
    // reasoning item under stateless replay). `meta` is the raw JSON array of
    // reasoning items captured from the response, encrypted_content included.
    if (!meta.empty()) {
      JsonDocument md;
      if (deserializeJson(md, meta) == DeserializationError::Ok &&
          md.is<JsonArrayConst>()) {
        for (JsonObjectConst ri : md.as<JsonArrayConst>()) in.add(ri);
      }
    }
    for (size_t ci : calls) {
      JsonObject fc = in.add<JsonObject>();
      fc["type"]      = "function_call";
      fc["call_id"]   = e[ci].id;
      fc["name"]      = e[ci].name;
      wire::putModelStr(fc["arguments"], e[ci].text, e[ci].name.c_str());
    }
    for (size_t ri : results) {
      JsonObject o = in.add<JsonObject>();
      o["type"]    = "function_call_output";
      o["call_id"] = e[ri].id;
      // No is_error field on function_call_output - encode failure in the text.
      wire::putModelStr(o["output"],
                        e[ri].isError ? (std::string("ERROR: ") + e[ri].text) : e[ri].text,
                        e[ri].name.c_str());
    }
  }
}

// STEP FACTORY (Stage 2 phase 5): one OpenAI Responses model turn against the
// shared canonical transcript. Same lifetime contract as antLoopStep -
// `ht`/`usage`/`tr` by reference, caller outlives the loop; the split
// `instructions` are copied.
nimbus::orch::HeadStepFn oaiLoopStep(const ProviderDeps& pd,
                                     const std::string& instructions,
                                     const HeadTools& ht,
                                     nimbus::orch::TokenUsage* usage,
                                     nimbus::orch::Transcript& tr) {
  namespace orch = nimbus::orch;
  const std::string model = pd.orchModel ? pd.orchModel("openai") : std::string();
  const std::string key   = pd.key ? pd.key("openai") : std::string();
  // The wire-ceiling bound: the replayed tool outputs must stay under the
  // measured ~52 KB body ceiling. trimToolOutputs clips only the NEWEST results
  // (never a round already sent, so each round extends the cached prefix) - the
  // pairing invariant holds and the full text is already in the results ring.
  const size_t kReplayToolBytes = 24576;

  // The static, summary and dynamic parts, split once per turn (prompt_layout.h)
  // from the prompt repaired to valid UTF-8 (wire.h: repaired, never clipped).
  std::string sys;
  wire::putFieldStr(sys, instructions, "the system prompt");
  const layout::PromptParts parts = layout::split(sys);

  return [&ht, usage, &tr, pd, model, key, kReplayToolBytes, parts](
             bool allowTools,
             const std::vector<orch::HeadToolResult>& prior,
             uint32_t budgetMs,
             const std::string& capReason) -> orch::HeadStep {
    namespace orch = nimbus::orch;
    (void)prior;   // the transcript already carries every result
    orch::HeadStep out;
    // F25: clamp this round's socket timeout to the turn's remaining budget so
    // slow rounds can't stack past the deadline (no-op when budgetMs==UINT32_MAX).
    const uint32_t roundMs = wire::clampRoundMs(budgetMs, kHeadRoundCeilingMs);
    if (tr.toolBytes() > kReplayToolBytes) tr.trimToolOutputs(kReplayToolBytes);
    JsonDocument req = makeDoc(pd);
    req["model"]        = model;
    // roundInstructions() appends the final-round notice when a cap forced this
    // tool-less round, so the model says what it could not finish instead of
    // promising work that will never run. It rides the per-turn part, so the
    // static developer message still reads from the cache. An unsplit prompt
    // keeps the top-level instructions field.
    layout::PromptParts round = parts;
    std::string& tail = round.split ? round.dyn : round.stat;
    tail = wire::roundInstructions(tail, capReason);
    if (!round.split) wire::putFieldStr(req["instructions"], round.stat, "the system prompt");
    req["store"]        = false;   // stateless: the device owns the turn's state
    // Glass Box A4: ask reasoning models for a summary of their thinking so it
    // can be captured into HeadStep.text (gated - a chat model would 400 on the
    // parameter). include: the ENCRYPTED reasoning items ride the response so
    // they can be replayed next round (required for reasoning models with
    // store:false between tool calls).
    if (oaiIsReasoningModel(model)) {
      req["reasoning"]["summary"] = "auto";
      req["include"].to<JsonArray>().add("reasoning.encrypted_content");
    }
    {
      JsonArray in = req["input"].to<JsonArray>();
      // A split prompt (prompt_layout.h): the static block (and the summary) as
      // developer messages, then the user seed led by the per-turn block. The
      // request then starts with the same bytes on every turn of a day.
      if (round.split) layout::putOpenAIDeveloper(in, round);
      renderOaiInput(tr, in);
      if (round.split)
        for (JsonObject m : in)
          if (!strcmp(m["role"] | "", "user")) {
            wire::putFieldStr(m["content"], layout::userWithDynamic(round, m["content"] | ""),
                              "the user message");
            break;
          }
      // Forced final after a STALL: if the newest round carried no tool calls
      // (prose-only - not rendered), this request would be byte-identical to the
      // one the model just stalled on; nudge with a user message so it answers
      // now. (Matches the old prior.empty() nudge - a bytes-cap forced final,
      // whose newest round HAS calls+results, gets no nudge.)
      if (!allowTools) {
        int maxRound = -1; bool lastRoundHasCalls = false;
        for (const auto& it : tr.entries()) {
          if (it.kind == orch::TranscriptItem::Kind::User) continue;
          if (it.round > maxRound) { maxRound = it.round; lastRoundHasCalls = false; }
          if (it.round == maxRound && it.kind == orch::TranscriptItem::Kind::ToolUse)
            lastRoundHasCalls = true;
        }
        if (!lastRoundHasCalls) {
          JsonObject m = in.add<JsonObject>();
          m["type"] = "message"; m["role"] = "user";
          m["content"] = "Now call orch_turn with your final complete turn.";
        }
      }
    }

    // tools: orch_turn always (the terminal); registry specs on tool rounds.
    JsonArray tools = req["tools"].to<JsonArray>();
    {
      JsonObject ot = tools.add<JsonObject>();     // Responses function tools are FLAT
      ot["type"]        = "function";
      ot["name"]        = "orch_turn";
      ot["description"] = "Return your COMPLETE orchestrator turn. This is your FINAL "
                          "answer and ENDS the turn - call it once you have everything.";
      ot["strict"]      = true;                    // schema is strict-valid (proven live)
      JsonDocument sd = makeDoc(pd);
      if (deserializeJson(sd, orch::ORCH_SCHEMA_BODY,
                          DeserializationOption::NestingLimit(16))) {
        out.ok = false; out.error = "schema parse"; return out;
      }
      ot["parameters"] = sd;
    }
    if (allowTools) {
      for (const auto& s : ht.specs) {
        JsonObject t = tools.add<JsonObject>();
        t["type"] = "function";
        t["name"] = s.name;
        t["description"] = s.description;
        t["strict"] = false;                       // registry schemas aren't strict-shaped
        JsonDocument ssd = makeDoc(pd);
        if (deserializeJson(ssd, s.schemaJson,
                            DeserializationOption::NestingLimit(16)) == DeserializationError::Ok)
          t["parameters"] = ssd;
        else
          t["parameters"].to<JsonObject>();
      }
      // "required" (not "auto"): the model must call SOME tool every round - either
      // a registry tool or the terminal orch_turn - so text-only stall rounds can't
      // happen (observed live with auto: a post-dispatch round answered in prose and
      // burned the stall-recovery path). orch_turn is the only exit, so termination
      // is unchanged.
      req["tool_choice"] = "required";
      if (pd.attachOpenAI) pd.attachOpenAI(req);   // hosted MCP still runs server-side
    } else {
      JsonObject tc = req["tool_choice"].to<JsonObject>();
      tc["type"] = "function"; tc["name"] = "orch_turn";   // forced final round
    }

    JsonDocument filter;
    filter["id"] = true; filter["status"] = true;
    filter["error"]["message"] = true;
    filter["error"]["code"] = true;            // insufficient_quota, invalid_json, ...
    filter["error"]["type"] = true;
    filter["incomplete_details"]["reason"] = true;   // the completeness gate
    JsonObject oi = filter["output"].add<JsonObject>();
    oi["type"] = true; oi["call_id"] = true; oi["name"] = true; oi["arguments"] = true;
    oi["id"] = true;                    // reasoning item id - required in the replay
    oi["encrypted_content"] = true;     // the replayable reasoning payload (include'd above)
    JsonObject ci = oi["content"].add<JsonObject>();
    ci["type"] = true; ci["text"] = true;
    // Reasoning-summary items ({"type":"reasoning","summary":[{type,text}]}) -
    // retained only when requested above; harmless in the filter otherwise.
    JsonObject si = oi["summary"].add<JsonObject>();
    si["type"] = true; si["text"] = true;
    filter["usage"] = true;   // whole usage object -> per-round token accounting
    filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

    std::string body;
    if (!wire::serializeChecked(req, body, out.error)) { out.ok = false; return out; }
    JsonDocument resp = makeDoc(pd);
    const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
    int code = oaiResponses(pd, key, std::move(body), roundMs, resp, filter);
    if (code <= 0) { wire::transportFailure(pd, t0, roundMs, out); return out; }
    if (code != 200) {
      const std::string e = wire::httpErrLine("resp", code, resp["error"]["message"] | "");
      wire::httpFailure(out, code, e,
                        e + " " + (const char*)(resp["error"]["code"] | "") + " " +
                            (const char*)(resp["error"]["type"] | ""));
      hlog::logf("orchTurn(oai-loop): %s", e.c_str());
      return out;
    }
    if (usage) *usage += nimbus::orch::tokenUsageFromJson(resp["usage"].as<ArduinoJson::JsonObjectConst>());
    nimbus::orch::captureServedModel(usage, resp.as<ArduinoJson::JsonVariantConst>());
    // The completeness gate: an incomplete response (the output limit, a content
    // filter) is never run or accepted - a cut `arguments` string would be
    // dispatched as invalid JSON, or a cut orch_turn read as the turn. There is no
    // cap to raise here (no max_output_tokens is sent: reasoning would starve the
    // answer), so it fails as Cut at once and the fabric may switch.
    const char* st = resp["status"] | "";
    if (!strcmp(st, "incomplete")) {
      wire::cutFailure(out, std::string("incomplete:") +
                                (const char*)(resp["incomplete_details"]["reason"] | "unknown"), 0);
      return out;
    }
    if (!strcmp(st, "failed")) {
      out.ok = false;
      out.kind = nimbus::orch::StepErr::ServerError;
      out.error = std::string("resp failed: ") + (const char*)(resp["error"]["message"] | "no reason given");
      return out;
    }

    // Walk EVERY function_call before deciding: the model may batch registry calls
    // WITH the terminal orch_turn in one response (tool_choice:"required" invites
    // it). CUM-506: the siblings are surfaced, before or after orch_turn, and the
    // controller runs them ONCE before accepting the final turn - dropping them
    // lost five memory writes while the reply said "saved permanently". (With
    // the stateless replay there is no chain to poison - the old F20 class is gone.)
    bool sawOrchTurn = false;
    for (JsonObjectConst item : resp["output"].as<JsonArrayConst>()) {
      // Reasoning items: summaries -> HeadStep.text (Glass Box A4, observed by
      // onText); the RAW item (id + encrypted_content) -> HeadStep.meta so the
      // next round's stateless replay can present it before its function_call
      // (required for reasoning models with store:false).
      if (strcmp(item["type"] | "", "reasoning") == 0) {
        for (JsonObjectConst s : item["summary"].as<JsonArrayConst>()) {
          const char* txt = s["text"] | "";
          if (txt[0]) {
            if (!out.text.empty()) out.text += "\n";
            out.text += txt;
          }
        }
        if ((item["encrypted_content"] | (const char*)nullptr) != nullptr) {
          std::string raw; serializeJson(item, raw);
          if (out.meta.empty()) out.meta = "[" + raw + "]";
          else out.meta.insert(out.meta.size() - 1, "," + raw);
        }
        continue;
      }
      if (strcmp(item["type"] | "", "function_call") != 0) continue;
      const char* nm   = item["name"] | "";
      const char* args = item["arguments"] | "";   // a JSON STRING
      if (!strcmp(nm, "orch_turn")) {
        if (!sawOrchTurn) out.finalTurn = (args && args[0]) ? args : "";
        sawOrchTurn   = true;
        out.finished  = true;
        continue;                                   // keep scanning for siblings
      }
      orch::HeadToolCall call;                      // a sibling of orch_turn, or
      call.id       = item["call_id"] | "";         // an ordinary tool round
      call.name     = nm;
      call.argsJson = (args && args[0]) ? args : "{}";
      out.toolCalls.push_back(call);
    }
    return out;   // finished (siblings ride along), a tool round, or text-only
  };
}

static bool runOaiLoop(const ProviderDeps& pd, std::string& convId,
                       const std::string& instructions, const std::string& inputs,
                       std::string& outJson, std::string& err, const HeadTools& ht,
                       nimbus::orch::TokenUsage* usage) {
  namespace orch = nimbus::orch;
  // The CANONICAL transcript (Stage 2): the controller records prose/calls/
  // results into it; every round's input[] is rendered fresh from it. Stateless
  // by construction - no previous_response_id, no store, no chain to poison.
  orch::Transcript tr;
  tr.addUser(inputs);
  // CUM-477: a request the device could not finish sending is resent once on the
  // same host after the heap recovers (the Cumulo router head runs this loop).
  const orch::HeadStepFn inner = oaiLoopStep(pd, instructions, ht, usage, tr);
  const ResendCtx resend{&pd, &ht.cfg, &inner, "orchTurn(oai-loop)"};
  orch::HeadStepFn step = resendUnsentOnce(&resend);

  orch::HeadLoopHooks hooks;
  hooks.step     = step;
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m) { hlog::logf("%s", m.c_str()); };
  hooks.onText   = ht.onRoundText;   // round prose / reasoning summaries -> engine
  hooks.spill    = ht.spill;         // clamped-result spill -> results ring
  hooks.transcript = &tr;            // the controller records; step() renders

  orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("orchTurn(oai-loop): rounds=%d cap=%s ok=%d heap=%u",
             res.rounds, res.capReason.c_str(), (int)res.ok,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the
  // failure return - a failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) { err = res.error.empty() ? std::string("loop failed") : res.error; return false; }
  outJson = res.finalTurn;
  convId  = "responses";   // stateless marker - the device owns the turn's state
  return true;
}

bool orchTurnOpenAI(const ProviderDeps& pd, std::string& convId,
                    const std::string& instructions, const std::string& inputs,
                    std::string& outJson, std::string& err,
                    const HeadTools* tools, nimbus::orch::TokenUsage* usage) {
  outJson.clear(); err.clear();
  const std::string key = pd.key ? pd.key("openai") : std::string();
  if (key.empty()) { err = "no OpenAI key"; return false; }

  // Multi-turn tool-use loop (opt-in): falls through to the single-shot strict
  // structured turn below when the knob is off or no tools were supplied.
  if (tools && tools->dispatch && pd.toolLoopOn && pd.toolLoopOn())
    return runOaiLoop(pd, convId, instructions, inputs, outJson, err, *tools, usage);

  std::string body;
  {
    JsonDocument d;
    d["model"]        = pd.orchModel ? pd.orchModel("openai") : std::string();  // per-provider orchestrator model
    // A split prompt (prompt_layout.h): developer messages for the static block
    // and the summary, then the per-turn block leading the user message.
    std::string sys;
    wire::putFieldStr(sys, instructions, "the system prompt");
    const layout::PromptParts parts = layout::split(sys);
    if (parts.split) {
      JsonArray in = d["input"].to<JsonArray>();
      layout::putOpenAIDeveloper(in, parts);
      JsonObject u = in.add<JsonObject>();
      u["type"] = "message";
      u["role"] = "user";
      wire::putFieldStr(u["content"], layout::userWithDynamic(parts, inputs), "the user message");
    } else {
      d["instructions"] = sys;
      wire::putFieldStr(d["input"], inputs, "the user message");
    }
    d["store"]        = true;
    if (convId.length()) d["previous_response_id"] = convId;   // stateful chain
    // STRICT structured output: the orch_turn schema (single source, now a
    // device[] discriminated union so strict mode accepts it) rides the request
    // - the API validates the shape, no "respond with ONLY JSON" prose needed.
    d["text"]["format"]["type"]   = "json_schema";
    d["text"]["format"]["name"]   = "orch_turn";
    d["text"]["format"]["strict"] = true;
    {
      JsonDocument sd;                       // parse the canonical schema string
      DeserializationError se = deserializeJson(sd, nimbus::orch::ORCH_SCHEMA_BODY,
          DeserializationOption::NestingLimit(16));  // schema depth > the default 10
      if (se) { err = "schema parse"; return false; }   // impossible unless the header regresses
      d["text"]["format"]["schema"] = sd;
    }
    // Hosted connectors/MCP run SERVER-SIDE within the one response, so the head
    // turn can use them mid-turn even with a strict json_schema text format.
    if (pd.attachOpenAI) pd.attachOpenAI(d);
    if (!wire::serializeChecked(d, body, err)) return false;
  }

  JsonDocument filter;
  filter["id"] = true; filter["status"] = true;
  filter["error"]["message"] = true;   // surface schema/param rejections verbatim
  filter["error"]["code"] = true;
  filter["incomplete_details"]["reason"] = true;   // the completeness gate
  JsonObject oi = filter["output"].add<JsonObject>();
  JsonObject ci = oi["content"].add<JsonObject>();
  ci["type"] = true; ci["text"] = true;
  filter["usage"] = true;   // whole usage object -> token accounting
  filter["model"] = true;   // served model echo -> fallback disclosure (CUM-236)

  JsonDocument doc = makeDoc(pd);   // response doc -> PSRAM (retained turn content)
  const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
  int code = oaiResponses(pd, key, std::move(body), OAI_TIMEOUT_MS, doc, filter);
  if (code <= 0)   { err = wire::headTransportError(pd, t0, OAI_TIMEOUT_MS); return false; }
  if (code != 200) {
    err = wire::httpErrLine("resp", code, doc["error"]["message"] | "");
    const char* ec = doc["error"]["code"] | "";
    if (ec[0] && !strstr(err.c_str(), ec)) err += std::string(" (") + ec + ")";
    hlog::logf("orchTurn(oai): %s", err.c_str());
    return false;
  }
  if (usage) *usage += nimbus::orch::tokenUsageFromJson(doc["usage"].as<ArduinoJson::JsonObjectConst>());
  nimbus::orch::captureServedModel(usage, doc.as<ArduinoJson::JsonVariantConst>());
  if (!strcmp(doc["status"] | "", "incomplete")) {   // a cut answer is never the turn
    err = std::string("cut at the output limit (incomplete:") +
          (const char*)(doc["incomplete_details"]["reason"] | "unknown") + ")";
    hlog::logf("orchTurn(oai): %s", err.c_str());
    return false;
  }

  // No cross-turn chain head, matching the loop path. The server-side chain is a
  // DUPLICATE of history this device already sends: the system prompt carries the
  // conversation summary (v3.6.0 fold) and the per-chat recent window (B1) on
  // every turn. Keeping it bought nothing and cost twice - OpenAI re-bills the
  // replayed context as input tokens (measured on Board 1: a single-shot turn
  // whose whole prompt is ~21.9 KB / ~5.5 K tokens was billed 16,897), and a
  // stored id is the thing that goes stale and 400s the next turn.
  convId.clear();
  for (JsonObjectConst item : doc["output"].as<JsonArrayConst>()) {
    for (JsonObjectConst c : item["content"].as<JsonArrayConst>()) {
      if (strcmp(c["type"] | "", "output_text") == 0) outJson = (const char*)(c["text"] | "");
    }
  }
  if (outJson.length() == 0) err = "no output_text";
  return outJson.length() > 0;
}

}  // namespace providers
}  // namespace agent
