#include "web_memory.h"
#include "../sys/ps_json.h"   // PSRAM-backed JsonDocument (episodic GET)
#include "nimbus/orch/orch_schema.h"  // ORCH_D_DEVICE - the Tools tab device rows can't drift

#include <ArduinoJson.h>
#include <SD.h>                            // SD.cardType() for /api/sdprobe
#include <memory>                          // unique_ptr - heap Info[] (kMaxConnectors)
#include <new>       // std::nothrow - alloc failure degrades, never panics
#include <solide/storage.h>                // solide::storage::begin() re-probe

#include "../agent/adapters/embeddings.h"
#include "../sfx/sound_fx.h"            // ::sfx::refreshConfig() after an SD promote
#include "../sys/agent_log.h"          // agentLogTail() for GET /api/log
#include "../agent/memory_subsystem.h"
#include "../agent/orchestrator.h"        // toolRidesLoop - /api/tools rides_loop flag (P7)
#include "../agent/connectors.h"          // provider connector catalog for /api/tools (P7)
#include "../agent/skills.h"              // skill capsules surfaced in /api/tools (P7)
#include "../agent/store.h"
#include "webui.h"                         // webAuthOk() - /mcp token gate
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/episodic_log.h"   // civilDate + EpiQueryInfo (deep history)
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/vector_memory.h"

namespace nimbus::net {

namespace mem = agent::memory;
using namespace nimbus::orch;

namespace {

void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// Gate a memory handler on the device token (prism). Returns true (and sends 401) if
// unauthenticated. Every route here uses it - reads included, since owner-batch-2 closed
// the open-GET surface; only GET / and /logo.svg are served without a token.
static bool authBlocked(AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  sendJson(r, 401, "{\"error\":\"Access token required. Scan the Sign-in QR on the device.\"}");
  return true;
}

String qparam(AsyncWebServerRequest* r, const char* name, const char* def = "") {
  if (r->hasParam(name)) return r->getParam(name)->value();
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  return def;
}

// ---- GET /api/mem/stats ----
void handleStats(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  mem::Stats s = mem::stats();
  JsonDocument d;
  d["vectors"]        = s.vectorCount;
  d["scratchItems"]   = s.scratchItems;
  d["episodicMsgs"]   = s.episodicMsgs;
  d["epiTruncated"]   = s.epiHydrateTruncated;
  d["epiFloor"]       = s.epiIndexFloorDay
                            ? nimbus::orch::AppendLogEpisodicStore::civilDate(
                                  (uint32_t)s.epiIndexFloorDay)
                            : std::string();
  d["embedAvailable"] = s.embedAvailable;
  d["embedLocked"]    = s.embedLocked;
  // Storage tier (docs/orchestrator-storage.md): where the bulk lives + the effective
  // cap, so the dashboard can show a "degraded - no SD" banner and the real ceiling.
  d["sdPresent"]      = s.sdPresent;
  d["flashFull"]      = s.flashFull;
  d["maxVectors"]     = s.maxVectors;
  d["store"]          = s.sdPresent ? "SD /mem" : "flash /data (no SD)";
  JsonObject e = d["embed"].to<JsonObject>();
  e["provider"] = agent::store::embedProvider();
  e["model"]    = agent::store::embedModel();
  e["dims"]     = agent::store::embedDims();
  String out; serializeJson(d, out);
  sendJson(r, 200, out);
}

// ---- GET /api/mem/vector?query=&limit= ----
void handleVectorGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  // Paginate: the store can hold thousands, so browse a page at a time (default
  // 10) and let search return only the closest handful. offset applies to browse.
  int limit = qparam(r, "limit", "10").toInt();
  if (limit <= 0 || limit > 100) limit = 10;
  int offset = qparam(r, "offset", "0").toInt();
  if (offset < 0) offset = 0;
  String query = qparam(r, "query");

  JsonDocument d;
  JsonArray arr = d["entries"].to<JsonArray>();

  // Embed the query FIRST, BEFORE taking the engine lock: embed() is a BLOCKING TLS
  // round-trip on the AsyncTCP task (~1-3 s, 20 s worst case) and must never be held
  // under mem::Lock (would stall the turn task for the whole handshake).
  std::vector<int8_t> qv;
  String err;
  if (query.length()) qv = agent::embeddings::embed(query, err);

