#include "tts_voices.h"

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include <set>
#include <string>

#include "../../sys/agent_log.h"
#include "../../sys/net_util.h"     // tlsClose
#include "../store.h"
#include "../../sys/tls_arbiter.h"
#include "nimbus/tts_catalog.h"     // core::mergeMistralVoicesPage (shared, host-tested)

namespace agent {
namespace ttsvoices {

// Verified static sets (2026-07): OpenAI gpt-4o-mini-tts (no listing endpoint) and a
// Mistral fallback used until the live /v1/audio/voices fetch lands.
static const char* kOpenaiStatic =
  R"([{"value":"alloy","label":"Alloy - neutral"},{"value":"ash","label":"Ash - male"},)"
  R"({"value":"ballad","label":"Ballad - male, British"},{"value":"coral","label":"Coral - female"},)"
  R"({"value":"echo","label":"Echo - male"},{"value":"fable","label":"Fable - male, British"},)"
  R"({"value":"onyx","label":"Onyx - deep male"},{"value":"nova","label":"Nova - female"},)"
  R"({"value":"sage","label":"Sage - female"},{"value":"shimmer","label":"Shimmer - female"},)"
  R"({"value":"verse","label":"Verse - male"},{"value":"marin","label":"Marin - female"},)"
  R"({"value":"cedar","label":"Cedar - male"}])";

// Structured fields (P4 voices split): name/gender/lang/emotion let the web UI
// offer cascading gender -> persona -> emotion pickers instead of one flat list.
// The stored NVS value stays the final slug - no migration.
//
// This is the OFFLINE fallback only - shown before the background fetch completes
// or if the provider is unreachable (in which case TTS wouldn't work anyway). It
// carries one neutral per persona so every gender/language/persona axis is present
// and nothing is misleading; the live fetch (fetchMistral) supplies the full
// 30-voice catalog with all emotions.
static const char* kMistralFallback =
  R"([{"value":"en_paul_neutral","label":"Paul (male, US) - neutral","name":"Paul","gender":"male","lang":"US","emotion":"neutral"},)"
  R"({"value":"gb_oliver_neutral","label":"Oliver (male, UK) - neutral","name":"Oliver","gender":"male","lang":"UK","emotion":"neutral"},)"
  R"({"value":"gb_jane_neutral","label":"Jane (female, UK) - neutral","name":"Jane","gender":"female","lang":"UK","emotion":"neutral"},)"
  R"({"value":"fr_marie_neutral","label":"Marie (female, FR) - neutral","name":"Marie","gender":"female","lang":"FR","emotion":"neutral"}])";

static String        g_mistralLive;          // cached live catalog (built from the API)
static volatile bool g_want = false;          // a mistral fetch has been requested
static volatile bool g_have = false;          // the live catalog is cached

// Read one HTTP/1.0 response into `body` (headers stripped), returning the status.
// Bounded by a wall-clock deadline and a byte cap (a page of 10 voices is ~3.4 KB).
static int readVoicesPage(WiFiClientSecure& c, String& body) {
  uint32_t deadline = millis() + 20000;
  int status = 0; String line; bool firstLine = true, inBody = false;
  while ((int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      char ch = c.read();
      if (!inBody) {
        if (ch == '\n') {
          if (firstLine) { int sp = line.indexOf(' '); if (sp > 0) status = line.substring(sp + 1, sp + 4).toInt(); firstLine = false; }
          else if (line.length() == 0) inBody = true;
          line = "";
        } else if (ch != '\r') { line += ch; }
      } else if (body.length() < 12000) { body += ch; }
    } else if (!c.connected()) { break; }
    else { delay(2); }
  }
  return status;
}

// GET https://api.mistral.ai/v1/audio/voices, PAGINATED via offset -> the full
// catalog as [{value,label,name,gender,lang,emotion}, ...].
//
// ⚠ The endpoint ignores the `page` field (always serves the first slice) and
// echoes a misleading page/total_pages in the body; only `offset`/`limit` advance
// it. So we walk `offset` and stop when a page adds no NEW voice (dedup by slug in
// the shared core helper) - which also protects against a server that ignores
// `offset` too. A hard page cap backstops any runaway.
static bool fetchMistral(String& out) {
  String key = store::mistralKey();
  if (!key.length()) return false;

  std::set<std::string> seen;
  std::string rows;
  int offset = 0, total = -1;
  bool anyHttpOk = false;

  for (int page = 0; page < 12; page++) {  // 12 pages (>=120 voices) safety backstop
    if (!arbiter::acquireWork(20000)) { alog("voices: tls busy"); break; }
    String body;
    int status = 0; bool connected = false;
    {
      WiFiClientSecure c;
      tlsSetup(c);
      c.setHandshakeTimeout(12);
      c.setConnectionTimeout(15000);  // F25: real socket bound (setTimeout inert)
      connected = c.connect("api.mistral.ai", 443);
      if (connected) {
        c.printf("GET /v1/audio/voices?type=all&limit=100&offset=%d HTTP/1.0\r\n", offset);
        c.print("Host: api.mistral.ai\r\n");
        c.printf("Authorization: Bearer %s\r\n", key.c_str());
        c.print("User-Agent: Nimbus\r\nConnection: close\r\n\r\n");
        status = readVoicesPage(c, body);
      }
      tlsClose(c);
    }
    arbiter::releaseWork();

    if (!connected) { alogf("voices: connect failed heap=%u", ESP.getFreeHeap()); break; }
    if (status < 200 || status >= 300) { alogf("voices: HTTP %d", status); break; }
    anyHttpOk = true;

    int added = 0;
    int processed = core::mergeMistralVoicesPage(body.c_str(), seen, rows, &added, &total);
    if (processed <= 0 || added == 0) break;      // exhausted, or nothing new -> done
    offset += processed;
    if (total > 0 && offset >= total) break;      // collected the whole catalog
  }

  if (!anyHttpOk || seen.empty()) return false;
  out = "[";
  out += rows.c_str();
  out += "]";
  alogf("voices: mistral catalog %u voices", (unsigned)seen.size());
  return true;
}

static void fetchTask(void*) {
  for (;;) {
    if (g_want && !g_have) {
      String live;
      if (fetchMistral(live)) { g_mistralLive = live; g_have = true; alog("voices: mistral catalog cached"); }
      g_want = false;   // one attempt per request; re-request re-tries
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void begin() {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreate(fetchTask, "voices", 6144, nullptr, 1, nullptr);
}

String voicesJson(const String& provider) {
  if (provider == "openai") return String(kOpenaiStatic);
  // mistral (default): live once cached, else fallback + request a fetch.
  if (g_have) return g_mistralLive;
  g_want = true;
  return String(kMistralFallback);
}

}  // namespace ttsvoices
}  // namespace agent
