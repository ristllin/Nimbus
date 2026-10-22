#include "provider_verify.h"

#include <WiFi.h>                 // WiFi.status()/WL_CONNECTED - retry pump gate
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "../sys/ps_json.h"       // PsramJsonAllocator - Mistral model-metadata parse off internal heap
#include "nimbus/orch/model_catalog.h"  // portable role/capability catalog (GET /api/models)
#include "nimbus/orch/fallback_rules.h" // portable fallback rule parse (Cumulo cloud sync)
#include "router_catalog.h"             // CUM-530: the Cumulo router list rule (host-tested)
#include "version.h"                    // NIMBUS_FW_VERSION - the model store header
#include <time.h>

#include "esp_heap_caps.h"
#include <atomic>                 // g_pending / probe-queue flags touched by three tasks (CUM-537)
#include <memory>                 // unique_ptr - heap-allocate the connectors Info[] (no big stack array)
#include <new>                    // std::nothrow
#include <solide/memory.h>        // NVS side channel for the verify-reason token
#include "agent_config.h"
#include "../sys/agent_log.h"
#include "../sys/config_nvs.h"    // nvsGetStr - a quiet read of the unset verify reason
#include "../sys/net_util.h"      // tlsClose - RST-close on every path
#include "store.h"
#include "connectors.h"           // Mistral Studio connector workspace-auth probe
#include "../sys/heap_watch.h"    // resting largest block, the worker spawn gate, activity tags
#include "../sys/tls_arbiter.h"   // one work TLS at a time (coexists with Telegram's)
#include "orchestrator.h"         // turnInFlight - never re-verify beside a live turn
#include "provider_verify_retry.h" // pure backoff policy (host-tested)
#include "workspace_probe.h"       // CUM-537 probe queue + yielding fetch (host-tested)