  {
    mem::Lock lk;   // guard all VectorMemory access (shared with the turn task)
    if (query.length()) {
      if (qv.empty()) { d["error"] = String("embed: ") + err; }
      else {
        // Top matches only, already sorted closest-first by search() (ascending
        // cosine distance). A thousand-entry store still returns ~10 rows.
        int cap = limit < 10 ? limit : 10;
        for (const auto& h : mem::vectors().search(qv, cap)) {
          JsonObject o = arr.add<JsonObject>();
          o["id"] = h.id; o["content"] = h.content;
          o["importance"] = h.importance; o["distance"] = h.distance;
        }
        d["mode"] = "search";
      }
    } else {
      // Browse: getAll() is importance-desc; window [offset, offset+limit).
      const auto all = mem::vectors().getAll();
      for (int i = offset; i < (int)all.size() && i < offset + limit; i++) {
        const auto& e = all[(size_t)i];
        JsonObject o = arr.add<JsonObject>();
        o["id"] = e.id; o["content"] = e.content;
        o["importance"] = e.importance; o["permanent"] = e.permanentFlag;
        o["source"] = e.source; o["ttlHours"] = e.ttlHours;
        o["tsHours"] = e.createdAtHours;          // wall-hours (epoch/3600 once synced)
        o["lastRecallHours"] = e.lastRecallHours; // 0 = never recalled
      }
      d["mode"] = "browse";
      d["offset"] = offset;
      d["limit"] = limit;
    }
    d["total"] = mem::vectors().size();
  }
  String out; serializeJson(d, out);
  sendJson(r, 200, out);
}

// ---- POST /api/mem/vector  op=delete|flush|flushnp|dedupe|permanent|temporary ----
void handleVectorPost(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  String op = qparam(r, "op");
  String id = qparam(r, "id");
  mem::Lock lk;   // op + persist + size all touch VectorMemory (shared with the turn task)
  int n = 0; bool ok = true;
  if (op == "delete")         ok = mem::vectors().remove(std::string(id.c_str()));
  else if (op == "permanent") ok = mem::vectors().markPermanent(std::string(id.c_str()));
  else if (op == "temporary") ok = mem::vectors().markTemporary(std::string(id.c_str()));  // unpin
  else if (op == "flush") {
    // Enforce the typed confirm SERVER-side, not just in the browser: a stale
    // scripted tab or reused Config-QR link shouldn't be able to wipe every
    // memory (permanent ones included) on the token alone.
    if (qparam(r, "confirm") != "DELETE") { sendJson(r, 400, "{\"error\":\"confirm phrase required\"}"); return; }
    n = mem::vectors().flushAll();
  }
  else if (op == "flushnp")   n = mem::vectors().flushNonPermanent();
  else if (op == "dedupe")    n = mem::vectors().deduplicate();
  else { sendJson(r, 400, "{\"error\":\"unknown op\"}"); return; }
  mem::persistVectors();
  JsonDocument d; d["ok"] = ok; d["affected"] = n; d["total"] = mem::vectors().size();
  String out; serializeJson(d, out); sendJson(r, 200, out);
}

// ---- scratchpad ----
void handleScratchGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  mem::Lock lk;   // Scratchpad is shared with the turn task
  Scratchpad& sp = mem::scratchpad();
  JsonDocument d;
  d["active"] = sp.activeTask();
  const Tier tiers[3] = {Tier::Short, Tier::Mid, Tier::Long};
  const char* names[3] = {"short", "mid", "long"};
  for (int i = 0; i < 3; i++) {
    JsonArray a = d[names[i]].to<JsonArray>();
    for (const auto& it : sp.items(tiers[i])) a.add(it);
  }
  String out; serializeJson(d, out); sendJson(r, 200, out);
}

