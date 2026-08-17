#include "audio_tts.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "../../sys/net_util.h"      // tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../store.h"
#include "../../sys/agent_log.h"
#include "b64_stream.h"              // shared base64 socket->file stream decoder

namespace agent {
namespace tts {

namespace {
// Resolve the configured TTS provider. Mistral (Voxtral) is the default; both use
// POST /v1/audio/speech but the response shapes differ (see below).
struct TtsProvider { bool mistral; const char* host; const char* model; String key; String voice; };
TtsProvider resolve(const char* voice, bool needWav) {
  String p = store::ttsProvider();
  // Voice precedence: explicit arg > user-selected (store) > provider default.
  String vc = (voice && voice[0]) ? String(voice) : store::ttsVoice();
  // Field bug 2026-07-16 ("the speaker def works so something in the tts flow
  // isn't working"): the SPEAKER plays canonical PCM WAV only, but Mistral's TTS
  // emits MP3 exclusively - with ttsProv=mistral every on-device speech request
  // wrote MP3 bytes into /reply.wav, playWavFile refused them, and the agent
  // reported "speaker unavailable". When the caller NEEDS WAV, reroute to OpenAI
  // (the one WAV-capable provider) if it has a key - and use OpenAI's default
  // voice, never the stored MISTRAL voice slug (OpenAI would 400 on it). The
  // owner's Mistral voice preference still applies to every MP3 path (Telegram).
  if (needWav && p != "openai" && store::openaiKey().length())
    return {false, "api.openai.com", "gpt-4o-mini-tts", store::openaiKey(),
            String("alloy")};
  if (p == "openai")
    return {false, "api.openai.com", "gpt-4o-mini-tts", store::openaiKey(),
            vc.length() ? vc : String("alloy")};
  return {true, "api.mistral.ai", "voxtral-mini-tts-latest", store::mistralKey(),
          vc.length() ? vc : String("en_paul_neutral")};
}

}  // namespace

bool available() {
  String p = store::ttsProvider();
  return (p == "openai") ? store::hasOpenaiKey() : store::hasMistralKey();
}

size_t synthesizeToFile(const String& text, const char* outPath,
                        const char* format, const char* voice) {
  if (text.length() == 0 || !outPath || !outPath[0]) return 0;
  const bool needWav = (format && strcmp(format, "wav") == 0);
  TtsProvider prov = resolve(voice, needWav);
  if (needWav && prov.mistral) {
    // No WAV-capable provider available: Mistral only emits MP3 and the speaker
    // can't play it. Fail LOUDLY and precisely - the old path wrote the MP3 into
    // the .wav and let playback fail as "speaker unavailable".
    alog("tts: on-device speech needs WAV (OpenAI); Mistral emits MP3 only and no OpenAI key is set");
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
    if (c.connect(prov.host, 443)) { connected = true; break; }
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) { arbiter::releaseWork(); alogf("tts: connect %s failed heap=%u", prov.host, ESP.getFreeHeap()); return 0; }

  c.printf("POST /v1/audio/speech HTTP/1.0\r\n");
  c.printf("Host: %s\r\n", prov.host);
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