namespace agent {
namespace provider_verify {

// One-slot queue. g_pending is the handoff flag between the starters (the web
// task's request(), the main loop's pumpRetry()) and the verify task (run). A
// starter CLAIMS it with compare_exchange before writing g_provider/g_wsOnly, so
// two starters can never both create a task (CUM-537 made the pump a regular
// starter); the task reads them only after it was created, and clears it last.
static std::atomic<bool> g_pending{false};
static char              g_provider[16] = {};

// Largest-contiguous-INTERNAL-block floor below which we DON'T attempt the handshake.
// mbedTLS's big RX/TX content buffers (~16 KB each) are now routed to PSRAM (main.cpp
// installs a PSRAM-backed mbedTLS allocator), so the handshake's remaining INTERNAL
// need is much smaller than the pre-PSRAM 50 KB - mostly lwIP/socket + cert-parse
// transients. Gated on the largest CONTIGUOUS internal block (not total free) because
// the handshake still wants one modest contiguous buffer.
//
// 8 KB (CUM-77 x1 item 6, was 16 KB, was 30 KB). The 30 KB floor perpetually
// deferred (proven on-device: max8=19444 -> deferred); 16 KB STILL did. Measured
// live on .61 (v4.4.8): the largest CONTIGUOUS internal block is heavily
// fragmentation-capped and swings ~11.7-25 KB (intFree stays ~34 KB the whole
// time), and the web UI's verify keeps landing in the low end - so at 16 KB EVERY
// provider deferred in one window ("mistral/zai/openai/cumulo deferred
// (max8=15348)"; later max8=11764), while anthropic, which happened to run at a
// higher moment, verified fine. That is the uniform "couldn't verify - retry" the
// owner saw: NOT a per-provider fault, just the gate parked above the device's own
// fragmented floor. The REAL contiguous need is smaller now that mbedTLS RX/TX ride
// PSRAM, but it has NOT been measured directly. Do NOT cite the 6,536 B from
// platformio.ini's AsyncTCP note as that peak: that figure is a service-task STACK
// high-water (surfaced as mem.asyncStackMin), not a contiguous heap block, so it
// cannot size this gate. 8 KB is therefore justified EMPIRICALLY, not analytically:
// it sits below the device's observed fragmented floor swing where 16 KB did not,
// and on-device all four direct verifies go green at it, while a genuine severe OOM
// is still refused (records -1 "low-memory"; a real OOM past the gate fails soft).
// TODO(CUM-387): measure the true contiguous high-water of a live TLS embed on
// v4.5.0 (PSRAM staging in) and restate this floor against that number; the same
// measurement settles relay_heap.h's unmeasured "below 8000 lwIP genuinely cannot
// run" claim and its stated agreement with this gate (the two gate different
// things: here largest>=8000, there largest>=5000 plus free>=8000).
// docs/memory-model.md.
static const size_t VERIFY_MIN_MAX8 = 8000;

uint32_t deferFloor() { return (uint32_t)VERIFY_MIN_MAX8; }

// The worker's stack: internal SRAM, carved out when the task is created. The memory
// gate therefore runs BEFORE xTaskCreate (row 43, panic F2.4): measured inside the new
// task it read 7,156 B while the device rested at 15,860 B - the stack had taken the
// block - and a loaded device deferred every verify for good.
static const uint32_t kVerifyStackBytes = 8192;

static void verifyTask(void*);   // spawned per request(); self-deletes

// ---- verify-reason side channel (CUM-77 x1 §4) ------------------------------
// A short machine token explaining the LAST verify's non-verified outcome, read
// by the web UI badge as `vfyReason`. Persisted in its OWN NVS key (vrs_<prov>)
// so the "R:TS" verify slot in store.cpp is untouched (that file is another
// lane's). See the contract in lanes/L1/PROGRESS.md.
static String reasonKey(const String& provider) {
  return String(AKEY_VERIFY_REASON_PFX) + provider;
}
// A rebuildable side channel: asked of the NVS reserve and reported (lane C).
static void setReason(const String& provider, const char* rsn) {
  const String k = reasonKey(provider);
  const String v = rsn ? rsn : "";
  if (!v.length()) {   // "no reason" reads the same as an absent key: an erase
    if (solide::memory::has(k.c_str()))
      nimbus::sys::notePersist(solide::memory::eraseKey(k.c_str()), k.c_str());
    return;
  }
  const bool admitted = nimbus::sys::nvsAdmit(nimbus::sys::kNvsNsSolide, k.c_str(),
                                              nimbus::store::NvsValKind::Str, v.length());
  nimbus::sys::notePersist(admitted && solide::memory::setString(k.c_str(), v), k.c_str());
}
String reason(const String& provider) {
  return nimbus::sys::nvsGetStr(reasonKey(provider).c_str());   // unset until a verify
}

// ---- low-memory deferral: measured number + self-retry (CUM-447) -------------
// The measured largest-contiguous-internal block at the last deferral, persisted
// in its OWN NVS key (vrm_<prov>, <=15 chars) so the pill's hover can show the real
// number after a reboot too. Only read when reason(prov)=="low-memory", so a stale
// value on a since-verified provider is never surfaced.
static const char* const kVerifyMax8Pfx = "vrm_";
static String max8Key(const String& provider) { return String(kVerifyMax8Pfx) + provider; }
static void setDeferMax8(const String& provider, uint32_t v) {
  const String k = max8Key(provider);
  const bool admitted = nimbus::sys::nvsAdmit(nimbus::sys::kNvsNsSolide, k.c_str(),
                                              nimbus::store::NvsValKind::Int, 0);
  nimbus::sys::notePersist(admitted && solide::memory::setInt(k.c_str(), (int32_t)v), k.c_str());
}
uint32_t deferMax8(const String& provider) {
  return (uint32_t)solide::memory::getInt(max8Key(provider).c_str(), 0);
}

// One backoff clock per provider that can defer. A fixed table (no reallocation)
// keeps the RAM state stable across the two tasks that touch it: the verify task
// writes it (onDeferred/clear), the main-loop pump reads it (due). Providers absent
// here simply never auto-retry (none defer that aren't listed).
static const char* const kRetryProv[] = {"openai", "anthropic", "mistral", "zai",
                                         "cumulo", "telegram", "tavily"};
static const int kNRetry = (int)(sizeof(kRetryProv) / sizeof(kRetryProv[0]));
static DeferralRetry g_retry[kNRetry];
// A verify refused only because a turn was in flight: the pump starts it once the turn
// ends (a key save during a turn kicks a verify that must not be dropped).
static bool g_wantStart[kNRetry] = {};
static int retryIdx(const String& provider) {
  for (int i = 0; i < kNRetry; ++i)
    if (provider == kRetryProv[i]) return i;
  return -1;
}
// A provider stuck deferred this long has cycled the backoff several times without
// clearing: the Memory health row calls it out (past the retry window, item 4).
static const uint32_t kStuckWindowMs = 1200000u;   // 20 min

// CUM-537 workspace-probe queue. g_wsOnly marks the claimed task as a probe-only
// run (written after the claim, like g_provider). g_wsq is the one coalesced
// request, touched by the web task (a save), the main loop (the pump) and the
// verify task (an attempt's outcome); it bounds the retries of a probe that could
// not run so a struggling device is asked again a few times on a slow clock, never
// every pump pass, and no actor can clear a request it did not serve
// (workspace_probe.h).
static bool                g_wsOnly = false;
static WorkspaceProbeQueue g_wsq;
// R5: the claimed task is a model-list refresh only (requestRefresh), written after
// the claim like g_wsOnly.
static bool                g_refreshOnly = false;

static void deferLowMemory(const String& provider, uint32_t max8);

static bool startTask(const char* provider, bool wsOnly, bool refreshOnly = false) {
  bool idle = false;
  if (!g_pending.compare_exchange_strong(idle, true)) return false;  // one verify at a time
  strncpy(g_provider, provider, sizeof(g_provider) - 1);
  g_provider[sizeof(g_provider) - 1] = 0;
  g_wsOnly = wsOnly;
  g_refreshOnly = refreshOnly;
  // Memory before the stack: never while a turn is in flight, only with room for the
  // stack plus 16 KB, and only when the RESTING largest block clears the handshake
  // floor (a single sample inside a transient read 7,156 B on a device resting at
  // 15,860 B). A refusal is never a silent drop: a turn queues the start for the pump,
  // low memory records the deferral and arms the self-retry.
  const nimbus::heapwatch::SpawnVerdict sv = nimbus::heapwatch::spawnGate(kVerifyStackBytes, "verify");
  const uint32_t resting = nimbus::heapwatch::restingLargest();
  const bool lowBlock = deferReason(resting, (uint32_t)VERIFY_MIN_MAX8)[0] != 0;
  if (sv != nimbus::heapwatch::SpawnVerdict::Ok || lowBlock) {
    g_pending = false;
    // The probe queue re-arms itself (rearmWorkspaceProbe); a refresh stays queued in
    // model_refresh and records no deferral (it is not a verify).
    if (wsOnly || refreshOnly) return false;
    const int ri = retryIdx(provider);
    if (sv == nimbus::heapwatch::SpawnVerdict::TurnInFlight) {
      if (ri >= 0) g_wantStart[ri] = true;
    } else {
      deferLowMemory(provider, resting);
    }
    return false;
  }
  // Spawn the worker HERE rather than keeping one resident. A verify happens
  // when a human clicks "verify" in the web UI - a handful of times in a device's
  // life - but the task's 8 KB stack is INTERNAL SRAM held for the whole uptime,
  // and internal SRAM is the resource the turn engine actually runs out of
  // (a multi-round tool loop caps at `cap=heap` when the turn-time heap dips
  // below ORCH_LOOP_MIN_HEAP). Reclaiming these 8 KB is what buys the tool loop
  // its rounds back. The task self-deletes when the verify completes.
  if (xTaskCreate(verifyTask, "pverify", kVerifyStackBytes, nullptr, 1, nullptr) != pdPASS) {
    g_pending = false;   // nothing will run it - fail the request honestly
    alog("verify: could not start worker (low memory)");
    return false;
  }
  return true;
}

bool request(const String& provider) { return startTask(provider.c_str(), false); }

bool requestRefresh(const String& provider) { return startTask(provider.c_str(), false, true); }

bool pending() { return g_pending; }

void queueWorkspaceProbeIfStale(uint32_t delayMs) {
  if (!store::hasMistralKey() || !agent::connectors::mistralWorkspaceStale()) return;
  g_wsq.request((uint32_t)millis(), delayMs);
}

// A queued probe (generation `gen`) that could not run: try again on the slow
// clock, at most kMaxTries attempts (a running turn defers without spending one),
// else drop it - the next save, restart or manual verify asks again. A save made
// since `gen` keeps its own clock.
static void rearmWorkspaceProbe(uint32_t gen, const char* why, bool countsTry) {
  using R = WorkspaceProbeQueue::Rearm;
  const R r = g_wsq.rearm(gen, (uint32_t)millis(), countsTry);
  agent::connectors::noteWorkspaceCheckMiss(why);   // the card shows why (22 Oct honesty)
  alogf("verify: mistral workspace probe did not run (%s)%s", why,
        r == R::Retry ? " - will retry in 2 min" : r == R::Newer ? " - a newer check is queued" : "");
}

bool workspaceProbePending() {
  return g_wsq.queued() || (g_pending && strcmp(g_provider, "mistral") == 0);
}

// Router prices after an upgrade: one Cumulo verify per boot while none is stored
// (armed at boot, fired by pumpRetry; RouterPriceRefill holds the policy).
static RouterPriceRefill g_rpRefill;

void queueRouterPriceRefill(uint32_t delayMs) {
  if (!store::hasCumuloKey()) return;   // the rest is checked when it fires
  g_rpRefill.arm((uint32_t)millis(), delayMs);
}

// The pump's refill step: true when it was due and acted (the pump stops this pass).
static bool pumpRouterPriceRefill(uint32_t now, bool inTurn, bool online) {
  if (!g_rpRefill.due(now, inTurn, online)) return false;
  using Next = RouterPriceRefill::Next;
  const Next next = RouterPriceRefill::decide(
      store::hasCumuloKey(), store::verifyResult("cumulo") == 1, store::routerPriceState());
  if (next == Next::Done) {
    g_rpRefill.done();
  } else if (next == Next::Later || !request("cumulo")) {
    g_rpRefill.startFailed(now);
  } else {
    g_rpRefill.done();
    alog("verify: cumulo router prices missing - verifying once to fetch them");
  }
  return true;
}

// ---- model list fetch (lane F) begin ------------------------------------------------
// Every LLM verify streams the provider's model list into its model store
// (/data/models_<slug>.txt, nimbus/orch/model_catalog.h). Nothing on this path writes
// NVS: the old harvest kept a short list in NVS (mch_<slug>) and logged "harvested"
// even when that write had failed. The body is read in small chunks through
// nimbus::orch::ModelsBodyReader, so the Cumulo router's ~95 KB list (cut at 64 KB
// before, CUM-564) is read whole in bounded memory, one row at a time; Anthropic is
// asked for 100 models a page and its has_more pages are followed. One line always
// says what was read and kept, and why a list was not stored.
static const size_t   kModelBodyMaxBytes = 512 * 1024;   // one page: a ceiling on a hostile body
static const uint32_t kModelBodyMs       = 20000;        // one page's read deadline
static const int      kModelPagesMax     = 4;            // Anthropic: up to 400 models

static void harvestCb(void* ctx, const char* obj, size_t len) {
  nimbus::orch::harvestRow(*static_cast<nimbus::orch::ModelHarvest*>(ctx), obj, len);
}

// What one fetch read, for its log line.
struct ModelFetch {
  size_t bytes = 0;
  int    pages = 0;
};

// Stream one page's body (its status line already read) into `h`. True when the
// provider says more pages follow (`lastId` is then the cursor).
static bool streamModelPage(WiFiClientSecure& client, nimbus::orch::ModelHarvest& h,
                            std::string& lastId, ModelFetch& f) {
  nimbus::orch::ModelsBodyReader rd(&harvestCb, &h);
  char chunk[512];
  const uint32_t deadline = millis() + kModelBodyMs;
  while ((int32_t)(millis() - deadline) < 0 && rd.bytes() < kModelBodyMaxBytes && !rd.complete()) {
    const int n = client.read((uint8_t*)chunk, sizeof(chunk));
    if (n > 0) {
      rd.feed(chunk, (size_t)n);
      continue;
    }
    if (!client.connected() && client.available() == 0) break;   // drained and closed
    delay(2);
  }
  f.bytes += rd.bytes();
  ++f.pages;
  if (!rd.complete()) h.cut = true;
  h.oversize += rd.oversize();
  lastId = rd.lastId();
  return rd.hasMore();
}

// The HTTP status code off a response's first line (0 when none arrived in time).
static int readStatusCode(WiFiClientSecure& client, uint32_t ms) {
  const uint32_t deadline = millis() + ms;
  String status;
  while ((int32_t)(millis() - deadline) < 0) {
    if (client.available()) {
      const char c = client.read();
      if (c == '\n') break;
      if (c != '\r') status += c;
    } else if (!client.connected()) {
      break;
    } else {
      delay(2);
    }
  }
  const int sp = status.indexOf(' ');
  return (sp > 0 && (int)status.length() >= sp + 4) ? status.substring(sp + 1, sp + 4).toInt() : 0;
}

// The next Anthropic page after `after` (a cleaned model id) on its own connection;
// the previous one is closed first (one TLS session at a time).
static bool fetchAnthropicPage(nimbus::orch::ModelHarvest& h, const String& key,
                               const std::string& after, std::string& lastId, ModelFetch& f) {
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  c.setConnectionTimeout(15000);
  bool more = false;
  if (!c.connect(ANTHROPIC_HOST, 443)) {
    h.cut = true;
  } else {
    c.print(String("GET /v1/models?limit=100&after_id=") + after.c_str() +
            " HTTP/1.0\r\nHost: " ANTHROPIC_HOST "\r\nx-api-key: " + key +
            "\r\nanthropic-version: " ANTHROPIC_VER
            "\r\nAccept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n");
    if (readStatusCode(c, 15000) == 200) more = streamModelPage(c, h, lastId, f);
    else h.cut = true;
  }
  tlsClose(c);
  return more;
}

// Router prices for the spend ledger: what the Cumulo router bills for each model this
// device keeps (the short list plus the configured head and sub-agent selectors, at
// most 24 rows), read from the fetched rows. A model with no priced row keeps no router
// price - the next price tier applies, never a wrong one. Stored in a flash file.
static void storeRouterPrices(const std::vector<nimbus::orch::ModelInfo>& rows, const std::string& choices) {
  std::vector<nimbus::orch::ModelRate> prices;
  std::vector<std::string> missing;
  const std::vector<std::string> keep = router_catalog::priceKeepIds(
      choices, std::string(store::orchModel("cumulo").c_str()),
      std::string(store::subModel("cumulo").c_str()));
  const size_t n = router_catalog::routerRates(rows, keep, prices, &missing);
  for (const std::string& id : missing)
    alogf("verify: the cumulo router lists no price for %s - left unset", id.c_str());
  if (!n) return;
  if (store::replaceRouterPrices(prices))
    alogf("verify: cumulo router prices kept for %u models", (unsigned)prices.size());
  else
    alogf("verify: cumulo router prices not stored");
}

// Order, bound and store one fetch's rows. A list cut short in transit never
// replaces a stored one (the last good list stays); an empty one never does.
static void storeModelHarvest(const String& provider, nimbus::orch::ModelHarvest& h, const ModelFetch& f) {
  using namespace nimbus::orch;
  finishHarvest(h);
  const bool router = (provider == "cumulo");
  size_t more = h.overflow + (router ? router_catalog::protectHeads(h.rows, CUMULO_MODEL) : 0);
  more += boundModelRows(h.rows);
  ModelStoreMeta meta;
  meta.fetchedAt = h.now;
  meta.fw = NIMBUS_FW_VERSION;
  meta.total = (uint32_t)(h.rows.size() + more);
  meta.more = (uint32_t)more;
  meta.cut = h.cut || h.hasMore;
  meta.src = "api";
  meta.choices = router ? router_catalog::storeChoices(h.rows, CUMULO_MODEL, h.now)
                        : directChoices(std::string(provider.c_str()), h.rows, h.now);
  alog(harvestLogLine(h, meta, f.bytes, f.pages).c_str());
  if (!harvestStorable(h, !store::modelStoreText(provider).empty())) {
    alogf("verify: %s model list %s - the last list stays", provider.c_str(),
          h.rows.empty() ? "empty" : "incomplete");
    return;
  }
  const std::string text = serializeModelStore(meta, h.rows);
  if (!store::setModelStoreText(provider, text)) {
    alogf("verify: %s model list not stored (file write failed) - the last list stays",
          provider.c_str());
    return;
  }
  alogf("verify: %s models stored: %u (%u B), short list %s", provider.c_str(),
        (unsigned)h.rows.size(), (unsigned)text.size(), meta.choices.c_str());
  if (router) storeRouterPrices(h.rows, meta.choices);
  store::noteUnlistedPicks(provider);   // a stored pick the fresh list dropped: told once
}

// The verify's GET answered 200 on `client`: read its model list (and Anthropic's
// further pages) and store it. Closes `client` (the next page, and the usability
// probe after this, each open their own session).
static void fetchModelList(const String& provider, WiFiClientSecure& client, const String& key) {
  std::unique_ptr<nimbus::orch::ModelHarvest> h(new (std::nothrow) nimbus::orch::ModelHarvest());
  if (!h) {
    alogf("verify: %s model list skipped (low memory)", provider.c_str());
    return;
  }
  h->provider = provider.c_str();
  h->now = nimbus::orch::calendarNowOrZero(time(nullptr));
  h->alloc = &PsramJsonAllocator::instance();
  ModelFetch f;
  std::string lastId;
  bool more = streamModelPage(client, *h, lastId, f);
  tlsClose(client);
  while (more && provider == "anthropic" && !h->cut && !lastId.empty() && f.pages < kModelPagesMax) {
    const std::string after = lastId;
    more = fetchAnthropicPage(*h, key, after, lastId, f);
  }
  h->hasMore = more;
  storeModelHarvest(provider, *h, f);
}
// ---- model list fetch (lane F) end --------------------------------------------------

// One minimal probe call confirming a specific model is usable by this key (the
// usability probe: "a model your key cannot use never appears"). Opens its own
// short-lived TLS session, so the caller MUST already hold the work arbiter and
// have closed the verify GET client (single TLS slot). Returns the portable
// verdict from the HTTP status + a bounded slice of the error body.
// Build the minimal per-provider probe request (host + full HTTP/1.0 request incl.
// body). Returns false for a provider that has no probe wire. Split out to keep
// probeModel under the complexity gate.
static bool buildProbeRequest(const String& provider, const String& model,
                              const char*& host, String& req) {
  String path, body, authHdr;
  if (provider == "openai") {
    host = OPENAI_HOST;
    path = "/v1/responses";
    body = String("{\"model\":\"") + model + "\",\"input\":\"hi\",\"max_output_tokens\":16}";
    authHdr = String("Authorization: Bearer ") + store::openaiKey() + "\r\n";
  } else if (provider == "anthropic") {
    host = ANTHROPIC_HOST;
    path = "/v1/messages";
    body = String("{\"model\":\"") + model +
           "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    authHdr = String("x-api-key: ") + store::anthropicKey() + "\r\nanthropic-version: " ANTHROPIC_VER "\r\n";
  } else if (provider == "mistral") {
    host = MISTRAL_HOST;
    path = "/v1/chat/completions";
    body = String("{\"model\":\"") + model +
           "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    authHdr = String("Authorization: Bearer ") + store::mistralKey() + "\r\n";
  } else {
    return false;
  }
  req = String("POST ") + path + " HTTP/1.0\r\nHost: " + host + "\r\n" + authHdr +
        "Content-Type: application/json\r\nContent-Length: ";
  req += (int)body.length();
  req += "\r\nAccept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n";
  req += body;
  return true;
}

// Read the HTTP status code and a bounded slice of the response body (enough for
// error classification) off an already-sent probe request. Returns the code (0 if
// none). Shares the read shape with the verify GET above.
static int readProbeResponse(WiFiClientSecure& client, String& errBody, size_t maxBytes = 1100) {
  const uint32_t deadline = millis() + 15000;
  String status;
  while ((int32_t)(millis() - deadline) < 0) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      if (c != '\r') status += c;
    } else if (!client.connected() && !client.available()) {
      break;
    } else {
      delay(2);
    }
  }
  int code = 0, sp = status.indexOf(' ');
  if (sp > 0 && (int)status.length() >= sp + 4) code = status.substring(sp + 1, sp + 4).toInt();
  errBody.reserve(maxBytes + 100);
  while ((int32_t)(millis() - deadline) < 0 && errBody.length() < maxBytes) {
    if (client.available()) errBody += (char)client.read();
    else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  return code;
}

static nimbus::orch::ProbeVerdict probeModel(const String& provider, const String& model) {
  const char* host = nullptr;
  String req;
  if (!buildProbeRequest(provider, model, host, req)) return nimbus::orch::ProbeVerdict::Unknown;
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);
  if (!client.connect(host, 443)) return nimbus::orch::ProbeVerdict::Unknown;
  client.print(req);
  String errBody;
  const int code = readProbeResponse(client, errBody);
  tlsClose(client);
  return nimbus::orch::probeVerdict(code, std::string(errBody.c_str()));
}