// POST /api/mem/scratchpad - proxied through the memory.scratchpad tool so the
// caps/validation are identical to the model's path. Body carries the tool args.
void handleScratchPost(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  mem::Lock lk;   // memory.scratchpad dispatch + persist mutate the shared Scratchpad
  JsonDocument args;
  args["action"]    = qparam(r, "action", "view");
  if (r->hasParam("tier", true)) args["tier"] = qparam(r, "tier");
  if (r->hasParam("text", true)) args["text"] = qparam(r, "text");
  // The web scratchpad panel is the token-authenticated owner surface.
  // Aggregate init would leave role = Role::Unknown (the struct's safe default)
  // and permsFor(Unknown) denies everything - this surface is the token-holding
  // owner, so build it through the helper that sets a role.
  const nimbus::orch::Principal who = nimbus::orch::principalForRole(
      "web", nimbus::orch::Role::Admin);
  ToolResult res = mem::registry().dispatch("memory.scratchpad", args.as<JsonObjectConst>(), who);
  mem::persistScratchpad();
  JsonDocument d; d["ok"] = res.success; d["msg"] = res.success ? res.output : res.error;
  String out; serializeJson(d, out); sendJson(r, res.success ? 200 : 400, out);
}

// ---- config ----
void handleConfigGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  MemConfig& c = mem::config();
  JsonDocument d;
  d["retrieval_count"]     = c.retrievalCount;
  d["relevance_threshold"] = c.relevanceThreshold;
  d["decay_factor"]        = c.decayFactor;
  d["max_context_bytes"]   = c.maxContextBytes;
  d["max_vectors"]         = c.maxVectors;
  d["recency_half_life_hours"] = c.recencyHalfLifeHours;
  d["mmr_lambda"]          = c.mmrLambda;
  String out; serializeJson(d, out); sendJson(r, 200, out);
}
void handleConfigPut(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  {
    mem::Lock lk;   // MemConfig is read by the turn task (recall); applyConfig touches the VDB
    MemConfig& c = mem::config();
    if (r->hasParam("retrieval_count", true))     c.setRetrievalCount(qparam(r, "retrieval_count").toInt());
    if (r->hasParam("relevance_threshold", true)) c.setRelevanceThreshold(qparam(r, "relevance_threshold").toFloat());
    if (r->hasParam("decay_factor", true))        c.setDecayFactor(qparam(r, "decay_factor").toFloat());
    if (r->hasParam("max_context_bytes", true))   c.setMaxContextBytes(qparam(r, "max_context_bytes").toInt());
    if (r->hasParam("max_vectors", true))         c.setMaxVectors(qparam(r, "max_vectors").toInt());
    if (r->hasParam("recency_half_life_hours", true)) c.setRecencyHalfLifeHours(qparam(r, "recency_half_life_hours").toInt());
    if (r->hasParam("mmr_lambda", true))          c.setMmrLambda(qparam(r, "mmr_lambda").toFloat());
    mem::applyConfig();   // push the (possibly new) cap into the VDB engine
  }
  mem::persistMemConfig();   // survive reboot (the knobs used to silently reset)
  handleConfigGet(r);
}

