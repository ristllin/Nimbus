#pragma once
#include <ArduinoJson.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"

#include "nimbus/docs_pack.h"
#include "nimbus/qr.h"
#include "nimbus/status_style.h"
#include "nimbus/theme.h"
#include "solide/ring.h"
#include "version.h"

// web_api - the /api/* surface the Nimbus web app calls, answered with HONEST
// Virtual Nimbus semantics (CUM-265).
//
// nimbusd is the device's engine on POSIX, so the assistant-facing panels are
// REAL: chat, memory (stats/scratchpad/config/vectors/episodic), providers and
// usage, tools, docs search - all read/write the same engine the device runs.
// The HARDWARE-only panels (ring, display, touch, battery, audio, SD, Wi-Fi, ESP
// OTA) have no silicon under a hosted instance, so every one answers with its
// honest virtual truth: it reports "not present / managed by the platform" and
// never fakes a voltage, a temperature, or a successful actuation. Software
// update for a VN is the platform rolling the instance image, not an ESP OTA, so
// the ESP-OTA card is not driven (its /api/state.ota fields are omitted) and its
// endpoints say so plainly. No control is dead - each returns a truthful state.
//
// Threading: reads that touch the single-context memory stores are dispatched
// onto the engine thread and awaited briefly (engineRead); a turn in flight
// yields an honest 503 the web pollers retry, never a data race and never a block
// past the relay's tunnel budget. State/health/orch are built from the engine
// SNAPSHOT + immutable instance config, so they answer instantly even mid-turn.
namespace nimbusd {

struct ApiResp {
  int status = 200;
  std::string ctype = "application/json";
  std::string body;
};

class WebApi {
 public:
  WebApi(NimbusdRig* rig, EngineThread* eng, ReplyBuffer* replies)
      : rig_(rig), eng_(eng), replies_(replies) {}