// Probe the owner's SELECTED models for a provider (bounded to protect the single
// TLS slot) and fold the verdicts into the model store. An Unknown (transient)
// verdict leaves the model as-is for a later retry; a definitive verdict marks the
// model probed and sets usable, so an unusable selection stops appearing.
static void probeSelectedModels(const String& provider) {
  String cands[2];
  int nc = 0;
  const String om = store::orchModel(provider);
  const String sm = store::subModel(provider);
  if (om.length()) cands[nc++] = om;
  if (sm.length() && sm != om) cands[nc++] = sm;
  if (!nc) return;
  nimbus::orch::ModelStoreMeta meta;
  std::vector<nimbus::orch::ModelInfo> cat;
  if (!nimbus::orch::parseModelStore(provider.c_str(), store::modelStoreText(provider), meta, &cat))
    return;
  bool changed = false;
  for (int i = 0; i < nc; ++i) {
    nimbus::orch::ModelInfo* found = nullptr;
    for (auto& mi : cat)
      if (mi.id == cands[i].c_str()) { found = &mi; break; }
    if (!found) continue;
    const nimbus::orch::ProbeVerdict v = probeModel(provider, cands[i]);
    if (v == nimbus::orch::ProbeVerdict::Unknown) continue;
    found->usable = (v == nimbus::orch::ProbeVerdict::Usable);
    found->probed = true;
    changed = true;
    alogf("verify: %s probe %s -> %s", provider.c_str(), cands[i].c_str(),
          found->usable ? "usable" : "UNUSABLE");
  }
  if (changed && !store::setModelStoreText(provider, nimbus::orch::serializeModelStore(meta, cat)))
    alogf("verify: %s probe verdicts not stored (file write failed)", provider.c_str());
}