// ---- episodic ----
void handleEpisodicGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  mem::Lock lk;   // episodic query/count shared with the turn task's captureMessage
  MsgQuery q;
  q.limit = qparam(r, "limit", "50").toInt();
  if (q.limit <= 0 || q.limit > 200) q.limit = 50;
  MsgKind k;
  if (r->hasParam("kind") && kindFromName(qparam(r, "kind").c_str(), k)) { q.haveKind = true; q.kind = k; }
  if (r->hasParam("session")) q.sessionId = std::string(qparam(r, "session").c_str());
  if (r->hasParam("text")) q.textContains = std::string(qparam(r, "text").c_str());
  // Time window, relative hours-ago (Glass Box A3 - the store's absolute bounds
  // were implemented but never plumbed): sinceh=24 => the last day only;
  // beforeh=2 => exclude the last two hours (paging older history).
  {
    const uint32_t nowH = mem::nowHours();
    long sh = r->hasParam("sinceh") ? qparam(r, "sinceh").toInt() : 0;
    long bh = r->hasParam("beforeh") ? qparam(r, "beforeh").toInt() : 0;
    if (sh > 0 && nowH > 0) q.sinceHours = nowH > (uint32_t)sh ? nowH - (uint32_t)sh : 1;
    if (bh > 0 && nowH > 0) q.beforeHours = nowH > (uint32_t)bh ? nowH - (uint32_t)bh : 1;
  }

  // v4.0.0 deep history: `before` pages (row id, or the "<day>:<off>" byte
  // cursor the previous page returned); `cold=1` opts into reading day-streams
  // below the boot-scan index. Off by default - the dashboard's default view
  // stays a zero-read RAM answer.
  if (r->hasParam("before")) q.before = std::string(qparam(r, "before").c_str());
  if (r->hasParam("cold")) q.coldScan = qparam(r, "cold").toInt() != 0;

  // The DOCUMENT (200 rows x ~1 KB of copied strings) must itself live in PSRAM
  // - serializing to a PSRAM buffer alone still built the ~150-200 KB node pool
  // on internal heap (prism Release-A finding, HIGH).
  JsonDocument d(&agent::PsramJsonAllocator::instance());
  JsonArray arr = d["messages"].to<JsonArray>();
  nimbus::orch::EpiQueryInfo qinfo;
  for (const auto& m : mem::episodic().query(q, &qinfo)) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = m.id; o["session"] = m.sessionId; o["ts"] = m.tsHours;
    o["role"] = m.role; o["kind"] = kindName(m.kind);
    o["text"] = m.text; o["blob"] = m.blobPath;
    o["tags"] = m.tags;   // "from:<sender>" on user rows (unified chat label)
    // Channel is DERIVED from the sessionId (== the routing chatId): the pseudo
    // channels are literal, anything else is a Telegram chat id. Serves the
    // unified chat view's per-bubble channel chip with zero storage change.
    o["channel"] = (m.sessionId == "web")    ? "web"
                 : (m.sessionId == "voice")  ? "voice"
                 : (m.sessionId == "serial") ? "serial"
                 : (m.sessionId == "system") ? "system"
                                             : "telegram";
  }
  d["total"] = mem::episodic().messageCount();
  // Say how far back this answer actually looked - "no results" from a bounded
  // read is not the same claim as "it never happened".
  d["searchedTo"]  = nimbus::orch::AppendLogEpisodicStore::civilDate(qinfo.searchedToDay);
  d["olderExists"] = qinfo.olderExists;
  d["nextBefore"]  = qinfo.nextBefore;
  d["coldFiles"]   = qinfo.coldFiles;   // what the deep read actually cost
  d["coldBytes"]   = (unsigned long)qinfo.coldBytes;
  // PSRAM + chunked (Glass Box A4 hardening): with trace rows a 200-row response
  // reaches ~150-200 KB - serializing into an internal-heap String here would
  // spike the AsyncTCP task straight through the TLS danger floor. Serialize
  // once into PSRAM and stream it out, same pattern as /api/lastturn.
  {
    size_t need = measureJson(d) + 1;
    char* raw = (char*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!raw) {   // PSRAM exhausted (shouldn't happen) - small-response fallback
      String out; serializeJson(d, out); sendJson(r, 200, out);
      return;
    }
    size_t len = serializeJson(d, raw, need);
    std::shared_ptr<char> body(raw, free);
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        "application/json",
        [body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          if (index >= len) return 0;
          size_t take = len - index;
          if (take > maxLen) take = maxLen;
          memcpy(buf, body.get() + index, take);
          return take;
        });
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  }
}

