#include "audio_tts.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "../../sys/net_util.h"      // tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../agent_config.h"         // CUMULO_HOST_DEFAULT
#include "../store.h"
#include "../../sys/agent_log.h"
#include "b64_stream.h"              // shared base64 socket->file stream decoder
#include "nimbus/tts_catalog.h"     // core::speakerTtsFormat (host-tested playback format)
#include "nimbus/orch/voice_route.h" // voiceActiveProvider / voiceRouteFor (host-tested routing)
#include "nimbus/audio_req.h"       // core::parseErrorCode (host-tested refusal parse)

namespace agent {
namespace tts {

namespace {
// Strip scheme + any path from a stored base, leaving a bare host for connect().
// (A stored cumuloBase may be a full URL; same shape as embeddings::bareEmbedHost.)
String bareHost(String h) {
  int s = h.indexOf("://");
  if (s >= 0) h = h.substring(s + 3);
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  return h;
}

// Resolve the EFFECTIVE TTS provider to host / path / model / key / voice. openai +
// mistral (Voxtral) hit the provider's own /v1/audio/speech; a Cumulo-only device
// (no BYOK key, a cumulo key) routes through the router at
// /router/openai/v1/audio/speech with the single router key (mirrors the embeddings
// viaCumuloRouter pattern, CUM-302). `mistral` is the WIRE shape: a direct Mistral
// call emits base64 MP3 and ignores response_format; openai AND cumulo (openai
// upstream) are raw-binary + response_format-honoring, so cumulo has mistral=false.
struct TtsProvider { bool mistral; String host; const char* path; const char* model; String key; String voice; };
TtsProvider resolve(const char* voice) {
  const String cfg = store::ttsProvider();
  const String eff = activeProvider();
  const nimbus::orch::VoiceRouteInfo r =
      nimbus::orch::voiceRouteFor(std::string(eff.c_str()), nimbus::orch::VoiceKind::Tts);
  // Voice precedence: explicit arg > stored voice (ONLY when the effective provider
  // IS the configured one) > route default. A stored voice slug is provider-specific
  // (a Mistral slug 400s on OpenAI, and vice versa), so on any key-driven fallback to
  // a different provider we drop it and use that route's default voice.
  const bool storedApplies = (eff == cfg);
  const String stored = storedApplies ? store::ttsVoice() : String();
  auto pick = [&](const char* dflt) -> String {
    if (voice && voice[0]) return String(voice);
    return stored.length() ? stored : String(dflt);
  };
  TtsProvider p;
  p.mistral = r.mistralShape;
  p.path = r.path;
  p.model = r.model;
  p.voice = pick(r.voiceDefault);
  if (r.viaCumuloRouter) {
    String base = store::cumuloBase(); if (!base.length()) base = CUMULO_HOST_DEFAULT;
    p.host = bareHost(base);
    p.key = store::cumuloKey();
  } else if (eff == "openai") {
    p.host = "api.openai.com"; p.key = store::openaiKey();
  } else {  // mistral
    p.host = "api.mistral.ai"; p.key = store::mistralKey();
  }
  return p;
}

}  // namespace

String activeProvider() {
  // The provider that will actually voice this request: the configured one, or a
  // fallback when it has no key but another provider does, so the device still speaks
  // instead of going silent. Adds cumulo as a valid effective provider - a one-key
  // device (no BYOK key, a cumulo key) voices through the router (CUM-376).
  // speakOnDevice derives the playback format from THIS (core::speakerTtsFormat), so
  // the format always matches the synthesizing provider. The decision is the
  // host-tested nimbus::orch::voiceActiveProvider.
  std::string eff = nimbus::orch::voiceActiveProvider(
      std::string(store::ttsProvider().c_str()), store::openaiKey().length() > 0,
      store::mistralKey().length() > 0, store::cumuloKey().length() > 0);
  return String(eff.c_str());
}

bool available() {
  // True when the effective provider has a usable key - a direct openai/mistral key
  // or the Cumulo router key (the one-key device voices through the router).
  return resolve(nullptr).key.length() > 0;
}

size_t synthesizeToFile(const String& text, const char* outPath,
                        const char* format, const char* voice) {
  if (text.length() == 0 || !outPath || !outPath[0]) return 0;
  const bool needWav = (format && strcmp(format, "wav") == 0);
  TtsProvider prov = resolve(voice);
  if (needWav && prov.mistral) {
    // A WAV was requested but Mistral (Voxtral) emits MP3 only. Do NOT synthesize:
    // Mistral ignores response_format and would return MP3, which written into a
    // .wav is exactly the field bug (playWavFile then rejects it). The on-device
    // speak path asks Mistral for "mp3" instead (minimp3 plays it); this guard just
    // protects any stray WAV caller (e.g. a console command) from that mismatch.
    alog("tts: wav requested but provider mistral emits mp3 only - not synthesizing");
    return 0;
  }
  if (prov.key.length() == 0) { alogf("tts: no key for provider %s", store::ttsProvider().c_str()); return 0; }

  JsonDocument doc;
  doc["model"] = prov.model;
  doc["input"] = text;
  doc["voice"] = prov.voice;
  // OpenAI honors response_format (wav for the speaker, mp3 for Telegram). Mistral
  // ignores it and always returns MP3 (base64 in JSON), so don't send the field.
  if (!prov.mistral) doc["response_format"] = (format && format[0]) ? format : "mp3";
  String body;
  serializeJson(doc, body);

  if (!arbiter::acquireWork(10000)) { alog("tts: arbiter busy"); return 0; }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  // F25: bound the socket (setTimeout is inert on this client) + ride ONE wall
  // clock over connect + I/O so a half-open cellular NAT can't wedge the speak
  // turn (this path is what hung tg_poll 10+ min live).
  const uint32_t deadline = millis() + 25000;
  c.setConnectionTimeout(25000);
  bool connected = false;
  for (int a = 0; a < 3 && !connected && (int32_t)(millis() - deadline) < 0; a++) {
    if (c.connect(prov.host.c_str(), 443)) { connected = true; break; }
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) { arbiter::releaseWork(); alogf("tts: connect %s failed heap=%u", prov.host.c_str(), ESP.getFreeHeap()); return 0; }

  c.printf("POST %s HTTP/1.0\r\n", prov.path);
  c.printf("Host: %s\r\n", prov.host.c_str());
  c.printf("Authorization: Bearer %s\r\n", prov.key.c_str());
  c.print("Content-Type: application/json\r\n");
  c.printf("Content-Length: %u\r\n", (unsigned)body.length());
  c.print("Connection: close\r\n\r\n");
  c.print(body);

  char line[512];
  auto readLine = [&](char* b, int cap) -> int {
    int i = 0;
    while ((int32_t)(millis() - deadline) < 0 && i < cap - 1) {
      if (c.available()) { char ch = c.read(); if (ch == '\n') break; if (ch != '\r') b[i++] = ch; }
      else if (!c.connected()) break;
      else vTaskDelay(1);
    }
    b[i] = 0; return i;
  };
  int status = 0;
  if (readLine(line, sizeof(line)) > 0) { const char* sp = strchr(line, ' '); if (sp) status = atoi(sp + 1); }
  while (readLine(line, sizeof(line)) > 0) { /* skip headers */ }
  if (status < 200 || status >= 300) {
    char errb[160] = {}; int n = 0;
    while ((int32_t)(millis() - deadline) < 0 && n < (int)sizeof(errb) - 1 && (c.available() || c.connected())) {
      if (c.available()) errb[n++] = c.read(); else vTaskDelay(1);
    }
    tlsClose(c); arbiter::releaseWork();
    // Log the refusal honestly (a router 4xx is {"error":<code>}); the spoken reply
    // is supplementary, so the turn's TEXT reply still reaches the owner - no silence.
    bool jok = false;
    std::string code = core::parseErrorCode(errb, &jok);
    if (jok && !code.empty())
      alogf("tts: %s HTTP %d refused (%s): %s", store::ttsProvider().c_str(), status,
            code.c_str(), nimbus::orch::voiceRefusalStatus(code).c_str());
    else
      alogf("tts: %s HTTP %d: %.100s", store::ttsProvider().c_str(), status, errb);
    return 0;
  }

  File f = LittleFS.open(outPath, FILE_WRITE);
  if (!f) { tlsClose(c); arbiter::releaseWork(); alog("tts: fs open fail"); return 0; }
  size_t total = 0;
  if (prov.mistral) {
    // Body is {"audio_data":"<base64 mp3>"} - skip to the value, stream-decode it.
    String head; const char* MARK = "\"audio_data\":\"";
    while ((int32_t)(millis() - deadline) < 0 && head.length() < 512) {
      if (c.available()) {
        head += (char)c.read();
        int idx = head.indexOf(MARK);
        if (idx >= 0) { total = b64decodeToFile(c, f, head.substring(idx + strlen(MARK)), deadline); break; }
      } else if (!c.connected()) break;
      else vTaskDelay(1);
    }
  } else {
    // OpenAI: raw binary audio body.
    uint8_t buf[512];
    while ((int32_t)(millis() - deadline) < 0) {
      if (c.available()) { int r = c.read(buf, sizeof(buf)); if (r > 0) { f.write(buf, r); total += r; } }
      else if (!c.connected()) break;
      else vTaskDelay(1);
    }
  }
  f.close();
  tlsClose(c);
  arbiter::releaseWork();
  alogf("tts: %s wrote %u bytes -> %s", prov.mistral ? "mistral" : "openai",
        (unsigned)total, outPath);
  return total;
}

}  // namespace tts
}  // namespace agent