// Does a Z.ai candidate host answer the models list for this token? One quick GET
// (must run inside the held work slot). Returns true only on HTTP 200.
static bool zaiHostAnswers(const char* h, const String& key) {
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);
  if (!client.connect(h, 443)) return false;
  String req = String("GET ") + ZAI_BASE_PATH + "/models HTTP/1.0\r\nHost: " + h +
               "\r\nAuthorization: Bearer " + key +
               "\r\nAccept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n";
  client.print(req);
  String errBody;
  const int code = readProbeResponse(client, errBody);
  tlsClose(client);
  return code == 200;
}

// Resolve the Z.ai host: the pinned one if known, else probe api.z.ai then
// open.bigmodel.cn and pin the winner. Falls back to the primary host so the main
// verify still runs (and records couldn't-verify) if neither answered.
static String zaiPickHost(const String& key) {
  const String pinned = store::zaiBase();
  if (pinned.length()) return pinned;
  if (!key.length()) return String(ZAI_HOST_PRIMARY);
  const char* cands[] = {ZAI_HOST_PRIMARY, ZAI_HOST_FALLBACK};
  for (const char* h : cands) {
    if (zaiHostAnswers(h, key)) {
      store::setZaiBase(h);
      alogf("verify: zai endpoint probe -> %s", h);
      return String(h);
    }
  }
  return String(ZAI_HOST_PRIMARY);
}

