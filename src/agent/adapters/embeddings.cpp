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

// Resolve the configured provider's host + key. Returns false if unknown/no key.
bool resolveProvider(const String& provider, const char*& host, String& key) {
  if (provider == "openai")  { host = OPENAI_HOST;  key = store::openaiKey();  return key.length() > 0; }
  if (provider == "mistral") { host = MISTRAL_HOST; key = store::mistralKey(); return key.length() > 0; }
  return false;  // anthropic has no public embeddings API; custom is future work
}
}  // namespace

bool available() {
  const char* host = nullptr;
  String key;
  return resolveProvider(store::embedProvider(), host, key);
}

std::vector<int8_t> embed(const String& text, String& err) {
  return embedWith(text, err, store::embedProvider(), store::embedModel(), store::embedDims());
}

std::vector<int8_t> embedWith(const String& text, String& err, const String& provider,
                              const String& model, int dims) {
  err = "";
  const char* host = nullptr;
  String key;
  if (!resolveProvider(provider, host, key)) { err = "no embeddings key for " + provider; return {}; }
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
      if (client.connect(host, 443)) { connected = true; break; }
      tlsClose(client);
      if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (!connected) {
      arbiter::releaseWork();
      err = "connect failed";
      alogf("embed: connect %s failed x3 heap=%u", host, ESP.getFreeHeap());
      return {};
    }

    String req = String("POST ") + OPENAI_EMBED_PATH + " HTTP/1.0\r\n"
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
