#include "relay_client.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <time.h>

#include <cctype>
#include <string>
#include <vector>

#include "../agent/agent_config.h"
#include "../agent/orchestrator.h"   // turnInFlight: big tunneled answers wait for the turn
#include "../agent/store.h"
#include "../agent/telegram.h"       // pollEverOk: the first dial waits for Telegram's first poll
#include "../sys/agent_log.h"     // alog
#include "../sys/config_nvs.h"    // nimbus::sys::deviceName
#include "../sys/net_util.h"      // CA bundle symbols + tlsClose
#include "../sys/ps_json.h"       // agent::PsramJsonAllocator
#include "../sys/heap_watch.h"    // activity tags (relay dial, bulk write, tunnel serve)
#include "../sys/tls_arbiter.h"   // pairing, the dial and big frames take the work slot
#include "nimbus/cloud/http_replay.h"
#include "nimbus/cloud/relay_codec.h"
#include "nimbus/cloud/relay_credential.h"   // CUM-52: exp/re-mint policy (pure core)
#include "nimbus/cloud/relay_timing.h"        // CUM-160: bound the TLS slot-hold below the watchdog
#include "nimbus/cloud/loopback_target.h"     // CUM-173: never dial 0.0.0.0 for the loopback fallback
#include "nimbus/cloud/relay_heap.h"          // CUM-167: heap-floor policy; the big-answer serve decision
#include "nimbus/cloud/relay_slot.h"          // heap F2: the dial and big frames through the TLS work slot
#include "nimbus/cloud/relay_liveness.h"      // CUM-191: steady-state heartbeat-ack liveness (host-tested)
#include "nimbus/cloud/relay_presence.h"      // CUM-182: hello-ack-gated, identity-bound presence
#include "nimbus/cloud/relay_ws.h"
#include "nimbus/cloud/ws_write.h"            // CUM-182: host-tested whole-frame write driver
#include "nimbus/cloud/tunnel_guard.h"   // canonicalize+deny secret paths, scrub secret bodies
#include "nimbus/cloud/upload_reassembly.h"   // chunked tunnel upload state machine (host-tested)
#include "nimbus/fault.h"                     // RELAY_NOSLOT: the bench counter-leg (test builds)
#include "nimbus/sys/log_window.h"   // forensic window: fold the per-serve lines (rows 44/57)
#include "version.h"