// PROVISIONAL cloud sync (CUM-41): when on a Cumulo key, pull the admin master
// fallback rule set from the router and store it (source becomes "cloud" via a
// non-zero syncTs). The exact endpoint is pending C3 (CUM-105); this hits
// "<base>/fallbacks" best-effort and is a SAFE NO-OP on any non-rule response
// (parse yields 0 rules -> nothing stored), so it does no harm on the old router.
// Runs inside the held work slot after a successful Cumulo verify.
static void syncCumuloFallbacks(const char* host, const String& key) {
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);
  if (!client.connect(host, 443)) return;
  String req = String("GET /fallbacks HTTP/1.0\r\nHost: ") + host +
               "\r\nAuthorization: Bearer " + key +
               "\r\nAccept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n";
  client.print(req);
  String body;
  const int code = readProbeResponse(client, body, 4096);
  tlsClose(client);
  if (code != 200) return;
  nimbus::orch::FallbackRuleSet rs;
  if (nimbus::orch::parseFallbackRules(std::string(body.c_str()), rs,
                                       &PsramJsonAllocator::instance()) == 0)
    return;  // not a rule set (old router / 404 page) - keep local/default
  store::setFallbackRulesJson(body);
  time_t now = time(nullptr);
  store::setFallbackSyncTs(now > 100000 ? (uint32_t)now : 1);
  alogf("verify: cumulo fallback rules synced (%u rules)", (unsigned)rs.rules.size());
}

// The fetch hooks over one WiFiClientSecure for GET /v1/connectors: connect and
// send the request, bulk reads (not byte-by-byte), RST-close on every path.
static FetchIo mistralConnectorsIo(WiFiClientSecure& client, const String& key) {
  FetchIo io;
  io.turnInFlight = [] { return orchestrator::turnInFlight(); };
  io.connect = [&client, &key]() {
    if (!client.connect(MISTRAL_HOST, 443)) return false;
    client.print(String("GET /v1/connectors?page_size=100 HTTP/1.0\r\nHost: ") + MISTRAL_HOST +
                 "\r\nAuthorization: Bearer " + key +
                 "\r\nAccept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n");
    return true;
  };
  io.read = [&client](char* dst, size_t want) -> int {
    const int r = client.read((uint8_t*)dst, want);
    if (r > 0) return r;
    return (!client.connected() && client.available() == 0) ? -1 : 0;
  };
  io.close = [&client] { tlsClose(client); };
  io.nowMs = [] { return (uint32_t)millis(); };
  io.idle = [] { delay(2); };
  return io;
}

// Parse a fetched /v1/connectors response and publish it; true when a fresh answer
// was published. A body cut by the byte cap or the deadline, an unparseable one,
// or one fetched with a key that has since changed keeps the last good answer.
static bool publishConnectorsBody(const char* buf, size_t blen, FetchResult fr, uint32_t keyGen) {
  if (fr != FetchResult::Ok) {
    alogf("verify: mistral connectors probed (%u bytes, %s - kept last answer)", (unsigned)blen,
          fr == FetchResult::Truncated ? "TRUNCATED" : "TIMED OUT");
    return false;
  }
  // Skip the HTTP headers to the JSON body (only compact status/header text precedes it).
  const char* jbody = strstr(buf, "\r\n\r\n");
  jbody = jbody ? jbody + 4 : (strstr(buf, "\n\n") ? strstr(buf, "\n\n") + 2 : buf);
  const bool noted = agent::connectors::noteMistralConnectorsProbe(jbody, keyGen);
  alogf("verify: mistral connectors probed (%u bytes%s)", (unsigned)blen,
        noted ? "" : ", unparseable or key changed - not published");
  return noted;
}

// Ask Mistral which Studio connectors the key's workspace lists (GET /v1/connectors),
// so a Studio connector (gcal/notion/slack: no device credential by design) is
// offered to the model once the workspace lists it as active. Runs on the held work
// slot: right after a successful Mistral verify, or as the automatic probe-only run.
// The body (~300 KB of connector metadata) is read into PSRAM; the portable
// workspace parse filters it to the listed ids (+ the is_authenticated hint). Any
// failure leaves the last good answer untouched (before the first answer a Studio
// connector reads "not checked yet", never usable by guess). `gen` is the probe
// request this fetch answers (read before it starts, so a save made during it stays
// queued). With `yieldToTurn` (the automatic probe) a turn that starts mid-fetch
// aborts it at the next read and gets the work slot back (TurnStarted).
static SyncOutcome syncMistralConnectors(const String& key, bool yieldToTurn = false,
                                         uint32_t gen = g_wsq.generation()) {
  if (!agent::connectors::wantsMistralWorkspaceProbe()) {   // skip the large fetch
    g_wsq.served(gen);
    return SyncOutcome::Landed;
  }
  // Captured before the fetch: a Mistral key saved while this probe runs bumps it,
  // and the old key's answer is then dropped instead of published for the new key.
  const uint32_t keyGen = agent::connectors::mistralProbeGeneration();
  const size_t kCap = 512 * 1024;   // headroom over the ~300 KB body; all in PSRAM
  char* buf = (char*)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM);
  if (!buf) return SyncOutcome::NoAnswer;
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);
  const FetchIo io = mistralConnectorsIo(client, key);
  size_t blen = 0;
  const FetchResult fr = fetchBody(io, buf, kCap, 25000, yieldToTurn, blen);   // 25 s for the body
  bool noted = false;
  if (fr == FetchResult::TurnStarted)
    alogf("verify: mistral connectors probe stopped for a turn (%u bytes read)", (unsigned)blen);
  else if (fr != FetchResult::ConnectFailed)
    noted = publishConnectorsBody(buf, blen, fr, keyGen);
  free(buf);
  if (noted) g_wsq.served(gen);   // the fresh answer covers every request up to gen
  if (fr == FetchResult::TurnStarted) return SyncOutcome::TurnStarted;
  return noted ? SyncOutcome::Landed : SyncOutcome::NoAnswer;
}

