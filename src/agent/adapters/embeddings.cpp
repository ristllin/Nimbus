#include "embeddings.h"

#include <WiFiClientSecure.h>

#include "../agent_config.h"
#include "../../sys/agent_log.h"
#include "../../sys/net_util.h"      // tlsClose
#include "../store.h"
#include "../../sys/tls_arbiter.h"
#include "nimbus/orch/embedding.h"       // portable build/parse (host-tested)
#include "nimbus/orch/vector_memory.h"   // quantize

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

  std::string body = nimbus::orch::buildEmbeddingRequest(
      std::string(model.c_str()), std::string(text.c_str()), dims);

  if (!arbiter::acquireWork(12000)) { err = "tls busy"; return {}; }

  std::vector<int8_t> out;
  {
    WiFiClientSecure client;
    tlsSetup(client);
    client.setHandshakeTimeout(12);
    client.setConnectionTimeout(EMBED_TIMEOUT_MS);  // F25: real socket bound

    // Back-to-back handshakes to the same host intermittently fail (each chat
    // adapter learned this live and retries) - and a mid-turn mem_write is
    // dispatched milliseconds after the provider round's socket was RST-closed,
    // exactly inside that window. The old single-attempt connect was why the
    // model's mem_write kept failing "embedding unavailable" DURING turns while
    // the idle web embed-verify succeeded (owner field bug 2026-07-16). Same
    // 3-attempt + fresh-socket + 400 ms settle pattern as openai_adapter.
    bool connected = false;
    for (int attempt = 0; attempt < 3 && !connected; attempt++) {
      if (client.connect(host.c_str(), 443)) { connected = true; break; }
      tlsClose(client);
      if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (!connected) {
      arbiter::releaseWork();
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

    uint32_t deadline = millis() + EMBED_TIMEOUT_MS;
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

    if (code == 401 || code == 403) { err = "key rejected"; return {}; }
    if (code != 200)                { err = String("HTTP ") + code; alogf("embed: HTTP %d", code); return {}; }

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
