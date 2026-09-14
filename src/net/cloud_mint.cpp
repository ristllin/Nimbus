#include "cloud_mint.h"

#include <WiFiClientSecure.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_heap_caps.h"

#include "nimbus/cloud/device_key.h"      // portable gate + request/response policy
#include "../agent/agent_config.h"        // CUMULO_HOST_DEFAULT
#include "../agent/store.h"
#include "../sys/agent_log.h"
#include "../sys/net_util.h"              // tlsSetup / tlsClose
#include "../sys/tls_arbiter.h"           // one work-TLS slot (serialized against turns)

namespace agent {
namespace cloud_mint {

// Single-slot handoff, mirroring provider_verify: g_pending is the flag between the
// web task (request) and the worker (run); the payload is written only while
// g_pending is false and read only after it flips true.
static volatile bool g_pending = false;
static long          g_capacity = 0;
static nimbus::cloud::MintTrigger g_trigger = nimbus::cloud::MintTrigger::UserSave;

// Result snapshot. Written by the worker under a brief critical section and copied
// out the same way, so the AsyncTCP reader never sees a torn multi-field state.
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static State         s_state = State::Idle;
static bool          s_ok = false;
static long          s_cap = 0;
static long          s_max = 0;
static char          s_label[64] = {0};
static char          s_message[176] = {0};

// Same empirical largest-contiguous-INTERNAL floor the verify path uses: below it,
// don't attempt the handshake (record a transient busy instead of a hard OOM).
static const size_t  kMintMinMax8 = 8000;

static void setResult(State st, bool ok, long cap, long max, const String& label,
                      const String& message) {
  portENTER_CRITICAL(&s_mux);
  s_state = st;
  s_ok = ok;
  s_cap = cap;
  s_max = max;
  strncpy(s_label, label.c_str(), sizeof(s_label) - 1);
  s_label[sizeof(s_label) - 1] = 0;
  strncpy(s_message, message.c_str(), sizeof(s_message) - 1);
  s_message[sizeof(s_message) - 1] = 0;
  portEXIT_CRITICAL(&s_mux);
}

// Resolve the cumulo router host: base override else the compiled default, scheme
// and any path stripped (POST goes to /router/device-key on port 443).
static String routerHost() {
  String h = store::cumuloBase();
  if (!h.length()) h = CUMULO_HOST_DEFAULT;
  int sch = h.indexOf("://");
  if (sch >= 0) h = h.substring(sch + 3);
  int sl = h.indexOf('/');
  if (sl >= 0) h = h.substring(0, sl);
  return h;
}

// Read the whole HTTP/1.0 response (headers + small JSON body) into `resp`. The
// request is Connection: close, so the server closes after the body and the drained
// + closed check terminates the read naturally; the 8 KB cap is only a runaway
// guard. It must be comfortably larger than headers + the small {key,capacity,label}
// body so a valid 200 is never truncated into a parse failure (a truncated success
// would report "couldn't mint" while the router had already issued a key - see the
// duplicate-mint note in PR_BODY: server-side idempotency is the real fix, tracked
// as a cloud contract item).
static void readResponse(WiFiClientSecure& client, String& resp) {
  const uint32_t deadline = millis() + 15000;
  resp.reserve(1024);
  while ((int32_t)(millis() - deadline) < 0 && resp.length() < 8192) {
    int c = client.read();
    if (c >= 0) { resp += (char)c; continue; }
    if (!client.connected()) break;
    delay(2);
  }
}

static void runOne() {
  const long capacity = g_capacity;

  // Re-assert the gate on the worker side too (defense in depth), on the trigger
  // the caller declared: only a user Save with a valid capacity may proceed.
  if (!nimbus::cloud::mintAllowed(g_trigger, capacity)) {
    setResult(State::Error, false, 0, 0, "",
              "Enter a capacity of at least 1 credit.");
    g_pending = false;
    return;
  }

  const String deviceId = store::cloudDeviceId();
  const String cred     = store::cloudCred();
  if (!deviceId.length() || !cred.length()) {
    setResult(State::Error, false, 0, 0, "",
              "This device is not paired with the cloud. Pair it first.");
    alog("mint: refused (not paired)");
    g_pending = false;
    return;
  }

  size_t max8 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (max8 < kMintMinMax8) {
    setResult(State::Error, false, 0, 0, "", "Device is busy. Try again.");
    alogf("mint: deferred (max8=%u)", (unsigned)max8);
    g_pending = false;
    return;
  }
  if (!arbiter::acquireWork(35000)) {
    setResult(State::Error, false, 0, 0, "", "Device is busy. Try again.");
    alog("mint: tls busy");
    g_pending = false;
    return;
  }

  const std::string body =
      nimbus::cloud::buildMintRequest(std::string(deviceId.c_str()),
                                      std::string(cred.c_str()), capacity);
  const String host = routerHost();

  int code = 0;
  String respBody;
  {
    WiFiClientSecure client;
    // The device pairing credential rides this POST. ALWAYS validate the server
    // against the embedded CA bundle, exactly like relayTlsSetup(): never
    // tlsSetup(), whose owner tlsVerify escape hatch would send the credential over
    // an unvalidated connection.
    client.setCACertBundle(_nimbusCrtBundleStart,
                           (size_t)(_nimbusCrtBundleEnd - _nimbusCrtBundleStart));
    client.setHandshakeTimeout(12);
    client.setConnectionTimeout(15000);
    if (client.connect(host.c_str(), 443)) {
      String req = String("POST /router/device-key HTTP/1.0\r\nHost: ") + host +
                   "\r\nContent-Type: application/json\r\nAccept-Encoding: identity"
                   "\r\nUser-Agent: Nimbus\r\nContent-Length: ";
      req += (int)body.size();
      req += "\r\nConnection: close\r\n\r\n";
      req += body.c_str();
      client.print(req);

      String resp;
      readResponse(client, resp);
      int sp = resp.indexOf(' ');
      if (sp > 0 && (int)resp.length() >= sp + 4)
        code = resp.substring(sp + 1, sp + 4).toInt();
      int bh = resp.indexOf("\r\n\r\n");
      if (bh >= 0) respBody = resp.substring(bh + 4);
    } else {
      alogf("mint: connect failed to %s", host.c_str());
    }
    tlsClose(client);
  }
  arbiter::releaseWork();

  nimbus::cloud::MintResult r =
      nimbus::cloud::parseMintResponse(code, std::string(respBody.c_str()));
  // Never log the key or the credential; the HTTP code and status are enough.
  alogf("mint: HTTP %d -> %s", code, r.ok() ? "minted" : "not minted");

  if (r.ok()) {
    const String minted(r.key.c_str());
    store::setCumuloKey(minted);
    // Confirm the NVS write actually landed before claiming success: on a full NVS
    // (the documented Lumi failure) the router has issued a live key that this
    // device would otherwise silently drop. Say so honestly; the user can revoke it.
    if (store::cumuloKey() != minted) {
      alog("mint: key not persisted (nvs write failed)");
      setResult(State::Error, false, r.capacity, 0, "",
                "The key was minted but this device could not save it. Free device "
                "storage, then revoke that key in the app and mint again.");
      g_pending = false;
      return;
    }
    // Mark the key unverified so the Providers card shows it as pending. The verify
    // itself is kicked by the web status readback once it observes Done, NOT here:
    // spawning the verify task from this still-live worker would briefly hold two
    // TLS-capable task stacks in scarce internal SRAM at once.
    store::setVerify("cumulo", -1, 0);
    setResult(State::Done, true, r.capacity, 0, String(r.label.c_str()),
              String(r.message.c_str()));
  } else {
    setResult(State::Error, false, r.capacity, r.max, "", String(r.message.c_str()));
  }
  g_pending = false;
}

static void mintTask(void*) {
  runOne();          // clears g_pending on every exit path
  g_pending = false; // belt-and-braces
  vTaskDelete(nullptr);
}

bool request(nimbus::cloud::MintTrigger trigger, long capacity) {
  // The gate is on the DECLARED trigger: anything but a user Save is refused here
  // and again on the worker, so no new call site can mint silently.
  if (!nimbus::cloud::mintAllowed(trigger, capacity)) return false;
  if (!store::cloudPaired()) return false;
  if (g_pending) return false;   // one mint at a time
  g_capacity = capacity;
  g_trigger  = trigger;
  g_pending = true;
  setResult(State::Pending, false, capacity, 0, "", "Minting a key.");
  // 8 KB stack, matching the proven provider_verify TLS task: one small TLS POST +
  // a bounded body read (mbedTLS RX/TX buffers ride PSRAM). Watchdog-free so the
  // up-to-35 s acquire + handshake can't trip the F12 loop watchdog.
  if (xTaskCreate(mintTask, "cloudmint", 8192, nullptr, 1, nullptr) != pdPASS) {
    g_pending = false;
    setResult(State::Error, false, 0, 0, "", "Device is busy. Try again.");
    return false;
  }
  return true;
}

bool pending() { return g_pending; }

Status status() {
  Status s;
  // Copy the fixed char buffers out under the lock, then build the Strings OUTSIDE
  // it: String construction allocates, and a spinlock (portENTER_CRITICAL disables
  // interrupts) must never be held across the heap allocator.
  char label[sizeof(s_label)], message[sizeof(s_message)];
  portENTER_CRITICAL(&s_mux);
  s.state = s_state;
  s.ok = s_ok;
  s.capacity = s_cap;
  s.max = s_max;
  memcpy(label, s_label, sizeof(label));
  memcpy(message, s_message, sizeof(message));
  portEXIT_CRITICAL(&s_mux);
  s.label = label;      // buffers stay null-terminated (setResult guarantees it)
  s.message = message;
  return s;
}

}  // namespace cloud_mint
}  // namespace agent