// CUM-537: the workspace half of a Mistral verify, alone (queued by a connector
// save or at boot): same task, same internal-memory gate and work slot as a
// verify, but no /v1/models, no model probes and no verdict write. One attempt
// (workspace_probe.h attemptProbe): it yields to the owner, never waiting long for
// the work slot and backing off when a turn starts before or during the fetch,
// rather than make that turn queue behind a ~300 KB fetch on the single slot. A
// probe that cannot run re-queues itself on a slow clock, at most kMaxTries times.
static void runWorkspaceProbeOnly() {
  const uint32_t gen = g_wsq.generation();
  const String key = store::mistralKey();
  if (!key.length()) { g_wsq.served(gen); return; }   // nothing to probe with
  ProbeGate gate;
  gate.lowMemory = [] { return nimbus::heapwatch::restingLargest() < VERIFY_MIN_MAX8; };
  gate.turnInFlight = [] { return orchestrator::turnInFlight(); };
  gate.acquireSlot = [] { return arbiter::acquireWork(5000); };
  gate.releaseSlot = [] { arbiter::releaseWork(); };
  const char* why = attemptProbe(gate, [&] { return syncMistralConnectors(key, true, gen); });
  if (why) rearmWorkspaceProbe(gen, why, countsAsTry(why));
}

// Record a verify result. A transient "couldn't verify" (-1) must not DEMOTE a
// definitive VERIFIED (1): the capProbe=2 tick re-verifies unattended (WiFi still
// rejoining, TLS slot busy behind a turn), and one blip was erasing the catalog's
// VERIFIED marking until the next success. Definitive verdicts (1 / 0) always overwrite.
static void recordVerify(const String& prov, int8_t result, const char* rsn = "") {
  if (result == -1 && store::verifyResult(prov) == 1) {
    alogf("verify: %s transient failure - keeping cached verified", prov.c_str());
    setReason(prov, "");   // displayed state is still verified: no badge reason
    return;
  }
  store::setVerify(prov, result, (uint32_t)millis());
  // verify===1 (and a plain rejection) carry their own UI copy, so the reason
  // token only rides a couldn't-verify (-1). Clearing it on 1/0 stops a stale
  // "nocredits" from lingering after the account is funded.
  setReason(prov, result == 1 ? "" : rsn);
  // A DEFINITIVE verdict (verified=1 or rejected=0) resolves any low-memory
  // deferral: stop the self-retry here, at the ONE place a real verdict lands.
  // A transient (-1: tlsbusy / connectfail / timeout) must NOT clear it, or a
  // busy slot on a memory-recovered attempt abandons the auto-retry forever and
  // the provider is stuck "deferred" with no way back (CUM-451; the live Cumulo
  // stuck-verify). The low-memory path passes -1 then arms onDeferred(), so the
  // retry survives until a verdict lands or memory + the slot are both free.
  if (result != -1) { int ri = retryIdx(prov); if (ri >= 0) g_retry[ri].clear(); }
}

// A model-list refresh (R5) touches the cached verdict only when the answer changed
// it (a key revoked since the last verify, or a first verdict): an unchanged verdict
// writes nothing to NVS, and a transient failure leaves verdict and reason alone.
static void recordOutcome(const String& prov, int8_t result, const char* rsn, bool refreshOnly) {
  if (refreshOnly && (result == -1 || store::verifyResult(prov) == result)) return;
  recordVerify(prov, result, rsn);
}

// A verify the memory gate refused (startTask). recordVerify keeps a cached VERIFIED
// as-is (a transient blip must not demote it), so the badge shows low-memory only for
// a provider that is not verified, and only then is the measured number stored. The
// self-retry policy (onLowMemoryDefer) decides when to re-arm (CUM-575): a verify is also
// how a verified provider's model list and router prices are refreshed, and that refresh
// must not be dropped.
static void deferLowMemory(const String& provider, uint32_t max8) {
  const LowMemoryDefer act = onLowMemoryDefer(store::verifyResult(provider) == 1);
  recordVerify(provider, -1, "low-memory");
  if (act.storeNumber) setDeferMax8(provider, max8);
  const int ri = retryIdx(provider);
  if (act.armRetry && ri >= 0) g_retry[ri].onDeferred((uint32_t)millis());
  alogf("verify: %s deferred low-memory (resting largest %u, need %u)", provider.c_str(),
        (unsigned)max8, (unsigned)VERIFY_MIN_MAX8);
}