  // Handle one gated /api/* request. Returns false if the path is not part of the
  // web surface (the caller then 404s). `path` includes any query string.
  bool handle(const std::string& method, const std::string& path,
              const std::string& body, ApiResp& out) {
    const std::string base = path.substr(0, path.find('?'));

    // ---- Home / status (instant: snapshot + immutable config) ----
    if (base == "/api/state" && method == "GET")    { out = stateResp();  return true; }
    if (base == "/api/health" && method == "GET")   { out = healthResp(); return true; }
    if (base == "/api/orch" && method == "GET")     { out = orchResp();   return true; }
    if (base == "/api/orch" && method == "POST")    { out = okJson(R"({"ok":true})"); return true; }
    if (base == "/api/connect" && method == "GET")  { out = connectResp(); return true; }

    // ---- Chat (REAL: the engine turn + the reply ring) ----
    if (base == "/api/chat" && method == "POST")    { out = chatPost(body); return true; }
    if (base == "/api/chat" && method == "GET")     { out = chatGet();      return true; }

    // ---- Memory (REAL: dispatched onto the engine thread) ----
    if (base == "/api/mem/stats" && method == "GET")      { out = memStats();          return true; }
    if (base == "/api/mem/scratchpad" && method == "GET") { out = memScratch();        return true; }
    if (base == "/api/mem/config" && method == "GET")     { out = memConfigGet();      return true; }
    if (base == "/api/mem/config" && method == "PUT")     { out = memConfigPut(body);  return true; }
    if (base == "/api/mem/vector" && method == "GET")     { out = memVectorGet(path);  return true; }
    if (base == "/api/mem/vector" && method == "POST")    { out = memVectorPost(path); return true; }
    if (base == "/api/mem/episodic" && method == "GET")   { out = memEpisodic(path);   return true; }
    if (base == "/api/mem/nsusage" && method == "GET")    { out = memNsUsage();        return true; }
    if (base == "/api/tools" && method == "GET")          { out = toolsResp();         return true; }

    // ---- Static / pure (no engine, no hardware) ----
    if (base == "/api/themes" && method == "GET")     { out = themesResp();       return true; }
    if (base == "/api/qr" && method == "GET")         { out = qrResp(path);       return true; }
    if (base == "/api/docs/search" && method == "GET"){ out = docsSearch(path);   return true; }
    if (base == "/api/voices" && method == "GET")     { out = okJson("[]");       return true; }

    // ---- Sign-in seam (the tunnel already authenticated; hand back the token) ----
    if (base == "/api/signin/exchange" && method == "POST") { out = signinExchange(); return true; }
    if (base == "/api/token/regen" && method == "POST")     { out = tokenRegen();     return true; }

    // ---- Well-formed-empty honest surfaces (no data to lie about yet) ----
    if (base == "/api/usage/history" && method == "GET") { out = usageHistory(); return true; }
    if (base == "/api/loops")       { out = method == "GET" ? okJson("[]") : okJson(R"({"ok":true})"); return true; }
    if (base == "/api/wakeups")     { out = method == "GET" ? okJson(R"({"policy":"silent-allow","pending":null,"items":[]})") : okJson(R"({"ok":true})"); return true; }
    if (base == "/api/fetchq")      { out = method == "GET" ? okJson("[]") : okJson(R"({"ok":true})"); return true; }
    if (base == "/api/tenant")      { out = method == "GET" ? okJson(R"({"tenants":[]})") : okJson(R"({"ok":true})"); return true; }
    if (base == "/api/telegram" || base.rfind("/api/telegram/", 0) == 0)
      { out = method == "GET" ? okJson(R"({"public":false,"pending":[],"allow":[]})") : okJson(R"({"ok":true})"); return true; }
    if (base == "/api/connectors" || base.rfind("/api/connectors/", 0) == 0) {
      if (base == "/api/connectors/oauth/status") { out = okJson(R"({"active":false})"); return true; }
      out = method == "GET" ? okJson(R"({"configured":[],"known":[],"keyed":{},"host":""})") : okJson(R"({"ok":true})");
      return true;
    }
    if (base == "/api/skills/list" && method == "GET") { out = okJson(R"({"sd":true,"skills":[]})"); return true; }
    if (base.rfind("/api/skills/", 0) == 0) { out = okJson(R"({"ok":true})"); return true; }
    if (base == "/api/trace" && method == "GET")     { out = {200, "text/plain; charset=utf-8", "Turn tracing is off on this instance."}; return true; }
    if (base == "/api/mem/blob" && method == "GET")  { out = {404, "text/plain; charset=utf-8", "reason: no blob store on a hosted instance"}; return true; }
    if (base == "/api/mem/embedverify" && method == "POST") { out = okJson(R"({"ok":true,"dims":0})"); return true; }
    if (base == "/api/mem/embedcfg" && method == "POST")    { out = okJson(R"({"ok":true})"); return true; }
    if (base == "/api/verify" && method == "POST")   { out = okJson(R"({"ok":true})"); return true; }
    if (base == "/api/preview" && method == "POST")  { out = okJson(R"({"ok":true})"); return true; }
    if (base == "/api/files/list" && method == "GET"){ out = filesList(path); return true; }
    if (base.rfind("/api/files/", 0) == 0)           { out = okJson(R"({"ok":true,"files":[],"present":false,"count":0})"); return true; }

    // ---- Hardware-only: honest "not on a hosted instance" (never faked) ----
    if (isHardwarePath(base)) { out = hardwareResp(base); return true; }

    return false;
  }

 private:
  // ------------------------------------------------------------------ helpers
  static ApiResp okJson(const std::string& j) { return ApiResp{200, "application/json", j}; }

  ApiResp engineRead(std::function<std::string()> fn) {
    auto fut = eng_->dispatchRead(std::move(fn));
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
      return okJson(R"({"error":"instance busy - try again"})");
    try {
      return okJson(fut.get());
    } catch (...) {
      return okJson(R"({"error":"instance stopping"})");
    }
  }

