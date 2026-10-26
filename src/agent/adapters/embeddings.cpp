#include "embeddings.h"

#include <WiFiClientSecure.h>

#include "../agent_config.h"
#include "../../sys/agent_log.h"
#include "../../sys/net_util.h"      // tlsClose
#include "../store.h"
#include "../../sys/tls_arbiter.h"
#include "nimbus/orch/embedding.h"       // portable build/parse (host-tested)
#include "nimbus/orch/vector_memory.h"   // quantize
#include "nimbus/audio_req.h"            // core::parseErrorCode (host-tested refusal parse)
#include "nimbus/harness/providers.h"    // testStatusOverride - the bench's scripted answer (test builds)
#include "../provider_gate.h"            // the one per-provider park (L19-8)
#include "nimbus/orch/seam_meter.h"      // meterSeamAnswerBody - its spend (lane DEF)
#include "nimbus/sys/web_guard.h"        // the web task's network budget
#include "../../sys/web_task.h"          // onWebTask
#include <lwip/dns.h>                    // dns_gethostbyname: the web task's cache-only lookup
#include <lwip/tcpip.h>                  // LOCK_TCPIP_CORE around it

namespace agent {
namespace embeddings {

namespace {
static const unsigned long EMBED_TIMEOUT_MS = 20000;

// Strip scheme + any path from a stored base, leaving a bare host for connect().
// (Same shape as cumulo_adapter's bareHost; a stored cumuloBase may be a full URL.)
String bareEmbedHost(String h) {
  int s = h.indexOf("://");
  if (s >= 0) h = h.substring(s + 3);
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  return h;
}

// Resolve the configured provider's host + path + key. Returns false if unknown or
// no key. The per-provider PATH is the host-tested nimbus::orch::embedRouteFor
// (openai/mistral -> /v1/embeddings direct; cumulo -> /router/openai/v1/embeddings
// on the router with the one router key - vectors stay byte-compatible).
bool resolveProvider(const String& provider, String& host, String& path, String& key) {
  const nimbus::orch::EmbedRoute route =
      nimbus::orch::embedRouteFor(std::string(provider.c_str()));
  if (!route.known) return false;
  path = route.path;
  if (route.viaCumuloRouter) {
    String base = store::cumuloBase();
    if (!base.length()) base = CUMULO_HOST_DEFAULT;
    host = bareEmbedHost(base);
    key = store::cumuloKey();
  } else if (provider == "openai") {
    host = OPENAI_HOST;  key = store::openaiKey();
  } else {  // mistral
    host = MISTRAL_HOST; key = store::mistralKey();
  }
  return key.length() > 0;
}

// A non-200 answer: the err token the memory engines classify (classifyEmbedFail),
// and the park told (a billing or key refusal closes the provider for every seam).
void failFromStatus(const String& provider, const String& key, int code, const char* body, String& err) {
  gate::noteAnswer(provider.c_str(), nimbus::orch::PaidSeam::Embeddings, key, code, body);
  if (code == 401 || code == 403) { err = "key rejected"; return; }
  err = String("HTTP ") + code;
  // A non-200 body may be a router/provider refusal {"error":"<code>"}. Append
  // the machine code (bounded, secret-safe: parseErrorCode returns only the code
  // field, and the memory tools echo just the numeric status unless it is a known
  // refusal code) so funding_cap_reached / rate_limited / endpoint_not_allowed map
  // to plain words upstream.
  bool jok = false;
  std::string rc = core::parseErrorCode(body, &jok);
  if (jok && !rc.empty()) { if (rc.size() > 48) rc.resize(48); err += String(" ") + rc.c_str(); }
  alogf("embed: HTTP %d %s", code, rc.c_str());
}

// From a web request the host must already be in lwIP's DNS cache (web_guard.h): a
// lookup that goes out to the network can outlast the watchdog, and the TLS client's
// connect resolves before its own timers start. An uncached host starts the lookup in
// the background (the next request finds it cached) and the request answers busy now.
bool webDnsReady(const String& host) {
  ip_addr_t addr;
  const bool own = sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
  if (!own) LOCK_TCPIP_CORE();
  const err_t e = dns_gethostbyname(host.c_str(), &addr, [](const char*, const ip_addr_t*, void*) {}, nullptr);
  if (!own) UNLOCK_TCPIP_CORE();
  return e == ERR_OK;
}

// The park closed this provider (out of credit or a refused key learned on any seam,
// or the pause switch): no request, and an err token the memory engines read as the
// same class the refusal itself would have been.
String closedToken(const nimbus::orch::ParkVerdict& v) {
  if (v.paused) return "paused";
  if (v.kind == nimbus::orch::ParkKind::Auth) return "key rejected";
  return "HTTP 402 parked";   // every 402 reads as billing (OutOfCredit), whatever its body
}
}  // namespace

bool available() {
  String host, path, key;
  return resolveProvider(store::embedProvider(), host, path, key);
}

std::vector<int8_t> embed(const String& text, String& err) {
  return embedWith(text, err, store::embedProvider(), store::embedModel(), store::embedDims());
}

std::vector<int8_t> embedWith(const String& text, String& err, const String& provider,
                              const String& model, int dims) {
  err = "";
  String host, path, key;
  if (!resolveProvider(provider, host, path, key)) { err = "no embeddings key for " + provider; return {}; }
  if (text.length() == 0) { err = "empty text"; return {}; }
  if (model.length() == 0) { err = "no model"; return {}; }
  const nimbus::orch::ParkVerdict gv = gate::check(provider.c_str(), nimbus::orch::PaidSeam::Embeddings, key);
  if (!gv.open) { err = closedToken(gv); return {}; }
#ifdef NIMBUS_TEST
  {   // the bench's scripted provider answer (PROVFAULT): the refusal path, no network
    int ovStatus = 0;
    std::string ovBody;
    if (agent::providers::testStatusOverride(std::string(host.c_str()), ovStatus, ovBody) && ovStatus >= 300) {
      alogf("embed: TEST override answered %s with HTTP %d", host.c_str(), ovStatus);
      failFromStatus(provider, key, ovStatus, ovBody.c_str(), err);
      return {};
    }
  }
#endif

  // Provider-aware body (CUM-469): mistral gets {model, input} - its schema has no
  // `dimensions`, which is why the one OpenAI-shaped body 422'd on every call.
  std::string body = nimbus::orch::buildEmbeddingRequest(
      std::string(provider.c_str()), std::string(model.c_str()), std::string(text.c_str()), dims);

  const bool web = nimbus::sys::onWebTask();   // the web task: its network budget (web_guard.h)
  if (web && !webDnsReady(host)) { err = "tls busy"; return {}; }
  if (!arbiter::acquireWork(12000)) { err = "tls busy"; return {}; }

  std::vector<int8_t> out;
  {
    // From a web request (the AsyncTCP task, which the task watchdog watches) the whole
    // exchange fits nimbus::sys::kWebNetBudgetMs (web_guard.h): one connect within the
    // budget's connect and handshake shares, the reply read against the same clock.
    const uint32_t startedMs = millis();
    WiFiClientSecure client;
    tlsSetup(client);
    client.setHandshakeTimeout(web ? nimbus::sys::kWebNetHandshakeS : 12);
    client.setConnectionTimeout(web ? nimbus::sys::kWebNetConnectMs : EMBED_TIMEOUT_MS);  // F25: real socket bound

    // Back-to-back handshakes to the same host intermittently fail (each chat
    // adapter learned this live and retries) - and a mid-turn mem_write is
    // dispatched milliseconds after the provider round's socket was RST-closed,
    // exactly inside that window. The old single-attempt connect was why the
    // model's mem_write kept failing "embedding unavailable" DURING turns while
    // the idle web embed-verify succeeded (owner field bug 2026-07-16). Same
    // 3-attempt + fresh-socket + 400 ms settle pattern as openai_adapter.
    bool connected = false;
    const int attempts = web ? 1 : 3;
    for (int attempt = 0; attempt < attempts && !connected; attempt++) {
      if (client.connect(host.c_str(), 443)) { connected = true; break; }
      tlsClose(client);
      if (attempt < attempts - 1) vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (!connected) {
      arbiter::releaseWork();
      gate::noteAnswer(provider.c_str(), nimbus::orch::PaidSeam::Embeddings, key, 0, "");
      err = "connect failed";
      alogf("embed: connect %s failed x3 heap=%u", host.c_str(), ESP.getFreeHeap());
      return {};
    }

    String req = String("POST ") + path + " HTTP/1.0\r\n"
               + "Host: " + host + "\r\n"
               + "Authorization: Bearer " + key + "\r\n"
               + "Content-Type: application/json\r\n"
               + "Content-Length: " + (int)body.size() + "\r\n"
               + "Connection: close\r\n\r\n";
    client.print(req);
    client.print(body.c_str());

    uint32_t deadline = (web ? startedMs : millis()) + nimbus::sys::netBudgetMs(web, EMBED_TIMEOUT_MS);
    // status line
    int code = 0;
    String status;
    while ((int32_t)(millis() - deadline) < 0) {
      if (client.available()) { char c = client.read(); if (c == '\n') break; if (c != '\r') status += c; }
      else if (!client.connected() && !client.available()) break;
      else delay(2);
    }
    int sp = status.indexOf(' ');
    if (sp > 0 && (int)status.length() >= sp + 4) code = status.substring(sp + 1, sp + 4).toInt();

    // skip headers
    String line; bool headersDone = false;
    while (!headersDone && (int32_t)(millis() - deadline) < 0) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') { if (line.length() == 0) headersDone = true; line = ""; }
        else if (c != '\r') line += c;
      } else if (!client.connected() && !client.available()) break;
      else delay(2);
    }

    // buffer the body (a 256-float embedding is a few KB - bufferable on the S3)
    String bodyResp;
    bodyResp.reserve(4096);
    while ((int32_t)(millis() - deadline) < 0) {
      if (client.available()) bodyResp += (char)client.read();
      else if (!client.connected() && !client.available()) break;
      else delay(2);
    }
    tlsClose(client);
    arbiter::releaseWork();

    // No status line parsed: separate a read timeout (deadline exceeded) from the
    // provider dropping the connection, so the memory tools can say which (CUM-435).
    if (code == 0) {
      gate::noteAnswer(provider.c_str(), nimbus::orch::PaidSeam::Embeddings, key, 0, "");
      const bool deadlinePassed = (int32_t)(millis() - deadline) >= 0;
      err = deadlinePassed ? "timeout" : "connect failed";
      alogf("embed: no status (%s) heap=%u", err.c_str(), ESP.getFreeHeap());
      return {};
    }
    if (code != 200) {
      failFromStatus(provider, key, code, bodyResp.c_str(), err);
      return {};
    }
    gate::noteAnswer(provider.c_str(), nimbus::orch::PaidSeam::Embeddings, key, code, "");
    // Lane DEF: an answered embedding is billed: its reported usage (else an estimate
    // from the text) counts toward the monthly spend ceiling.
    nimbus::orch::meterSeamAnswerBody(nimbus::orch::SpendSeam::Embed, provider.c_str(),
                                      model.c_str(), bodyResp.c_str(), text.length(), 0);

    std::vector<float> floats;
    std::string perr;
    if (!nimbus::orch::parseEmbeddingResponse(bodyResp.c_str(), dims, floats, perr)) {
      err = String("parse: ") + perr.c_str();
      alogf("embed: parse failed: %s", perr.c_str());
      return {};
    }
    out = nimbus::orch::VectorMemory::quantize(floats);
  }
  return out;
}

}  // namespace embeddings
}  // namespace agent