static void runOne() {
  if (g_wsOnly) { runWorkspaceProbeOnly(); g_pending = false; return; }   // CUM-537
  String provider = g_provider;
  const bool refreshOnly = g_refreshOnly;   // R5: the model list only
  if (refreshOnly) alogf("verify: %s model list refresh (GET only)", provider.c_str());

  String hostBuf;               // backs a dynamic host (zai probe result / cumulo base)
  const char* host = nullptr;
  String path = "/v1/models";
  String key;
  // Telegram is a pseudo-provider: same one-slot, arbited, watchdog-free verify seam,
  // but it proves the BOT TOKEN via getMe instead of an API key via /v1/models (owner:
  // "when you save the telegram token, first run a tiny verification"). The token rides
  // the URL path (/bot<token>/getMe), so there's no auth header.
  const bool isTg  = (provider == "telegram");
  // Tavily is POST-shaped: there is no cheap GET, so the verify is one minimal
  // /search (1 credit) - the same "tiny real call" contract as the others.
  const bool isTav = (provider == "tavily");
  // Z.ai host is probed AFTER the arbiter is held (the probe itself is a TLS call);
  // the base path is /api/paas/v4, not /v1.
  const bool isZai = (provider == "zai");
  if (provider == "openai")         { host = OPENAI_HOST;    key = store::openaiKey(); }
  else if (provider == "anthropic") { host = ANTHROPIC_HOST; key = store::anthropicKey();
                                      path = "/v1/models?limit=100"; }   // has_more pages follow
  else if (provider == "mistral")   { host = MISTRAL_HOST;   key = store::mistralKey(); }
  else if (isZai)                   { key = store::zaiKey(); path = ZAI_BASE_PATH "/models"; }
  else if (provider == "cumulo")    { key = store::cumuloKey();
                                      hostBuf = store::cumuloBase();
                                      if (!hostBuf.length()) hostBuf = CUMULO_HOST_DEFAULT;
                                      int sch = hostBuf.indexOf("://");
                                      if (sch >= 0) hostBuf = hostBuf.substring(sch + 3);
                                      int sl = hostBuf.indexOf('/');
                                      if (sl >= 0) hostBuf = hostBuf.substring(0, sl);
                                      host = hostBuf.c_str();
                                      path = "/router/models"; }
  else if (isTg)                    { host = "api.telegram.org"; key = store::telegramToken();
                                      path = String("/bot") + key + "/getMe"; }
  else if (isTav)                   { host = "api.tavily.com"; key = store::tavilyKey();
                                      path = "/search"; }
  else { g_pending = false; return; }  // web handler validates; defensive

  if (!key.length()) {  // nothing to verify - rejected, no TLS spent
    store::setVerify(provider, 0, (uint32_t)millis());
    setReason(provider, "");   // rejected carries its own copy; no badge reason
    // A no-key result is DEFINITIVE (rejected), so resolve any outstanding
    // low-memory self-retry here too - this path returns before the memory-
    // sufficient clear below, so without it a provider that was low-memory
    // deferred and then had its key removed keeps re-firing the retry every pump
    // tick (a verify-task + setVerify NVS-write storm). (CUM-447 gate fix.)
    { int ri = retryIdx(provider); if (ri >= 0) g_retry[ri].clear(); }
    alogf("verify: %s no key -> rejected", provider.c_str());
    g_pending = false;
    return;
  }
  // Memory was checked before this task existed (startTask: the resting largest block
  // against VERIFY_MIN_MAX8, and room for this stack). Do NOT clear the low-memory
  // retry here: clearing before the slot is acquired means a "tls busy" (or
  // connectfail) below abandons the auto-retry, leaving the provider stuck "deferred"
  // (CUM-451). The retry is cleared only on a DEFINITIVE verdict, in recordVerify().
  // Outlast one full Telegram long-poll cycle (30 s) so a verify queued behind
  // an orchestrator turn still lands instead of bouncing "tls busy".
  if (!arbiter::acquireWork(35000)) {
    recordOutcome(provider, -1, "tlsbusy", refreshOnly);
    alogf("verify: %s tls busy", provider.c_str());
    g_pending = false;
    return;
  }

  // Z.ai endpoint probe (inside the held slot): pick whichever host the token
  // answers, pinned in NVS for next time.
  if (isZai) { hostBuf = zaiPickHost(key); host = hostBuf.c_str(); }

  int8_t result = -1;  // couldn't verify; 1 on HTTP 200, 0 on 401/403
  const char* rsn = "";  // vfyReason token for the couldn't-verify (-1) cases
  {
    WiFiClientSecure client;
    tlsSetup(client);
    client.setHandshakeTimeout(12);
    client.setConnectionTimeout(15000);  // F25: real socket bound (setTimeout inert)
    if (client.connect(host, 443)) {
      // Tavily: a minimal POST /search proves the key does real work; the others
      // stay GET-shaped.
      const char* body = isTav ? "{\"query\":\"ping\",\"max_results\":1}" : nullptr;
      String req = String(isTav ? "POST " : "GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\n";
      if (provider == "anthropic")
        req += String("x-api-key: ") + key + "\r\nanthropic-version: " ANTHROPIC_VER "\r\n";
      else if (!isTg)   // telegram carries the token in the path, no auth header
        req += String("Authorization: Bearer ") + key + "\r\n";
      if (body) {
        req += "Content-Type: application/json\r\nContent-Length: ";
        req += (int)strlen(body);
        req += "\r\n";
      }
      // identity: some provider edges gzip even an HTTP/1.0 request with no
      // Accept-Encoding (live-caught: a 16 KB unreadable body -> harvest EMPTY);
      // the harvest scans for literal "id":" strings, so compression is fatal.
      req += "Accept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n";
      if (body) req += body;
      client.print(req);

      uint32_t deadline = millis() + 15000;  // read just the status line
      String status;
      while ((int32_t)(millis() - deadline) < 0) {
        if (client.available()) {
          char c = client.read();
          if (c == '\n') break;
          if (c != '\r') status += c;
        } else if (!client.connected() && !client.available()) {
          break;
        } else {
          delay(2);
        }
      }
      int code = 0, sp = status.indexOf(' ');
      if (sp > 0 && (int)status.length() >= sp + 4)
        code = status.substring(sp + 1, sp + 4).toInt();
      // Telegram: a bad/malformed bot token answers 401 OR 404 (unknown bot path).
      result = (code == 200) ? 1
             : (code == 401 || code == 403 || (isTg && code == 404)) ? 0
             : -1;
      // vfyReason for the -1 cases (contract in lanes/L1/PROGRESS.md). 402 from the
      // router probe = no credits/subscription; 404 from the cumulo /router/models
      // probe = a router that predates the wave-2 deploy. Any other non-2xx/4xx
      // (5xx/timeout) stays a generic couldn't-verify (empty reason).
      if (result == -1) {
        if (code == 402)                             rsn = "nocredits";
        else if (code == 404 && provider == "cumulo") rsn = "router_outdated";
      }
      // NVS-priority (CUM-77 item 6 follow-on): a definitive verdict is tiny and
      // load-bearing. Before CUM-479 the catalog blob written below lived in NVS and
      // could EXHAUST it, starving the verdict write that otherwise runs only at the
      // END of runOne - so a real HTTP 200 persisted as a stuck -1 (live-caught on
      // .61: zai/cumulo verified 200 yet stored -1 while their catalogs "did not fit
      // NVS"). The catalog is a flash file now, but persisting the definitive verdict
      // FIRST still costs nothing and keeps it ahead of any later write. The
      // recordVerify at the end is then idempotent.
      if (result != -1) recordOutcome(provider, result, rsn, refreshOnly);
      // LLM providers: the verify already fetched the model list - store it (lane F:
      // the whole list, streamed, in the provider's model store; no NVS write).
      if (!isTg && !isTav && code == 200) fetchModelList(provider, client, key);
      // Telegram bonus (owner ask: show WHICH bot is connected in the web app):
      // getMe's tiny body carries the bot's username - read the rest of the
      // response (bounded) and pull it out. Display-only NVS sidecar.
      if (isTg && code == 200) {
        String rest;
        rest.reserve(512);
        while ((int32_t)(millis() - deadline) < 0 && rest.length() < 700) {
          if (client.available()) rest += (char)client.read();
          else if (!client.connected() && !client.available()) break;
          else delay(2);
        }
        int u = rest.indexOf("\"username\":\"");
        if (u >= 0) {
          u += 12;
          int e = rest.indexOf('"', u);
          if (e > u && e - u <= 64) {
            store::setTgBotName(rest.substring(u, e));
            alogf("verify: telegram bot @%s", rest.substring(u, e).c_str());
          }
        }
      }
      alogf("verify: %s HTTP %d -> %s", provider.c_str(), code,
            result == 1 ? "verified" : result == 0 ? "rejected" : "couldn't verify");
    } else {
      rsn = "connectfail";
      alogf("verify: %s connect failed -> couldn't verify", provider.c_str());
    }
    tlsClose(client);  // RST-close on EVERY path (connected or not)
  }
  // Usability probe (bounded): now that the GET client is closed but we still hold
  // the work slot, confirm the owner's SELECTED models actually run on this key.
  // A model-list refresh (R5) stops at the list: no paid probe, no other sync.
  const bool fullVerify = result == 1 && !refreshOnly;
  if (fullVerify && (provider == "openai" || provider == "anthropic" || provider == "mistral"))
    probeSelectedModels(provider);
  // On a Cumulo key, pull the admin master fallback rule set (provisional; CUM-41).
  if (fullVerify && provider == "cumulo" && host) syncCumuloFallbacks(host, key);
  // On a Mistral key, learn which Studio connectors are authenticated in the owner's
  // Mistral account, so gcal/notion/slack become usable only once connected there.
  if (fullVerify && provider == "mistral") (void)syncMistralConnectors(key);
  arbiter::releaseWork();
  recordOutcome(provider, result, rsn, refreshOnly);
  // A verify verdict is tiny and load-bearing. On a maxed NVS the verdict write
  // above can fail, leaving a real HTTP 200 stuck at -1 (live-caught on .61: 5
  // provisioned providers' catalog caches overflowed the ~20 KB NVS). Since CUM-479
  // the catalogs live in flash files and the boot heal evicts any legacy NVS copy,
  // so this normally frees nothing; it stays as the backstop (a device downgraded
  // and re-upgraded mid-session). Only fires under genuine NVS exhaustion; a
  // transient -1 is left for a later retry.
  if (result != -1 && store::verifyResult(provider) != result) {
    const int freed = store::evictLegacyCatalogCaches();
    recordVerify(provider, result, rsn);
    alogf("verify: %s verdict was NVS-starved - evicted %d catalog cache(s), verdict now %d",
          provider.c_str(), freed, (int)store::verifyResult(provider));
  }
  g_pending = false;  // clear LAST so pending() covers the whole run
}

// ONE verify, then the task exits and returns its 8 KB stack to internal SRAM.
// (It used to spin here forever polling g_pending every 250 ms, holding the stack
// for the device's whole uptime - see the note in request().)
static void verifyTask(void*) {
  {
    nimbus::heapwatch::Scope worker(nimbus::heapwatch::kWorker);
    nimbus::heapwatch::Scope verifying(nimbus::heapwatch::kVerify);
    runOne();               // clears g_pending on every exit path
  }
  g_pending = false;        // belt-and-braces: never leave the slot stuck busy
  vTaskDelete(nullptr);     // self-delete; frees the stack
}

// One provider's pending start, if any (no turn, online: the caller checked). A start a
// turn refused goes first (a refusal again re-queues or defers it); then a deferred
// verify whose heap recovered (row 43) or whose backoff came due. True once a verify
// started.
static bool pumpOneRetry(int i, uint32_t now, uint32_t resting) {
  if (g_wantStart[i]) {
    g_wantStart[i] = false;
    return request(kRetryProv[i]);
  }
  const bool recovered = g_retry[i].dueOnRecovery(now, false, true, /*busy=*/false, resting,
                                                  (uint32_t)VERIFY_MIN_MAX8);
  if (!recovered && !g_retry[i].due(now, false, true, /*busy=*/false)) return false;
  if (recovered) {
    g_retry[i].onRecoveryFired();
    alogf("verify: %s retry - memory recovered (resting largest %u)", kRetryProv[i], (unsigned)resting);
  }
  return request(kRetryProv[i]);
}

// Re-arm a low-memory-deferred verify when its backoff comes due and the device is
// free (CUM-447). Called from the main loop; enqueues at most one verify per pass
// through the SAME self-deleting, TLS-arbited task the Verify button uses. The task
// re-measures memory and re-arms via onDeferred if it is still low, so the backoff
// walks itself forward with no scheduling state kept here.
void pumpRetry() {
  if (g_pending) return;   // one slot; a verify is already queued/running
  // The loop calls this thousands of times a second; the backoff is minutes wide,
  // so a 1 s gate keeps the common (nothing-armed) path free without changing when a
  // retry actually fires. now-relative, so it survives the millis() wrap.
  static uint32_t s_nextPump = 0;
  const uint32_t now = (uint32_t)millis();
  if ((int32_t)(now - s_nextPump) < 0) return;
  s_nextPump = now + 1000;
  const bool     inTurn = orchestrator::turnInFlight();
  const bool     online = (WiFi.status() == WL_CONNECTED);
  // CUM-537: a queued workspace probe (connector save / boot) goes first - it is
  // what turns an owner-visible "connect it in Mistral" into the truth.
  // The started worker settles the request itself (the generation it read).
  if (g_wsq.due(now) && !inTurn && online) {
    const uint32_t gen = g_wsq.generation();
    if (!startTask("mistral", /*wsOnly=*/true))
      rearmWorkspaceProbe(gen, "could not start the worker", /*countsTry=*/true);
    return;
  }
  if (pumpRouterPriceRefill(now, inTurn, online)) return;
  if (inTurn || !online) return;
  const uint32_t resting = nimbus::heapwatch::restingLargest();
  for (int i = 0; i < kNRetry; ++i)
    if (pumpOneRetry(i, now, resting)) return;   // one start per pass; the slot is now busy
}

bool anyDeferredStuck() {
  const uint32_t now = (uint32_t)millis();
  for (int i = 0; i < kNRetry; ++i)
    if (g_retry[i].stuckFor(now, kStuckWindowMs)) return true;
  return false;
}

void begin() {
  // No worker to start: it is spawned per request() and self-deletes, so no stack
  // is held while idle. Kept as a stable entry point for main.cpp, which calls it
  // once at boot in both modes - the moment the CUM-479 heal runs: every legacy
  // mcat_ catalog leaves NVS, so a device that is full today gets its owner-config
  // room back on this boot. The copy to its flash file happens only where LittleFS
  // is mounted (Orchestrator mode); in Notifier mode the caches are just evicted.
  store::migrateCatalogCachesOffNvs();
  // Lane F: the older build's model lists move into the model store (the NVS short
  // lists go on the boot after this image is marked valid).
  store::migrateModelStores();
}

}  // namespace provider_verify
}  // namespace agent
