#pragma once
#include <ArduinoJson.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"
#include "nimbus/harness/reply_overflow.h"   // the whole text of a reply the envelope cut (L19-36)
#include "nimbus/orch/provider_error.h"   // providerErrLine, oneLineText - the one error-line shape (CUM-620)
#include "nimbus/orch/spend_ceiling.h"  // lane DEF: the paid-call gate before any request
#include "nimbus/orch/wire_limits.h"   // kJsonStrGuard - the provider-bound string ceiling
#include "nimbus/utf8.h"               // utf8Valid / utf8Repair / boundary-safe cuts

// wire.h - INTERNAL helpers shared by the four portable provider files. Not part
// of the public harness surface (lives under src/, not include/).
namespace agent {
namespace providers {
namespace wire {

// What the most recent exchange()'s transport failure looked like (CUM-477,
// CUM-544). The turn path is single-task by design (AGENTS.md: no on-device
// concurrency), so one static record read right after a failed step is exact;
// exchange() resets it on entry, so a step that fails before reaching the
// transport never inherits an older call's flags. `unsent`: the request never
// fully left the device (short head/body write) - a resend duplicates nothing;
// `unsentAtStart`: its first write left 0 bytes (the device's fault, L19-14).
// `lowMem`: the transport tagged the failure as memory pressure. `noResponse`:
// the request went out and no status line came back (deadline or peer close).
// `partial`: a 2xx whose body did not parse whole (cut mid-body by the deadline or
// a peer close, or not JSON): a transport failure, never an empty answer.
// `tooLarge`: the 2xx body was more than the device can hold. `allocFailed`: the
// memory EVIDENCE - the transport saw a heap allocation fail during this exchange
// (the device's failed-allocation counter advanced; its error text says "alloc
// failed"), and only then is a failure classed LowMemory on that evidence alone.
// Without it the CUM-404 v2 rule below holds: a fragmented but healthy board's
// outage must still fail over.
struct LastExchange {
  bool unsent = false;
  bool unsentAtStart = false;   // ...and its FIRST write left 0 bytes (L19-14: Unsent)
  bool lowMem = false;
  bool noResponse = false;
  bool partial = false;
  bool tooLarge = false;
  bool allocFailed = false;
  bool spendFrozen = false;   // lane DEF: refused before the send (monthly spend ceiling)
};
inline LastExchange& lastExchange() {
  static LastExchange s;
  return s;
}

// The head/step error for a transport failure (status 0). "low memory: ..." on
// allocation evidence (an allocation failed during the exchange), or when the
// transport tagged the exchange as begun starved (largest block under the handshake
// need) AND total free heap is genuinely short now - the tag alone also fires on a
// fragmented-but-healthy Freenove, where a real outage must still fail over
// (CUM-404 v2). Then the engine answers with the memory reply instead of a retry, a
// failover and "check the connection" (CUM-477); else "network".
inline constexpr uint32_t kStarvedTotalFree = 12000;   // == ORCH_TURN_HARD_FLOOR
inline std::string transportError(const ProviderDeps& pd) {
  if (lastExchange().spendFrozen) return nimbus::orch::kSpendFrozenError;
  if (lastExchange().allocFailed) return "low memory: the device could not make the request";
  const bool shortNow = !pd.freeHeap || pd.freeHeap() < kStarvedTotalFree;
  return (lastExchange().lowMem && shortNow)
             ? std::string("low memory: the device could not make the request")
             : std::string("network");
}

// The FabricErr for a sub dispatch whose request failed in the transport (status
// 0), ONE rule for every sub dispatcher on the device and the hosted builds
// (CUM-538). A request that never fully left never delivered the work: Network
// ("did not start", a respawn duplicates nothing). One that went out with no
// answer back ("no response": the peer closed, or the deadline passed) may have
// started a paid run - a synchronous sub runs inside its POST, an async one was
// created by it - so it is Timeout ("may have started", never spawned again).
// Otherwise a failure that took about the whole `deadlineMs` since `sentAtMs` most
// likely started it too, and a fast one (connect, DNS, TLS) did not.
inline FabricErr subTransportErr(const ProviderDeps& pd, uint32_t sentAtMs, uint32_t deadlineMs) {
  if (lastExchange().unsent) return FabricErr::Network;
  if (lastExchange().noResponse) return FabricErr::Timeout;
  const uint32_t elapsedMs = (pd.nowMs ? pd.nowMs() : 0) - sentAtMs;
  return elapsedMs >= (deadlineMs / 10) * 9 ? FabricErr::Timeout : FabricErr::Network;
}

// ---- the one per-provider park, fed by every harness request (L19-8) -------------
// The provider a request host belongs to: the four API hosts, the Cumulo router (a
// routed request's host is the router's), else "" (a LAN endpoint, a test host).
inline std::string parkProviderOf(const ProviderDeps& pd, const std::string& host) {
  if (host == "api.anthropic.com") return "anthropic";
  if (host == "api.openai.com") return "openai";
  if (host == "api.mistral.ai") return "mistral";
  if (host == "api.z.ai" || host == "open.bigmodel.cn") return "zai";
  std::string base = pd.routerBase ? pd.routerBase() : std::string();
  const size_t sch = base.find("://");
  if (sch != std::string::npos) base = base.substr(sch + 3);
  base = base.substr(0, base.find_first_of("/:"));
  return !base.empty() && base == host ? "cumulo" : std::string();
}

// An answered request that refused for billing (every 402, the shared token table)
// or with a refused key (401) closes the provider for EVERY seam, whatever called
// it: the head, a fold, a sub-agent dispatch or poll. One place, so no adapter can
// forget. A 403 is left to the typed seams (it may name one model or endpoint), and
// a success is noted only where a model call succeeded (a free session poll that
// answers 200 says nothing about the account's credit).
inline void noteParkRefusal(const ProviderDeps& pd, const std::string& host, int status,
                            const JsonDocument& doc) {
  if (status < 400) return;
  const std::string prov = parkProviderOf(pd, host);
  if (prov.empty()) return;
  std::string text;
  serializeJson(doc, text);
  const nimbus::orch::ParkKind kind = nimbus::orch::parkKindFor(status, text);
  if (kind == nimbus::orch::ParkKind::None || (kind == nimbus::orch::ParkKind::Auth && status != 401))
    return;
  const std::string key = prov == "cumulo" ? (pd.routerKey ? pd.routerKey() : std::string())
                                           : (pd.key ? pd.key(prov.c_str()) : std::string());
  nimbus::orch::providerPark().park(prov.c_str(), nimbus::orch::PaidSeam::Chat,
                                    nimbus::orch::parkKeyFingerprint(key), kind, status,
                                    pd.nowMs ? pd.nowMs() : 0);
  hlog::logf("provider: %s parked for every seam (%s, HTTP %d)", prov.c_str(),
             nimbus::orch::parkKindSlug(kind), status);
}

// One provider REST exchange through the transport: complete request in, HTTP
// status out, response body filter-parsed into `doc` (the same bounded-memory
// ArduinoJson filter reads the pre-split adapters did from the socket - now
// from the returned body string). Returns the HTTP status code; 0 on transport
// failure (connect/TLS/timeout/arbiter - the pre-split adapters' 0 and -1
// returns both mapped to "network" at every call site, so they collapse here).
// `rateLimitHeaders` (optional): on an error response, the provider's
// x-ratelimit-* headers land there (see HttpRequest::errHeaders) - the quota
// window a 429 refused on.
inline int exchange(const ProviderDeps& pd, const char* host, uint16_t port, bool tls,
                    const char* method, const std::string& path,
                    std::vector<std::pair<std::string, std::string>> headers,
                    std::string body, uint32_t timeoutMs,
                    JsonDocument& doc, const JsonDocument& filter,
                    HeaderList* rateLimitHeaders = nullptr) {
  doc.clear();
  lastExchange() = LastExchange{};
  // Lane DEF: the monthly spend ceiling. A request that starts paid work (anything but
  // a GET poll or a DELETE cancel of work already running) is refused before it leaves
  // the device - head rounds, sub dispatch, the fold, every provider and the router -
  // so the freeze never relies on the provider to stop (auto-recharge cannot run away).
  // Ahead of the bench's scripted answer below, so a fake provider is refused too.
  if (std::strcmp(method, "GET") != 0 && std::strcmp(method, "DELETE") != 0 &&
      !nimbus::orch::paidCallAllowed(nimbus::orch::SpendSeam::Wire)) {
    lastExchange().spendFrozen = true;
    hlog::logf("provider: %s%s refused - %s", host, path.c_str(), nimbus::orch::kSpendFrozenError);
    return 0;
  }
  if (!pd.http) return 0;
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
  {   // test and host builds only: the bench's scripted provider answer
    int ovStatus = 0;
    std::string ovBody;
    if (testStatusOverride(host, ovStatus, ovBody) && ovStatus == kTestUnsentStatus) {
      lastExchange().unsent = lastExchange().unsentAtStart = true;
      hlog::logf("provider: TEST override: request to %s%s not sent (first write)", host, path.c_str());
      return 0;
    }
    if (testStatusOverride(host, ovStatus, ovBody)) {
      deserializeJson(doc, ovBody, DeserializationOption::Filter(filter));
      hlog::logf("provider: TEST override answered %s%s with HTTP %d", host, path.c_str(), ovStatus);
      noteParkRefusal(pd, host, ovStatus, doc);
      return ovStatus;
    }
  }
#endif
  HttpRequest req;
  req.method = method;
  req.host = host;
  req.port = port;
  req.tls = tls;
  req.path = path;
  req.headers = std::move(headers);
  req.body = std::move(body);
  req.timeoutMs = timeoutMs;
  if (rateLimitHeaders) {
    req.errHeaderPrefix = "x-ratelimit";
    req.errHeaders = rateLimitHeaders;
  }
  // execJson parses the response into `doc` through the filter - on the device it
  // STREAMS off the socket (never buffering a fat connector-write body); on the
  // host/fake it filter-parses the scripted body string. Either way only the
  // filter-retained fields land in `doc`. Empty/garbage bodies leave doc cleared.
  std::string terr;
  int status = pd.http->execJson(req, doc, filter, terr);
  lastExchange().allocFailed = terr.find("alloc failed") != std::string::npos;
  noteParkRefusal(pd, host, status, doc);
  if (status == 0) {
    LastExchange& last = lastExchange();
    last.unsent = terr.find("body truncated") != std::string::npos ||
                  terr.find("head write failed") != std::string::npos;
    // The request head, or the first body slice ("write failed after 0 of N bytes"):
    // nothing reached the provider, and the device could not write at all.
    last.unsentAtStart = terr.find("head write failed") != std::string::npos ||
                         terr.find("after 0 of ") != std::string::npos;
    last.lowMem = terr.find("low memory") != std::string::npos;
    last.noResponse = terr.find("no response") != std::string::npos;
  }
  if (status == 0 && !terr.empty())
    hlog::logf("provider: transport fail %s%s: %s", host, path.c_str(), terr.c_str());
  // A response that arrived but did not parse leaves `doc` holding whatever was
  // read before the break - a fragment that looks like a thin-but-valid reply.
  // Drop it and log loudly: a partial provider response silently became an empty
  // or truncated answer with nothing anywhere to say why.
  else if (status != 0 && !terr.empty()) {
    hlog::logf("provider: %s%s status %d but %s -- discarding partial response",
               host, path.c_str(), status, terr.c_str());
    doc.clear();
    // A 2xx that did not arrive whole is a TRANSPORT failure (status 0), so every
    // step takes its transport branch: a cut body (the deadline or a peer close
    // mid-body) reads as no answer, a body too big for the device as such. It
    // used to return 200 with an empty doc, which the loops read as a model that
    // said nothing and answered with a forced "stalled" round. An error status
    // keeps its code (a Cloudflare HTML 502 is still an HTTP 502).
    if (status >= 200 && status < 300) {
      LastExchange& last = lastExchange();
      last.partial = true;
      last.tooLarge = terr.find("NoMemory") != std::string::npos ||
                      terr.find("too large") != std::string::npos;
      last.noResponse = terr.find("IncompleteInput") != std::string::npos;
      return 0;
    }
  }
  return status;
}

// ---- typed step failures (lane K) ------------------------------------------------

// A provider's error message as one log-safe line: control bytes become spaces
// (provider text can never forge a second log line, or reach the owner's reply
// with them), capped at 300 bytes on a character boundary.
inline std::string oneLine(std::string s) { return nimbus::orch::oneLineText(std::move(s)); }

// "<what> HTTP <status>[: <provider message>]" - the one shape every adapter's
// error line takes (classifyStepError reads the status back from it).
inline std::string httpErrLine(const char* what, int code, const std::string& msg) {
  return nimbus::orch::providerErrLine(what, code, msg);   // the one helper (CUM-620)
}

// F25 + the one time policy: a round's socket timeout is the turn's remaining
// budget, at most `ceilingMs` (the host's head ceiling), at least kMinRoundMs so a
// near-exhausted final round still gets a real answer. budgetMs == UINT32_MAX (no
// deadline) gives the ceiling.
inline uint32_t clampRoundMs(uint32_t budgetMs, uint32_t ceilingMs) {
  if (budgetMs >= ceilingMs) return ceilingMs;
  return budgetMs < kMinRoundMs ? kMinRoundMs : budgetMs;
}

// The error line for a request that got no usable HTTP answer, one rule for every
// head (CUM-544 hoisted from the custom head): "timeout: the provider did not
// answer in time" when the request went out and the silence used (nearly) the whole
// round, so the fabric fails over without a same-host retry and the owner hears the
// real reason; "low memory: ..." on memory evidence; else "network" (one retry).
// Memory evidence wins over a body too large to hold: a parse that ran out of memory
// (NoMemory) while an allocation failure was recorded is the device out of memory,
// LowMemory (no retry, no switch: another provider cannot give the device more
// memory, it only spends money). Without evidence a body too large stays Cut.
inline std::string headTransportError(const ProviderDeps& pd, uint32_t sentAtMs, uint32_t timeoutMs) {
  const LastExchange& last = lastExchange();
  if (last.allocFailed) return "low memory: the device could not make the request";
  if (last.tooLarge) return "response too large for the device";
  const uint32_t waited = pd.nowMs ? pd.nowMs() - sentAtMs : timeoutMs;
  if (last.noResponse && waited >= timeoutMs - timeoutMs / 10)
    return "timeout: the provider did not answer in time";
  return transportError(pd);
}

// Fail a head step on a transport failure, typed (Timeout / LowMemory / Cut for a
// body too large to hold / Transport).
inline void transportFailure(const ProviderDeps& pd, uint32_t sentAtMs, uint32_t timeoutMs,
                             nimbus::orch::HeadStep& out) {
  out.ok = false;
  out.error = headTransportError(pd, sentAtMs, timeoutMs);
  out.kind = nimbus::orch::classifyStepError(out.error);
  // A request the spend ceiling refused fails the same way on every host: typed like
  // an unreadable request (never retried, never switched, never parks a provider).
  if (lastExchange().spendFrozen) out.kind = nimbus::orch::StepErr::BadRequest;
}

// Fail a head step on an HTTP answer that is not a 200: `error` is the log/owner
// line ("messages HTTP 400: ..."), `providerText` everything the provider said
// (message, type, code) for the billing and unreadable-body markers.
inline void httpFailure(nimbus::orch::HeadStep& out, int code, const std::string& error,
                        const std::string& providerText) {
  out.ok = false;
  out.error = error;
  out.httpStatus = code;
  out.kind = nimbus::orch::classifyHttpStatus(code, providerText);
}

// A loop that ended without a final turn: `err` names why, and a cut keeps what it
// wrote (CUM-621): the engine salvages the finished part of the cut orch_turn with
// its honest marker (lane L) instead of the bare cut line. Always false.
inline bool loopFailed(const nimbus::orch::HeadOutcome& res, std::string& outJson, std::string& err) {
  err = res.error.empty() ? std::string("loop failed") : res.error;
  if (!res.partialTurn.empty()) outJson = res.partialTurn;
  return false;
}

// A cut that survived the in-step headroom retry: never finished, never
// dispatched; the cut orch_turn arguments stay in finalTurn for the salvage.
inline void cutFailure(nimbus::orch::HeadStep& out, const std::string& why, uint32_t tokens) {
  out.ok = false;
  out.finished = false;
  out.toolCalls.clear();
  out.cut = why;
  out.kind = nimbus::orch::StepErr::Cut;
  out.error = tokens ? "cut at the output limit (" + std::to_string(tokens) + " tokens)"
                     : "cut at the output limit (" + why + ")";
  hlog::logf("provider: %s", out.error.c_str());
}

// Cumulo router HEAD rewrite (CUM-242). When `pd` carries a router override
// (routerBase() non-empty), rewrite an upstream head request IN PLACE to travel
// through the router: host <- bare routerBase() (scheme/port honoured: an
// "http://" base is plain HTTP on port 80, anything else is TLS on 443, an
// explicit ":port" wins), path <- "/router/<upstream>" + path, and the client's
// own auth (Authorization / x-api-key) is dropped and replaced by a single
// "Authorization: Bearer routerKey()" - the router validates that cumulo key and
// injects the upstream's real provider key server-side. `upstream` is the wire's
// own provider ("anthropic" | "openai" | "mistral"). Returns true when it
// rewrote; a no-op returning false leaves everything byte-identical to the direct
// wire (so a device with no Cumulo head, and every existing wire test, is
// untouched). Kept here, next to exchange(), so all three provider files share
// ONE rewrite and it cannot drift between them.
// The mutable request coordinates applyRouter rewrites in place. Bundled into one
// struct so callers build it once and hand it straight to exchange() (and so the
// helper stays under the args<=6 complexity gate).
struct UpstreamReq {
  std::string host;
  uint16_t port = 443;
  bool tls = true;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
};
// Drop the client's own upstream auth headers in place (case-insensitive). The
// router sets the real provider auth itself, so Authorization / x-api-key from the
// device must not ride the request. Split out to keep applyRouter under the gate.
inline void dropClientAuth(std::vector<std::pair<std::string, std::string>>& headers) {
  std::vector<std::pair<std::string, std::string>> kept;
  kept.reserve(headers.size() + 1);
  for (auto& h : headers) {
    std::string k = h.first;
    for (char& c : k) c = (char)tolower((unsigned char)c);
    if (k == "authorization" || k == "x-api-key") continue;
    kept.push_back(std::move(h));
  }
  headers.swap(kept);
}
inline bool applyRouter(const ProviderDeps& pd, const char* upstream, UpstreamReq& r) {
  std::string base = pd.routerBase ? pd.routerBase() : std::string();
  if (base.empty()) return false;
  const bool http = base.rfind("http://", 0) == 0;   // plain HTTP only when explicit
  r.tls  = !http;
  r.port = http ? 80 : 443;
  size_t sch = base.find("://"); if (sch != std::string::npos) base = base.substr(sch + 3);
  size_t sl  = base.find('/');   if (sl  != std::string::npos) base = base.substr(0, sl);
  size_t colon = base.find(':');
  if (colon != std::string::npos) {
    r.port = (uint16_t)atoi(base.c_str() + colon + 1);
    base = base.substr(0, colon);
  }
  r.host = base;
  r.path = std::string("/router/") + upstream + r.path;
  dropClientAuth(r.headers);
  std::string rk = pd.routerKey ? pd.routerKey() : std::string();
  // SECURITY (mirror custom.cpp's keyless-http contract): the Cumulo key is the
  // master "one key, all upstreams" credential - NEVER put it on a cleartext
  // socket. A plain-http base (a LAN rig) gets NO Authorization header rather than
  // leaking cumulo_sk_ to a passive sniffer; the router requires auth, so an http
  // base then fails honestly (use https). A missing routerKey also adds nothing.
  if (rk.empty()) return true;
  if (http)
    hlog::logf("router: base is http:// - NOT sending the Cumulo key in cleartext (use https)");
  else
    r.headers.push_back({"Authorization", "Bearer " + rk});
  return true;
}

// A sub-session dispatch the provider refused with HTTP 429: hand the quota window
// back through the Directive (when the caller asked for it) and report RateLimited,
// so the spawn refusal names the window the way a head-turn 429 does (CUM-465).
inline FabricErr rateLimited(const Directive& d, RateLimit window) {
  if (d.rateLimit) *d.rateLimit = window;
  return FabricErr::RateLimited;
}
// The same, for a provider that names the window only in its error text (OpenAI's
// "... requests per day (RPD)", an Anthropic-style "per minute", Z.ai, the Cumulo
// router): logged, then read the way a head turn's 429 text is read.
inline FabricErr rateLimitedByText(const Directive& d, const char* who, const char* text) {
  hlog::logf("%s: dispatch HTTP 429: %s", who, text);
  return rateLimited(d, rateLimitFromText(text));
}

// Serialize a request document into ONE contiguous string (the transport then
// writes it in a single client.write - the serialize-into-one-buffer rule; on
// the device the string's storage lands in PSRAM via the >=128 B malloc spill).
inline std::string serializeBody(const JsonDocument& d) {
  std::string out;
  out.reserve(measureJson(d));
  serializeJson(d, out);
  return out;
}

// ---- the provider-bound string class (UTF-8 + the 64 KB ceiling) -------------
// A provider rejects a body that is not valid UTF-8, or one where a string went
// missing, with a 400 that is decided before any output: free, deterministic, and
// identical on every other host. Two layers keep a request clean:
//   1. putModelStr / putFieldStr at every renderer: repair invalid UTF-8 and (for
//      clippable data) clip under kJsonStrGuard with a visible note;
//   2. serializeChecked over the whole document: the last line of defence. It
//      refuses a request with a dropped string, a string over kJsonStrGuard, or
//      invalid UTF-8, and names the tool (or field) the bad text came from, so a
//      renderer that skipped layer 1 fails closed and loudly, before the send.

// Tool results, call arguments and replayed prose: repaired, then clipped
// UTF-8-safe to kJsonStrGuard with a "[cut: kept K of N bytes]" note inside the
// cap. `what` names the source in the one log line a repair or clip writes.
// `dst` is the member or element proxy being written (c["text"]): a proxy is
// assigned, never converted to a JsonVariant first - a converted proxy for a
// member that does not exist yet is a null variant that ignores the write.
template <typename Dst>
inline void putModelStr(Dst&& dst, const std::string& v, const char* what) {
  if (v.size() <= nimbus::orch::kJsonStrGuard && nimbus::utf8Valid(v)) {
    dst = v;
    return;
  }
  std::string t = v;
  const size_t fixed = nimbus::utf8Repair(t);
  const bool clipped = nimbus::utf8ClipNoted(t, nimbus::orch::kJsonStrGuard);
  if (fixed) hlog::logf("utf8: repaired %u sequence(s) in %s", (unsigned)fixed, what);
  if (clipped) hlog::logf("wire: clipped %s from %u B", what, (unsigned)v.size());
  dst = t;
}

// The system prompt and the user seed: repaired, never clipped - a longer one is
// refused whole by serializeChecked ("request field over 60 KB") rather than sent
// with its tail silently gone.
template <typename Dst>
inline void putFieldStr(Dst&& dst, const std::string& v, const char* what) {
  if (nimbus::utf8Valid(v)) {
    dst = v;
    return;
  }
  std::string t = v;
  const size_t fixed = nimbus::utf8Repair(t);
  hlog::logf("utf8: repaired %u sequence(s) in %s", (unsigned)fixed, what);
  dst = t;
}

namespace detail {
// Bounded id -> tool-name pairs: a tool_use/function_call/tool_calls entry names
// the tool, its result names only the id.
struct ToolIds {
  static constexpr size_t kMax = 48;
  const char* id[kMax];
  const char* name[kMax];
  size_t n = 0;
  const char* find(const char* want) const {
    for (size_t i = 0; want && i < n; i++)
      if (!strcmp(id[i], want)) return name[i];
    return nullptr;
  }
};
inline const char* objName(JsonObjectConst o) {
  const char* nm = o["name"] | (const char*)nullptr;
  return nm ? nm : (o["function"]["name"] | (const char*)nullptr);
}
inline void collectToolIds(JsonVariantConst v, ToolIds& ids) {
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) collectToolIds(e, ids);
    return;
  }
  if (!v.is<JsonObjectConst>()) return;
  JsonObjectConst o = v.as<JsonObjectConst>();
  const char* nm = objName(o);
  const char* id = o["id"] | (const char*)nullptr;
  if (!id) id = o["call_id"] | (const char*)nullptr;
  if (nm && id && ids.n < ToolIds::kMax) {
    ids.id[ids.n] = id;
    ids.name[ids.n] = nm;
    ids.n++;
  }
  for (JsonPairConst kv : o) collectToolIds(kv.value(), ids);
}
// The first string that would make the body bad; `top` is the request field it
// sits under, `tool` the tool it belongs to (nullptr = none).
struct BadText {
  bool found = false;
  bool tooLong = false;
  size_t size = 0;
  std::string where;
};
// Where a bad string sits, for the error: the tool it belongs to, else the field.
struct TextPlace {
  const char* top;    // the request field (system, messages, input, ...)
  const char* key;    // the string's own key
  const char* tool;   // the tool it belongs to; nullptr = none
};
inline void checkText(JsonVariantConst v, const TextPlace& at, BadText& bad) {
  const JsonString s = v.as<JsonString>();
  const bool tooLong = s.size() > nimbus::orch::kJsonStrGuard;
  if (!tooLong && nimbus::utf8Valid(s.c_str(), s.size())) return;
  bad.found = true;
  bad.tooLong = tooLong;
  bad.size = s.size();
  bad.where = at.tool ? std::string("tool ") + at.tool : std::string(at.top);
  if (at.key && *at.key && strcmp(at.key, at.top) != 0) bad.where += std::string(" (") + at.key + ")";
}
// The tool an object belongs to: a result names it by id, a call or a tool message
// by name; otherwise it inherits its parent's.
inline const char* toolOf(JsonObjectConst o, const ToolIds& ids, const char* parent) {
  for (const char* idKey : {"tool_use_id", "call_id", "tool_call_id"}) {
    const char* nm = ids.find(o[idKey] | (const char*)nullptr);
    if (nm) return nm;
  }
  const char* nm = objName(o);
  return nm ? nm : parent;
}
inline void findBadText(JsonVariantConst v, const TextPlace& at, const ToolIds& ids, BadText& bad) {
  if (bad.found) return;
  if (v.is<const char*>()) {
    checkText(v, at, bad);
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) findBadText(e, at, ids, bad);
  } else if (v.is<JsonObjectConst>()) {
    JsonObjectConst o = v.as<JsonObjectConst>();
    const char* tool = toolOf(o, ids, at.tool);
    for (JsonPairConst kv : o)
      findBadText(kv.value(), TextPlace{at.top, kv.key().c_str(), tool}, ids, bad);
  }
}
}  // namespace detail