namespace nimbus {
namespace relay {

using namespace nimbus::cloud;

namespace {

constexpr uint16_t kTlsPort = 443;
// Folds the per-serve log lines into one durable summary per window (relay task).
nimbus::sys::RelayFold g_serveFold;
constexpr char kWsPath[] = "/device";
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kWelcomeDeadlineMs = 10000;
// Loopback RESPONSE cap (PSRAM-backed). CUM-173: the config page (GET /) is the
// biggest tunneled body and it GREW PAST the old 256 KB cap (~278 KB now) as the UI
// gained features - so the parser flagged overflow and handleReq turned every tunneled
// GET / into a 5xx (the field "white screen + bad gateway"; it serves fine to LAN
// clients, which have no cap). The value + a host regression guard live in lib/core
// (loopback_target.h) so the NEXT page growth fails the battery, not the field.
constexpr size_t kMaxRespBody = nimbus::cloud::kLoopbackMaxRespBody;
// Inbound cap for frames FROM the relay. A `req` frame is a browser request (method +
// path + headers + small body), not a large response, so it is bounded low: the WS
// parser + the decoded body accumulate in scarce INTERNAL SRAM, so a 512 KB frame
// (the protocol max) would OOM + reboot the device (found by the security audit). 16 KB
// fits comfortably in the largest internal block and covers any real request; an
// oversized frame is rejected from its length header before any payload is buffered.
constexpr size_t kMaxInboundFrame = 16 * 1024;
constexpr size_t kMaxReqBody = 16 * 1024;
// Chunked tunnel upload (protocol 2): a large body arrives as ordered uchunk frames the
// device pipes straight into its local server over the loopback, never buffering the
// whole thing. The hard ceiling matches the device music store (64 MB); the cloud
// refuses anything larger before it streams. Each uchunk's decoded slice is bounded by
// the inbound frame cap, so it lands in a small internal-SRAM buffer.
constexpr uint32_t kMaxUploadBytes = 64u * 1024 * 1024;
constexpr uint32_t kLoopbackTimeoutMs = 15000;
// Deadline for writing ONE outbound WS frame in full (CUM-182). The largest res
// frame is the config/login page: ~277 KB body -> a ~370 KB masked frame. A
// single WiFiClientSecure::write does not always flush that much at once (a
// partial or WANT_WRITE return under a momentarily full TLS/socket buffer), and
// the old code treated "did not write it all in one call" as failure - which
// truncated the frame on the wire and dropped the whole session mid-page (the
// field "can't load the page over the tunnel" 502 + reconnect). We now loop the
// write until the frame is fully sent, bounded by this deadline (kept below the
// relay's 30 s request timeout so a genuinely stuck socket still fails cleanly).
constexpr uint32_t kResWriteTimeoutMs = 20000;
// Loopback CONNECT timeout (CUM-173): a working lwIP loopback connects in ~2 ms and a
// refusal returns immediately, so this only bounds a pathological no-answer case. Kept
// short so a bad target never adds the multi-second stall the old 4 s value did.
constexpr uint32_t kLoopbackConnectMs = 1500;
constexpr uint32_t kHeartbeatMinMs = 5000;     // clamp the relay-supplied heartbeat to a
constexpr uint32_t kHeartbeatMaxMs = 300000;   // sane band (a huge value disables liveness)
constexpr size_t kMaxHandshakeHead = 4096;     // WS upgrade response head cap (anti-OOM)
// The relay heap floor (CUM-167) lives in lib/core (nimbus/cloud/relay_heap.h),
// host-tested: the relay's big buffers are PSRAM-backed so it only needs a small
// contiguous internal block (the 4 KB WS handshake head), and the largest-block floor
// was lowered to coexist with solide-drivers v0.6.1's internal DMA bounce buffer.

// --- status snapshot (written by the task, read by web/console) --------------
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
State g_state = State::Disabled;
bool g_online = false;
char g_code[16] = {0};
char g_url[192] = {0};
char g_err[96] = {0};
TaskHandle_t g_task = nullptr;

// --- staged control (drained at the loop top) --------------------------------
volatile int8_t g_reqOptIn = -1;  // -1 none, 0 off, 1 on
volatile bool g_reqPair = false;
volatile bool g_reqUnpair = false;

// --- resident WS socket + parser (task-owned) --------------------------------
WiFiClientSecure* g_ws = nullptr;
uint32_t g_heartbeatMs = kDefaultHeartbeatMs;

// --- the online session's pump state (task-owned) ------------------------------
// Shared by the online loop and a big answer's wait for the TLS work slot: while a
// big answer waits, the socket is still read, pings answered and heartbeats sent,
// and a request that arrives meanwhile is deferred (a request is never served inside
// another one), then served in arrival order by the online loop.
struct OnlineSession {
  ws::Parser* parser = nullptr;
  const char* deviceId = nullptr;
  bool welcomed = false;
  bool ended = false;          // a frame, the socket or a broken write ended the session
  uint16_t closeCode = 0;
  nimbus::cloud::HeartbeatLiveness live;
  bool armed = false;
  uint32_t lastInbound = 0;
};
OnlineSession g_on;

struct DeferredMsg {
  ws::Message m;
  uint32_t atMs = 0;   // when it arrived (the cloud's 30 s bound runs from then)
};
constexpr size_t kMaxDeferred = 8;
DeferredMsg g_deferred[kMaxDeferred];
size_t g_defHead = 0, g_defCount = 0;

// Internal close code for "the dial could not get the TLS work slot": not a WebSocket
// close code, never sent; the task just tries again shortly.
constexpr uint16_t kSlotBusyClose = 1;

// The bench counter-leg (FAULT relay_noslot, test builds only): serve like v4.5.13.
bool relayNoSlot() { return nimbus::fault::active(nimbus::fault::RELAY_NOSLOT); }

bool pumpWhileWaiting();
void emitBusy(const char* id, uint32_t retryAfterS);

void setState(State s) {
  bool clearedOnline = false;
  portENTER_CRITICAL(&g_mux);
  g_state = s;
  if (s != State::Online && g_online) { g_online = false; clearedOnline = true; }
  portEXIT_CRITICAL(&g_mux);
  // A state move out of Online (e.g. into Backoff after a drop) also ends presence.
  if (clearedOnline) agent::alogf("relay: online=0 cause=state-%s", stateName());
}
// CUM-182: every cloud.online transition is logged with its cause so the field
// (via /api/log) shows exactly why presence changed. `cause` is a short machine
// tag (e.g. "hello-ack", "session-end"), never a secret.
void setOnline(bool on, const char* cause) {
  bool changed;
  portENTER_CRITICAL(&g_mux);
  changed = (g_online != on);
  g_online = on;
  g_state = on ? State::Online : g_state;
  portEXIT_CRITICAL(&g_mux);
  if (changed) agent::alogf("relay: online=%d cause=%s", on ? 1 : 0, cause ? cause : "");
}
void setErr(const char* e) {
  portENTER_CRITICAL(&g_mux);
  strncpy(g_err, e ? e : "", sizeof(g_err) - 1);
  g_err[sizeof(g_err) - 1] = 0;
  portEXIT_CRITICAL(&g_mux);
}
void setPairing(const char* code, const char* url) {
  portENTER_CRITICAL(&g_mux);
  strncpy(g_code, code ? code : "", sizeof(g_code) - 1);
  g_code[sizeof(g_code) - 1] = 0;
  strncpy(g_url, url ? url : "", sizeof(g_url) - 1);
  g_url[sizeof(g_url) - 1] = 0;
  portEXIT_CRITICAL(&g_mux);
}

bool heapFloorOk() {
  size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return nimbus::cloud::relayCanDial(freeInt, largest);
}

void fillMask(uint8_t m[4]) {
  uint32_t r = esp_random();
  m[0] = r & 0xFF; m[1] = (r >> 8) & 0xFF; m[2] = (r >> 16) & 0xFF; m[3] = (r >> 24) & 0xFF;
}

// Always validate the relay cert against the embedded CA bundle. NEVER setInsecure()
// here: the relay is the security boundary. (Cloudflare-fronted, so a specific SPKI
// pin is a documented follow-up; bundle-root is the MVP win.)
void relayTlsSetup(WiFiClientSecure& c) {
  c.setCACertBundle(_nimbusCrtBundleStart,
                    (size_t)(_nimbusCrtBundleEnd - _nimbusCrtBundleStart));
  c.setTimeout(kConnectTimeoutMs / 1000);
}

bool wifiReady() { return WiFi.status() == WL_CONNECTED; }

// Read available bytes (up to max) with a short wait. Returns count (0 = none yet).
int readSome(Client& c, uint8_t* buf, size_t max, uint32_t waitMs) {
  uint32_t deadline = millis() + waitMs;
  while (millis() < deadline) {
    int a = c.available();
    if (a > 0) {
      int n = c.read(buf, a > (int)max ? (int)max : a);
      if (n > 0) return n;
    }
    if (!c.connected() && c.available() == 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return 0;
}

// Permanent instrumentation for one loopback serve (device evidence; the CUM-174
// release gate asserts on the loopback line): which target served, connect latency,
// status, body size, and the internal-SRAM low-water during the serve (intMin,
// CUM-387). Every serve's lines are in the RAM ring + serial (GET /api/log); only an
// abnormal serve (no response, a 5xx, internal memory under the floor) also goes to
// the durable log, and the rest are folded into one durable summary per window -
// these lines were 83-98% of every rotated SD log, which pushed whatever preceded a
// crash out within hours (rows 44, 57).
void logServe(const char* via, uint32_t connMs, http_replay::ResponseParser& rp, size_t intMin) {
  const nimbus::sys::RelayFold::Serve serve{rp.status(), (uint32_t)rp.body().size(),
                                            (uint32_t)intMin};
  agent::alogLevelf(nimbus::sys::RelayFold::abnormal(serve) ? nimbus::errlog::Level::Warn
                                                            : nimbus::errlog::Level::Info,
                    "relay: loopback via=%s conn=%ums status=%d bytes=%u intMin=%u", via, connMs,
                    rp.status(), (unsigned)rp.body().size(), (unsigned)intMin);
  // CUM-387 bench probe: a serve is the relay/cloud-sync memory event, so emit a line in
  // the same `[sram]` family as the boot snapshots (main.cpp sramSnap) - intFree is the
  // internal free right after the serve, heapMin is the all-time internal low-water
  // (ESP.getMinFreeHeap, same metric as the boot lines). The supervisor's before/after
  // capture is then one `grep '[sram]'`. Serial/RAM ring only; heapMin rides the fold.
  const uint32_t heapMin = (uint32_t)ESP.getMinFreeHeap();
  agent::alogif("[sram] relay-sync intFree=%u heapMin=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heapMin);
  // The fold belongs to the relay task (its only writer, so it needs no lock); a test
  // console self-test serve on another task logs its own line above and stays out.
  if (xTaskGetCurrentTaskHandle() != g_task) return;
  const std::string folded = g_serveFold.note(serve, millis(), heapMin);
  if (!folded.empty()) agent::alogc(nimbus::errlog::cat::kRelay, folded.c_str());
}

// --- loopback replay ---------------------------------------------------------
// Replay one request into the device's own web server; fill `rp`. Returns false on a
// connect/timeout failure (the caller turns that into a 502/504). Shared by the online
// req handler and CLOUDLOOP.
bool doLoopback(const std::string& method, const std::string& path,
                const http_replay::Headers& headers, const uint8_t* body, size_t bodyLen,
                http_replay::ResponseParser& rp) {
  WiFiClient c;
  // CUM-173: replay INTO the device's own web server over the lwIP loopback.
  // 127.0.0.1 is the reliable primary on esp32s3 (CONFIG_LWIP_NETIF_LOOPBACK=y -
  // verified ~2 ms, serves every route); no wire, no AP hairpin. The STA self-IP is
  // only a fallback, and ONLY when it is a real address - WiFi.localIP() can be
  // 0.0.0.0 just after a (re)join, and dialing that guarantees a 502 (the policy is
  // host-tested in loopbackFallbackUsable). Short connect timeout: a real loopback
  // connects instantly and a refusal returns immediately, so the old 4 s stalls only
  // ever added latency (part of the field "lag").
  const uint32_t t0 = millis();
  const char* via = "127.0.0.1";
  bool connected = c.connect(IPAddress(127, 0, 0, 1), 80, kLoopbackConnectMs);
  if (!connected) {
    IPAddress self = WiFi.localIP();
    const uint32_t ipABCD = (uint32_t(self[0]) << 24) | (uint32_t(self[1]) << 16) |
                            (uint32_t(self[2]) << 8) | uint32_t(self[3]);
    if (nimbus::cloud::loopbackFallbackUsable(ipABCD)) {
      via = "sta-ip";
      connected = c.connect(self, 80, kLoopbackConnectMs);
    } else {
      via = "sta-ip-skip";   // no usable self-IP yet: fail fast rather than dial 0.0.0.0
    }
  }
  const uint32_t connMs = millis() - t0;
  if (!connected) {
    // Distinct tag for the tunnel-502 path (the release gate asserts on these lines).
    agent::alogf("relay: loopback REFUSED via=%s conn=%ums (tunnel 502)", via, connMs);
    return false;
  }
  std::string head = http_replay::buildRequestHead(method, path, headers, bodyLen,
                                                   agent::store::webAuthToken().c_str());
  if (head.empty()) { c.stop(); return false; }  // rejected (CRLF/control in the request line)
  c.write(reinterpret_cast<const uint8_t*>(head.data()), head.size());
  if (bodyLen) c.write(body, bodyLen);

  // Read buffer off the task stack (heap) - a large on-stack buffer here overflowed
  // the relay task while a tunneled request was in flight (crash + reboot, found live).
  // Prefer PSRAM (CUM-167): keep this transient buffer OUT of the scarce internal SRAM
  // the display bounce buffer now shares, falling back to internal only if PSRAM is full.
  const size_t kBuf = 1460;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(kBuf, MALLOC_CAP_8BIT);
  if (!buf) { c.stop(); return false; }
  // CUM-387 diagnostic: track the INTERNAL-SRAM low-water reached while the response
  // streams in. The staging (ws frame, response body, and the parser's unparsed bytes)
  // is PSRAM-backed, so this shows the relay's real per-serve internal footprint and
  // proves the buffers stay OFF the scarce internal heap (docs/memory-model.md ethos).
  size_t intMin = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t deadline = millis() + kLoopbackTimeoutMs;
  while (millis() < deadline && !rp.complete() && !rp.error()) {
    int n = readSome(c, buf, kBuf, 200);
    if (n > 0) {
      rp.feed(buf, (size_t)n);
      size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      if (f < intMin) intMin = f;
    } else if (n < 0) {
      rp.endOfStream();
      break;
    }
  }
  if (!rp.complete()) rp.endOfStream();
  heap_caps_free(buf);
  c.stop();
  logServe(via, connMs, rp, intMin);
  return rp.complete();
}

// --- WS send -----------------------------------------------------------------
// Write an entire framed message to the resident WS, tolerating partial and
// would-block writes (CUM-182). A large res frame does not always leave in one
// WiFiClientSecure::write; treat a short write as "more to send", not failure,
// so the frame is never truncated on the wire. Loops until the whole buffer is
// out, the socket dies, or kResWriteTimeoutMs elapses. The relay task is not
// watchdog-subscribed, so the bounded retry-yield here cannot starve the WDT.
bool wsWriteRange(const uint8_t* data, size_t len, uint32_t deadline) {
  if (!g_ws) return false;
  // Bounded chunks: handing mbedtls a single ~370 KB buffer is exactly what
  // returned short before; 8 KB records drain reliably as the TLS/socket buffer
  // frees, and a partial return just resumes at the sent offset. The loop lives
  // in lib/core (drainAll) so it is host-tested.
  return nimbus::cloud::drainAll(
      len, 8192,
      [&](size_t off, size_t want) -> size_t { return g_ws->write(data + off, want); },
      [&]() -> bool { return g_ws && g_ws->connected(); },
      [&]() -> bool { return (int32_t)(deadline - millis()) <= 0; },
      []() { vTaskDelay(pdMS_TO_TICKS(5)); });  // WANT_WRITE / full buffer: yield, then retry
}

bool wsWriteAll(const uint8_t* data, size_t len) {
  return wsWriteRange(data, len, millis() + kResWriteTimeoutMs);
}

bool wsSendSmall(ws::Opcode op, const uint8_t* payload, size_t len) {
  if (!g_ws) return false;
  uint8_t mask[4];
  fillMask(mask);
  std::string frame;
  ws::encodeClientFrame(op, payload, len, mask, frame);
  return wsWriteAll(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
}

// Secret-containment policy for the tunnel lives in the portable, host-tested guard
// (lib/core, nimbus::cloud::tunnel): it canonicalizes the raw tunneled path to the form
// the local router dispatches on (percent-decode + query/trailing-slash strip) BEFORE the
// denylist check, refuses the secret-bearing endpoints (connect / token-regen / signin),
// and scrubs the durable token + AP password from any tunneled JSON body as a backstop.
// Cap the backstop copy: a JSON secret response is a few hundred bytes, so only small
// bodies are copied+scrubbed; the big bodies (the UI page, downloads) skip it untouched.
constexpr size_t kMaxScrubBody = 16 * 1024;

// Return the value of a header by case-insensitive name, or "" if absent.
std::string headerValue(const http_replay::Headers& headers, const char* nameLower) {
  for (const auto& kv : headers) {
    std::string n = kv.first;
    for (char& c : n) c = (char)tolower((unsigned char)c);
    if (n == nameLower) return kv.second;
  }
  return std::string();
}

// Collect the tunneled request's headers + decoded body, then replay it into the local
// server. Returns doLoopback's success. Body decode is bounded first so a hostile
// bodyB64 can't blow the scarce internal-SRAM decode.
bool replayReq(const ReqFrame& req, http_replay::ResponseParser& rp) {
  http_replay::Headers hdrs;
  http_replay::BodyBuf body;  // decoded request body: PSRAM on device (CUM-387)
  // Collect headers into the portable vector.
  if (!req.headers.isNull()) {
    for (JsonPairConst kv : req.headers) {
      if (kv.value().is<const char*>())
        hdrs.emplace_back(std::string(kv.key().c_str()),
                          std::string(kv.value().as<const char*>()));
    }
  }
  // Decode request body (small; POST /api/config and friends). Bound the encoded
  // length first so a hostile bodyB64 can't blow the internal-SRAM decode.
  if (req.bodyB64 && req.bodyB64[0] && strlen(req.bodyB64) <= kMaxReqBody * 4 / 3 + 4) {
    if (!b64Decode(req.bodyB64, strlen(req.bodyB64), body) || body.size() > kMaxReqBody) {
      body.clear();
    }
  }
  return doLoopback(req.method, req.path, hdrs, body.data(), body.size(), rp);
}

// How a frame's write ended. Busy/Ended/Broken only come from a big frame's write
// through the TLS work slot (writeBigFrame); Failed is a small frame that did not leave.
enum class BigFrame : uint8_t { Sent, Failed, Busy, Ended, Broken };

// Write one big frame through the TLS work slot (heap F2): wait for the slot in short
// takes with the WebSocket pumped between, bounded so the request gets a busy answer
// kRelayBusyAfterMs after it arrived; then 8 KB chunks, each under the slot, which is
// released between chunks so a waiting turn gets it within one chunk (tg_poll
// outranks the relay task, so it takes the slot the moment it is given back). Never
// called across the loopback replay: a tunneled handler can take the slot itself.
BigFrame writeBigFrame(const uint8_t* frame, size_t len, size_t bodyLen, uint32_t reqAtMs) {
  using namespace nimbus::cloud;
  const uint32_t t0 = millis();
  const SlotWait w = waitForSlot(
      reqAtMs + kRelayBusyAfterMs, [] { return (uint32_t)millis(); },
      [](uint32_t ms) { return agent::arbiter::acquireWork(ms); }, [] { return pumpWhileWaiting(); });
  if (w == SlotWait::Busy) return BigFrame::Busy;
  if (w == SlotWait::Ended) return BigFrame::Ended;
  nimbus::heapwatch::Scope bulk(nimbus::heapwatch::kRelayBulk);
  const uint32_t deadline = reqAtMs + kRelayResBudgetMs;
  const uint32_t at = millis();
  uint32_t waited = at - t0;   // before the first chunk, then between chunks
  const BulkResult r = writeBulk(
      len, deadline,
      [&waited](uint32_t dl) {   // mid-frame nothing else may go out, so this wait only waits
        const uint32_t w0 = millis();
        bool got = false;
        for (int32_t left = (int32_t)(dl - millis()); left > 0 && !got; left = (int32_t)(dl - millis()))
          got = agent::arbiter::acquireWork(std::min<uint32_t>(kRelaySlotTakeMs, (uint32_t)left));
        waited += millis() - w0;
        return got;
      },
      [] { agent::arbiter::releaseWork(); },
      [&](size_t off, size_t n) {
        nimbus::heapwatch::tlsIoBegin("relay.bulk");
        const bool ok = wsWriteRange(frame + off, n, deadline);
        nimbus::heapwatch::tlsIoEnd();
        return ok;
      },
      [] { vTaskDelay(1); });
  if (r != BulkResult::Sent) return BigFrame::Broken;
  // One line per big frame: when it went out and how long it waited for the slot
  // (before and between chunks), so a bench run can line it up with the turn's
  // exchanges (transport heaptrace at=/ms=).
  agent::alogf("relay: bulk res body=%u frame=%u at=%u ms=%u waited=%u", (unsigned)bodyLen,
               (unsigned)len, (unsigned)at, (unsigned)(millis() - at), (unsigned)waited);
  return BigFrame::Sent;
}

// Send one built frame: a small answer straight out, a big one through the TLS work slot
// (or, on the bench counter-leg, straight out as v4.5.13 did). Decided on the body, not
// the frame: base64 and the head make the frame about a third larger, so the dashboard's
// 6-7 KB polls are 8.5-9.5 KB frames that must not wait out a turn's exchange.
BigFrame sendFrame(const uint8_t* frame, size_t len, size_t bodyLen, uint32_t reqAtMs) {
  if (!nimbus::cloud::frameNeedsSlot(bodyLen)) return wsWriteAll(frame, len) ? BigFrame::Sent : BigFrame::Failed;
  if (!relayNoSlot()) return writeBigFrame(frame, len, bodyLen, reqAtMs);
  nimbus::heapwatch::tlsIoBegin("relay.bulk");
  const bool ok = wsWriteAll(frame, len);
  nimbus::heapwatch::tlsIoEnd();
  return ok ? BigFrame::Sent : BigFrame::Failed;
}

// After a frame went (or did not go) out: a busy wait gets the busy answer instead, a
// session that ended while waiting gets nothing, a frame cut mid-write ends the
// session (the stream is unusable; the task redials and the cloud page retries).
// Returns false when there is nothing more to log for this answer.
bool settleFrame(BigFrame out, const char* id, int status, size_t bodyLen, uint32_t reqAtMs) {
  if (out == BigFrame::Busy) {
    agent::alogf("relay: res %d body=%u waited %ums for the TLS slot -> busy", status,
                 (unsigned)bodyLen, (unsigned)(millis() - reqAtMs));
    emitBusy(id, nimbus::cloud::kBusyRetrySlotS);
    return false;
  }
  if (out == BigFrame::Ended) return false;
  if (out == BigFrame::Broken) {
    agent::alogf("relay: res %d body=%u cut mid-write - ending the session", status,
                 (unsigned)bodyLen);
    if (g_ws) tlsClose(*g_ws);
    g_on.ended = true;
  }
  return true;
}

// Frame a `res` back over the WS: a SMALL head {t,id,status,headers} built with
// ArduinoJson (correct escaping), then the base64 body spliced directly into ONE PSRAM
// frame buffer. This avoids handing ArduinoJson a 300 KB linked string (which it did not
// reliably retain) and keeps the whole res in one PSRAM buffer. A body over
// kRelayBulkBytes goes out through the TLS work slot (writeBigFrame); `reqAtMs` is when
// the request arrived (0 = now), the start of the cloud's 30 s bound.
void emitResFrame(const char* id, int status, const http_replay::Headers& headers,
                  const uint8_t* obody, size_t obodyLen, uint32_t reqAtMs = 0) {
  if (!reqAtMs) reqAtMs = millis();
  JsonDocument head;  // small: a couple of headers
  head["t"] = "res";
  head["id"] = id;
  head["status"] = status;
  JsonObject h = head["headers"].to<JsonObject>();
  for (const auto& kv : headers) h[kv.first.c_str()] = kv.second.c_str();
  std::string headJson;
  serializeJson(head, headJson);        // {"t":"res","id":"..","status":200,"headers":{..}}
  if (!headJson.empty() && headJson.back() == '}') headJson.pop_back();  // reopen for bodyB64

  static const char kMid[] = ",\"bodyB64\":\"";
  static const char kSuf[] = "\"}";
  size_t b64len = obodyLen ? b64EncodedLen(obodyLen) : 0;
  size_t payloadLen = headJson.size() + (obodyLen ? (sizeof(kMid) - 1) + b64len + (sizeof(kSuf) - 1)
                                                  : 1 /* just '}' */);
  uint8_t mask[4];
  fillMask(mask);
  uint8_t whdr[14];
  size_t hn = ws::writeFrameHeader(ws::Opcode::Text, payloadLen, mask, whdr);
  uint8_t* frame = (uint8_t*)heap_caps_malloc(hn + payloadLen, MALLOC_CAP_SPIRAM);
  if (!frame) frame = (uint8_t*)heap_caps_malloc(hn + payloadLen, MALLOC_CAP_8BIT);
  if (!frame) {
    agent::alog("relay: res drop (no mem)");
    return;
  }
  size_t off = 0;
  memcpy(frame, whdr, hn);
  off = hn;
  memcpy(frame + off, headJson.data(), headJson.size());
  off += headJson.size();
  if (obodyLen) {
    memcpy(frame + off, kMid, sizeof(kMid) - 1);
    off += sizeof(kMid) - 1;
    b64EncodeRaw(obody, obodyLen, reinterpret_cast<char*>(frame) + off);  // straight into the frame
    off += b64len;
    memcpy(frame + off, kSuf, sizeof(kSuf) - 1);
    off += sizeof(kSuf) - 1;
  } else {
    frame[off++] = '}';
  }
  for (size_t i = hn; i < off; i++) frame[i] ^= mask[(i - hn) & 3];  // mask the payload
  // Reliable full-frame write (CUM-182): a large page (~370 KB) is flushed in as
  // many WiFiClientSecure::write calls as it takes, never truncated into a
  // session-killing partial frame. A big one takes the TLS work slot per chunk.
  const BigFrame out = sendFrame(frame, off, obodyLen, reqAtMs);
  heap_caps_free(frame);
  if (!settleFrame(out, id, status, obodyLen, reqAtMs)) return;
  const bool sent = out == BigFrame::Sent;
  // Durable only when the frame did not leave or the status is a server error (the
  // forensic window); a routine res line stays in the RAM ring + serial.
  const String resLine = String("relay: res ") + status + " body=" + (uint32_t)obodyLen +
                         " sent=" + (sent ? 1 : 0);
  if (!sent || status >= 500) agent::alog(resLine.c_str());
  else agent::alogi(resLine.c_str());
}

// Answer a tunneled request: replay into the local server, frame the response back.
// The plain-text body for a tunnel error status (kept out of handleReq to hold it
// under the complexity gate). 500 is the CUM-173 over-cap backstop.
const char* tunnelErrReason(bool denied, int status) {
  if (denied) return "Not available over the cloud; use this device on its network.";
  if (status == 500) return "This page is too large to load over the cloud link.";
  if (status == 502) return "response too large";
  if (status == 503) return nimbus::cloud::kBusyBody;
  return "device timeout";
}

// The busy answer (lane X contract): 503 with Retry-After, which the cloud uses as
// the floor of its next retry. Small, so it never waits for the slot.
void emitBusy(const char* id, uint32_t retryAfterS) {
  http_replay::Headers h;
  h.emplace_back("content-type", "text/plain");
  h.emplace_back("retry-after", std::to_string(retryAfterS));
  emitResFrame(id, 503, h, reinterpret_cast<const uint8_t*>(nimbus::cloud::kBusyBody),
               strlen(nimbus::cloud::kBusyBody));
}

// Admission for a big tunneled answer (panic F2): while a turn is in flight or
// internal memory is short, a body over 16 KB is answered busy instead. The web
// server already answers its own loopback 503 for the routes it gates; this is the
// backstop for every other big body. Returns the Retry-After seconds, 0 = serve.
uint32_t busyRetryFor(size_t bodyLen) {
  if (relayNoSlot()) return 0;
  const bool turn = agent::orchestrator::turnInFlight();
  const size_t intFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (nimbus::cloud::tunnelServeDecision(bodyLen, turn, intFree) != nimbus::cloud::ServeDecision::Busy)
    return 0;
  return turn ? nimbus::cloud::kBusyRetryTurnS : nimbus::cloud::kBusyRetryLowMemS;
}

// True when the busy answer went out instead of a big body (busyRetryFor).
bool heldBack(const char* id, size_t bodyLen) {
  const uint32_t retryS = busyRetryFor(bodyLen);
  if (!retryS) return false;
  agent::alogf("relay: %u B answer held back (%s) -> busy", (unsigned)bodyLen,
               retryS == nimbus::cloud::kBusyRetryTurnS ? "turn in flight" : "low memory");
  emitBusy(id, retryS);
  return true;
}

// A request that waited out the busy window in the queue (it arrived while a big
// answer waited for the slot) is answered busy at once: a page could not follow
// inside the cloud's 30 s bound. True when it was.
bool staleInQueue(const char* id, uint32_t arrivedAt) {
  if (relayNoSlot() || (uint32_t)(millis() - arrivedAt) < nimbus::cloud::kRelayBusyAfterMs) return false;
  emitBusy(id, nimbus::cloud::kBusyRetrySlotS);
  return true;
}

// The headers of a plain-text error answer. A loopback 5xx keeps its Retry-After (the
// web server's own busy answer): the cloud uses it as the floor of its next retry.
void errorHeaders(http_replay::Headers& out, bool ok, const http_replay::ResponseParser& rp) {
  out.emplace_back("content-type", "text/plain");
  const std::string retryAfter = ok ? headerValue(rp.headers(), "retry-after") : std::string();
  if (!retryAfter.empty()) out.emplace_back("retry-after", retryAfter);
}

void handleReq(const ReqFrame& req, uint32_t arrivedAt) {
  nimbus::heapwatch::Scope serving(nimbus::heapwatch::kTunnel);
  if (staleInQueue(req.id, arrivedAt)) return;
  const bool denied = tunnel::isTunnelDenied(req.path ? req.path : "");
  http_replay::ResponseParser rp(kMaxRespBody);
  bool ok = denied ? false : replayReq(req, rp);

  int status = denied ? 403 : (ok ? rp.status() : 504);
  if (ok && rp.overflow()) {
    // CUM-173: a body over kMaxRespBody would be truncated - never frame a partial
    // page. Return an EXPLICIT 500-with-reason (distinct tag) so the cloud maps it to
    // the "Reaching your Nimbus" interstitial instead of a bare Cloudflare 502, and so
    // the regression is loud in the logs. (The cap now clears the config page; this is
    // the backstop for anything genuinely oversized.)
    status = 500;
    agent::alogf("relay: response over cap (%u bytes, cap %u) -> 500",
                 (unsigned)rp.body().size(), (unsigned)kMaxRespBody);
  }

  // Choose the response body: the real loopback body for a clean <500 result, otherwise
  // a small plain-text explanation (the same policy the inline version used).
  http_replay::Headers outHdrs;
  const uint8_t* obody = nullptr;
  size_t obodyLen = 0;
  std::string errStr;
  std::string scrubbed;  // holds a redacted copy when the JSON secret backstop fires
  if (ok && status < 500) {
    if (heldBack(req.id, rp.body().size())) return;
    outHdrs = rp.headers();
    if (!rp.body().empty()) { obody = rp.body().data(); obodyLen = rp.body().size(); }
    // Backstop: strip the durable token / AP password from a small JSON body before it is
    // framed to the cloud, so an un-denied endpoint can never carry them off the device.
    if (obodyLen && obodyLen <= kMaxScrubBody) {
      std::string ct = headerValue(outHdrs, "content-type");
      scrubbed.assign(reinterpret_cast<const char*>(obody), obodyLen);
      if (tunnel::scrubJsonSecrets(ct, scrubbed)) {
        obody = reinterpret_cast<const uint8_t*>(scrubbed.data());
        obodyLen = scrubbed.size();
        agent::alog("relay: scrubbed secret field from tunneled body");
      }
    }
  } else {
    errorHeaders(outHdrs, ok, rp);
    errStr = tunnelErrReason(denied, status);
    obody = reinterpret_cast<const uint8_t*>(errStr.data());
    obodyLen = errStr.size();
  }
  emitResFrame(req.id, status, outHdrs, obody, obodyLen, arrivedAt);
}

// --- chunked tunnel upload (protocol 2) --------------------------------------
// A large body (an mp3, a firmware image) can't ride one `req` frame: it would blow the
// 16 KB inbound cap. Instead the relay streams it as ubegin -> uchunk... -> uend, and the
// device pipes each slice straight into its own web server over a PERSISTENT loopback
// socket (the same handler a direct LAN upload uses, which already streams to SD). The
// device acks the durable offset after each slice, giving the cloud real progress and
// backpressure. Ordering/caps live in the host-tested UploadReassembly; this seam owns
// the socket. Uploads are serialized like every other tunneled request (one at a time).
struct UploadSession {
  bool active = false;
  bool failed = false;          // a connect/write/cap failure: answer an error at uend
  bool denied = false;          // a tunnel-denied path: answer 403 at uend
  int failStatus = 502;         // the status to answer if failed
  WiFiClient loop;              // the persistent loopback connection (open across chunks)
  nimbus::cloud::UploadReassembly reasm;
  std::string id;
};
UploadSession g_up;

// Connect the loopback to the device's own web server (mirrors doLoopback's target
// policy: 127.0.0.1 primary, the STA self-IP only when it is a real address).
bool connectUploadLoopback(WiFiClient& c) {
  if (c.connect(IPAddress(127, 0, 0, 1), 80, kLoopbackConnectMs)) return true;
  IPAddress self = WiFi.localIP();
  const uint32_t ipABCD = (uint32_t(self[0]) << 24) | (uint32_t(self[1]) << 16) |
                          (uint32_t(self[2]) << 8) | uint32_t(self[3]);
  if (nimbus::cloud::loopbackFallbackUsable(ipABCD)) return c.connect(self, 80, kLoopbackConnectMs);
  return false;
}

// Tear down any in-flight upload (a drop, an abort, or the start of a new one).
void resetUpload() {
  if (g_up.loop.connected()) g_up.loop.stop();
  g_up.reasm.abort();
  g_up = UploadSession{};
}

// Emit a small plain-text error `res` for an upload that cannot be served, then reset.
void emitUploadError(const char* id, int status, const char* reason) {
  http_replay::Headers h;
  h.emplace_back("content-type", "text/plain");
  emitResFrame(id, status, h, reinterpret_cast<const uint8_t*>(reason), strlen(reason));
  resetUpload();
}

// ubegin: open the loopback, write the request head with Content-Length=totalLen, and
// arm the reassembler. Failures are latched (answered at uend) so the cloud's ack-gated
// stream still terminates honestly rather than hanging.
void handleUploadBegin(const UploadFrame& u) {
  if (g_up.active) resetUpload();  // a new upload supersedes a stale one (should not happen)
  g_up = UploadSession{};
  g_up.active = true;
  g_up.id = u.id ? u.id : "";
  g_up.reasm.begin(g_up.id, u.totalLen, kMaxUploadBytes);

  if (tunnel::isTunnelDenied(u.path ? u.path : "")) {
    g_up.denied = true;  // ack chunks to a discard sink, answer 403 at uend
    return;
  }
  if (u.totalLen > kMaxUploadBytes) {
    g_up.failed = true;
    g_up.failStatus = 413;
    return;
  }
  // Rebuild the request head into the loopback with the declared body length. Header
  // forwarding + token injection + hop-by-hop/Content-Length handling are the same
  // host-tested path a small request uses.
  http_replay::Headers hdrs;
  if (!u.headers.isNull()) {
    for (JsonPairConst kv : u.headers) {
      if (kv.value().is<const char*>())
        hdrs.emplace_back(std::string(kv.key().c_str()), std::string(kv.value().as<const char*>()));
    }
  }
  if (!connectUploadLoopback(g_up.loop)) {
    agent::alog("relay: upload loopback REFUSED (tunnel 502)");
    g_up.failed = true;
    g_up.failStatus = 502;
    return;
  }
  std::string head = http_replay::buildRequestHead(
      u.method ? u.method : "POST", u.path ? u.path : "/", hdrs, u.totalLen,
      agent::store::webAuthToken().c_str());
  if (head.empty()) {  // rejected (CRLF/control in the request line)
    g_up.failed = true;
    g_up.failStatus = 400;
    g_up.loop.stop();
    return;
  }
  g_up.loop.write(reinterpret_cast<const uint8_t*>(head.data()), head.size());
  agent::alogf("relay: upload begin id=%s total=%u", g_up.id.c_str(), (unsigned)u.totalLen);
}

// uchunk: decode the slice and stream it into the loopback socket, then ack the durable
// offset. A denied/failed session still acks (to a discard sink) so the cloud completes
// and gets the honest error at uend, rather than hanging on a stalled window.
void handleUploadChunk(const UploadFrame& u) {
  if (!g_up.active || g_up.id != (u.id ? u.id : "")) return;  // stale / wrong id
  http_replay::BodyBuf slice;  // decoded upload chunk: PSRAM on device (CUM-387)
  if (u.bodyB64 && u.bodyB64[0]) {
    if (strlen(u.bodyB64) > kMaxInboundFrame ||
        !b64Decode(u.bodyB64, strlen(u.bodyB64), slice)) {
      emitUploadError(u.id, 400, "bad chunk");
      return;
    }
  }
  // The sink writes into the loopback socket, unless the session is denied/failed, in
  // which case it discards (we still advance the offset so the cloud's stream drains).
  auto sink = [&](const uint8_t* d, size_t n) -> bool {
    if (g_up.denied || g_up.failed) return true;
    return g_up.loop.write(d, n) == n;
  };
  auto outcome = g_up.reasm.chunk(g_up.id, u.seq, u.off, slice.data(), slice.size(), sink);
  if (outcome != nimbus::cloud::UploadReassembly::ChunkOutcome::Accepted) {
    // Out-of-order, over-cap, or a socket write failure: the body is unrecoverable.
    agent::alogf("relay: upload chunk rejected id=%s (outcome=%d)", g_up.id.c_str(), (int)outcome);
    emitUploadError(u.id, 502, "upload failed");
    return;
  }
  JsonDocument ack;
  buildUploadAck(ack, g_up.id.c_str(), g_up.reasm.acked());
  std::string s;
  serializeJson(ack, s);
  wsSendSmall(ws::Opcode::Text, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Read the local server's response off the loopback (the body is already fully written)
// and frame it back as the ordinary `res`, then reset the session. Split out of
// handleUploadEnd to keep each function inside the complexity gate.
void streamUploadResponse(const char* id) {
  http_replay::ResponseParser rp(kMaxRespBody);
  const size_t kBuf = 1460;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(kBuf, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(kBuf, MALLOC_CAP_8BIT);
  if (!buf) { emitUploadError(id, 500, "no memory"); return; }
  uint32_t deadline = millis() + kLoopbackTimeoutMs;
  while (millis() < deadline && !rp.complete() && !rp.error()) {
    int n = readSome(g_up.loop, buf, kBuf, 200);
    if (n > 0) rp.feed(buf, (size_t)n);
    else if (n < 0) { rp.endOfStream(); break; }
  }
  if (!rp.complete()) rp.endOfStream();
  heap_caps_free(buf);
  const int status = rp.complete() ? rp.status() : 504;
  agent::alogf("relay: upload end id=%s status=%d bytes=%u", g_up.id.c_str(), status,
               (unsigned)g_up.reasm.acked());
  http_replay::Headers outHdrs = rp.headers();
  const uint8_t* obody = rp.body().empty() ? nullptr : rp.body().data();
  size_t obodyLen = rp.body().size();
  // Same small-JSON secret backstop the req path applies (an upload answer is tiny JSON).
  std::string scrubbed;
  if (obodyLen && obodyLen <= kMaxScrubBody) {
    std::string ct = headerValue(outHdrs, "content-type");
    scrubbed.assign(reinterpret_cast<const char*>(obody), obodyLen);
    if (tunnel::scrubJsonSecrets(ct, scrubbed)) {
      obody = reinterpret_cast<const uint8_t*>(scrubbed.data());
      obodyLen = scrubbed.size();
    }
  }
  emitResFrame(id, status, outHdrs, obody, obodyLen);
  resetUpload();
}

// uend: finalize. A denied/failed/incomplete session answers its error; otherwise the
// local server has the whole body, so read its response and frame it back.
void handleUploadEnd(const UploadFrame& u) {
  if (!g_up.active || g_up.id != (u.id ? u.id : "")) return;
  if (g_up.denied) {
    emitUploadError(u.id, 403, "Not available over the cloud; use this device on its network.");
    return;
  }
  if (g_up.failed) {
    emitUploadError(u.id, g_up.failStatus,
                    g_up.failStatus == 413 ? "That file is too large."
                                           : "The upload could not be saved.");
    return;
  }
  if (!g_up.reasm.end(g_up.id)) {  // incomplete body (declared length not met)
    emitUploadError(u.id, 400, "upload incomplete");
    return;
  }
  streamUploadResponse(u.id);
}

void handleUploadAbort(const UploadFrame& u) {
  if (g_up.active && g_up.id == (u.id ? u.id : "")) {
    agent::alogf("relay: upload abort id=%s reason=%s", g_up.id.c_str(),
                 u.reason ? u.reason : "");
    resetUpload();
  }
}

// Dispatch a chunked-upload frame. Returns true if `f` was one (so handleFrame's chain
// stays flat and inside the complexity gate), false for any non-upload frame.
bool handleUploadFrame(const RelayFrame& f) {
  switch (f.type) {
    case FrameType::UploadBegin: handleUploadBegin(f.upload); return true;
    case FrameType::UploadChunk: handleUploadChunk(f.upload); return true;
    case FrameType::UploadEnd:   handleUploadEnd(f.upload);   return true;
    case FrameType::UploadAbort: handleUploadAbort(f.upload); return true;
    default: return false;
  }
}

// --- pairing -----------------------------------------------------------------
// One-shot HTTPS POST of a JSON body to the relay host; returns status, body in out.
int httpsPostJson(const String& host, const char* path, const String& reqBody, String& out) {
  if (!agent::arbiter::acquireWork(15000)) return -1;
  // CUM-160: the one relay path that holds the single TLS work slot AND does a blocking
  // connect + read. A real relay/Cloudflare TLS connect legitimately takes many seconds
  // - that is latency, NOT a hang - so it is NOT aborted early (an earlier 5.5 s abort
  // stopped the device reconnecting at all, a field regression). The relay task is not
  // watchdog-subscribed, so this blocking connect does not itself trip the 8 s watchdog;
  // the READ then runs as watchdog-fed, bounded steps (driveStagedWait) so no single
  // step blocks longer than relayStepMs and a fed task can never starve. Instrumented.
  const uint32_t t0 = millis();
  WiFiClientSecure c;
  relayTlsSetup(c);
  int status = -1;
  const bool connected = c.connect(host.c_str(), kTlsPort, kConnectTimeoutMs);
  const uint32_t connMs = millis() - t0;
  if (connected) {
    String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
                 "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                 String(reqBody.length()) + "\r\n\r\n" + reqBody;
    c.print(req);
    // Robust, host-tested parser (Content-Length / chunked / until-close), driven as
    // bounded + fed steps so the slot-holding read never starves the watchdog.
    http_replay::ResponseParser rp(8192);  // pairing JSON is small
    uint8_t buf[512];
    nimbus::cloud::driveStagedWait(
        kConnectTimeoutMs, nimbus::cloud::relayStepMs,
        []() { return (uint32_t)millis(); },
        [&]() { return rp.complete() || rp.error(); },
        [&](uint32_t stepMs) {
          const uint32_t stepEnd = millis() + stepMs;
          while ((int32_t)(stepEnd - millis()) > 0 && !rp.complete() && !rp.error()) {
            int n = readSome(c, buf, sizeof(buf), 200);
            if (n > 0) rp.feed(buf, (size_t)n);
            else if (n < 0) { rp.endOfStream(); break; }
          }
        },
        []() { vTaskDelay(1); });   // yield between steps (relay task is not WDT-subscribed)
    if (!rp.complete()) rp.endOfStream();
    if (rp.complete()) {
      status = rp.status();
      out = "";
      out.reserve(rp.body().size());
      for (uint8_t b : rp.body()) out += (char)b;
    }
  }
  tlsClose(c);
  agent::arbiter::releaseWork();
  // Hold-instrumentation (kept per CUM-160; the CUM-174 release gate asserts on it):
  // total slot-hold, the real connect latency (proves connects legitimately take
  // seconds), and the status.
  const uint32_t heldMs = millis() - t0;
  agent::alogf("relay: httpsPost held=%ums connect=%ums status=%d", heldMs, connMs, status);
  return status;
}

// A "claimed" poll response: parse the relay host out of relayUrl, persist the pairing
// credential, and return true. A missing credential is a terminal failure (returns
// false); either way the caller stops polling.
bool persistClaimed(JsonDocument& pd, const String& host, const String& deviceId) {
  String cred = pd["credential"] | "";
  String name = pd["deviceName"] | "";
  String relayUrl = pd["relayUrl"] | "";
  String rHost = host;
  // relayUrl like wss://<host>/device -> keep the host.
  int hs = relayUrl.indexOf("://");
  if (hs > 0) {
    int he = relayUrl.indexOf('/', hs + 3);
    rHost = relayUrl.substring(hs + 3, he > 0 ? he : relayUrl.length());
  }
  if (cred.isEmpty()) { setErr("Pairing service error."); setPairing("", ""); return false; }
  if (!agent::store::setCloudPairing(deviceId, cred, rHost, name)) {
    // The credential did not persist even after evicting the rebuildable caches:
    // device storage is genuinely full. Surface it honestly instead of logging
    // "paired" and silently reverting to idle (which read as a permanent portal
    // "Never connected"), so the owner gets an actionable line, not a black hole.
    setErr("Couldn't save the cloud sign-in. Device storage is full.");
    setPairing("", "");
    agent::alog("relay: pairing SAVE FAILED (NVS full) - not paired");
    return false;
  }
  setPairing("", "");
  agent::alog("relay: paired");
  return true;
}

// --- CUM-52: credential expiry + re-mint (consumes the C6 contract, CUM-87) ------
// The device credential is a JWT that now carries an optional `exp` (30d default).
// The device re-mints it BEFORE expiry at a jittered [50-80%] point, and re-mints an
// expired-within-grace credential before reconnecting; past grace it re-pairs. All
// re-mint POSTs run while the WSS is DOWN (single-TLS-slot rule) and never mid-turn.

// Stable per-device jitter seed so a fleet that restarted together does not all
// re-mint at the same instant. FNV-1a over the deviceId (stable across reboots).
uint32_t remintSeed() {
  String id = agent::store::cloudDeviceId();
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < id.length(); ++i) { h ^= (uint8_t)id[i]; h *= 16777619u; }
  return h;
}

// The proactive re-mint target for the CURRENT credential (epoch s; 0 = none, e.g. a
// legacy no-exp credential). Cached at session start so the online loop can end the
// session (while idle) the moment the window opens, without re-parsing every tick.
uint64_t g_remintAtEpoch = 0;

// After a TRANSIENT re-mint failure (5xx / rate-limit / network blip), do not recycle
// the online session again until this epoch. Without it, the proactive target stays in
// the past (the credential is unchanged), so every reconnect would tear the session
// down before serving a single request - a multi-day reconnect storm that also breaks
// cloud access while the OLD credential is still perfectly valid (review finding). The
// credential stays valid until its own exp, so we simply serve normally and retry the
// re-mint on this slow cadence.
uint64_t g_remintBackoffUntil = 0;
constexpr uint32_t kRemintRetryBackoffSec = 1800;   // 30 min between failed re-mint tries

enum class RemintResult { Ok, Reauth, Transient };

// POST /device/credential/remint with the CURRENT credential; on 200 persist the fresh
// one atomically (setCloudPairing -> NVS) BEFORE returning. Caller guarantees the WSS
// is down. Reauth => must re-pair; Transient => retry later.
RemintResult remintCredential() {
  String host = agent::store::cloudHost();
  String deviceId = agent::store::cloudDeviceId();
  String cred = agent::store::cloudCred();
  if (host.isEmpty() || deviceId.isEmpty() || cred.isEmpty()) return RemintResult::Transient;
  JsonDocument req;
  req["deviceId"]   = deviceId;
  req["credential"] = cred;
  String body;
  serializeJson(req, body);
  String resp;
  const int st = httpsPostJson(host, "/device/credential/remint", body, resp);
  if (st == 200) {
    JsonDocument pd;
    if (deserializeJson(pd, resp)) return RemintResult::Transient;   // garbled 200
    String newCred = pd["credential"] | "";
    if (newCred.isEmpty()) return RemintResult::Transient;
    String relayUrl = pd["relayUrl"] | "";
    String rHost = host;
    int hs = relayUrl.indexOf("://");
    if (hs > 0) {
      int he = relayUrl.indexOf('/', hs + 3);
      rHost = relayUrl.substring(hs + 3, he > 0 ? he : relayUrl.length());
    }
    // Atomic swap: setCloudPairing writes NVS, then future reads see the new cred.
    // If the write is NVS-starved (setCloudPairing evicts caches + retries, then
    // returns false), keep serving on the still-valid OLD credential and retry the
    // re-mint later - never claim a re-mint that did not land.
    if (!agent::store::setCloudPairing(deviceId, newCred, rHost, agent::store::cloudName())) {
      agent::alog("relay: re-mint SAVE FAILED (NVS full) - keeping current credential");
      return RemintResult::Transient;
    }
    agent::alog("relay: credential re-minted");
    return RemintResult::Ok;
  }
  if (st == 401 || st == 404) return RemintResult::Reauth;   // past grace / rotated / unpaired
  return RemintResult::Transient;                            // network / 5xx / rate-limited
}

// Wipe the dead pairing and hand off to the pairing flow (shows the claim code).
void dropToPairing(const char* why) {
  agent::store::clearCloudPairing();
  setState(State::Idle);
  setErr(why);
  g_reqPair = true;   // auto-start pairing so the device shows a fresh claim code
}

// Between-session maintenance (WSS down): re-mint proactively at the jittered window,
// or re-mint an expired-within-grace credential before the next connect; re-pair once
// past grace. Returns false only when a re-pair is needed (caller must not connect).
bool maintainCredential() {
  String cred = agent::store::cloudCred();
  if (cred.isEmpty()) return true;
  const uint64_t now = (uint64_t)time(nullptr);
  if (now < 1000000000ULL) return true;   // clock unsynced: never act on a bogus time
  const std::string credStd(cred.c_str());
  const uint64_t exp = nimbus::cloud::credentialExp(credStd);
  const uint64_t iat = nimbus::cloud::credentialIat(credStd);
  const uint32_t seed = remintSeed();
  if (nimbus::cloud::expiredPastGrace(exp, now)) { dropToPairing("Sign-in expired. Pair again when ready."); return false; }
  const bool due = nimbus::cloud::remintDueProactive(iat, exp, now, seed) ||
                   nimbus::cloud::expiredWithinGrace(exp, now);
  if (due && now >= g_remintBackoffUntil) {
    const RemintResult r = remintCredential();
    if (r == RemintResult::Reauth) { dropToPairing("Sign-in expired. Pair again when ready."); return false; }
    // Ok: the new credential's target is far in the future, so the session will not
    // recycle again. Transient: keep serving on the still-valid credential and do not
    // retry (or recycle) for a while - otherwise the past-due target would tear down
    // every reconnect immediately (the storm the review caught).
    g_remintBackoffUntil = (r == RemintResult::Ok) ? 0 : now + kRemintRetryBackoffSec;
  }
  return true;
}

// After a server BadToken (4001) close: the relay rejected our credential.
//   Repaired  - past grace or a 401/404: pairing flow started, do not reconnect.
//   Refreshed - a fresh credential was minted: reconnect now, and this is PROGRESS
//               (the caller must NOT count it as a bad-token strike, or a legit
//               re-mint would push the device toward the Disabled state - review).
//   Retry     - transient failure: reconnect with the old (still in-grace) credential
//               and count a strike so repeated hard failures eventually back off.
enum class BadTokenOutcome { Repaired, Refreshed, Retry };
BadTokenOutcome recoverFromBadToken() {
  const uint64_t now = (uint64_t)time(nullptr);
  const uint64_t exp = nimbus::cloud::credentialExp(std::string(agent::store::cloudCred().c_str()));
  if (now > 1000000000ULL && nimbus::cloud::expiredPastGrace(exp, now)) {
    dropToPairing("Sign-in expired. Pair again when ready.");
    return BadTokenOutcome::Repaired;
  }
  const RemintResult r = remintCredential();
  if (r == RemintResult::Reauth) { dropToPairing("Sign-in expired. Pair again when ready."); return BadTokenOutcome::Repaired; }
  return r == RemintResult::Ok ? BadTokenOutcome::Refreshed : BadTokenOutcome::Retry;
}

// Run the full pairing exchange: /pair/init then poll /pair/poll. On success, persist
// the credential and return true (caller connects). Surfaces the code+URL for the panel.
bool runPairing() {
  String host = agent::store::cloudHost();
  String mac;
  {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    char b[13];
    snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    mac = b;
  }
  JsonDocument reqDoc;
  reqDoc["name"] = nimbus::sys::deviceName();
  reqDoc["fw"] = NIMBUS_FW_VERSION;
  reqDoc["hwSerial"] = mac;
  String reqBody;
  serializeJson(reqDoc, reqBody);

  String resp;
  int st = httpsPostJson(host, "/pair/init", reqBody, resp);
  if (st != 200) { setErr("Couldn't start pairing. Check Wi-Fi and try again."); return false; }
  JsonDocument initDoc;
  if (deserializeJson(initDoc, resp)) { setErr("Pairing service error."); return false; }
  String deviceId = initDoc["deviceId"] | "";
  String code = initDoc["code"] | "";
  String claimUrl = initDoc["claimUrl"] | "";
  int expiresSec = initDoc["expiresInSec"] | 600;
  if (deviceId.isEmpty() || code.isEmpty()) { setErr("Pairing service error."); return false; }
  if (claimUrl.isEmpty()) claimUrl = String("https://") + host + "/pair";
  setState(State::Pairing);
  setPairing(code.c_str(), claimUrl.c_str());
  setErr("");
  // Do not log the code value: it is a claim credential (whoever has it can claim the
  // device). It reaches the owner through setPairing() (the screen / Sign-in QR), not the
  // diagnostic log, which is served over /api/log and echoed to USB serial (CUM-281).
  agent::alog("relay: pairing code issued (shown on device)");

  // Poll for the claim.
  uint32_t deadline = millis() + (uint32_t)expiresSec * 1000;
  JsonDocument pollReq;
  pollReq["deviceId"] = deviceId;
  pollReq["code"] = code;
  String pollBody;
  serializeJson(pollReq, pollBody);
  int interval = 3000;
  while (millis() < deadline) {
    if (g_reqUnpair || g_reqOptIn == 0) { setPairing("", ""); return false; }
    vTaskDelay(pdMS_TO_TICKS(interval));
    interval = 5000;
    String pr;
    int ps = httpsPostJson(host, "/pair/poll", pollBody, pr);
    if (ps != 200) continue;
    JsonDocument pd;
    if (deserializeJson(pd, pr)) continue;
    const char* status = pd["status"] | "";
    if (strcmp(status, "claimed") == 0) return persistClaimed(pd, host, deviceId);
    if (strcmp(status, "expired") == 0) {
      setErr("Code expired. Start pairing again when ready.");
      setPairing("", "");
      return false;
    }
  }
  setErr("Code expired. Start pairing again when ready.");
  setPairing("", "");
  return false;
}

// --- connect + online pump ---------------------------------------------------

// Map a `bye` reason to a WS close code so close-code policy applies even when the TCP
// drops without a following WS Close frame.
uint16_t byeToCloseCode(const char* reason) {
  if (strcmp(reason, "unpaired") == 0) return (uint16_t)CloseCode::Unpaired;
  if (strcmp(reason, "revoked") == 0) return (uint16_t)CloseCode::EntitlementRevoked;
  if (strcmp(reason, "superseded") == 0) return (uint16_t)CloseCode::Superseded;
  return 4000;
}

// Handle a Welcome (the relay's hello-ack). Returns false to END the session:
// CUM-182 presence is hello-ack-gated AND identity-bound, so a Welcome that
// echoes a DIFFERENT device id (wrong endpoint / spoof) is refused rather than
// latched as a false "connected". A matching id (or a legacy relay that echoes
// none) goes online.
bool handleWelcome(const RelayFrame& f, bool& welcomed, uint16_t& closeOut,
                   const char* ourDeviceId) {
  if (evaluateHelloAck(f.deviceId, ourDeviceId) != AckResult::Accept) {
    agent::alogf("relay: hello-ack MISMATCH got=%s want=%s - refusing online",
                 f.deviceId ? f.deviceId : "", ourDeviceId ? ourDeviceId : "");
    setErr("Cloud identity mismatch.");
    closeOut = (uint16_t)CloseCode::ProtocolError;
    return false;
  }
  welcomed = true;
  // Clamp the relay-supplied heartbeat to a sane band: a huge value would push the
  // next ping to the far future and overflow the silence watchdog, leaving the
  // device stuck "online" on a dead/half-open socket.
  g_heartbeatMs = f.heartbeatMs < kHeartbeatMinMs   ? kHeartbeatMinMs
                  : f.heartbeatMs > kHeartbeatMaxMs ? kHeartbeatMaxMs
                                                    : f.heartbeatMs;
  agent::alogf("relay: hello-ack id=%s hb=%ums",
               (f.deviceId && f.deviceId[0]) ? f.deviceId : "(none)", (unsigned)g_heartbeatMs);
  setOnline(true, "hello-ack");
  setErr("");
  return true;
}

// Handle one parsed WS message (it arrived at `atMs`). Returns false to end the
// session (g_on.closeCode set). `gotPong` is set true when a matched heartbeat pong
// arrives (CUM-191 steady-state liveness) so the caller can clear its missed-ack count.
bool handleFrame(const ws::Message& m, uint32_t atMs, bool& gotPong) {
  if (m.op == ws::Opcode::Text) {
    JsonDocument doc(&agent::PsramJsonAllocator::instance());
    if (deserializeJson(doc, m.payload.data(), m.payload.size())) return true;
    RelayFrame f;
    if (!parseRelayFrame(doc, f)) return true;
    if (f.type == FrameType::Welcome) {
      if (!handleWelcome(f, g_on.welcomed, g_on.closeCode, g_on.deviceId)) return false;
    } else if (f.type == FrameType::Req) {
      handleReq(f.req, atMs);
    } else if (handleUploadFrame(f)) {
      // an ubegin/uchunk/uend/uabort was dispatched
    } else if (f.type == FrameType::Pong) {
      gotPong = true;  // CUM-191: matched heartbeat ack - relay->device direction is live
    } else if (f.type == FrameType::Bye) {
      agent::alog((String("relay: bye ") + f.byeReason).c_str());
      g_on.closeCode = byeToCloseCode(f.byeReason);
      return false;
    }
  } else if (m.op == ws::Opcode::Ping) {
    wsSendSmall(ws::Opcode::Pong, m.payload.data(), m.payload.size());
  } else if (m.op == ws::Opcode::Close) {
    g_on.closeCode = m.closeCode ? m.closeCode : 4000;
    return false;
  }
  return true;
}

// Queue a data frame that arrived while a big answer waited for the slot.
bool deferMessage(ws::Message& m) {
  if (g_defCount >= kMaxDeferred) return false;
  DeferredMsg& d = g_deferred[(g_defHead + g_defCount) % kMaxDeferred];
  d.m = std::move(m);
  d.atMs = millis();
  ++g_defCount;
  return true;
}

// The oldest deferred frame, if any (they are older than anything still in the parser).
bool popDeferred(ws::Message& out, uint32_t& atMs) {
  if (!g_defCount) return false;
  DeferredMsg& d = g_deferred[g_defHead];
  out = std::move(d.m);
  d.m.payload.clear();
  atMs = d.atMs;
  g_defHead = (g_defHead + 1) % kMaxDeferred;
  --g_defCount;
  return true;
}

// Drain all ready inbound messages, deferred ones first. Returns false once a message
// (or a broken write) ends the session. Sets `gotPong` if any drained frame was a
// matched heartbeat pong (CUM-191).
bool pumpInbound(ws::Parser& parser, bool& gotPong) {
  ws::Message m;
  for (;;) {
    uint32_t atMs = millis();
    if (!popDeferred(m, atMs) && !parser.next(m)) return true;
    if (!handleFrame(m, atMs, gotPong) || g_on.ended) return false;
  }
}

// Send a heartbeat ping carrying the current millis.
void sendPing(uint32_t now) {
  JsonDocument pd;
  buildPing(pd, (int64_t)now);
  std::string s;
  serializeJson(pd, s);
  wsSendSmall(ws::Opcode::Text, (const uint8_t*)s.data(), s.size());
}

// Send the WS Upgrade handshake and validate the 101 response; seed `parser` with any
// bytes that arrived after the header. Returns false on handshake failure. g_ws must be
// connected. The response head is capped so a hostile relay can't grow it unbounded.
bool wsUpgrade(const String& host, ws::Parser& parser) {
  uint8_t keyBytes[16];
  for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)esp_random();
  std::string keyB64;
  b64Encode(keyBytes, 16, keyB64);
  std::string upgrade = ws::buildUpgradeRequest(host.c_str(), kWsPath, keyB64);
  g_ws->write(reinterpret_cast<const uint8_t*>(upgrade.data()), upgrade.size());

  std::string head;
  uint32_t deadline = millis() + kConnectTimeoutMs;
  uint8_t buf[512];
  while (millis() < deadline && head.find("\r\n\r\n") == std::string::npos &&
         head.size() < kMaxHandshakeHead) {
    int n = readSome(*g_ws, buf, sizeof(buf), 200);
    if (n > 0) head.append((const char*)buf, n);
    else if (n < 0) break;
  }
  // CUM-182 instrumentation: log the WS upgrade HTTP status + validity. A stale
  // or wrong Cloudflare-fronted endpoint typically answers non-101 here (or a
  // 101 with a bad Sec-WebSocket-Accept), which is the real cause of the field
  // "Cloud handshake failed" oscillation - now visible in /api/log.
  int httpStatus = -1;
  if (head.compare(0, 5, "HTTP/") == 0) {
    size_t sp = head.find(' ');
    if (sp != std::string::npos) httpStatus = atoi(head.c_str() + sp + 1);
  }
  size_t hend = head.find("\r\n\r\n");
  const bool valid = (hend != std::string::npos) &&
                     ws::validateUpgradeResponse(head.substr(0, hend + 4), keyB64);
  agent::alogf("relay: ws upgrade http=%d %s (head=%uB)", httpStatus,
               valid ? "ok" : "FAIL", (unsigned)head.size());
  if (!valid) {
    return false;
  }
  // Any bytes past the handshake are the first WS frames.
  if (head.size() > hend + 4)
    parser.feed(reinterpret_cast<const uint8_t*>(head.data() + hend + 4), head.size() - hend - 4);
  return true;
}

// Send the `hello` frame (deviceId + credential). Returns false if the write failed.
bool sendHello(const String& deviceId, const String& cred) {
  JsonDocument doc;
  buildHello(doc, deviceId.c_str(), cred.c_str(), NIMBUS_FW_VERSION);
  std::string s;
  serializeJson(doc, s);
  return wsSendSmall(ws::Opcode::Text, (const uint8_t*)s.data(), s.size());
}

// A locally-initiated stop: opt-out/unpair staged, WiFi gone, the socket dropped, or a
// proactive re-mint just came due while idle (CUM-52: we can't open a 2nd TLS to
// re-mint, so we end the idle session, re-mint between sessions, then reconnect). Any
// of these ends the session cleanly (close code 0). This is only ever reached between
// tunneled requests, so it never fires mid-turn.
bool sessionShouldStop() {
  if (g_reqOptIn == 0 || g_reqUnpair || !wifiReady() || !g_ws->connected()) return true;
  if (g_remintAtEpoch) {
    const uint64_t now = (uint64_t)time(nullptr);
    // Recycle to re-mint only when the window is open AND we are not in a post-failure
    // backoff - otherwise a transient re-mint failure (target still past) would recycle
    // every reconnect and storm the link while the old credential is still valid.
    if (now > 1000000000ULL && now >= g_remintAtEpoch && now >= g_remintBackoffUntil) return true;
  }
  return false;
}

// Drive the steady-state heartbeat once welcomed (CUM-191): arm on the first call, send a
// ping when one is due, and report whether too many acks have gone unanswered - a
// half-open link the caller must tear down and redial. Kept a separate helper so the
// online loop stays under the complexity gate.
bool driveHeartbeat(nimbus::cloud::HeartbeatLiveness& live, bool& armed, uint32_t now) {
  if (!armed) {
    live.reset(now, g_heartbeatMs * 4 / 5);
    armed = true;
  }
  if (!live.duePing(now)) return false;
  sendPing(now);
  live.notePingSent(now);
  if (!live.dead()) return false;
  agent::alogf("relay: heartbeat ack timeout (%u unacked) - link half-open, redialing",
               (unsigned)live.outstanding);
  return true;
}

// One message during a big answer's slot wait: control frames now, data frames
// deferred (or, when the queue is full, a request answered busy at once). No frame is
// in progress during the wait, so a small frame may go out. False ends the session.
bool pumpOneWhileWaiting(ws::Message& m) {
  if (m.op == ws::Opcode::Ping) {
    wsSendSmall(ws::Opcode::Pong, m.payload.data(), m.payload.size());
    return true;
  }
  if (m.op == ws::Opcode::Close) {
    g_on.closeCode = m.closeCode ? m.closeCode : 4000;
    return false;
  }
  if (m.op != ws::Opcode::Text) return true;
  JsonDocument doc(&agent::PsramJsonAllocator::instance());
  RelayFrame f;
  if (deserializeJson(doc, m.payload.data(), m.payload.size()) || !parseRelayFrame(doc, f)) return true;
  if (f.type == FrameType::Pong) {
    g_on.live.notePong();
    return true;
  }
  if (f.type == FrameType::Bye) {
    agent::alog((String("relay: bye ") + f.byeReason).c_str());
    g_on.closeCode = byeToCloseCode(f.byeReason);
    return false;
  }
  if (deferMessage(m)) return true;
  if (f.type == FrameType::Req) emitBusy(f.req.id, nimbus::cloud::kBusyRetrySlotS);
  else if (f.upload.id && f.upload.id[0]) emitUploadError(f.upload.id, 503, nimbus::cloud::kBusyBody);
  return true;
}

// One pass of the WebSocket while a big answer waits for the TLS work slot: read
// what arrived, answer pings and acks, keep the heartbeat going (the relay drops a
// socket that misses a pong for one 30 s sweep). False once the session is over.
bool pumpWhileWaiting() {
  if (!g_on.parser || g_on.ended || !g_ws || !g_ws->connected()) return false;
  uint8_t buf[512];
  const int n = readSome(*g_ws, buf, sizeof(buf), 5);
  if (n < 0) { g_on.ended = true; return false; }
  if (n > 0) {
    g_on.parser->feed(buf, (size_t)n);
    g_on.lastInbound = millis();
  }
  if (g_on.parser->protocolError()) g_on.closeCode = (uint16_t)CloseCode::ProtocolError;
  ws::Message m;
  while (!g_on.parser->protocolError() && g_on.parser->next(m))
    if (!pumpOneWhileWaiting(m)) break;
  if (g_on.closeCode || g_on.parser->protocolError() ||
      (g_on.welcomed && driveHeartbeat(g_on.live, g_on.armed, millis()))) {
    g_on.ended = true;
    return false;
  }
  return true;
}

// The online read/pump/heartbeat loop, after the handshake + hello. Returns the WS close
// code (0 = clean/local drop).
uint16_t runOnlineLoop(ws::Parser& parser, const char* ourDeviceId) {
  uint32_t welcomeBy = millis() + kWelcomeDeadlineMs;
  g_heartbeatMs = kDefaultHeartbeatMs;
  // CUM-191: steady-state liveness. Ping cadence is 4/5 of the heartbeat window so a
  // pong has room to arrive before the next ping falls due; the ack-gated monitor
  // (armed once the Welcome lands) trips after kMaxMissedHeartbeatAcks unacked pings.
  // The state lives in g_on so a big answer's slot wait keeps it going (heap F2).
  g_on = OnlineSession{};
  g_on.parser = &parser;
  g_on.deviceId = ourDeviceId;
  g_on.lastInbound = millis();
  uint8_t buf[512];
  while (true) {
    if (sessionShouldStop()) { g_on.closeCode = 0; break; }
    int n = readSome(*g_ws, buf, sizeof(buf), 100);
    if (n > 0) {
      parser.feed(buf, (size_t)n);
      g_on.lastInbound = millis();
    } else if (n < 0) {
      g_on.closeCode = 0;
      break;
    }
    if (parser.protocolError()) { g_on.closeCode = (uint16_t)CloseCode::ProtocolError; break; }
    bool gotPong = false;
    if (!pumpInbound(parser, gotPong)) break;
    if (gotPong) g_on.live.notePong();  // matched ack clears the missed-heartbeat count
    if (!g_on.welcomed && millis() > welcomeBy) { setErr("Cloud handshake timed out."); break; }
    uint32_t now = millis();
    // Half-open link (too many unacked heartbeats): drop presence honestly and let the
    // task loop redial with backoff, re-registering a live session - the reboot-free
    // recovery CUM-191 requires. Not an error to the user.
    if (g_on.welcomed && driveHeartbeat(g_on.live, g_on.armed, now)) { g_on.closeCode = 0; break; }
    // Coarse backstop: no inbound bytes at all for 2 heartbeat windows (the ack path
    // above trips first on a half-open link, but this catches a wedged read).
    if (g_on.welcomed && now - g_on.lastInbound > g_heartbeatMs * 2) { g_on.closeCode = 0; break; }
  }
  const uint16_t closeCode = g_on.closeCode;
  // Deferred frames die with the session (the cloud fails those requests and retries).
  ws::Message drop;
  uint32_t at = 0;
  while (popDeferred(drop, at)) {
  }
  g_on.parser = nullptr;
  return closeCode;
}

// Holds the TLS work slot for the dial's scope; released on every path out.
struct DialSlot {
  bool held = false;
  ~DialSlot() { release(); }
  void release() {
    if (held) agent::arbiter::releaseWork();
    held = false;
  }
};

enum class Dial : uint8_t { Up, Failed, SlotBusy };

// The dial: TLS connect, WebSocket upgrade and hello, inside the TLS work slot (heap
// F2), like pairing. The handshake is the session's costliest internal-SRAM moment,
// and a dial beside a turn's exchange is the second concurrent TLS session the device
// forbids (on 14 Oct a redial ran its handshake during the next turn's round).
Dial dialSession(const String& host, const String& deviceId, const String& cred, ws::Parser& parser) {
  DialSlot slot;
  if (!relayNoSlot()) {
    if (!agent::arbiter::acquireWork(nimbus::cloud::kRelayDialSlotMs)) {
      agent::alog("relay: dial waits (TLS work slot busy)");
      return Dial::SlotBusy;
    }
    slot.held = true;
  }
  nimbus::heapwatch::Scope dialing(nimbus::heapwatch::kRelayDial);
  struct TlsMark {   // the dial's TLS I/O, for the concurrent-TLS detector
    TlsMark() { nimbus::heapwatch::tlsIoBegin("relay.dial"); }
    ~TlsMark() { nimbus::heapwatch::tlsIoEnd(); }
  } mark;
  // CUM-182 instrumentation: log the resolved dial target (host:port + WS path)
  // before every TLS connect so /api/log shows exactly WHERE the device dials
  // (catches a stale/wrong NVS relay host). deviceId is not a secret (it rides
  // the public tunnel URL); the credential never appears in any log.
  agent::alogf("relay: dial host=%s:%u path=%s id=%s", host.c_str(), (unsigned)kTlsPort,
               kWsPath, deviceId.c_str());
  const uint32_t tlsT0 = millis();
  const bool tlsOk = g_ws->connect(host.c_str(), kTlsPort, kConnectTimeoutMs);
  const uint32_t tlsMs = millis() - tlsT0;
  agent::alogf("relay: tls %s in %ums", tlsOk ? "ok" : "FAIL", tlsMs);
  if (!tlsOk) {
    setErr("Couldn't reach the cloud. Retrying.");
    return Dial::Failed;
  }
  if (!wsUpgrade(host, parser)) {
    setErr("Cloud handshake failed.");
    tlsClose(*g_ws);
    return Dial::Failed;
  }
  if (!sendHello(deviceId, cred)) {
    agent::alog("relay: hello write FAIL");
    tlsClose(*g_ws);
    return Dial::Failed;
  }
  agent::alogf("relay: hello sent id=%s fw=%s", deviceId.c_str(), NIMBUS_FW_VERSION);
  return Dial::Up;
}

// Returns a WS close code (0 = clean/local drop) after the session ends, or
// kSlotBusyClose when the dial could not get the TLS work slot.
uint16_t runSession() {
  String host = agent::store::cloudHost();
  String deviceId = agent::store::cloudDeviceId();
  String cred = agent::store::cloudCred();
  if (deviceId.isEmpty() || cred.isEmpty()) return 0;

  // CUM-52: cache this credential's jittered re-mint target so the idle online loop can
  // recycle the session the moment the window opens (0 => legacy/no-exp, never recycles).
  const std::string credStd(cred.c_str());
  g_remintAtEpoch = nimbus::cloud::remintTarget(nimbus::cloud::credentialIat(credStd),
                                                nimbus::cloud::credentialExp(credStd),
                                                remintSeed());

  setState(State::Connecting);
  if (!g_ws) g_ws = new WiFiClientSecure();
  relayTlsSetup(*g_ws);
  // Inbound frames (relay -> device `req`) are small browser requests; cap the parser
  // buffers low so a malicious oversized frame can't OOM the scarce internal SRAM.
  ws::Parser parser(kMaxInboundFrame);
  const Dial dial = dialSession(host, deviceId, cred, parser);
  if (dial == Dial::SlotBusy) return kSlotBusyClose;
  if (dial == Dial::Failed) return 0;

  uint16_t closeCode = runOnlineLoop(parser, deviceId.c_str());
  resetUpload();  // drop any in-flight upload's loopback socket with the session
  tlsClose(*g_ws);
  setOnline(false, "session-end");
  return closeCode;
}

// Drain staged opt-in/pair/unpair control at the top of the task loop. Unpair runs
// BEFORE the opt-out park: an "optout" stages BOTH unpair and optIn=false, so draining
// unpair first wipes the cloud credential (per store.h's "wiped on unpair" contract)
// before we park - and leaves no stale unpair to fire a surprise drop on the next
// re-enable.
void drainStagedControl() {
  if (g_reqUnpair) {
    agent::store::clearCloudPairing();
    g_reqUnpair = false;
    setState(State::Idle);
    setErr("Unpaired. Pair again when ready.");
    agent::alog("relay: unpaired");
  }
  if (g_reqOptIn == 0) {
    agent::store::setCloudOptIn(false);
    g_reqOptIn = -1;
    setState(State::Disabled);
    agent::alog("relay: disabled");
    // Park until re-enabled.
    while (g_reqOptIn != 1 && !agent::store::cloudOptIn()) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (g_reqOptIn == 1) { agent::store::setCloudOptIn(true); g_reqOptIn = -1; }
}

// Apply the close-code policy after a session ends: state, user message, and the sleep
// each close code dictates. `backoff`/`badToken` carry across reconnects.
void applyCloseCodePolicy(uint16_t code, uint32_t& backoff, uint32_t& badToken) {
  if (code == (uint16_t)CloseCode::Unpaired) {
    agent::store::clearCloudPairing();
    setState(State::Idle);
    setErr("This device was unpaired. Pair again when ready.");
    backoff = 500;
    return;
  }
  if (code == (uint16_t)CloseCode::BadToken) {
    // CUM-52: the relay rejected our credential (expired or rotated). Try to recover
    // by re-minting (an expired-within-grace token) or re-pairing, rather than idling
    // 5 min. A SUCCESSFUL re-mint is progress and must not count as a strike, or a
    // legit re-mint would march the device to Disabled (review finding).
    const BadTokenOutcome o = recoverFromBadToken();
    if (o == BadTokenOutcome::Repaired)  { badToken = 0; backoff = 500; return; }
    if (o == BadTokenOutcome::Refreshed) { badToken = 0; vTaskDelay(pdMS_TO_TICKS(2000)); return; }
    // Retry (transient): count a strike; back off hard only after repeated failures.
    if (++badToken >= 3) { setState(State::Disabled); setErr("Sign-in expired. Pair again when ready."); vTaskDelay(pdMS_TO_TICKS(300000)); return; }
    vTaskDelay(pdMS_TO_TICKS(2000));
    return;
  }
  badToken = 0;
  if (code == (uint16_t)CloseCode::EntitlementRevoked) {
    setState(State::Backoff);
    setErr("Cloud subscription inactive. The device still works on your network.");
    vTaskDelay(pdMS_TO_TICKS(1800000));
    return;
  }
  if (code == (uint16_t)CloseCode::Superseded) {
    setState(State::Backoff);
    vTaskDelay(pdMS_TO_TICKS(60000 + (esp_random() % 60000)));
    return;
  }

  // Ordinary drop: exponential backoff.
  setState(State::Backoff);
  vTaskDelay(pdMS_TO_TICKS(backoff));
  backoff = backoff < 15000 ? backoff * 2 : 15000;
}

// --- the task ---------------------------------------------------------------
void relayTask(void*) {
  // Let WiFi + the association RX burst settle before the first TLS (tg_poll lesson).
  while (!wifiReady()) vTaskDelay(pdMS_TO_TICKS(500));
  const uint32_t wifiAt = millis();
  vTaskDelay(pdMS_TO_TICKS(3000));
  // The first dial waits for Telegram's first poll (or 20 s): the two boot handshakes
  // started within 30 ms of each other at every boot (15 of 15 in the field logs).
  while (!nimbus::cloud::firstDialReady(agent::telegram::pollEverOk(), millis() - wifiAt))
    vTaskDelay(pdMS_TO_TICKS(250));

  uint32_t backoff = 500;
  uint32_t badToken = 0;

  for (;;) {
    drainStagedControl();

    if (!agent::store::cloudOptIn()) { setState(State::Disabled); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (!wifiReady()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    if (!heapFloorOk()) {
      // CUM-167: refuse to dial only while genuinely starved, and RE-CHECK often so the
      // relay comes online promptly once internal SRAM frees (the old 60 s latch left it
      // "disabled" long after recovery). The next pass clears this error when it proceeds
      // to dial. Log the live numbers so the bench sees the actual free/largest.
      setState(State::Disabled);
      setErr("Not enough memory right now.");
      agent::alogf("relay: heap floor - free=%u largest=%u (need free>=%u largest>=%u); retry 10s",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)nimbus::cloud::kRelayHeapFloorFree,
                   (unsigned)nimbus::cloud::kRelayHeapFloorLargest);
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }

    // Pairing requested (or not yet paired but explicitly asked).
    if (g_reqPair) {
      g_reqPair = false;
      if (runPairing()) { backoff = 500; }
      continue;
    }
    if (!agent::store::cloudPaired()) { setState(State::Idle); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

    // CUM-52: proactive/within-grace re-mint runs here, while the WSS is DOWN (single
    // TLS slot). If the credential is past grace, maintainCredential re-pairs and we
    // loop back without connecting.
    if (!maintainCredential()) continue;

    uint16_t code = runSession();
    if (code == kSlotBusyClose) {   // the dial waits its turn: retry soon, no owner-visible change
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    applyCloseCodePolicy(code, backoff, badToken);
  }
}

}  // namespace

void begin() {
  if (g_task) return;
  // 16 KB: a tunneled-request handler (loopback read + ArduinoJson serialize + TLS
  // write) is deeper than the connect path; 12 KB overflowed while a req was in flight
  // (with the loopback read buffer now off-stack, 16 KB gives ~5 KB peak headroom).
  xTaskCreatePinnedToCore(relayTask, "relay", 16384, nullptr, 2, &g_task, 0);
}
bool isRunning() { return g_task != nullptr; }

void requestOptIn(bool on) {
  agent::store::setCloudOptIn(on);
  g_reqOptIn = on ? 1 : 0;
}
void requestPair() { g_reqPair = true; }
void requestUnpair() { g_reqUnpair = true; }

State state() {
  portENTER_CRITICAL(&g_mux);
  State s = g_state;
  portEXIT_CRITICAL(&g_mux);
  return s;
}
const char* stateName() {
  switch (state()) {
    case State::Disabled: return "disabled";
    case State::Idle: return "idle";
    case State::Pairing: return "pairing";
    case State::Connecting: return "connecting";
    case State::Online: return "online";
    case State::Backoff: return "backoff";
  }
  return "?";
}
bool online() {
  portENTER_CRITICAL(&g_mux);
  bool o = g_online;
  portEXIT_CRITICAL(&g_mux);
  return o;
}
bool pairingActive() { return state() == State::Pairing; }
String claimCode() {
  portENTER_CRITICAL(&g_mux);
  String c = g_code;
  portEXIT_CRITICAL(&g_mux);
  return c;
}
String claimUrl() {
  portENTER_CRITICAL(&g_mux);
  String u = g_url;
  portEXIT_CRITICAL(&g_mux);
  return u;
}
String statusLine() {
  portENTER_CRITICAL(&g_mux);
  String err = g_err;
  portEXIT_CRITICAL(&g_mux);
  if (online()) return "Connected to the cloud.";
  if (err.length()) return err;
  switch (state()) {
    case State::Disabled: return agent::store::cloudOptIn() ? "Starting up." : "Cloud access is off.";
    case State::Idle: return "Ready to pair.";
    case State::Pairing: return "Waiting for you to claim the code.";
    case State::Connecting: return "Connecting to the cloud.";
    case State::Backoff: return "Reconnecting to the cloud.";
    default: return "";
  }
}
void statusInto(JsonObject d) {
  portENTER_CRITICAL(&g_mux);
  String code = g_code, url = g_url, err = g_err;
  portEXIT_CRITICAL(&g_mux);
  d["optIn"] = agent::store::cloudOptIn();
  d["paired"] = agent::store::cloudPaired();
  d["state"] = stateName();
  d["online"] = online();
  d["host"] = agent::store::cloudHost();
  String name = agent::store::cloudName();
  if (name.length()) d["name"] = name;
  if (code.length()) d["code"] = code;
  if (url.length()) d["claimUrl"] = url;
  if (err.length()) d["err"] = err;
  d["line"] = statusLine();
  d["stackMin"] = stackMinFree();
}
void statusJson(String& out) {
  JsonDocument d;
  statusInto(d.to<JsonObject>());
  serializeJson(d, out);
}
int stackMinFree() { return g_task ? (int)uxTaskGetStackHighWaterMark(g_task) : -1; }

int loopbackSelfTest(const String& path, String& out) {
  http_replay::Headers hdrs;
  http_replay::ResponseParser rp(kMaxRespBody);
  bool ok = doLoopback("GET", path.c_str(), hdrs, nullptr, 0, rp);
  if (!ok) { out = "loopback failed"; return -1; }
  const auto& b = rp.body();
  size_t n = b.size() < 160 ? b.size() : 160;
  out = "";
  for (size_t i = 0; i < n; i++) out += (char)b[i];
  return rp.status();
}

}  // namespace relay
}  // namespace nimbus