// ---- GET /api/tools ----
// The live on-device tool surface, straight from the MCP registry manifest (so it
// never drifts from what the orchestrator actually advertises to the model + what
// an external MCP client sees over POST /mcp). memory.* + session.* live here;
// device actions (led/tts/reboot/config) travel the turn contract's device[] path
// rather than the registry, so they are listed separately as a static note in the
// UI. Read-only, no secrets.
// Dynamic + COMPLETE tools view (P7): the registry tools (with a rides_loop flag),
// the always-available orch_turn device actions, the on-device skill capsules, and
// the provider-side connectors with availability tags - grouped so the Tools tab
// and the model see one honest, non-stale list.
void handleToolsGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  JsonDocument d;
  JsonArray arr = d["tools"].to<JsonArray>();   // create ONCE - .to<>() clears on each call
  auto grp = [&](const char* group, const char* name, const char* desc,
                 const char* tag, bool loop) {
    JsonObject o = arr.add<JsonObject>();
    o["group"] = group; o["name"] = name; o["description"] = desc;
    if (tag && tag[0]) o["tag"] = tag;
    o["rides_loop"] = loop;
  };
  // Registry tools (memory.*/session.*/web.search/system.health/skill.*/reply.*).
  for (const auto& t : mem::registry().manifest())
    grp("registry", t.name.c_str(), t.description.c_str(), "",
        agent::orchestrator::toolRidesLoop(t.name));
  // orch_turn device actions - always available via the turn contract (not the loop).
  // One authoritative doc row generated from the ORCH_D_DEVICE macro - the same
  // string the model's schema carries, so this list can never drift from the wire
  // (the four rows below are just friendly headings).
  grp("device", "device.*", ORCH_D_DEVICE, "turn contract", false);
  grp("device", "device.led", "Ring pattern: solid/spinner/pulse/flash/rainbow + RGB + brightness", "turn action", false);
  grp("device", "device.lights", "Turn the ring on (full) or off", "turn action", false);
  grp("device", "device.config", "Adjust ring level, brightness, theme, power profile, voice", "turn action", false);
  grp("device", "device.tts", "Speak text to the owner (Telegram voice / desk speaker)", "turn action", false);
  // Skill capsules (readable via skill.get; SD capsules also inject at spawn).
  for (const auto& c : agent::skills::list()) {
    std::string tag = c.source == "sd" ? "sd skill.get" : "skill.get";
    if (c.origin == "agent") tag += c.approved ? " agent" : " agent pending";
    grp("skill", ("skill:" + c.id).c_str(), c.title.c_str(), tag.c_str(), false);
  }
  // Provider-side connectors, with a "<provider> only" availability tag. The Info
  // array is String-heavy; keep it small + skip it entirely under heap pressure so
  // this diagnostic endpoint can NEVER crash the async task on a tight device
  // (an Info[12] on the small AsyncTCP stack + a big doc reset the connection at
  // ~18 KB free - prism/on-device). The registry tools above are the important part.
  if (ESP.getFreeHeap() <= 30000) {
    grp("connector", "(connectors hidden)",
        "Device memory is momentarily low - connector rows are omitted to keep this "
        "endpoint safe. Reload in a moment.", "low memory", false);
  }
  if (ESP.getFreeHeap() > 30000) {
    // HEAP, not stack - the AsyncTCP stack is the one an Info[12] already reset
    // (comment above); and the old Info[4] cap silently hid every connector past
    // the fourth from this dashboard (see kMaxConnectors in connectors.h).
    std::unique_ptr<agent::connectors::Info[]> ci(
        new (std::nothrow) agent::connectors::Info[agent::connectors::kMaxConnectors]);
    int nc = ci ? agent::connectors::list(ci.get(), agent::connectors::kMaxConnectors) : 0;
    for (int i = 0; i < nc; i++) {
      String tag = ci[i].prov + " " + ci[i].kind + (ci[i].enabled ? "" : " (disabled)");
      grp("connector", ci[i].name.c_str(),
          "Provider-side connector - the cloud provider runs it for the model.",
          tag.c_str(), false);
    }
  }
  if (!agent::store::tavilyKey().length())
    grp("connector", "web.search (Tavily)", "Live web search - add a Tavily key to enable.",
        "needs Tavily key", false);
  d["count"] = d["tools"].as<JsonArray>().size();
  String out; serializeJson(d, out); sendJson(r, 200, out);
}

// ---- embed config (SET-ONCE) ----
void handleEmbedCfgGet(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;  // strict gate (owner R2)
  mem::Lock lk;   // reads vectors().size()
  JsonDocument d;
  d["provider"] = agent::store::embedProvider();
  d["model"]    = agent::store::embedModel();
  d["dims"]     = agent::store::embedDims();
  d["locked"]   = agent::store::embedLocked();
  d["vectors"]  = mem::vectors().size();
  String out; serializeJson(d, out); sendJson(r, 200, out);
}
// POST embedverify: run a REAL embeddings API call with a CANDIDATE provider/model/
// dims (owner: "once replaced, a quick real api call to validate"). Saves nothing -
// the UI calls this first and only proceeds to save if the config actually works.
void handleEmbedVerify(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;
  String provider = qparam(r, "provider", "openai");
  String model    = qparam(r, "model");
  int dims        = qparam(r, "dims", "0").toInt();
  if (provider != "openai" && provider != "mistral") { sendJson(r, 400, "{\"ok\":false,\"error\":\"provider must be openai|mistral\"}"); return; }
  if (!model.length()) { sendJson(r, 400, "{\"ok\":false,\"error\":\"model required\"}"); return; }
  String err;
  auto v = agent::embeddings::embedWith("nimbus embedding check", err, provider, model, dims);
  JsonDocument d;
  d["ok"]   = !v.empty();
  d["dims"] = (int)v.size();
  if (!v.empty()) d["provider"] = provider, d["model"] = model;
  else            d["error"]    = err;
  String out; serializeJson(d, out); sendJson(r, v.empty() ? 400 : 200, out);
}