// Serialize `d` into `out` only when every string in it is whole, at most
// kJsonStrGuard bytes and valid UTF-8. Otherwise `out` is left empty, `err` names
// what is wrong and where ("request text is not valid UTF-8: tool files_search
// (content)"), and the caller fails the step as a bad request (no
// same-host retry and no switch: the same body is just as bad on every host).
inline bool serializeChecked(const JsonDocument& d, std::string& out, std::string& err) {
  out.clear();
  if (d.overflowed()) {
    err = "request field over 64 KB (dropped while the request was built)";
    hlog::logf("wire: %s", err.c_str());
    return false;
  }
  detail::ToolIds ids;
  detail::collectToolIds(d.as<JsonVariantConst>(), ids);
  detail::BadText bad;
  if (d.is<JsonObjectConst>()) {
    for (JsonPairConst kv : d.as<JsonObjectConst>()) {
      const char* top = kv.key().c_str();
      detail::findBadText(kv.value(), detail::TextPlace{top, top, nullptr}, ids, bad);
    }
  } else {
    detail::findBadText(d.as<JsonVariantConst>(), detail::TextPlace{"body", "", nullptr}, ids, bad);
  }
  if (bad.found) {
    err = bad.tooLong ? "request field over 60 KB: " + bad.where + " (" +
                            std::to_string((unsigned)bad.size) + " B)"
                      : "request text is not valid UTF-8: " + bad.where;
    hlog::logf("wire: %s", err.c_str());
    return false;
  }
  out.reserve(measureJson(d));
  serializeJson(d, out);
  return true;
}

