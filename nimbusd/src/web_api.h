#pragma once
#include <ArduinoJson.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <utility>
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
  // web surface (the caller then 404s). `path` includes any query string. The work
  // is split across grouped dispatchers so each stays within the complexity gate.
  bool handle(const std::string& method, const std::string& path,
              const std::string& body, ApiResp& out) {
    const std::string base = path.substr(0, path.find('?'));
    return tryStatus(method, base, body, out) ||
           tryChat(method, base, path, body, out) ||
           tryMemory(method, base, path, body, out) ||
           tryStatic(method, base, path, out) ||
           tryStubs(method, base, out) ||
           tryHardware(base, out);
  }

 private:
  // Home + Assistant snapshots (instant: engine snapshot + immutable config).
  bool tryStatus(const std::string& m, const std::string& base, const std::string& body,
                 ApiResp& out) {
    if (base == "/api/state" && m == "GET")   { out = stateResp();  return true; }
    if (base == "/api/health" && m == "GET")  { out = healthResp(); return true; }
    if (base == "/api/orch" && m == "GET")    { out = orchResp();   return true; }
    if (base == "/api/orch" && m == "POST")   { out = orchPost(body); return true; }
    if (base == "/api/connect" && m == "GET") { out = connectResp(); return true; }
    return false;
  }

  // Chat (REAL: the engine turn + the reply ring). GET gets the full path so it can
  // read the ?turn=<id> the client polls its own turn's reply with.
  bool tryChat(const std::string& m, const std::string& base, const std::string& path,
               const std::string& body, ApiResp& out) {
    if (base != "/api/chat") return false;
    out = m == "POST" ? chatPost(body) : chatGet(path);
    return true;
  }

  // Memory + tools (REAL: dispatched onto the engine thread). Split across two
  // dispatchers so each stays within the complexity gate.
  bool tryMemory(const std::string& m, const std::string& base, const std::string& path,
                 const std::string& body, ApiResp& out) {
    return tryMemReads(m, base, out) || tryMemData(m, base, path, body, out);
  }
  bool tryMemReads(const std::string& m, const std::string& base, ApiResp& out) {
    if (m != "GET") return false;
    if (base == "/api/mem/stats")      { out = memStats();     return true; }
    if (base == "/api/mem/scratchpad") { out = memScratch();   return true; }
    if (base == "/api/mem/config")     { out = memConfigGet(); return true; }
    if (base == "/api/mem/nsusage")    { out = memNsUsage();   return true; }
    if (base == "/api/tools")          { out = toolsResp();    return true; }
    return false;
  }
  bool tryMemData(const std::string& m, const std::string& base, const std::string& path,
                  const std::string& body, ApiResp& out) {
    if (base == "/api/mem/config" && m == "PUT")   { out = memConfigPut(body);  return true; }
    if (base == "/api/mem/vector" && m == "GET")   { out = memVectorGet(path);  return true; }
    if (base == "/api/mem/vector" && m == "POST")  { out = memVectorPost(path); return true; }
    if (base == "/api/mem/episodic" && m == "GET") { out = memEpisodic(path);   return true; }
    return false;
  }

  // Pure/static surfaces (no engine, no hardware).
  bool tryStatic(const std::string& m, const std::string& base, const std::string& path, ApiResp& out) {
    if (base == "/api/themes" && m == "GET")      { out = themesResp();     return true; }
    if (base == "/api/qr" && m == "GET")          { out = qrResp(path);     return true; }
    if (base == "/api/docs/search" && m == "GET") { out = docsSearch(path); return true; }
    if (base == "/api/voices" && m == "GET")      { out = okJson("[]");     return true; }
    if (base == "/api/token/regen" && m == "POST"){ out = tokenRegen();     return true; }
    if (base == "/api/usage/history" && m == "GET") { out = usageHistory(); return true; }
    return false;
  }

  // Well-formed-empty honest surfaces (nothing to lie about yet) - each returns a
  // truthful, correctly-shaped body so the app renders and no control is dead. The
  // constant surfaces are a table; a GET returns the empty-but-shaped body, a write
  // acks. `prefix` covers the sub-routes (/api/telegram/add, /api/files/rm, ...).
  bool tryStubs(const std::string& m, const std::string& base, ApiResp& out) {
    const bool get = m == "GET";
    struct Stub { const char* base; bool prefix; const char* getBody; };
    static const Stub kStubs[] = {
        {"/api/loops", false, "[]"},
        {"/api/fetchq", false, "[]"},
        {"/api/tenant", false, R"({"tenants":[]})"},
        {"/api/wakeups", false, R"({"policy":"silent-allow","pending":null,"items":[]})"},
        {"/api/telegram", true, R"({"public":false,"pending":[],"allow":[]})"},
        {"/api/skills", true, R"({"sd":true,"skills":[]})"},
        {"/api/files", true, R"({"present":true,"count":0,"bytes":0,"files":[]})"},
    };
    for (const Stub& s : kStubs) {
      const bool hit = s.prefix ? base.rfind(s.base, 0) == 0 : base == s.base;
      if (hit) { out = get ? okJson(s.getBody) : okJson(R"({"ok":true})"); return true; }
    }
    return tryStubsExtra(get, base, out);
  }

  bool tryStubsExtra(bool get, const std::string& base, ApiResp& out) {
    if (base == "/api/connectors" || base.rfind("/api/connectors/", 0) == 0) {
      out = base == "/api/connectors/oauth/status" ? okJson(R"({"active":false})")
            : get ? okJson(R"({"configured":[],"known":[],"keyed":{},"host":""})") : okJson(R"({"ok":true})");
      return true;
    }
    if (base == "/api/trace" && get)    { out = {200, "text/plain; charset=utf-8", "Turn tracing is off on this instance."}; return true; }
    if (base == "/api/mem/blob" && get) { out = {404, "text/plain; charset=utf-8", "reason: no blob store on a hosted instance"}; return true; }
    if (base == "/api/mem/embedverify") { out = okJson(R"({"ok":true,"dims":0})"); return true; }
    if (base == "/api/mem/embedcfg" || base == "/api/verify" || base == "/api/preview")
      { out = okJson(R"({"ok":true})"); return true; }
    return false;
  }

  // Hardware-only: honest "not on a hosted instance" (never faked, never dead).
  bool tryHardware(const std::string& base, ApiResp& out) {
    if (!isHardwarePath(base)) return false;
    out = hardwareResp(base);
    return true;
  }

 private:
  // ------------------------------------------------------------------ helpers
  static ApiResp okJson(const std::string& j) { return ApiResp{200, "application/json", j}; }

  static ApiResp busy() {
    return ApiResp{503, "application/json", R"({"error":"instance busy - try again"})"};
  }

  ApiResp engineRead(std::function<std::string()> fn) {
    // Never stall the single HTTP thread behind a turn: a turn can hold the engine
    // for a long time, and this thread also serves /healthz, /readyz and the
    // snapshot routes. If a turn is in flight, tell the poller to retry (503) at
    // once rather than blocking every other request behind a 2 s wait. Only when
    // the engine is idle do we dispatch and await (it answers in milliseconds).
    if (eng_->snapshot().turnInFlight) return busy();
    auto fut = eng_->dispatchRead(std::move(fn));
    if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return busy();
    try {
      return okJson(fut.get());
    } catch (...) {
      return busy();
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
    // Cumulo Nimbus first (the flagship one-key path for a VN), then the BYOK heads.
    // The key FIELD the web form posts is `<slug>Key` - orchPost consumes the same.
    static const P kP[] = {{"cumulo", "Cumulo Nimbus"}, {"mistral", "Mistral"},
                           {"openai", "OpenAI"}, {"anthropic", "Anthropic"}};
    for (const P& p : kP) {
      const std::string slug = p.slug;
      JsonObject o = provs[slug].to<JsonObject>();
      o["label"] = p.label;
      o["keyField"] = slug + "Key";
      // cumulo is a router key, not a canonical-env BYOK slot, so hostAvailable()
      // does not see it - report it from hasCumulo() (CUM-286).
      o["hasKey"] = (slug == kCumuloSlug) ? rig_->hasCumulo() : rig_->hostAvailable(slug);
      if (slug == kCumuloSlug) o["recommended"] = true;
      // A hosted instance has no cheap UI key-verify path; the badge stays an honest
      // "unverified" (vts 0 + verify -1). The page's hosted save-flow does not spin on
      // a verify poll - it saves and reports "applied" directly (CUM-279).
      o["verify"] = -1;
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

  // ------------------------------------------------------------------ /api/orch POST
  // In-app provider keys, device parity (CUM-279): the SAME fields the device's
  // Providers & keys form posts (`<slug>Key`, and `clr_<slug>Key` to remove), applied
  // through the rig seam so a key set in the UI takes effect - no external attach.
  // Non-key orch settings are acked honestly (a VN's model/priority knobs default).
  // The apply runs on the engine thread (serialized; refused mid-turn with a 503
  // retry) because it rebuilds the engine to register the newly-keyed head.
  ApiResp orchPost(const std::string& body) {
    static const char* kFields[] = {"cumuloKey", "mistralKey", "openaiKey", "anthropicKey"};
    std::vector<std::pair<std::string, std::string>> writes;  // (host, key); empty key clears
    for (const char* f : kFields) {
      const std::string host = NimbusdRig::hostForKeyField(f);
      if (host.empty()) continue;
      const std::string v = formValue(body, f);
      if (!v.empty()) writes.push_back({host, v});
      else if (!formValue(body, std::string("clr_") + f).empty())
        writes.push_back({host, std::string()});
    }
    if (writes.empty()) return okJson(R"({"ok":true})");   // non-key settings: honest ack
    if (eng_->snapshot().turnInFlight) return busy();
    auto fut = eng_->dispatchRead([this, writes]() -> std::string {
      int applied = 0;
      for (const auto& w : writes)
        if (rig_->applyProviderKey(w.first, w.second)) applied++;
      JsonDocument d;
      d["ok"] = true;
      d["applied"] = applied;
      d["note"] = "Key saved and applied.";
      std::string out;
      serializeJson(d, out);
      return out;
    });
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return busy();
    try { return okJson(fut.get()); } catch (...) { return busy(); }
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
    // A mid-turn message is QUEUED, never rejected: the prompt row is recorded and
    // the turn is posted to the engine mailbox (which runs turns strictly in order).
    // The turn id IS this prompt row's seq, so the fence for matching its reply is
    // exactly one-past its own prompt - a prior in-flight turn's later reply can
    // never satisfy it (that was the "6" stale replay, CUM-218).
    const uint64_t turn = replies_->push("user", text, kWebChat);
    pending_.push_back(PendingTurn{turn, time(nullptr) + 900});
    // Flood backstop (symmetry with resolved_'s cap): an authenticated owner hammering
    // POSTs faster than the single engine thread replies cannot grow this without bound.
    while (pending_.size() > 256) pending_.pop_front();
    eng_->postMessage(kWebChat, text);
    JsonDocument d;
    d["pending"] = true;
    d["turn"] = turn;   // the client polls GET /api/chat?turn=<turn> for THIS reply
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  ApiResp chatGet(const std::string& path) {
    resolveWebTurns();
    JsonDocument d;
    const std::string tp = queryParam(path, "turn");
    if (!tp.empty()) {
      // Turn-matched poll: hand back only THIS turn's own reply.
      const uint64_t turn = std::strtoull(tp.c_str(), nullptr, 10);
      auto it = resolved_.find(turn);
      if (it != resolved_.end()) {
        fillChatReply(d, it->second);
        resolved_.erase(it);   // delivered; a repeat poll must not replay it
      } else {
        d["reply"] = "";
        d["pending"] = turnPending(turn);   // unknown/old id -> not pending, empty
      }
    } else {
      // Param-less poll (legacy clients / HIL): hand back the oldest ready reply,
      // FIFO. Kept so the single-turn contract still works without a turn id.
      if (!resolved_.empty()) {
        auto it = resolved_.begin();   // map ordered by turn id ascending = FIFO
        fillChatReply(d, it->second);
        resolved_.erase(it);
      } else {
        d["reply"] = "";
        d["pending"] = !pending_.empty();
      }
    }
    std::string out;
    serializeJson(d, out);
    return okJson(out);
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
    if (limit < 0) limit = 0;
    int offset = std::max(0, atoiOr(queryParam(path, "offset"), 0));
    return engineRead([this, query, limit, offset] {
      auto all = rig_->vectors().getAll();   // importance-desc
      // Filter to the matches FIRST, then paginate the matches: total, offset and
      // limit all refer to the same (filtered) set the client is browsing.
      std::vector<const orch::VecEntry*> match;
      for (const auto& e : all)
        if (query.empty() || e.content.find(query) != std::string::npos) match.push_back(&e);
      JsonDocument d;
      d["total"] = (int)match.size();
      d["mode"] = query.empty() ? "browse" : "search";
      d["offset"] = offset;
      d["limit"] = limit;
      JsonArray arr = d["entries"].to<JsonArray>();
      for (size_t i = (size_t)offset; i < match.size() && (int)(i - offset) < limit; i++) {
        const auto& e = *match[i];
        JsonObject o = arr.add<JsonObject>();
        o["id"] = e.id;
        o["content"] = e.content;
        o["importance"] = e.importance;
        o["permanent"] = e.permanentFlag;
        o["ttlHours"] = e.ttlHours;
        o["lastRecallHours"] = e.lastRecallHours;
        o["nsLabel"] = e.ns;
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

  // Web chat reply matching (CUM-218). A single slot could track only ONE pending
  // web turn, so a mid-turn rapid tap either dropped a turn or replayed a prior
  // turn's reply. Now every web turn is tracked by id (its prompt row's seq) and
  // resolved in the order posted. `pending_` is the FIFO of turns still awaiting a
  // reply; `resolved_` holds ready replies (keyed by turn id) until the client
  // polls for them; `webConsumedSeq_` is the highest assistant ring seq already
  // handed to a web turn, so no reply is ever handed to two turns. Touched only on
  // the single HTTP thread, so no synchronization is needed.
  // The web channel's delivery chat id (a web turn is posted as, and its reply is
  // delivered to, this chat). The reply matcher consumes only assistant replies on
  // this channel, so a Telegram/routine reply cannot surface in the web bubble.
  static constexpr const char* kWebChat = "owner";
  struct PendingTurn { uint64_t userSeq; time_t deadline; };
  std::deque<PendingTurn> pending_;
  std::map<uint64_t, std::string> resolved_;
  uint64_t webConsumedSeq_ = 0;

  // Advance FIFO resolution: match each waiting turn, oldest first, to the first
  // unconsumed assistant reply after its own prompt row. Stops at the first turn
  // with no reply yet - nothing behind it can be ready before it (turns run in
  // order). A turn past its backstop deadline resolves to an empty reply so the
  // bubble stops spinning honestly.
  void resolveWebTurns() {
    while (!pending_.empty()) {
      PendingTurn& t = pending_.front();
      const uint64_t fence = t.userSeq > webConsumedSeq_ ? t.userSeq : webConsumedSeq_;
      uint64_t rseq = 0;
      std::string text;
      if (replies_->firstAssistantSince(fence, kWebChat, rseq, text)) {
        webConsumedSeq_ = rseq;   // consumed; never handed to another turn
        stashResolved(t.userSeq, text);
        pending_.pop_front();
        continue;
      }
      if (time(nullptr) > t.deadline) { stashResolved(t.userSeq, ""); pending_.pop_front(); continue; }
      break;
    }
  }

  // Record a ready reply for pickup, bounding the map so an abandoned page (a turn
  // whose client never polls back) cannot grow it without limit.
  void stashResolved(uint64_t turn, const std::string& text) {
    resolved_[turn] = text;
    while (resolved_.size() > 64) resolved_.erase(resolved_.begin());
  }

  bool turnPending(uint64_t turn) const {
    for (const auto& t : pending_) if (t.userSeq == turn) return true;
    return false;
  }

  // Fill a reply payload (reply + fallback disclosure) into the response document.
  void fillChatReply(JsonDocument& d, const std::string& reply) {
    d["reply"] = reply;
    d["pending"] = false;
    // Served-by disclosure (CUM-236): best-effort from the latest snapshot.
    const StateSnapshot ss = eng_->snapshot();
    if (ss.lastFallback) { d["fallback"] = true; d["servedBy"] = ss.lastServedBy; }
  }
};

}  // namespace nimbusd