// POST embedcfg: refuse a change while locked UNLESS reset=1, which WIPES the VDB
// (the destructive set-once path). This is the server-side guard behind the UI
// warning - a change can never silently strand incomparable vectors.
void handleEmbedCfgPost(AsyncWebServerRequest* r) {
  if (authBlocked(r)) return;   // can WIPE the VDB - gate it
  String provider = qparam(r, "provider", "openai");
  String model    = qparam(r, "model");
  int dims        = qparam(r, "dims", "256").toInt();
  bool reset      = qparam(r, "reset") == "1";
  if (provider != "openai" && provider != "mistral") { sendJson(r, 400, "{\"error\":\"provider must be openai|mistral\"}"); return; }
  if (!model.length()) { sendJson(r, 400, "{\"error\":\"model required\"}"); return; }

  if (agent::store::embedLocked() && !reset) {
    sendJson(r, 409, "{\"error\":\"embed config is locked (vectors exist). Pass reset=1 to WIPE the vector memory and re-set it.\"}");
    return;
  }
  {
    mem::Lock lk;   // WIPE + reconfigure the VDB atomically wrt the turn task
    if (reset) {
      mem::vectors().flushAll();
      mem::persistVectors();
      agent::store::setEmbedLocked(false);
    }
    agent::store::setEmbedConfig(provider, model, dims);
    mem::vectors().configure(dims > 0 ? dims : 256);
  }
  handleEmbedCfgGet(r);
}

}  // namespace

