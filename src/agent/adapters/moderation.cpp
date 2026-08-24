#include "moderation.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include "../store.h"
#include "../../sys/agent_log.h"
#include "../../sys/net_util.h"      // tlsSetup / tlsClose
#include "../../sys/tls_arbiter.h"

namespace agent {
namespace moderation {

using nimbus::orch::ClassifierVerdict;
using nimbus::orch::ModGate;
using nimbus::orch::ModProvider;

// Read one CRLF-terminated line (bounded by deadline). Local copy so this TU is
// self-contained (url_fetch's readLine is file-static).
static bool readLine(WiFiClientSecure& c, String& out, uint32_t deadline) {
  out = "";
  while (millis() < deadline) {
    if (!c.connected() && !c.available()) return false;
    while (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') { if (out.endsWith("\r")) out.remove(out.length() - 1); return true; }
      out += ch;
      if (out.length() > 4096) return true;   // never unbounded
    }
    delay(2);
  }
  return false;
}

// POST a Mistral /v1/moderations request; flagged = any category true. Returns the
// verdict, or Error on any failure. Cumulo endpoint is a documented contract to N1
// (no Cumulo provider key exists yet), so pickProvider currently resolves Mistral.
static ClassifierVerdict classifyMistral(const std::string& text, uint32_t acquireMs) {
  const String key = store::mistralKey();
  if (!key.length()) return ClassifierVerdict::Error;

  JsonDocument d;
  d["model"] = "mistral-moderation-latest";
  d["input"].add(text);   // array of one
  String body; serializeJson(d, body);

  if (!arbiter::acquireWork(acquireMs)) { alog("moderation: device busy (TLS slot)"); return ClassifierVerdict::Error; }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(15);
  c.setConnectionTimeout(15000);
  if (!c.connect("api.mistral.ai", 443)) { arbiter::releaseWork(); alog("moderation: connect failed"); return ClassifierVerdict::Error; }
  c.printf("POST /v1/moderations HTTP/1.0\r\nHost: api.mistral.ai\r\n");
  c.printf("Authorization: Bearer %s\r\n", key.c_str());
  c.printf("Content-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
           (unsigned)body.length());
  c.print(body);

  const uint32_t deadline = millis() + 30000;
  String line;
  if (!readLine(c, line, deadline)) { tlsClose(c); arbiter::releaseWork(); alog("moderation: no response"); return ClassifierVerdict::Error; }
  while (readLine(c, line, deadline) && line.length() > 0) {}   // skip headers

  // Filtered parse: only the per-category booleans of the first result.
  JsonDocument filter;
  filter["results"][0]["categories"] = true;
  JsonDocument resp;
  DeserializationError je = deserializeJson(resp, c, ArduinoJson::DeserializationOption::Filter(filter));
  tlsClose(c);
  arbiter::releaseWork();
  if (je) { alog("moderation: bad response"); return ClassifierVerdict::Error; }

  JsonObject cats = resp["results"][0]["categories"];
  if (cats.isNull()) return ClassifierVerdict::Error;   // no verdict body
  for (JsonPair kv : cats) {
    if (kv.value().as<bool>()) {
      alogf("moderation: flagged (%s)", kv.key().c_str());
      return ClassifierVerdict::Flag;
    }
  }
  return ClassifierVerdict::Allow;
}

ClassifierVerdict classify(const std::string& text, ModGate /*gate*/, uint32_t acquireMs) {
  if (text.empty()) return ClassifierVerdict::Allow;   // nothing to screen
  // No Cumulo provider key concept exists on-device yet (contract with N1), so
  // hasCumuloKey=false and this resolves to Mistral when a Mistral key is set.
  ModProvider p = nimbus::orch::pickProvider(/*cumulo=*/false, store::mistralKey().length() > 0);
  switch (p) {
    case ModProvider::Mistral: return classifyMistral(text, acquireMs);
    case ModProvider::Cumulo:  // fall through until the Cumulo endpoint lands
    case ModProvider::None:
    default:                   return ClassifierVerdict::Error;
  }
}

}  // namespace moderation
}  // namespace agent