// A sub-agent's result into the fixed ResultEnvelope.reply buffer: whole when it
// fits, else cut on a character boundary with a visible "[cut at 16 KB]" marker
// inside the buffer, and repaired when the provider text was not valid UTF-8. A
// raw byte cut used to split a character that then rode the next head prompt.
// Appends in pieces (several agent messages, a closing note); the first piece that
// does not fit ends the buffer with the marker. L19-36: from that piece on, the
// writer keeps the WHOLE reply in the overflow record (reply_overflow.h), so a
// caller keeps feeding it every piece even after append() returns false, and the
// job engine spills it to the card (the envelope used to be the only copy).
inline constexpr const char* kReplyCutMarker = nimbus::orch::kEnvelopeCutMarker;   // "\n[cut at 16 KB]"
struct ReplyWriter {
  char* out;
  size_t cap;        // sizeof the buffer, NUL included
  size_t used = 0;
  size_t textEnd = 0;   // where the reply text ends (a marker or a tail may follow)
  bool cut = false;
  ReplyWriter(char* o, size_t c) : out(o), cap(c) {
    if (cap) out[0] = 0;
    replyOverflowReset();
  }
  // Append `n` bytes of `s`; false once the buffer is full (the marker is in).
  bool append(const char* s, size_t n) {
    if (cap == 0) return false;
    std::string fixed;
    if (!nimbus::utf8Valid(s, n)) {
      fixed.assign(s, n);
      nimbus::utf8Repair(fixed);
      s = fixed.data();
      n = fixed.size();
    }
    if (cut) {
      replyOverflowAppend(s, n);
      return false;
    }
    if (used + n <= cap - 1) {
      memcpy(out + used, s, n);
      used += n;
      textEnd = used;
      out[used] = 0;
      return true;
    }
    replyOverflowStart(out, textEnd);   // the reply so far, before any tail
    replyOverflowAppend(s, n);
    const size_t mk = strlen(kReplyCutMarker);
    size_t room = cap - 1 > used + mk ? cap - 1 - used - mk : 0;
    if (room == 0) {   // no room left after what is in: back the text up for the marker
      used = cap - 1 > mk ? (size_t)nimbus::utf8CapLen(out, (int)used, (int)(cap - 1 - mk)) : 0;
    } else {
      const size_t keep = (size_t)nimbus::utf8CapLen(s, (int)n, (int)room);
      memcpy(out + used, s, keep);
      used += keep;
    }
    textEnd = used;
    const size_t m = mk < cap - 1 - used ? mk : cap - 1 - used;
    memcpy(out + used, kReplyCutMarker, m);
    used += m;
    out[used] = 0;
    cut = true;
    return false;
  }
  bool append(const std::string& s) { return append(s.data(), s.size()); }
  // Append `tail` WHOLE (a note that is the only channel for what it reports, so it
  // must never be the thing that gets dropped): when text and tail do not both fit,
  // the text before it is cut on a character boundary, marker included.
  void appendTail(const std::string& tail) {
    if (cap == 0 || tail.size() >= cap) return;
    const size_t target = cap - 1 - tail.size();
    if (!cut && used <= target) {
      memcpy(out + used, tail.data(), tail.size());
      used += tail.size();
      out[used] = 0;
      return;
    }
    if (!cut) replyOverflowStart(out, textEnd);   // the tail cuts the reply: keep it whole (L19-36)
    const size_t mk = strlen(kReplyCutMarker);
    const size_t text = textEnd;   // the reply text, before any marker or earlier tail
    const size_t room = target > mk ? target - mk : 0;
    used = (size_t)nimbus::utf8CapLen(out, (int)text, (int)(text < room ? text : room));
    textEnd = used;
    if (used + mk <= target) {
      memcpy(out + used, kReplyCutMarker, mk);
      used += mk;
    }
    memcpy(out + used, tail.data(), tail.size());
    used += tail.size();
    out[used] = 0;
    cut = true;
  }
};