void registerMemoryRoutes(AsyncWebServer& server) {
  server.on("/api/mem/stats",      HTTP_GET,  handleStats);
  server.on("/api/mem/vector",     HTTP_GET,  handleVectorGet);
  server.on("/api/mem/vector",     HTTP_POST, handleVectorPost);
  server.on("/api/mem/scratchpad", HTTP_GET,  handleScratchGet);
  server.on("/api/mem/scratchpad", HTTP_POST, handleScratchPost);
  server.on("/api/mem/config",     HTTP_GET,  handleConfigGet);
  server.on("/api/mem/config",     HTTP_PUT,  handleConfigPut);
  server.on("/api/mem/episodic",   HTTP_GET,  handleEpisodicGet);
  server.on("/api/tools",          HTTP_GET,  handleToolsGet);
  server.on("/api/mem/embedcfg",   HTTP_GET,  handleEmbedCfgGet);
  server.on("/api/mem/embedcfg",   HTTP_POST, handleEmbedCfgPost);
  server.on("/api/mem/embedverify",HTTP_POST, handleEmbedVerify);

  // GET /api/log - the agent RAM log ring (last ~2.5 KB), plain text. Lets a
  // network-dependent failure (STT "didn't catch that", turn errors) be diagnosed
  // over HTTP without opening serial (which would drop the WiFi being diagnosed).
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate (owner R2)
    AsyncWebServerResponse* res = r->beginResponse(200, "text/plain", agent::agentLogTail());
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // GET /api/sdprobe - re-attempt the SD mount and report, so a user can RESEAT the
  // card and re-check without reflashing. solide::storage::begin() retries whenever
  // it isn't already mounted; a cardType of 0 (CARD_NONE) means the card never
  // answered on the SPI bus (not a format issue - reseat / check the slot).
  server.on("/api/sdprobe", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate (owner R2)
    // This endpoint MUTATES the storage tier (re-mount + promote + repoint), so -
    // like every other endpoint - it is token-gated (prism 2026-07-12: it was an
    // unauthenticated GET). The gate above is unconditional, so there is no longer an
    // unauthenticated read-only path here; `authed` stays in the response as a constant
    // true purely so the shape doesn't change.
    // Two mutate cases: (a) a card DEMOTED mid-run (cold joint / pull) - promoteSd()
    // re-mounts + probes + re-wires the SD tier fully; (b) no card at boot - the
    // legacy late-mount (setDataFs) routes data to a freshly-inserted card.
    bool promoted = false, ok;
    int ct;
    unsigned long long totalMB, freeMB;
    {
      // Hold the memory Lock across ALL SD-bus access on this web task: begin()/late-mount
      // and the freeMB() FAT scan (SD.usedBytes) both drive the non-reentrant SD/SPI bus,
      // which the main loop's tickSdHealth() also probes - same race the promoteSd() fix
      // closed, one level up (prism residual). promoteSd() takes the same recursive Lock
      // internally; nesting is safe.
      agent::memory::Lock g;
      promoted = agent::memory::sdLost() && agent::memory::promoteSd();
      ok = solide::storage::begin();
      if (ok && !agent::memory::haveSd()) agent::memory::setDataFs(SD);  // late mount
      ct = (int)SD.cardType();
      totalMB = (unsigned long long)solide::storage::cardSizeMB();
      freeMB = (unsigned long long)solide::storage::freeMB();
    }
    // The SD tier just came back - ask the sfx task to re-count its clip
    // variants (sets a flag it services on its own task; single-writer safe).
    if (promoted) ::sfx::refreshConfig();
    char body[288];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"cardType\":%d,\"totalMB\":%llu,\"freeMB\":%llu,"
             "\"sdLost\":%s,\"promoted\":%s,\"authed\":true,\"hint\":\"%s\"}",
             ok ? "true" : "false", ct, totalMB, freeMB,
             agent::memory::sdLost() ? "true" : "false",
             promoted ? "true" : "false",
             ok ? "mounted" : ct == 0 ? "card not seen on the bus - reseat / check slot"
                                      : "card seen but FAT mount failed - reformat FAT32");
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", body);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // LAN MCP endpoint: raw JSON-RPC 2.0 body -> memory::handleMcp. The body is a
  // JSON document (not form fields), so it is accumulated across chunks in the
  // request's own _tempObject (per-request, no shared/static state), then the
  // request handler dispatches + responds. We delete + null _tempObject before
  // responding so the framework's raw free() never double-frees the std::string.
  // (Edge case: a client that aborts mid-body leaks the partial buffer - rare and
  // bounded; an auth + size cap are the Ph4 hardening.)
  server.on("/mcp", HTTP_POST,
            [](AsyncWebServerRequest* r) {
              std::string* acc = static_cast<std::string*>(r->_tempObject);
              std::string body = acc ? *acc : std::string();
              if (acc) { delete acc; r->_tempObject = nullptr; }   // free before any early return
              // SECURITY (prism): the LAN MCP endpoint mutates memory / drives sub-agents /
              // spends the Tavily key - require the device token (X-Nimbus-Token header).
              if (!webAuthOk(r)) {
                r->send(401, "application/json", "{\"error\":\"auth required - X-Nimbus-Token\"}");
                return;
              }
              // v3.7.0: the LAN MCP endpoint authenticates with ONE device
              // token and carries no per-caller identity, so it gets its OWN
              // namespace rather than being silently treated as the owner.
              // Making that explicit is the point of the Principal type.
              // ⚠ Build through principalForRole, never aggregate init: `{ns, owner}`
              // leaves role at its Unknown default, which denies every memory
              // tool - a silent regression of the whole LAN MCP surface.
              // A token-authenticated LAN client acts for the device's owner but
              // carries no per-caller identity, so it gets its OWN namespace with
              // User rights rather than the admin's data.
              nimbus::orch::Principal who = nimbus::orch::principalForRole(
                  nimbus::orch::kMcpNs, nimbus::orch::Role::User);
              who.ns = nimbus::orch::kMcpNs;   // its own namespace, not a chat's
              std::string resp = mem::handleMcp(body, who);
              AsyncWebServerResponse* res =
                  r->beginResponse(200, "application/json", resp.c_str());
              res->addHeader("Cache-Control", "no-store");
              r->send(res);
            },
            nullptr,
            [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
              if (!r->_tempObject) r->_tempObject = new std::string();
              static_cast<std::string*>(r->_tempObject)->append((const char*)data, len);
            });
}

}  // namespace nimbus::net
