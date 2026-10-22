#pragma once
#include <ArduinoJson.h>

#include <algorithm>
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

#include "embed_check.h"
#include "engine_thread.h"
#include "multipart_form.h"
#include "reply_buffer.h"
#include "rig.h"
#include "tg_web.h"

#include "nimbus/docs_pack.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/directive.h"
#include "nimbus/orch/provider_slots.h"        // kProviderSlots (usage models body)
#include "nimbus/orch/usage_models_json.h"     // writeUsageModelsJson (usage models body)
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
      : rig_(rig), eng_(eng), replies_(replies), tg_(&rig->telegramAccess()) {}

  // Handle one gated /api/* request. Returns false if the path is not part of the
  // web surface (the caller then 404s). `path` includes any query string. The work
  // is split across grouped dispatchers so each stays within the complexity gate.
  // `contentType` is the request's Content-Type: a multipart form (the page's
  // FormData posts) is read into the urlencoded body every handler parses (CUM-543).
  bool handle(const std::string& method, const std::string& path,
              const std::string& rawBody, ApiResp& out,
              const std::string& contentType = std::string()) {
    const std::string base = path.substr(0, path.find('?'));
    std::string fields;
    const bool multipart = form::isMultipart(contentType) && base != "/api/files/upload";
    if (multipart && !readForm(rawBody, contentType, fields, out)) return true;
    const std::string& body = multipart ? fields : rawBody;
    return tryStatus(method, base, body, out) ||
           tryChat(method, base, path, body, out) ||
           tryMemory(method, base, path, body, out) ||
           tryConnectors(method, base, body, out) ||
           tryTelegram(method, base, body, out) ||
           tryStatic(method, base, path, out) ||
           tryInsight(method, base, path, out) ||
           tryFiles(method, base, path, body, out) ||
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
    if (base == "/api/config")                { out = m == "POST" ? configPost(body) : configGet(); return true; }
    if (base == "/api/verify" && m == "POST") { out = verifyPost(body); return true; }
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
    if (base == "/api/mem/embedverify" && m == "POST") { out = embedVerify(body); return true; }
    if (base == "/api/mem/embedcfg")    { out = m == "POST" ? embedCfgPost(body) : embedCfgGet(); return true; }
    return false;
  }

  // Telegram access + people (CUM-459): the device's routes over the instance's
  // durable allowlist + RBAC table (tg_web.h). Answered on the HTTP thread: the
  // access store is thread-safe and never touches the engine.
  bool tryTelegram(const std::string& m, const std::string& base, const std::string& body,
                   ApiResp& out) {
    TgWebResp r;
    const TelegramWeb::Form form = [&body](const std::string& k) { return formValue(body, k); };
    if (!tg_.handle(m, base, form, rig_->cfg().has("TELEGRAM_BOT_TOKEN"), r)) return false;
    out = ApiResp{r.status, "application/json", r.body};
    return true;
  }

  // Pure/static surfaces (no engine, no hardware).
  bool tryStatic(const std::string& m, const std::string& base, const std::string& path, ApiResp& out) {
    if (base == "/api/themes" && m == "GET")      { out = themesResp();     return true; }
    if (base == "/api/qr" && m == "GET")          { out = qrResp(path);     return true; }
    if (base == "/api/docs/search" && m == "GET") { out = docsSearch(path); return true; }
    if (base == "/api/voices" && m == "GET")      { out = okJson("[]");     return true; }
    if (base == "/api/token/regen" && m == "POST"){ out = tokenRegen();     return true; }
    if (base == "/api/usage/history" && m == "GET") { out = usageHistory(); return true; }
    if (base == "/api/usage/models" && m == "GET")  { out = usageModels();  return true; }

    return false;
  }

  // Surfaces a hosted instance does not have yet (CUM-543): a GET returns the
  // truthful, correctly-shaped empty body so the page renders, and a write is
  // REFUSED with the feature named - it used to answer {"ok":true} and drop it, so
  // the page printed "Created" for a routine that never existed.
  bool tryStubs(const std::string& m, const std::string& base, ApiResp& out) {
    const bool get = m == "GET";
    struct Stub { const char* base; bool prefix; const char* getBody; const char* feature; };
    static const Stub kStubs[] = {
        {"/api/loops", false, "[]", "routines"},
        {"/api/fetchq", false, "[]", "downloads"},
        {"/api/wakeups", false, R"({"policy":"silent-allow","pending":null,"items":[]})", "wake-ups"},
        {"/api/skills", true, R"({"sd":true,"skills":[]})", "skills"},
    };
    for (const Stub& s : kStubs) {
      const bool hit = s.prefix ? base.rfind(s.base, 0) == 0 : base == s.base;
      if (!hit) continue;
      out = get ? okJson(s.getBody) : refuse(std::string("Not available on a hosted Nimbus yet: ") + s.feature);
      return true;
    }
    return tryStubsExtra(get, base, out);
  }

  // Read-only insight surfaces: the model catalog (CUM-545) and the diagnostics.
  bool tryInsight(const std::string& m, const std::string& base, const std::string& path, ApiResp& out) {
    if (m != "GET") return false;
    if (base == "/api/models") { out = modelsResp(path); return true; }
    return tryDiagnostics(base, path, out);
  }

  // ---- diagnostics (CUM-546): the device's turn anatomy and logs -------------
  // /api/lastturn: the last turn's prompt, input and raw output (it carries memories
  // and the directive, so it is token-gated like every /api/* route); /api/log: the
  // bounded engine log; /api/errlog/recent: its failure lines (?bytes=, <= 64 KB).
  bool tryDiagnostics(const std::string& base, const std::string& path, ApiResp& out) {
    static const char* kText = "text/plain; charset=utf-8";
    if (base == "/api/lastturn") {
      const std::string t = rig_->lastTurnAnatomy();
      out = t.empty() ? ApiResp{404, kText, "no turn has run yet"} : ApiResp{200, kText, t};
      return true;
    }
    if (base == "/api/log") { out = ApiResp{200, kText, NimbusdRig::logTail(64 * 1024, false)}; return true; }
    if (base == "/api/errlog/recent") {
      int bytes = atoiOr(queryParam(path, "bytes"), 4096);
      bytes = std::min(std::max(bytes, 256), 64 * 1024);
      out = ApiResp{200, kText, NimbusdRig::logTail((size_t)bytes, true)};
      return true;
    }
    return false;
  }

  // ---- files (CUM-543): the device's /api/files/* over the instance disk ------
  // The same store the assistant's files.* tools use (PosixFiles, the FileStore
  // traversal gate), so an upload is readable by the assistant and a saved
  // artifact shows in the panel. Engine thread (the store is single-context).
  // Downloads are served as application/octet-stream: never rendered on this
  // origin (the page saves or previews them from a blob).
  bool tryFiles(const std::string& m, const std::string& base, const std::string& path,
                const std::string& body, ApiResp& out) {
    if (base.rfind("/api/files/", 0) != 0) return false;
    const std::string proj = queryParam(path, "project"), name = queryParam(path, "name");
    if (base == "/api/files/list" && m == "GET") out = filesList(proj);
    else if (base == "/api/files/dl" && m == "GET") out = filesGet(proj, name);
    else if (base == "/api/files/upload" && m == "POST") out = filesUpload(proj, name, body);
    else if (base == "/api/files/rm" && m == "POST") out = filesRemove(body);
    else if (base == "/api/files/rmproject" && m == "POST") out = filesRemoveProject(body, proj);
    else out = refuse("Not available on a hosted Nimbus yet: " + base);
    return true;
  }
  ApiResp filesList(const std::string& proj) {
    return engineRead([this, proj] {
      JsonDocument d;
      d["present"] = true;
      d["unsupported"] = false;
      d["count"] = (int)rig_->files().count();
      d["bytes"] = (double)rig_->files().totalBytes();
      rig_->files().listInto(d["files"].to<JsonArray>(), proj);
      std::string out; serializeJson(d, out); return out;
    });
  }
  ApiResp filesGet(const std::string& proj, const std::string& name) {
    auto found = std::make_shared<bool>(false);
    ApiResp r = engineRead([this, proj, name, found] {
      std::string bytes;
      *found = rig_->files().fetch(proj, name, bytes);
      return bytes;
    });
    if (r.status != 200) return r;
    if (!*found) return notFound();
    r.ctype = "application/octet-stream";
    return r;
  }
  // The page posts one multipart file part (FormData "file"); the bytes run from
  // the part's header end to the next boundary line, binary-safe.
  static bool multipartFile(const std::string& body, std::string& bytes, std::string& filename) {
    const size_t eol = body.find("\r\n");
    if (body.rfind("--", 0) != 0 || eol == std::string::npos) return false;
    const std::string boundary = body.substr(0, eol);
    const size_t hdrEnd = body.find("\r\n\r\n", eol);
    if (hdrEnd == std::string::npos) return false;
    const std::string hdr = body.substr(eol, hdrEnd - eol);
    const size_t fn = hdr.find("filename=\"");
    if (fn != std::string::npos) filename = hdr.substr(fn + 10, hdr.find('"', fn + 10) - fn - 10);
    const size_t start = hdrEnd + 4;
    const size_t end = body.find("\r\n" + boundary, start);
    if (end == std::string::npos) return false;
    bytes = body.substr(start, end - start);
    return true;
  }
  ApiResp filesUpload(std::string proj, std::string name, const std::string& body) {
    std::string bytes, filename;
    if (!multipartFile(body, bytes, filename)) return refuse("No file in the upload. Choose a file and try again.");
    if (proj.empty()) proj = "uploads";
    if (name.empty()) name = filename;
    auto err = std::make_shared<std::string>();
    ApiResp r = engineWrite([this, proj, name, bytes, err] {
      return rig_->files().put(proj, name, bytes, *err) ? std::string(R"({"ok":true})") : std::string();
    }, 5);
    if (r.status == 200 && !err->empty()) return refuse("Couldn't store the file: " + *err + ".");
    return r;
  }
  ApiResp filesRemove(const std::string& body) {
    const std::string proj = formValue(body, "project"), name = formValue(body, "name");
    auto ok = std::make_shared<bool>(false);
    ApiResp r = engineWrite([this, proj, name, ok] {
      *ok = rig_->files().remove(proj, name);
      return std::string(R"({"ok":true})");
    }, 5);
    if (r.status == 200 && !*ok) return notFound();
    return r;
  }
  ApiResp filesRemoveProject(const std::string& body, std::string proj) {
    if (proj.empty()) proj = formValue(body, "project");
    if (formValue(body, "confirm") != "DELETE") return refuse("confirm phrase required");
    auto n = std::make_shared<int>(-1);
    ApiResp r = engineWrite([this, proj, n] {
      *n = rig_->files().removeProject(proj);
      return std::string();
    }, 5);
    if (r.status != 200) return r;
    if (*n < 0) return notFound();
    return okJson(R"({"ok":true,"removed":)" + std::to_string(*n) + "}");
  }

  // ---- connectors (CUM-424) - the REAL registry, device-contract-compatible.
  // GET returns the sanitized {configured,known,keyed,host} view (secrets never
  // echoed - tok/oauth become has-flags); POST takes exactly one of
  // blob=<array> | del=<name> | patch=<object> (form-encoded, token-gated at the
  // http layer like every /api/* route) and mirrors the device semantics:
  // patch preserves stored secrets when omitted, the final set is validated at
  // save time (CUM-255), and every successful write resets the conversation so
  // the new set rides the next turn. OAuth device-flow sign-in (CUM-256) is not
  // available on a hosted instance yet: /oauth/start says so honestly instead of
  // acking a flow that never runs; /oauth/status stays {"active":false}.
  bool tryConnectors(const std::string& m, const std::string& base,
                     const std::string& body, ApiResp& out) {
    const bool get = (m == "GET");
    if (base == "/api/connectors/oauth/status") {
      out = okJson(R"({"active":false})");
      return true;
    }
    if (base == "/api/connectors/oauth/start" || base == "/api/connectors/oauth/cancel") {
      out = base.rfind("cancel") != std::string::npos
                ? okJson(R"({"ok":true})")
                : ApiResp{400, "application/json",
                          R"({"error":"OAuth sign-in is not available on a hosted instance yet. Use a pasted token or a Studio connector."})"};
      return true;
    }
    if (base == "/api/connectors/catalog" && get) {
      // The model-facing "[PROVIDERS & CONNECTORS]" block exactly as the next turn's
      // context carries it, so an operator (and the e2e) can confirm the catalog and
      // the badges below agree. Token-gated like every /api/* route; no secrets in it.
      out = ApiResp{200, "text/plain; charset=utf-8", rig_->connectorsCatalog()};
      return true;
    }
    if (base != "/api/connectors") return false;
    if (get) {
      JsonDocument outDoc;
      // Each entry carries the derived `auth` (device parity, webui.cpp GET): the
      // same shared rule + workspace answer the catalog reads.
      rig_->connectors().sanitizedConfigured(
          outDoc["configured"].to<JsonArray>(),
          [this](const nimbus::orch::ConnectorInfo& ci) { return rig_->connectorAuth(ci); });
      JsonDocument known;
      if (!deserializeJson(known, nimbus::orch::knownCatalogJson())) outDoc["known"] = known;
      JsonObject keyed = outDoc["keyed"].to<JsonObject>();
      keyed["openai"] = rig_->providerKeyed("openai");
      keyed["anthropic"] = rig_->providerKeyed("anthropic");
      keyed["mistral"] = rig_->providerKeyed("mistral");
      outDoc["host"] = rig_->hostBadge();
      std::string s;
      serializeJson(outDoc, s);
      out = okJson(s);
      return true;
    }
    // POST: exactly one mode, device-identical error strings.
    const std::string blob = formValue(body, "blob");
    const std::string del = formValue(body, "del");
    const std::string patch = formValue(body, "patch");
    std::string err;
    if (!blob.empty())      err = rig_->connectors().replaceBlob(blob);
    else if (!del.empty())  err = rig_->connectors().removeByName(del);
    else                    err = rig_->connectors().patchUpsert(patch);
    if (!err.empty()) {
      JsonDocument e;
      e["error"] = err;
      std::string s;
      serializeJson(e, s);
      out = ApiResp{400, "application/json", s};
      return true;
    }
    // Engine-thread follow-ups (the conversation id and the memory stores belong to
    // the engine thread): drop the pinned conversation so the new set rides the next
    // turn, and - when the Mistral workspace has not answered yet (a first Studio
    // connector just enabled) - probe it so the connector can read usable without a
    // verify button (the hosted page has none).
    NimbusdRig* rig = rig_;
    eng_->postWork([rig] {
      rig->noteConnectorsWrite();
      rig->persist();
    });
    if (rig_->wantsWorkspaceProbe() && rig_->workspaceStale()) kickProbe();
    out = okJson(R"({"ok":true})");
    return true;
  }

  bool tryStubsExtra(bool get, const std::string& base, ApiResp& out) {
    if (base == "/api/trace" && get)    { out = {200, "text/plain; charset=utf-8", "Turn tracing is off on this instance."}; return true; }
    if (base == "/api/mem/blob" && get) { out = {404, "text/plain; charset=utf-8", "reason: no blob store on a hosted instance"}; return true; }
    if (base == "/api/preview")
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

  static constexpr int kReadWaitS = 2;   // an idle engine answers a read in milliseconds

  static ApiResp notFound() { return ApiResp{404, "application/json", R"({"error":"not found"})"}; }

  ApiResp engineRead(std::function<std::string()> fn) {
    // Never stall the single HTTP thread behind a turn: a turn can hold the engine
    // for a long time, and this thread also serves /healthz, /readyz and the
    // snapshot routes. If a turn is in flight, tell the poller to retry (503) at
    // once rather than blocking every other request behind the wait. Only when
    // the engine is idle do we dispatch and await (it answers in milliseconds;
    // the live embedding check runs as background work instead, see checkEmbed).
    if (eng_->snapshot().turnInFlight || eng_->busy()) return busy();
    auto fut = eng_->dispatchRead(std::move(fn));
    if (fut.wait_for(std::chrono::seconds(kReadWaitS)) != std::future_status::ready) return busy();
    try {
      return okJson(fut.get());
    } catch (...) {
      return busy();
    }
  }

  // A WRITE dispatched onto the engine thread. Once posted it will run, so a wait
  // that runs out answers "queued" (like orchPost), never a 503 that reads as
  // "nothing happened" and invites a duplicate write.
  ApiResp engineWrite(std::function<std::string()> fn, int waitS) {
    if (eng_->snapshot().turnInFlight || eng_->busy()) return busy();
    auto fut = eng_->dispatchRead(std::move(fn));
    if (fut.wait_for(std::chrono::seconds(waitS)) != std::future_status::ready)
      return ApiResp{202, "application/json", R"({"ok":true,"queued":true,"note":"Saved. It applies in a moment."})"};
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

  // A form value from a urlencoded request body (k=v&...; handle() reads a multipart
  // form into this shape first). "" when absent.
  static std::string formValue(const std::string& body, const std::string& key) {
    return kvLookup(body, key, '&', /*decode=*/true);
  }

  // Every form field NAME present in a urlencoded request body (`k=v&...`, name is
  // the part before `=`, URL-decoded; handle() reads a multipart form into this
  // shape first). Values are ignored - this exists so orchPost can refuse an unknown
  // key field rather than silently drop it (CUM-445).
  static std::vector<std::string> formFieldNames(const std::string& body) {
    std::vector<std::string> names;
    for (size_t start = 0; start < body.size();) {
      const size_t amp = body.find('&', start);
      const size_t end = (amp == std::string::npos) ? body.size() : amp;
      const std::string tok = body.substr(start, end - start);
      const size_t eq = tok.find('=');
      const std::string name = urlDecode(eq == std::string::npos ? tok : tok.substr(0, eq));
      if (!name.empty()) names.push_back(name);
      if (amp == std::string::npos) break;
      start = amp + 1;
    }
    return names;
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
    const HostedSettings st = rig_->settings();
    JsonDocument d;
    d["name"]    = st.devName;
    d["devName"] = st.devName;
    // Honest marker: a hosted instance, not a physical device. The current web app
    // ignores it; it is the seam a future UI (and the parity ledger) keys on to
    // hide hardware-only panels, and it is the truth for any API consumer.
    d["virtual"] = true;
    d["host"]    = "nimbusd";
    d["fw"]      = NIMBUS_FW_VERSION;   // the engine version this instance runs
    d["build"]   = NIMBUS_FW_BUILD;
    // The container image tag this instance runs, REPORTED SEPARATELY from the engine
    // version (CUM-444): the portal showed "fw nimbusd:latest" because the image tag
    // was all it had. `fw` is the real version (v4.5.x, shown like a physical device);
    // `image` is the secondary detail the platform bakes in (NIMBUSD_IMAGE_TAG, set by
    // the release build). Empty when unknown - the portal then says so honestly rather
    // than faking a version.
    d["image"]   = rig_->cfg().get("NIMBUSD_IMAGE_TAG");
    d["mode"]    = 1;                    // Orchestrator (a VN is an assistant)
    d["jobs"]    = s.turnInFlight ? 1 : 0;
    d["needsOnboarding"] = false;        // provisioned by the platform
    // Lane DEF: the monthly spend ceiling (the same shape as the device's).
    {
      JsonDocument cd;
      if (deserializeJson(cd, rig_->spendCeiling().stateJson()) == DeserializationError::Ok)
        d["spend"] = cd.as<JsonObject>();
      d["setupDefaults"] = rig_->spendCeiling().setupText();
    }
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
    clock["tz"] = rig_->clockTz();   // the owner's time zone (CUM-543), else the operator's, else UTC
    clock["epoch"] = (double)now;
    clock["local"] = hosted::localTime(now, "%Y-%m-%d %H:%M");
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
    // Walk the CANONICAL provider registry (lib/core provider_slots.h) so this GET,
    // the device UI, and orchPost all read the SAME slug/label/keyField table. The
    // reported keyField is the exact field the UI posts back (`oaiKey`/`antKey`/
    // `mistKey`/`zaiKey`/`cumuloKey`), which is what unlocks "Key set" and the model
    // pickers; the old `<slug>Key` drifted from the UI and dropped every write
    // (CUM-445). A slot added to the registry appears here already gated, no edit.
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++) {
      const auto& slot = nimbus::orch::kProviderSlots[i];
      const std::string slug = slot.slug;
      JsonObject o = provs[slug].to<JsonObject>();
      o["label"] = slot.label;
      o["keyField"] = slot.keyField;
      // cumulo is a router key, not a canonical-env BYOK slot, so hostAvailable()
      // does not see it - report it from hasCumulo() (CUM-286). Every other slot
      // (including Z.ai's Z_AI_TOKEN) is a real env key hostAvailable() reads.
      o["hasKey"] = rig_->keyedForEngine(slug);
      if (slot.recommended) o["recommended"] = true;
      // The real verdict (CUM-545): the provider's model-list read on start, on a
      // key save and on POST /api/verify, or a turn that answered on this head.
      // The page unlocks the model pickers on verify 1 and shows vfyReason on -1.
      const ProviderVerify pv = rig_->verifyState(slug);
      o["verify"] = pv.verify;
      o["vts"] = (double)pv.vts;
      if (!pv.reason.empty()) o["vfyReason"] = pv.reason;
      o["choices"] = rig_->modelChoicesFor(slug);
      // The resolved model this head requests (an in-app pick or the provider
      // default), same field the device page reads (CUM-425), and the sub-agent
      // model a spawn there runs (device subModel).
      o["orchModel"] = rig_->modelFor(slug);
      o["subModel"] = rig_->subModelFor(slug);
    }
    const HostedSettings st = rig_->settings();
    d["orchHost"] = st.orchHost;   // device parity ("" = first keyed)
    d["provPrio"] = st.provPrio;
    d["subPrio"] = rig_->subPriority();
    d["orchLoop"] = st.toolLoop;   // device parity (store::orchToolLoop)
    d["directive"] = st.directive;   // "" = the shipped default (the page pre-fills it)
    d["directiveDefault"] = nimbus::orch::kOwnerDirectiveDefault;
    d["directiveMax"] = nimbus::orch::kMemDirectiveMax;
    fillHostedKnobs(d);
    d["hasTav"] = rig_->cfg().has("TAVILY_API_KEY");
    d["hasTg"] = rig_->cfg().has("TELEGRAM_BOT_TOKEN");
    d["tgLive"] = rig_->cfg().has("TELEGRAM_BOT_TOKEN");
    // The Telegram allowlist (device /api/orch tgAllow): the readiness line reads it
    // ("No one approved yet" until the owner approves a chat).
    d["tgAllow"] = rig_->telegramAccess().allowCsv();
    // Usage (real session counters).
    JsonObject u = d["usage"].to<JsonObject>();
    u["sessIn"] = s.sessionTokensIn;
    u["sessOut"] = s.sessionTokensOut;
    u["turns"] = s.turnCount;
    u["lastIn"] = s.lastTokensIn;
    u["lastOut"] = s.lastTokensOut;
    u["byProvider"].to<JsonArray>();
    {
      JsonDocument cd;   // lane DEF: the Usage page's monthly ceiling meter
      if (deserializeJson(cd, rig_->spendCeiling().stateJson()) == DeserializationError::Ok)
        u["ceiling"] = cd.as<JsonObject>();
    }
    d["jobs"].to<JsonArray>();
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // The device knobs a hosted instance does not offer, reported at their hosted
  // values (hosted_settings.h fixedKnobs) so the page renders them truthfully and
  // its Save Changes round-trips them as a no-op. The custom endpoint is absent.
  static void fillHostedKnobs(JsonDocument& d) {
    for (const auto& k : hosted::fixedKnobs()) {
      const std::string f = k.field;
      if (f.rfind("cust", 0) == 0) continue;   // reported under "cust" below
      if (k.boolean) d[f] = std::string(k.value) == "1";
      else d[f] = std::atoi(k.value);
    }
    JsonObject cust = d["cust"].to<JsonObject>();
    cust["base"] = "";
    cust["conv"] = "openai";
    cust["model"] = "";
    cust["hasKey"] = false;
  }

  // ------------------------------------------------------------------ /api/orch POST
  // In-app provider keys + model picks, device parity (CUM-279, CUM-425): the SAME
  // fields the device's Providers & keys form posts (`<slug>Key` / `clr_<slug>Key`
  // for keys, `orchM_<slug>` / `clr_orchM_<slug>` for the per-head model), applied
  // through the rig seams so a value set in the UI takes effect - no external
  // attach. Every other field applies or is refused by name (CUM-543, see orchPost).
  // The apply runs on the engine thread (serialized; refused mid-turn with a 503
  // retry) because it rebuilds the engine to register the newly-keyed head or the
  // re-routed model.
  // One field pair per write surface: `<name>` sets, `clr_<name>` clears. Returns
  // true when either form field was present (a set beats a clear in one post).
  static bool formWrite(const std::string& body, const std::string& name,
                        const std::string& host,
                        std::vector<std::pair<std::string, std::string>>& out) {
    const std::string v = formValue(body, name);
    if (!v.empty()) { out.push_back({host, v}); return true; }
    if (!formValue(body, "clr_" + name).empty()) {
      out.push_back({host, std::string()});
      return true;
    }
    return false;
  }

  // A posted field name is a key write iff it is `<x>Key` (or `clr_<x>Key`).
  static bool looksLikeKeyField(std::string name, std::string& baseOut) {
    if (name.rfind("clr_", 0) == 0) name = name.substr(4);
    const size_t n = name.size();
    if (n <= 3 || name.compare(n - 3, 3, "Key") != 0) return false;
    baseOut = name;
    return true;
  }

  // Whether a `<x>Key` base is a key field the orch surface legitimately handles: a
  // canonical PROVIDER keyField (oaiKey/antKey/mistKey/zaiKey/cumuloKey), OR one of the
  // non-provider key fields the shared orch form also posts - the custom endpoint key
  // and the Tavily key. Anything else is provider-key drift (openaiKey/mistralKey/a
  // typo) that must be refused (CUM-445), NOT the custom/Tavily fields the Save Changes
  // payload carries (refusing those would break saving the orch settings).
  static bool isRecognizedKeyField(const std::string& base) {
    if (!NimbusdRig::hostForKeyField(base).empty()) return true;
    return base == "custKey" || base == "tavKey";
  }

  // A 400 ApiResp naming the first unknown key field in the body, or status 0 when
  // every key field is one the canonical registry owns. Refusing an unknown key field
  // LOUDLY (instead of the old silent {"ok":true}) is what keeps the CUM-445 field-name
  // drift that dropped every direct-provider write from hiding again.
  ApiResp unknownKeyFieldError(const std::string& body) {
    for (const std::string& name : formFieldNames(body)) {
      std::string base;
      if (looksLikeKeyField(name, base) && !isRecognizedKeyField(base)) return refuse("unknown field " + base);
    }
    return ApiResp{0, "", ""};
  }

  // A new Mistral key resets the workspace answer (applyProviderKey); queue a fresh
  // probe behind the key apply so a Studio connector reads usable again without a
  // verify button (device parity: save-and-verify runs the probe after the verify).
  // (A clear re-probes too: an env/file key may be active again underneath.)
  void reprobeOnMistralKey(const std::vector<std::pair<std::string, std::string>>& keyWrites) {
    if (!rig_->wantsWorkspaceProbe(/*assumeKey=*/true)) return;   // nothing to probe for
    for (const auto& w : keyWrites) {
      if (w.first != "mistral") continue;
      kickProbe();
      return;
    }
  }

  // Queue ONE background workspace probe (coalesced: none while one is queued).
  void kickProbe() {
    if (!rig_->claimProbe()) return;
    NimbusdRig* rig = rig_;
    eng_->postWork([rig] { rig->refreshMistralWorkspace(); });
  }

  // tgAllow=<csv> (device /api/orch): replace the Telegram allowlist, persisted on the
  // instance volume and live at once (no restart). An empty value clears it, which
  // fails CLOSED (nobody is served). Status 0 = the field was absent or applied.
  ApiResp allowlistWrite(const std::string& body) {
    const auto names = formFieldNames(body);
    if (std::find(names.begin(), names.end(), "tgAllow") == names.end()) return ApiResp{0, "", ""};
    std::string err;
    if (rig_->telegramAccess().setAllowCsv(formValue(body, "tgAllow"), err)) return ApiResp{0, "", ""};
    return refuse(err);
  }

  using Writes = std::vector<std::pair<std::string, std::string>>;   // (host, value); "" clears

  // The key and model writes a body carries. The key FIELD names come from the
  // canonical registry (provider_slots.h keyField), the SAME table hostForKeyField and
  // /api/orch read, so what the UI posts and what this consumes cannot drift.
  static void collectWrites(const std::string& body, Writes& keyWrites, Writes& modelWrites) {
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++) {
      const std::string field = nimbus::orch::kProviderSlots[i].keyField;
      const std::string host = NimbusdRig::hostForKeyField(field);
      if (host.empty()) continue;
      formWrite(body, field, host, keyWrites);
      formWrite(body, "orchM_" + host, host, modelWrites);
    }
  }

  // A multipart form as the urlencoded body (multipart_form.h), or the refusal: a
  // file part outside the Files panel's upload, a form past the bounds, or one that
  // cannot be read.
  static bool readForm(const std::string& body, const std::string& contentType,
                       std::string& fields, ApiResp& out) {
    switch (form::toUrlencoded(body, form::boundaryOf(contentType), fields)) {
      case form::Result::Ok:
        return true;
      case form::Result::FilePart:
        out = refuse("Files can be added only from the Files panel.");
        return false;
      case form::Result::TooLarge:
        out = refuse("The form is too large. Shorten it and try again.");
        out.status = 413;
        return false;
      default:
        out = refuse("Couldn't read the form. Reload the page and try again.");
        return false;
    }
  }

  // A 400 with the sentence the page shows (never a 200 over a dropped write).
  static ApiResp refuse(const std::string& why) {
    JsonDocument e;
    e["ok"] = false;
    e["error"] = why;
    std::string out;
    serializeJson(e, out);
    return ApiResp{400, "application/json", out};
  }

  // The settings and actions one POST carries, validated before anything applies.
  struct OrchWrites {
    Writes keys, models, subs;
    std::vector<std::function<void(HostedSettings&)>> settings;
    bool clearConv = false, clearMem = false, convReset = false;
    bool empty() const {
      return keys.empty() && models.empty() && subs.empty() && settings.empty() && !clearConv &&
             !clearMem && !convReset;
    }
  };

  static bool present(const std::vector<std::string>& names, const char* f) {
    return std::find(names.begin(), names.end(), f) != names.end();
  }

  // Validate the routing/directive/loop fields; "" or the refusal sentence.
  static std::string collectSettings(const std::string& body, const std::vector<std::string>& names,
                                     OrchWrites& w) {
    if (present(names, "orchHost")) {
      const std::string h = formValue(body, "orchHost");
      if (h == "custom") return hosted::notAvailable("custom endpoint");
      if (!hosted::validHost(h)) return "Choose a provider this instance can run.";
      w.settings.push_back([h](HostedSettings& s) { s.orchHost = h; });
    }
    for (const char* f : {"provPrio", "subPrio"}) {
      if (!present(names, f)) continue;
      const std::string order = hosted::sanitizePriority(formValue(body, f));
      if (order.empty()) return "Turn on at least one provider in the order.";
      const bool head = std::string(f) == "provPrio";
      w.settings.push_back([order, head](HostedSettings& s) { (head ? s.provPrio : s.subPrio) = order; });
    }
    if (present(names, "sysPrompt")) {
      const std::string dir = formValue(body, "sysPrompt");
      if ((int)dir.size() > nimbus::orch::kMemDirectiveMax)
        return "The directive is too long. Keep it under " + std::to_string(nimbus::orch::kMemDirectiveMax) +
               " characters.";
      w.settings.push_back([dir](HostedSettings& s) { s.directive = dir; });
    }
    if (present(names, "orchLoop")) {
      const bool on = hosted::normBool(formValue(body, "orchLoop")) == "1";
      w.settings.push_back([on](HostedSettings& s) { s.toolLoop = on; });
    }
    return std::string();
  }

  // Every posted field applies or is refused before anything changes, so a 400 always
  // means nothing was stored. "" or the refusal.
  static std::string collectOrchWrites(const std::string& body, OrchWrites& w) {
    const std::vector<std::string> names = formFieldNames(body);
    for (const std::string& f : names)
      if (hosted::orchFieldVerdict(f, formValue(body, f)) == hosted::Verdict::Refuse)
        return hosted::notAvailable(f);
    collectWrites(body, w.keys, w.models);
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++) {
      const std::string slug = nimbus::orch::kProviderSlots[i].slug;
      formWrite(body, "subM_" + slug, slug, w.subs);
    }
    for (const auto* list : {&w.models, &w.subs})
      for (const auto& m : *list)
        if (!NimbusdRig::validSelector(m.second)) return "That model name is not valid.";
    // The device's own key rule and sentence (lib/core key_shape.h, CUM-545): spaces
    // around a key are dropped, and it is stored as checked.
    for (auto& k : w.keys) {
      if (k.second.empty()) continue;
      const std::string why = NimbusdRig::keyShapeErrorFor(k.first, k.second);
      if (!why.empty()) return why;
      k.second = NimbusdRig::storableKey(k.second);
      // A posted key never becomes "" (which erases the stored one), even if a shape
      // ever allowed what the store refuses.
      if (k.second.empty()) return "That key is not valid. Paste just the key and save again.";
    }
    w.clearConv = formValue(body, "clearConv") == "1";
    w.clearMem = formValue(body, "clearMem") == "1";
    w.convReset = present(names, "convReset");
    return collectSettings(body, names, w);
  }

  // Apply a validated write set on the engine thread (keys and model picks rebuild
  // the engine; the clears touch the conversation and memory stores).
  // Returns "" when the settings could not be stored (the caller answers 500;
  // nothing in the settings changed - SettingsStore applies only what persists).
  std::string applyOrchWrites(const OrchWrites& w) {
    int applied = applyKeyWrites(w.keys);
    for (const auto& m : w.models) applied += rig_->applyOrchModel(m.first, m.second) ? 1 : 0;
    for (const auto& m : w.subs) applied += rig_->applySubModel(m.first, m.second) ? 1 : 0;
    if (!w.settings.empty()) {
      if (!rig_->updateSettings([&w](HostedSettings& s) { for (const auto& fn : w.settings) fn(s); }))
        return std::string();
      applied += (int)w.settings.size();
    }
    if (w.clearConv) { rig_->clearConversation(kWebChat); applied++; }
    if (w.clearMem) { rig_->clearRunningMemory(); applied++; }
    if (w.convReset) { rig_->resetConversations(); applied++; }
    const char* note = "Saved and applied.";
    if (!w.keys.empty()) note = "Key saved and applied.";
    else if (!w.models.empty()) note = "Model saved and applied.";
    JsonDocument d;
    d["ok"] = true;
    d["applied"] = applied;
    d["note"] = note;
    std::string out;
    serializeJson(d, out);
    return out;
  }

  // Apply the key writes (engine thread) and queue the checks each new key needs
  // HERE, right after the apply, so they always read the key just applied: a check
  // queued earlier (the boot verify, a probe) runs before this apply and releases
  // its claim as it starts, so the claim is free again (CUM-545: a key saved while
  // a verify was still queued used to be checked with the OLD key, then never again).
  int applyKeyWrites(const Writes& keys) {
    int applied = 0;
    for (const auto& k : keys) applied += rig_->applyProviderKey(k.first, k.second) ? 1 : 0;
    reprobeOnMistralKey(keys);
    for (const auto& k : keys)
      if (!k.second.empty()) kickVerify(k.first);
    return applied;
  }

  // Lane DEF: POST spendCeil=<cents> (1 to 100,000 USD), the owner's monthly ceiling;
  // status 0 when absent. Applied at once (the state file, not a hosted setting).
  ApiResp spendCeilWrite(const std::string& body) {
    if (!present(formFieldNames(body), "spendCeil")) return ApiResp{0, "", ""};
    const uint64_t micro = nimbus::orch::parseCeilingCents(formValue(body, "spendCeil"));
    if (micro == 0) return refuse("spend ceiling must be a positive whole number of cents");
    if (!rig_->spendCeiling().setLimitMicro(micro))
      return ApiResp{507, "application/json", R"({"error":"storage_full","note":"Couldn't save the ceiling."})"};
    return okJson(R"({"ok":true,"applied":1})");
  }

  ApiResp orchPost(const std::string& body) {
    const ApiResp unknown = unknownKeyFieldError(body);
    if (unknown.status != 0) return unknown;
    OrchWrites w;
    const std::string why = collectOrchWrites(body, w);
    if (!why.empty()) return refuse(why);
    // Refuse a mid-turn write BEFORE anything else in the body applies, so a 503
    // always means "nothing changed".
    if (!w.empty() && eng_->snapshot().turnInFlight) return busy();
    const ApiResp ceil = spendCeilWrite(body);
    if (ceil.status != 0 && ceil.status != 200) return ceil;
    const ApiResp allow = allowlistWrite(body);
    if (allow.status != 0) return allow;
    if (w.empty()) {   // only no-op knobs, or the allowlist / the ceiling (applied above)
      const bool tg = present(formFieldNames(body), "tgAllow") || ceil.status == 200;
      return okJson(tg ? R"({"ok":true,"applied":1})" : R"({"ok":true,"applied":0})");
    }
    auto fut = eng_->dispatchRead([this, w]() { return applyOrchWrites(w); });
    // The write is QUEUED on the engine thread and will apply: past the wait (a
    // background probe ahead of it), say so rather than a 503 that reads as failed
    // and invites a duplicate save.
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
      return okJson(R"({"ok":true,"queued":true,"note":"Saved. It applies in a moment."})");
    try {
      const std::string out = fut.get();
      return out.empty() ? ApiResp{500, "application/json", R"({"ok":false,"error":"Couldn't save. Try again."})"}
                         : okJson(out);
    } catch (...) {
      return busy();
    }
  }

  // ------------------------------------------------------------------ /api/config
  // The Settings page (device parity webui.cpp /api/config): the name and the time
  // zone apply at once (persisted on the instance volume), "Sync now" is a no-op
  // (the host clock is always synced), and every device-only field (mode, battery,
  // display, onboarding, updates) is refused with its name.
  ApiResp configPost(const std::string& body) {
    const std::vector<std::string> names = formFieldNames(body);
    for (const std::string& f : names)
      if (hosted::configFieldVerdict(f) == hosted::Verdict::Refuse) return refuse(hosted::notAvailable(f));
    std::vector<std::function<void(HostedSettings&)>> fns;
    if (present(names, "devName")) {
      const std::string n = formValue(body, "devName");
      if (!hosted::validName(n)) return refuse("Choose a name of 1 to 32 characters.");
      fns.push_back([n](HostedSettings& s) { s.devName = n; });
    }
    if (present(names, "devTz")) {
      const std::string tz = hosted::trim(formValue(body, "devTz"));
      if (!hosted::validTz(tz))
        return refuse("Unknown time zone. Use a POSIX string such as GMT0BST,M3.5.0/1,M10.5.0/2 or a "
                      "zone name such as Europe/London.");
      fns.push_back([tz](HostedSettings& s) { s.devTz = tz; });
    }
    // The settings store and the time zone are thread-safe; no engine state changes.
    if (!fns.empty() && !rig_->updateSettings([&fns](HostedSettings& s) { for (const auto& fn : fns) fn(s); }))
      return ApiResp{500, "application/json", R"({"ok":false,"error":"Couldn't save. Try again."})"};
    return okJson(R"({"ok":true})");
  }
  ApiResp configGet() {
    const HostedSettings st = rig_->settings();
    JsonDocument d;
    d["devName"] = st.devName;
    d["devTz"] = st.devTz;
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ /api/verify
  // Device parity (webui.cpp /api/verify): accept the request, run the check in the
  // background, answer at once. Every keyed provider gets the device's verify (one
  // model-list read, CUM-545; GET /api/orch shows the verdict); provider=mistral also
  // re-runs the workspace probe that makes Studio connectors usable (GET
  // /api/connectors shows it). Both are queued on the engine thread, serialized with
  // turns.
  ApiResp verifyPost(const std::string& body) {
    const std::string prov = formValue(body, "provider");
    if (prov == "mistral" && rig_->wantsWorkspaceProbe()) kickProbe();
    // Nothing to check without a key: acknowledged, and the badge stays unverified.
    if (!rig_->keyedForEngine(prov)) return okJson(R"({"ok":true,"verified":false})");
    kickVerify(prov);
    return okJson(R"({"ok":true})");
  }
  // Queue ONE background verify for a provider (coalesced, serialized with turns).
  void kickVerify(const std::string& slug) {
    if (!rig_->claimVerify(slug)) return;
    NimbusdRig* rig = rig_;
    eng_->postWork([rig, slug] { rig->verifyProvider(slug); });
  }

  // GET /api/models[?provider=<slug>] - the model catalog the page's pickers group
  // (device shape, webui.cpp buildModelsCatalog): per provider the verdict and the
  // models the last verify harvested (the router's priced catalog for Cumulo).
  ApiResp modelsResp(const std::string& path) {
    const std::string only = queryParam(path, "provider");
    JsonDocument d;
    d["generatedAt"] = (double)time(nullptr);
    d["ttlSec"] = 86400;
    int rc = 0;
    const char* const* rt = nimbus::orch::roleTokens(rc);
    JsonArray roles = d["roles"].to<JsonArray>();
    for (int i = 0; i < rc; i++) roles.add(rt[i]);
    JsonObject provs = d["providers"].to<JsonObject>();
    for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++) {
      const std::string slug = nimbus::orch::kProviderSlots[i].slug;
      if (!only.empty() && only != slug) continue;
      const ProviderVerify pv = rig_->verifyState(slug);
      JsonObject o = provs[slug].to<JsonObject>();
      o["keyed"] = rig_->keyedForEngine(slug);
      o["verified"] = pv.verify;
      if (!pv.reason.empty()) o["vfyReason"] = pv.reason;
      o["refreshedAt"] = (double)pv.catalogAt;
      o["stale"] = pv.catalogAt == 0;
      nimbus::orch::modelsToJson(pv.models, o["models"].to<JsonArray>(), /*includeUnusable=*/false);
    }
    std::string out;
    serializeJson(d, out);
    return okJson(out);
  }

  // ------------------------------------------------------------------ /api/connect
  ApiResp connectResp() {
    JsonDocument d;
    d["name"] = rig_->settings().devName;
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
    // Lane DEF: paid calls are paused at the monthly spend ceiling: answer at once.
    if (nimbus::orch::spendFrozen())
      return ApiResp{409, "application/json",
                     R"({"error":"Paid AI calls are paused: this month's spend ceiling is reached. )"
                     R"(Raise it on Assistant > Usage.","code":"spend_ceiling"})"};
    // A mid-turn message is QUEUED, never rejected: the prompt row is recorded and
    // the turn is posted to the engine mailbox (which runs turns strictly in order).
    // The turn id IS this prompt row's seq, and it rides the postMessage so every
    // reply the engine delivers for this turn is tagged with it. The web matcher then
    // pairs each turn to its OWN reply by id, never by ring position - so a turn that
    // delivers zero or several messages can no longer shift or drop a neighbour's
    // reply (CUM-218 stale-replay -> CUM-293 the burst residual).
    const uint64_t turn = replies_->push("user", text, kWebChat);
    pending_.push_back(PendingTurn{turn, time(nullptr) + 900});
    // Flood backstop (symmetry with resolved_'s cap): an authenticated owner hammering
    // POSTs faster than the single engine thread replies cannot grow this without bound.
    while (pending_.size() > 256) pending_.pop_front();
    eng_->postMessage(kWebChat, text, turn);
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
      // The truth for THIS instance (CUM-540): a direct key, or the Cumulo router
      // carrying the same model, can embed right now.
      d["embedAvailable"] = rig_->embedAvailable();
      d["sdPresent"] = true;    // durable instance volume plays the SD role
      d["flashFull"] = false;
      d["embedLocked"] = rig_->vectors().size() > 0;
      JsonObject e = d["embed"].to<JsonObject>();
      e["provider"] = rig_->embedProvider();
      e["model"] = rig_->embedModelName();
      e["dims"] = rig_->embedWidth();
      e["route"] = rig_->embedRouteLabel();
      fillEmbedCheck(d);
      std::string out; serializeJson(d, out); return out;
    });
  }

  // ---- embedding settings (CUM-540, device parity web_memory.cpp) -----------
  // Verify and Save make one live embeddings call for the candidate (embed_check.h).
  // It runs on the engine thread and can take the whole embed timeout (30 s), while
  // this thread also serves /healthz and every poller and the relay times a request
  // out at 30 s: so these wait at most kEmbedWaitS, then answer "still checking"
  // (the call goes on; the next Verify or Save of the same candidate answers from
  // its result at once). Refused at once mid-turn.
  static constexpr int kEmbedWaitS = 5;
  static constexpr const char* kStillChecking = "Still checking this model. Save again in a few seconds.";
  static constexpr const char* kCheckingOther = "Still checking another model. Try again in a few seconds.";

  // The candidate a verify or save names, or the refusal sentence ("" = valid).
  std::string embedCandidate(const std::string& body, EmbedCandidate& c) const {
    c.provider = formValue(body, "provider");
    c.model = formValue(body, "model");
    c.dims = atoiOr(formValue(body, "dims"), 0);
    c.keyGen = rig_->keyGen();
    return NimbusdRig::embedCfgRefusal(c.provider, c.model, c.dims);
  }

  // A settled check of `c` in `r`: a fresh result is reused, else ONE live call is
  // queued on the engine thread and awaited for kEmbedWaitS. False (`r` = the check
  // still running) when nothing has settled in time.
  bool checkEmbed(const EmbedCandidate& c, EmbedCheck& r) {
    const EmbedCheckSlot::Next n = embedCheck_.begin(c, steadyS(), r);
    if (n != EmbedCheckSlot::Next::Run) return n == EmbedCheckSlot::Next::Settled;
    // Background work, not a turn: /api/state and the other writes are not held
    // up as if the assistant were answering while the call runs.
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> fut = done->get_future();
    const uint32_t gen = r.gen;
    eng_->postWork([this, c, gen, done] {
      runEmbedCheck(c, gen);
      done->set_value();
    });
    const bool settled = fut.wait_for(std::chrono::seconds(kEmbedWaitS)) == std::future_status::ready;
    r = embedCheck_.get();
    if (!settled || !(r.cand == c) || r.state == EmbedCheck::State::Checking) return false;
    embedCheck_.markReported();
    return true;
  }
  // The live call (engine thread): the model must answer, and at the width asked
  // for when one is set (Mistral ignores a requested width; saving one it does not
  // produce made every later memory write fail).
  void runEmbedCheck(const EmbedCandidate& c, uint32_t gen) {
    if (!embedCheck_.start(gen, steadyS())) return;   // replaced while it waited: no call
    std::string err;
    int width = 0;
    try {
      width = (int)rig_->embedWith(NimbusdRig::kEmbedProbeText, err, c.provider, c.model, c.dims).size();
      if (width > 0 && c.dims > 0 && width != c.dims)
        err = "That model returns " + std::to_string(width) + " dimensions. Set Dimensions to " +
              std::to_string(width) + " or 0.";
      if (width == 0 && err.empty()) err = "no answer";
    } catch (...) {
      err = "check failed";
    }
    embedCheck_.settle(gen, width, err, steadyS());
  }
  static int64_t steadyS() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  // A check that has not settled: nothing is saved. Verify answers 202 (the page
  // shows the sentence and saves nothing); Save answers 503, because the page
  // reads any 2xx as saved.
  static ApiResp embedNotSettled(const EmbedCheck& r, const EmbedCandidate& c, int status) {
    JsonDocument d;
    d["ok"] = false;
    d["checking"] = true;
    d["error"] = (r.cand == c) ? kStillChecking : kCheckingOther;
    std::string out;
    serializeJson(d, out);
    return ApiResp{status, "application/json", out};
  }

  // The shared start of verify and save: the candidate the body names, then its
  // settled check in `r`. An early answer (a refusal, busy mid-turn, or "still
  // checking" with `pendingStatus`) has a non-zero status; status 0 = `r` is settled.
  ApiResp settledEmbedCheck(const std::string& body, int pendingStatus, EmbedCandidate& c, EmbedCheck& r) {
    const std::string why = embedCandidate(body, c);
    if (!why.empty()) return refuse(why);
    if (eng_->snapshot().turnInFlight) return busy();
    if (!checkEmbed(c, r)) return embedNotSettled(r, c, pendingStatus);
    return ApiResp{0, "", ""};
  }

  // POST embedverify: one REAL embeddings call with a candidate provider/model/dims,
  // saving nothing; the page only saves when it works.
  ApiResp embedVerify(const std::string& body) {
    EmbedCandidate c;
    EmbedCheck r;
    const ApiResp early = settledEmbedCheck(body, 202, c, r);
    if (early.status != 0) return early;
    const bool ok = r.state == EmbedCheck::State::Ok;
    JsonDocument d;
    d["ok"] = ok;
    d["dims"] = r.width;
    if (!ok) d["error"] = r.error;
    std::string out;
    serializeJson(d, out);
    return ApiResp{ok ? 200 : 400, "application/json", out};
  }

  // POST embedcfg: saved only at the width the live check read (dims 0 = the
  // model's own; a set width must be the one the model returns), never on an
  // unchecked width.
  ApiResp embedCfgPost(const std::string& body) {
    EmbedCandidate c;
    EmbedCheck r;
    const ApiResp early = settledEmbedCheck(body, 503, c, r);
    if (early.status != 0) return early;
    if (r.state != EmbedCheck::State::Ok)
      return refuse(r.width > 0 ? r.error : "Couldn't check the model (" + r.error + "). Nothing was saved.");
    const bool reset = formValue(body, "reset") == "1";
    const int width = r.width;
    auto st = std::make_shared<int>(200);
    // Posted even if the engine is still finishing the check's work (engineWrite
    // would answer busy in that instant); mid-turn was refused above. A save still
    // queued past the wait is NOT answered as saved: the engine thread re-checks the
    // lock when it runs (a turn ahead of it may store the first memory), so it can
    // still be refused, and the page reads any 2xx as saved.
    auto fut = eng_->dispatchRead([this, c, width, reset, st]() -> std::string {
      const std::string err = rig_->applyEmbedCfg(c.provider, c.model, width, reset, *st);
      return err.empty() ? embedCfgJson() : refuse(err).body;
    });
    if (fut.wait_for(std::chrono::seconds(kEmbedWaitS)) != std::future_status::ready)
      return ApiResp{503, "application/json", R"({"ok":false,"pending":true,"error":"Still saving. Check the embedding settings again in a moment."})"};
    try {
      ApiResp out = okJson(fut.get());
      if (out.status == 200) out.status = *st;
      return out;
    } catch (...) {
      return busy();
    }
  }

  ApiResp embedCfgGet() {
    return engineRead([this] { return embedCfgJson(); });
  }
  std::string embedCfgJson() {
    JsonDocument d;
    d["ok"] = true;
    d["provider"] = rig_->embedProvider();
    d["model"] = rig_->embedModelName();
    d["dims"] = rig_->embedWidth();
    d["locked"] = rig_->vectors().size() > 0;
    d["vectors"] = rig_->vectors().size();
    fillEmbedCheck(d);
    std::string out; serializeJson(d, out); return out;
  }
  // The last embedding check, settled or still running (GET /api/mem/embedcfg and
  // /api/mem/stats), so a check that outlived its request is not lost to the page.
  void fillEmbedCheck(JsonDocument& d) const {
    const EmbedCheck r = embedCheck_.get();
    if (r.state == EmbedCheck::State::None) return;
    static const char* const kState[] = {"none", "checking", "ok", "failed"};
    JsonObject o = d["check"].to<JsonObject>();
    o["state"] = kState[(int)r.state];
    o["provider"] = r.cand.provider;
    o["model"] = r.cand.model;
    o["dims"] = r.cand.dims;
    o["width"] = r.width;
    if (!r.error.empty()) o["error"] = r.error;
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
      count += connectorToolRows(arr);
      d["count"] = count;
      std::string out; serializeJson(d, out); return out;
    });
  }

  // prov/kind are free-form blob strings and the page renders the tag as HTML, so
  // only an identifier-shaped value passes through; anything else reads "custom".
  static std::string tagSafe(const std::string& v) {
    if (v.empty() || v.size() > 24) return "custom";
    for (char ch : v)
      if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return "custom";
    return v;
  }

  // Provider-side connector rows for the Capabilities table, device parity
  // (web_memory.cpp handleToolsGet): each classified by the shared connectorScope
  // over the SAME auth-stamped view the catalog and GET /api/connectors read.
  int connectorToolRows(JsonArray arr) {
    const nimbus::orch::ProviderState ps = rig_->providerState();
    int n = 0;
    for (const auto& c : rig_->connectorsView()) {
      JsonObject o = arr.add<JsonObject>();
      o["group"] = "connector";
      o["name"] = c.name;
      o["description"] = "Provider-side connector: the cloud provider runs it for the model.";
      o["tag"] = tagSafe(c.prov) + " " + tagSafe(c.kind) + (c.enabled ? "" : " (disabled)");
      o["rides_loop"] = false;
      o["availability"] = nimbus::orch::capScopeSlug(nimbus::orch::connectorScope(c, ps));
      n++;
    }
    return n;
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

  // Per-model spend and prices (device GET /api/usage/models): a hosted instance
  // keeps no usage ledger yet (CUM-562), so there are no spend rows and no editable
  // models (the Prices editor is hidden on hosted). The body comes from the same
  // portable writer as the device's, over an empty ledger, so every provider's prices
  // ride the device's shape - the fold price (provRate oIn/oOut) a page uses for a
  // model with no row of its own included (gate G4) - and the two cannot drift.
  ApiResp usageModels() {
    const nimbus::orch::UsageLedger empty;
    std::vector<nimbus::orch::EditableModels> provs;
    for (const auto& slot : nimbus::orch::kProviderSlots) provs.push_back({slot.slug, ""});
    provs.push_back({"custom", ""});
    std::string out(nimbus::orch::usageModelsJsonCap(empty, provs), '\0');
    out.resize(nimbus::orch::writeUsageModelsJson(empty, provs, &out[0], out.size()));
    return okJson(out);
  }

  // ------------------------------------------------------------------ hardware
  static bool isHardwarePath(const std::string& base) {
    static const char* kHw[] = {
        "/api/audio/mic", "/api/audio/beep", "/api/audio/sfx", "/api/audio/loopback",
        "/api/battcal", "/api/ota/check", "/api/ota/apply", "/api/sdprobe",
        "/api/sdreset", "/api/sdformat", "/api/wifi", "/api/poweroff",
        "/api/factory-reset", "/api/ble/forget", "/api/cloud",
        "/api/onboard/complete", "/api/onboard/restart", "/api/wifi/handoff"};
    for (const char* h : kHw) if (base == h) return true;
    return false;
  }

  ApiResp hardwareResp(const std::string& base) {
    // Honest virtual truth: the actuation is not available on a hosted instance.
    // The web app surfaces the message; nothing is faked and nothing silently
    // "succeeds". (/api/config has its own handler: configPost.)
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


 public:
  // The instance web token, seeded into the page and handed back by signin. Set by
  // HttpControl at construction (kept here for the sign-in seam).
  void setWebToken(std::string t) { webToken_ = std::move(t); }

 private:
  NimbusdRig* rig_;
  EngineThread* eng_;
  ReplyBuffer* replies_;
  TelegramWeb tg_;   // /api/telegram* + /api/tenant (CUM-459)
  std::string webToken_;
  EmbedCheckSlot embedCheck_;   // the embedding settings' live check (CUM-540)

  // Web chat reply matching (CUM-218). A single slot could track only ONE pending
  // web turn, so a mid-turn rapid tap either dropped a turn or replayed a prior
  // turn's reply. Now every web turn is tracked by id (its prompt row's seq) and
  // resolved in the order posted. `pending_` is the FIFO of turns still awaiting a
  // reply; `resolved_` holds ready replies (keyed by turn id) until the client
  // polls for them. Each turn is matched to its reply by the turn id carried on the
  // assistant ring entries (CUM-293), so no reply is ever handed to the wrong turn
  // even when a turn delivers zero or several messages. Touched only on the single
  // HTTP thread, so no synchronization is needed.
  // The web channel's delivery chat id (a web turn is posted as, and its reply is
  // delivered to, this chat). The reply matcher consumes only assistant replies on
  // this channel, so a Telegram/routine reply cannot surface in the web bubble.
  static constexpr const char* kWebChat = kWebChatId;   // rig.h: the synchronous channel
  struct PendingTurn { uint64_t userSeq; time_t deadline; };
  std::deque<PendingTurn> pending_;
  std::map<uint64_t, std::string> resolved_;

  // Advance FIFO resolution: match each waiting turn, oldest first, to the reply(ies)
  // tagged with its OWN id (the last, when a turn delivered several). Stops at the
  // first turn not yet resolvable - nothing behind it can settle before it (turns run
  // in order). A turn that delivered nothing resolves to an honest empty as soon as a
  // LATER web turn has delivered (proof this turn already ran), or at its backstop
  // deadline - so a silent turn never spins forever and never steals a later turn's
  // reply (CUM-293).
  void resolveWebTurns() {
    while (!pending_.empty()) {
      PendingTurn& t = pending_.front();
      std::string text;
      const ReplyBuffer::TurnMatch m = replies_->matchWebTurn(t.userSeq, kWebChat, text);
      if (m == ReplyBuffer::TurnMatch::Own) {
        stashResolved(t.userSeq, text);
        pending_.pop_front();
        continue;
      }
      if (m == ReplyBuffer::TurnMatch::Later || time(nullptr) > t.deadline) {
        stashResolved(t.userSeq, "");   // this turn ran and produced no reply of its own
        pending_.pop_front();
        continue;
      }
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