// JsonDocument bound to the deps allocator (device: PSRAM) or the default.
inline JsonDocument makeDoc(const ProviderDeps& pd) {
  return pd.alloc ? JsonDocument(pd.alloc) : JsonDocument();
}

inline std::string s(const std::function<std::string()>& f) { return f ? f() : std::string(); }

// The system prompt for ONE round. On the forced tool-less round (capReason set)
// it carries kFinalRoundNotice, which tells the model its tools are gone, why,
// and that there is no later turn.
//
// ⚠ Without this every provider confabulated: the loop removed the tools
// silently, and rather than admit it could not finish, each model promised work
// it would never do ("I'll report back when the scan finishes"). The device then
// went quiet. Reproduced on the host against all three providers - see
// kFinalRoundNotice in nimbus/orch/head_loop.h.
inline std::string roundInstructions(const std::string& instructions,
                                     const std::string& capReason) {
  if (capReason.empty()) return instructions;
  const char* why = nimbus::orch::capReasonText(capReason);
  std::string notice = nimbus::orch::kFinalRoundNotice;
  const size_t at = notice.find("%s");
  if (at != std::string::npos) notice.replace(at, 2, why);
  // The notice is the load-bearing part of the final round, so it always fits:
  // when the prompt plus the notice would pass the string ceiling, the prompt's
  // tail gives way (on a character boundary) instead of the whole field going null.
  if (instructions.size() + notice.size() > nimbus::orch::kJsonStrGuard) {
    const int keep = nimbus::utf8CapLen(instructions.data(), (int)instructions.size(),
                                        (int)(nimbus::orch::kJsonStrGuard - notice.size()));
    return instructions.substr(0, (size_t)keep) + notice;
  }
  return instructions + notice;
}

}  // namespace wire
}  // namespace providers
}  // namespace agent