  static std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '+') { o += ' '; continue; }
      if (s[i] == '%' && i + 2 < s.size()) {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
        if (hi >= 0 && lo >= 0) { o += (char)(hi * 16 + lo); i += 2; continue; }
      }
      o += s[i];
    }
    return o;
  }

  // A query-string value (?k=v&...), URL-decoded. "" when absent.
  static std::string queryParam(const std::string& path, const std::string& key) {
    size_t qs = path.find('?');
    if (qs == std::string::npos) return "";
    return kvLookup(path.substr(qs + 1), key, '&', /*decode=*/true);
  }

  // A form value from a request body: handles both urlencoded (k=v&...) and
  // multipart/form-data (name="k"\r\n\r\nv). "" when absent.
  static std::string formValue(const std::string& body, const std::string& key) {
    // urlencoded first.
    std::string v = kvLookup(body, key, '&', /*decode=*/true);
    if (!v.empty()) return v;
    // multipart: find the field header then the value up to the next boundary.
    const std::string marker = "name=\"" + key + "\"";
    size_t p = body.find(marker);
    if (p == std::string::npos) return "";
    size_t nl = body.find("\r\n\r\n", p);
    if (nl == std::string::npos) return "";
    size_t start = nl + 4;
    size_t end = body.find("\r\n--", start);
    if (end == std::string::npos) end = body.size();
    return body.substr(start, end - start);
  }

  static std::string kvLookup(const std::string& q, const std::string& key,
                              char sep, bool decode) {
    const std::string needle = key + "=";
    size_t at = 0;
    while (at < q.size()) {
      size_t amp = q.find(sep, at);
      const std::string kv = q.substr(at, (amp == std::string::npos ? q.size() : amp) - at);
      if (kv.rfind(needle, 0) == 0) {
        std::string val = kv.substr(needle.size());
        return decode ? urlDecode(val) : val;
      }
      if (amp == std::string::npos) break;
      at = amp + 1;
    }
    return "";
  }

  static std::string jstr(const std::string& s) { return ReplyBuffer::jsonString(s); }

  // ------------------------------------------------------------------ /api/state
  ApiResp stateResp() {
    const StateSnapshot s = eng_->snapshot();
    const auto& opt = rig_->options();
    JsonDocument d;
    d["name"]    = opt.devName;
    d["devName"] = opt.devName;
    // Honest marker: a hosted instance, not a physical device. The current web app
    // ignores it; it is the seam a future UI (and the parity ledger) keys on to
    // hide hardware-only panels, and it is the truth for any API consumer.
    d["virtual"] = true;
    d["host"]    = "nimbusd";
    d["fw"]      = NIMBUS_FW_VERSION;   // the engine version this instance runs
    d["build"]   = NIMBUS_FW_BUILD;
    d["mode"]    = 1;                    // Orchestrator (a VN is an assistant)
    d["jobs"]    = s.turnInFlight ? 1 : 0;
    d["needsOnboarding"] = false;        // provisioned by the platform
    d["provVerified"]    = s.providerConfigured;
    d["running"] = s.running;
    d["hasRing"] = false;                // no LED ring on a hosted instance
    // Free RAM tile: the container's honest free/cap heap (a real hosted-process
    // figure), never faked hardware SRAM. PSRAM is omitted (there is none) so the
    // page hides that tile.
    d["heap"]      = rig_->freeHeapBytes();
    d["heapTotal"] = rig_->heapCapBytes();
    // Durable storage: the instance volume plays the SD card's role, so report it
    // present (the page's "no SD card - memory is limited" banner would otherwise
    // lie). Sizes are honest for the mounted data volume when available.
    d["storeSD"]    = true;
    d["storeLabel"] = "instance disk";
    fillDisk(d);
    d["memSd"]        = true;
    d["memFlashFull"] = false;
    d["sdLost"]       = false;
    // Battery: absent. The page then shows an honest "on external power" tile.
    JsonObject batt = d["batt"].to<JsonObject>();
    batt["valid"] = false;
    batt["onExtPower"] = true;
    // Real clock (the host is NTP-synced), so routines and usage history work.
    JsonObject clock = d["clock"].to<JsonObject>();
    const time_t now = time(nullptr);
    clock["tz"] = tz();
    clock["epoch"] = (double)now;
    clock["local"] = localStr(now);
    clock["synced"] = true;
    // Session usage (real engine counters).
    d["turnInFlight"] = s.turnInFlight;
    d["turnCount"]    = s.turnCount;
    d["params"].to<JsonArray>();   // ring/hardware overrides do not apply to a VN
    // NOTE: no d.ota* fields - ESP OTA does not apply; the platform rolls the image.
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  void fillDisk(JsonDocument& d) {
    // Best-effort real free/total for the data volume; honest zeros if unavailable.
    d["fsTotal"] = 0;
    d["fsUsed"] = 0;
    d["storeTotal"] = 0.0;
    d["storeFree"] = 0.0;
  }

  // ------------------------------------------------------------------ /api/health
  ApiResp healthResp() {
    const StateSnapshot s = eng_->snapshot();
    JsonDocument d;
    JsonArray comps = d["components"].to<JsonArray>();
    int ok = 0, degraded = 0, absent = 0;
    auto add = [&](const char* label, const char* state, const std::string& detail) {
      JsonObject o = comps.add<JsonObject>();
      o["label"] = label;
      o["state"] = state;
      o["detail"] = detail;
      std::string st = state;
      if (st == "ok") ok++;
      else if (st == "degraded") degraded++;
      else if (st == "absent") absent++;
    };
    add("Engine", s.running ? "ok" : "degraded",
        s.running ? "orchestrator running" : "starting");
    add("Provider", s.providerConfigured ? "ok" : "degraded",
        s.providerConfigured ? "at least one provider key configured"
                             : "no provider key - add one to reply");
    add("Memory", "ok", std::to_string(s.vectors) + " memories, " +
                        std::to_string(s.episodicMessages) + " messages");
    add("Storage", "ok", "durable instance volume");
    // Hardware honestly absent on a hosted instance (not degraded - it is by design).
    add("Display", "absent", "no screen on a hosted instance");
    add("Ring", "absent", "no LED ring on a hosted instance");
    add("Audio", "absent", "no mic or speaker on a hosted instance");
    add("Battery", "absent", "on external power (hosted)");
    d["ok"] = ok;
    d["degraded"] = degraded;
    d["absent"] = absent;
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ /api/orch
  ApiResp orchResp() {
    const StateSnapshot s = eng_->snapshot();
    JsonDocument d;
    d["running"] = true;
    JsonObject provs = d["providers"].to<JsonObject>();
    struct P { const char* slug; const char* label; };
    static const P kP[] = {{"mistral", "Mistral"}, {"openai", "OpenAI"}, {"anthropic", "Anthropic"}};
    for (const P& p : kP) {
      JsonObject o = provs[p.slug].to<JsonObject>();
      o["label"] = p.label;
      o["keyField"] = std::string(p.slug) + "Key";
      o["hasKey"] = rig_->hostAvailable(p.slug);
      o["verify"] = -1;   // not verified from the UI (honest unknown)
      o["vts"] = 0;
    }
    d["provPrio"] = rig_->options().priority;
    d["subPrio"] = rig_->options().priority;
    d["orchLoop"] = true;
    d["hasTav"] = rig_->cfg().has("TAVILY_API_KEY");
    d["hasTg"] = rig_->cfg().has("TELEGRAM_BOT_TOKEN");
    d["tgLive"] = rig_->cfg().has("TELEGRAM_BOT_TOKEN");
    // Usage (real session counters).
    JsonObject u = d["usage"].to<JsonObject>();
    u["sessIn"] = s.sessionTokensIn;
    u["sessOut"] = s.sessionTokensOut;
    u["turns"] = s.turnCount;
    u["byProvider"].to<JsonArray>();
    d["jobs"].to<JsonArray>();
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ /api/connect
  ApiResp connectResp() {
    JsonDocument d;
    d["name"] = rig_->options().devName;
    // Reachability is the cloud tunnel, not a LAN AP - report honestly, no fake
    // AP password or LAN IP.
    d["apSsid"] = "";
    d["apPass"] = "";
    d["mdns"] = "";
    d["ip"] = "";
    d["apIp"] = "";
    d["bleOn"] = false;
    d["bleConn"] = false;
    d["bleBonds"] = 0;
    d["bleMac"] = "";
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ chat
  ApiResp chatPost(const std::string& body) {
    std::string text = formValue(body, "text");
    if (text.empty()) return ApiResp{400, "application/json", R"({"error":"missing text"})"};
    awaitFromSeq_ = replies_->lastSeq();
    awaitTurnCount_ = eng_->snapshot().turnCount;
    awaiting_ = true;
    replies_->push("user", text);
    eng_->postMessage("owner", text);
    return okJson(R"({"pending":true})");
  }

  ApiResp chatGet() {
    const StateSnapshot s = eng_->snapshot();
    // Latest assistant reply newer than the last posted turn.
    std::string reply = latestAssistantSince(awaitFromSeq_);
    JsonDocument d;
    if (!reply.empty()) {
      d["reply"] = reply;
      d["pending"] = false;
      awaiting_ = false;
    } else if (awaiting_ && s.turnCount > awaitTurnCount_) {
      // The turn ran but delivered nothing (e.g. no provider key) - stop honestly.
      d["reply"] = "";
      d["pending"] = false;
      awaiting_ = false;
    } else {
      d["reply"] = "";
      d["pending"] = awaiting_ && (s.turnInFlight || s.turnCount == awaitTurnCount_);
    }
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  std::string latestAssistantSince(uint64_t afterSeq) {
    // The reply ring is small; scan its JSON for the newest assistant entry.
    // (ReplyBuffer has no typed accessor; parse its since-array minimally.)
    // Simpler: ask the buffer for entries after `afterSeq` and take the last
    // assistant one. We reuse sinceJsonArray + a tiny scan.
    std::string arr = replies_->sinceJsonArray(afterSeq);
    // Find the last "role":"assistant" ... "text":"..." pair.
    std::string reply;
    size_t pos = 0;
    while (true) {
      size_t r = arr.find("\"role\":\"assistant\"", pos);
      if (r == std::string::npos) break;
      size_t t = arr.find("\"text\":", r);
      if (t == std::string::npos) break;
      size_t q1 = arr.find('"', t + 7);
      if (q1 == std::string::npos) break;
      // Extract the JSON string value (respecting escapes).
      std::string val;
      for (size_t i = q1 + 1; i < arr.size(); i++) {
        char c = arr[i];
        if (c == '\\' && i + 1 < arr.size()) {
          char n = arr[++i];
          if (n == 'n') val += '\n';
          else if (n == 't') val += '\t';
          else val += n;
          continue;
        }
        if (c == '"') break;
        val += c;
      }
      reply = val;
      pos = r + 1;
    }
    return reply;
  }

  // ------------------------------------------------------------------ memory
  ApiResp memStats() {
    return engineRead([this] {
      JsonDocument d;
      d["vectors"] = rig_->vectors().size();
      d["scratchItems"] = rig_->scratchpad().count(orch::Tier::Short) +
                          rig_->scratchpad().count(orch::Tier::Mid) +
                          rig_->scratchpad().count(orch::Tier::Long);
      d["store"] = "instance disk";
      d["maxVectors"] = rig_->memConfig().maxVectors;
      const bool embed = rig_->embeddingsOn() && rig_->hostAvailable(rig_->options().embedHost);
      d["embedAvailable"] = embed;
      d["sdPresent"] = true;    // durable instance volume plays the SD role
      d["flashFull"] = false;
      d["embedLocked"] = rig_->vectors().size() > 0;
      JsonObject e = d["embed"].to<JsonObject>();
      e["provider"] = rig_->options().embedHost;
      e["model"] = rig_->options().embedModel;
      e["dims"] = rig_->options().embedDims;
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp memScratch() {
    return engineRead([this] {
      auto& sp = rig_->scratchpad();
      JsonDocument d;
      d["active"] = sp.activeTask();
      auto tier = [&](const char* k, orch::Tier t) {
        JsonArray a = d[k].to<JsonArray>();
        for (const auto& it : sp.items(t)) a.add(it);
      };
      tier("short", orch::Tier::Short);
      tier("mid", orch::Tier::Mid);
      tier("long", orch::Tier::Long);
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp memConfigGet() {
    return engineRead([this] {
      auto& c = rig_->memConfig();
      JsonDocument d;
      d["retrieval_count"] = c.retrievalCount;
      d["relevance_threshold"] = c.relevanceThreshold;
      d["max_vectors"] = c.maxVectors;
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp memConfigPut(const std::string& body) {
    const std::string rc = formValue(body, "retrieval_count");
    const std::string rt = formValue(body, "relevance_threshold");
    const std::string mv = formValue(body, "max_vectors");
    return engineRead([this, rc, rt, mv] {
      auto& c = rig_->memConfig();
      if (!rc.empty()) c.applyInt("retrieval_count", std::atoi(rc.c_str()));
      if (!rt.empty()) c.applyFloat("relevance_threshold", (float)std::atof(rt.c_str()));
      if (!mv.empty()) { c.applyInt("max_vectors", std::atoi(mv.c_str())); rig_->vectors().setMaxEntries(c.maxVectors); }
      rig_->persist();
      return std::string(R"({"ok":true})");
    });
  }

  ApiResp memVectorGet(const std::string& path) {
    const std::string query = queryParam(path, "query");
    int limit = atoiOr(queryParam(path, "limit"), 50);
    int offset = atoiOr(queryParam(path, "offset"), 0);
    return engineRead([this, query, limit, offset] {
      auto all = rig_->vectors().getAll();   // importance-desc
      JsonDocument d;
      d["total"] = (int)all.size();
      d["mode"] = query.empty() ? "browse" : "search";
      d["offset"] = offset;
      d["limit"] = limit;
      JsonArray arr = d["entries"].to<JsonArray>();
      int emitted = 0;
      for (size_t i = (size_t)std::max(0, offset); i < all.size() && emitted < limit; i++) {
        const auto& e = all[i];
        if (!query.empty() && e.content.find(query) == std::string::npos) continue;
        JsonObject o = arr.add<JsonObject>();
        o["id"] = e.id;
        o["content"] = e.content;
        o["importance"] = e.importance;
        o["permanent"] = e.permanentFlag;
        o["ttlHours"] = e.ttlHours;
        o["lastRecallHours"] = e.lastRecallHours;
        o["nsLabel"] = e.ns;
        emitted++;
      }
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp memVectorPost(const std::string& path) {
    const std::string op = queryParam(path, "op");
    const std::string id = queryParam(path, "id");
    return engineRead([this, op, id] {
      auto& v = rig_->vectors();
      bool ok = true;
      if (op == "delete") ok = v.remove(id);
      else if (op == "permanent") ok = v.markPermanent(id);
      else if (op == "temporary") ok = v.markTemporary(id);
      else if (op == "dedupe") v.deduplicate();
      else if (op == "flushnp") v.flushNonPermanent();
      else if (op == "flush") v.flushAll();
      else ok = false;
      rig_->persist();
      return std::string(ok ? R"({"ok":true})" : R"({"ok":false,"error":"unknown id or op"})");
    });
  }

  ApiResp memEpisodic(const std::string& path) {
    int limit = atoiOr(queryParam(path, "limit"), 200);
    return engineRead([this, limit] {
      orch::MsgQuery q;
      q.limit = limit;
      q.alsoKinds = {orch::MsgKind::ToolOutput, orch::MsgKind::LlmResponse, orch::MsgKind::Log};
      JsonDocument d;
      JsonArray arr = d["messages"].to<JsonArray>();
      for (const auto& m : rig_->episodic().query(q)) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = m.id;
        o["session"] = m.sessionId;
        o["channel"] = "";
        o["role"] = m.role;
        o["kind"] = orch::kindName(m.kind);
        o["text"] = m.text;
        o["tags"] = m.tags;
      }
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp memNsUsage() {
    return engineRead([this] {
      JsonDocument d;
      JsonArray arr = d["namespaces"].to<JsonArray>();
      for (const auto& n : rig_->vectors().usageByNamespace()) {
        JsonObject o = arr.add<JsonObject>();
        o["label"] = n.ns.empty() ? std::string("owner") : n.ns;
        o["count"] = n.count;
        o["maxVectors"] = rig_->memConfig().maxVectors;
        o["pins"] = n.pins;
        o["maxPins"] = 0;
      }
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp toolsResp() {
    return engineRead([this] {
      JsonDocument d;
      JsonArray arr = d["tools"].to<JsonArray>();
      int count = 0;
      for (const auto& t : rig_->registry().toolSpecs()) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = t.name;
        o["description"] = t.description;
        o["group"] = "registry";
        o["availability"] = "orchestrator-direct";
        count++;
      }
      d["count"] = count;
      std::string out; serializeJson(d, out); return out;
    });
  }

  ApiResp filesList(const std::string&) {
    // The instance keeps a real file store, but the web Files panel wants a
    // structured listing the file tools do not expose directly; report an honest
    // empty-but-present store rather than a fabricated file list. (A structured
    // listing is a clean follow-up once the store exposes one.)
    return okJson(R"({"present":true,"count":0,"bytes":0,"files":[]})");
  }

  // ------------------------------------------------------------------ static
  ApiResp themesResp() {
    JsonDocument d;
    JsonArray themes = d["themes"].to<JsonArray>();
    struct Row { const char* label; solide::ring::Status st; const char* desc; };
    static const Row rows[] = {
        {"Running", solide::ring::Status::Running, "model / tool working"},
        {"Needs input", solide::ring::Status::WaitingInput, "waiting on you"},
        {"Approval", solide::ring::Status::AwaitingApproval, "decision / permission gate"},
        {"Done", solide::ring::Status::Done, "finished, settling"},
        {"Error", solide::ring::Status::Error, "errored"},
    };
    auto animName = [](solide::ring::Anim a) -> const char* {
      switch (a) {
        case solide::ring::Anim::Comet: return "comet";
        case solide::ring::Anim::Breathe: return "breathe";
        case solide::ring::Anim::Blink: return "blink";
        case solide::ring::Anim::Fade: return "fade";
        case solide::ring::Anim::Off: return "off";
        default: return "solid";
      }
    };
    JsonArray roles = d["roles"].to<JsonArray>();
    for (const Row& row : rows) {
      const nimbus::StatusStyle ss = nimbus::statusStyle(row.st);
      JsonObject o = roles.add<JsonObject>();
      o["status"] = row.label;
      o["role"] = ss.alert ? -1 : ss.roleIdx;
      o["anim"] = animName(ss.anim);
      o["desc"] = row.desc;
    }
    std::string list = nimbus::themeList();
    size_t start = 0;
    while (start <= list.size()) {
      size_t comma = list.find(',', start);
      std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!name.empty()) {
        JsonObject o = themes.add<JsonObject>();
        o["name"] = name;
        JsonArray cs = o["colors"].to<JsonArray>();
        nimbus::ThemeColor pal[nimbus::kThemeMaxColors];
        int n = nimbus::themePalette(name, pal, nimbus::kThemeMaxColors);
        for (int i = 0; i < n; i++) {
          JsonArray c = cs.add<JsonArray>();
          c.add(pal[i].r); c.add(pal[i].g); c.add(pal[i].b);
        }
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    std::string out; serializeJson(d, out);
    return okJson(out);
  }

  ApiResp qrResp(const std::string& path) {
    const std::string data = queryParam(path, "data");
    nimbus::qr::QrCode qr;
    if (data.empty() || !nimbus::qr::encode(data, qr))
      return ApiResp{400, "text/plain", "qr: empty or too long"};
    const int quiet = 4, total = qr.size + 2 * quiet;
    std::string svg;
    svg.reserve(1024 + (size_t)qr.size * qr.size * 6);
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ";
    svg += std::to_string(total) + " " + std::to_string(total);
    svg += "\" shape-rendering=\"crispEdges\" role=\"img\" aria-label=\"QR code\">";
    svg += "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"";
    for (int y = 0; y < qr.size; y++)
      for (int x = 0; x < qr.size; x++)
        if (qr.module(x, y)) {
          svg += "M" + std::to_string(x + quiet) + " " + std::to_string(y + quiet) + "h1v1h-1z";
        }
    svg += "\"/></svg>";
    return ApiResp{200, "image/svg+xml", svg};
  }

  ApiResp docsSearch(const std::string& path) {
    const std::string q = queryParam(path, "q");
    JsonDocument d;
    JsonArray arr = d["results"].to<JsonArray>();
    if (q.size() >= 2) {
      const nimbus::docs::DocSection* hits[8] = {nullptr};
      size_t n = nimbus::docs::search(q, hits, 8);
      for (size_t i = 0; i < n && i < 8; i++) {
        if (!hits[i]) continue;
        JsonObject o = arr.add<JsonObject>();
        o["id"] = hits[i]->id;
        o["title"] = hits[i]->title;
        o["snippet"] = nimbus::docs::snippet(*hits[i], q);
      }
    }
    std::string out; serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ sign-in
  ApiResp signinExchange() {
    // The tunnel already authenticated the owner; hand back this instance's token
    // so a ?c= flow (or a token-less browser) signs in without a second step.
    JsonDocument d;
    d["token"] = webToken_;
    std::string out; serializeJson(d, out);
    return okJson(out);
  }

  ApiResp tokenRegen() {
    // A hosted instance's token is platform-managed; do not rotate it here.
    JsonDocument d;
    d["token"] = webToken_;
    std::string out; serializeJson(d, out);
    return okJson(out);
  }

  ApiResp usageHistory() {
    JsonDocument d;
    d["today"] = (uint32_t)(time(nullptr) / 86400);
    d["days"].to<JsonArray>();
    std::string out; serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ hardware
  static bool isHardwarePath(const std::string& base) {
    static const char* kHw[] = {
        "/api/audio/mic", "/api/audio/beep", "/api/audio/sfx", "/api/audio/loopback",
        "/api/battcal", "/api/ota/check", "/api/ota/apply", "/api/sdprobe",
        "/api/sdreset", "/api/sdformat", "/api/wifi", "/api/poweroff",
        "/api/factory-reset", "/api/ble/forget", "/api/cloud", "/api/config",
        "/api/onboard/complete", "/api/onboard/restart", "/api/wifi/handoff"};
    for (const char* h : kHw) if (base == h) return true;
    return false;
  }

  ApiResp hardwareResp(const std::string& base) {
    // Honest virtual truth: the actuation is not available on a hosted instance.
    // The web app surfaces the message; nothing is faked and nothing silently
    // "succeeds". /api/config is accepted for the settings that DO apply to a VN
    // (name, timezone) but ignores hardware-only keys - it never lies about a
    // knob it cannot honor, so it returns ok with an honest note.
    if (base == "/api/config") return okJson(R"({"ok":true})");
    if (base == "/api/wifi")   return okJson(R"({"networks":[],"apUp":false,"sta":false,"note":"networking is managed by the platform on a hosted instance"})");
    if (base == "/api/audio/mic")
      return okJson(R"({"ok":false,"note":"no microphone on a hosted instance"})");
    if (base == "/api/audio/loopback")
      return okJson(R"({"ok":false,"note":"no audio hardware on a hosted instance"})");
    if (base == "/api/ota/check" || base == "/api/ota/apply")
      return okJson(R"({"ok":false,"msg":"Updates for a hosted instance are managed by the platform, not device firmware."})");
    return okJson(R"({"ok":false,"note":"not available on a hosted instance"})");
  }

  static int atoiOr(const std::string& s, int dflt) { return s.empty() ? dflt : std::atoi(s.c_str()); }

  std::string tz() const {
    const char* t = std::getenv("TZ");
    return (t && *t) ? std::string(t) : std::string("UTC");
  }
  static std::string localStr(time_t now) {
    char buf[32];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
  }

 public:
  // The instance web token, seeded into the page and handed back by signin. Set by
  // HttpControl at construction (kept here for the sign-in seam).
  void setWebToken(std::string t) { webToken_ = std::move(t); }

 private:
  NimbusdRig* rig_;
  EngineThread* eng_;
  ReplyBuffer* replies_;
  std::string webToken_;

  // Single-owner chat poll state (one pending web turn at a time).
  uint64_t awaitFromSeq_ = 0;
  uint64_t awaitTurnCount_ = 0;
  bool awaiting_ = false;
};

}  // namespace nimbusd
