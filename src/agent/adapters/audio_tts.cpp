#include "audio_tts.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "../../sys/net_util.h"      // tlsClose
#include "../../sys/tls_arbiter.h"   // single-TLS arena
#include "../agent_config.h"         // CUMULO_HOST_DEFAULT
#include "../provider_gate.h"        // the one per-provider park (L19-8)
#include "../store.h"
#include "../../sys/agent_log.h"
#include "b64_stream.h"              // shared base64 socket->file stream decoder
#include "nimbus/tts_catalog.h"     // core::speakerTtsFormat (host-tested playback format)
#include "nimbus/orch/voice_route.h" // voiceActiveProvider / voiceRouteFor (host-tested routing)
#include "nimbus/orch/seam_meter.h"  // meterSpeech - its spend (lane DEF)
#include "nimbus/audio_req.h"       // core::parseErrorCode (host-tested refusal parse)
#include "nimbus/orch/provider_error.h"  // providerErrLine - the one error-line shape (CUM-620)
#include "nimbus/orch/voice_notes.h"     // ttsDeadlineMs - the read window sized to the text

#include <strings.h>                     // strncasecmp (Content-Length)

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
struct TtsProvider { bool mistral; String host; const char* path; const char* model; String key; String voice;
                     std::string slug; };   // the provider the park knows it by
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
    p.slug = "cumulo";
  } else if (eff == "openai") {
    p.host = "api.openai.com"; p.key = store::openaiKey();
    p.slug = "openai";
  } else {  // mistral
    p.host = "api.mistral.ai"; p.key = store::mistralKey();
    p.slug = "mistral";
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

namespace {

void setWhy(String* why, const String& s) {
  if (why) *why = s;
}

// Connect within the request's window: up to three attempts, 400 ms apart.
bool connectTts(WiFiClientSecure& c, const String& host, uint32_t deadline) {
  for (int a = 0; a < 3 && (int32_t)(millis() - deadline) < 0; a++) {
    if (c.connect(host.c_str(), 443)) return true;
    tlsClose(c);
    if (a < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  return false;
}

// One header or status line (CR stripped), read within the window.
int readLine(WiFiClientSecure& c, char* b, int cap, uint32_t deadline) {
  int i = 0;
  while ((int32_t)(millis() - deadline) < 0 && i < cap - 1) {
    if (c.available()) { char ch = c.read(); if (ch == '\n') break; if (ch != '\r') b[i++] = ch; }
    else if (!c.connected()) break;
    else vTaskDelay(1);
  }
  b[i] = 0;
  return i;
}

// The status code, with the header block consumed; *contentLength is -1 when absent.
int readStatus(WiFiClientSecure& c, uint32_t deadline, long* contentLength) {
  char line[512];
  int status = 0;
  *contentLength = -1;
  if (readLine(c, line, sizeof(line), deadline) > 0) {
    const char* sp = strchr(line, ' ');
    if (sp) status = atoi(sp + 1);
  }
  while (readLine(c, line, sizeof(line), deadline) > 0)
    if (!strncasecmp(line, "Content-Length:", 15)) *contentLength = atol(line + 15);
  return status;
}

// A refused request: read the short error body, tell the park, log it, name it in why.
void noteRefusal(WiFiClientSecure& c, const TtsProvider& prov, int status, uint32_t deadline,
                 String* why) {
  char errb[160] = {};
  int n = 0;
  while ((int32_t)(millis() - deadline) < 0 && n < (int)sizeof(errb) - 1 && (c.available() || c.connected())) {
    if (c.available()) errb[n++] = c.read(); else vTaskDelay(1);
  }
  gate::noteAnswer(prov.slug.c_str(), nimbus::orch::PaidSeam::Tts, prov.key, status, errb);
  // Log the refusal honestly (a router 4xx is {"error":<code>}); the spoken reply
  // is supplementary, so the turn's TEXT reply still reaches the owner - no silence.
  bool jok = false;
  const std::string code = core::parseErrorCode(errb, &jok);
  if (jok && !code.empty()) {
    alogf("tts: %s HTTP %d refused (%s): %s", store::ttsProvider().c_str(), status,
          code.c_str(), nimbus::orch::voiceRefusalStatus(code).c_str());
    setWhy(why, String("the voice provider refused (HTTP ") + status + ", " + code.c_str() + ")");
    return;
  }
  const std::string line = nimbus::orch::providerErrLine(prov.slug.c_str(), status,
                                                         nimbus::orch::providerErrorText(errb));
  alogf("tts: %s", line.c_str());
  setWhy(why, String("the voice provider refused (HTTP ") + status + ")");
}

// Mistral: {"audio_data":"<base64 mp3>"} - skip to the value, stream-decode it.
// b64decodeToFile's clean flag: the closing quote arrived AND every byte was written.
void streamMistral(WiFiClientSecure& c, File& f, uint32_t deadline, nimbus::orch::ClipRead& rd) {
  String head;
  const char* MARK = "\"audio_data\":\"";
  while ((int32_t)(millis() - deadline) < 0 && head.length() < 512) {
    if (c.available()) {
      head += (char)c.read();
      const int idx = head.indexOf(MARK);
      if (idx < 0) continue;
      bool clean = false;
      rd.bytes = b64decodeToFile(c, f, head.substring(idx + strlen(MARK)), deadline, &clean);
      rd.endSeen = rd.writesOk = clean;
      return;
    }
    if (!c.connected()) return;
    vTaskDelay(1);
  }
}

// OpenAI and the router: the raw audio body, until the server closes or the stated
// length arrives (the end), or the window passes or a write fails (cut).
void streamRaw(WiFiClientSecure& c, File& f, uint32_t deadline, nimbus::orch::ClipRead& rd) {
  uint8_t buf[512];
  rd.writesOk = true;
  while (rd.writesOk && (int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      const int r = c.read(buf, sizeof(buf));
      if (r > 0) { rd.writesOk = (size_t)f.write(buf, r) == (size_t)r; rd.bytes += r; }
    } else if (!c.connected()) {
      rd.endSeen = true;
      return;
    } else {
      vTaskDelay(1);
    }
    if (rd.contentLength >= 0 && (long)rd.bytes >= rd.contentLength) { rd.endSeen = true; return; }
  }
}

// One synthesis request: the JSON body, the flash file it fills and the window it
// must finish in.
struct TtsRequest {
  const String& body;
  const char*   outPath;
  uint32_t      deadline;
};

// What one request produced: the bytes of a whole clip (0 = none), and whether the
// provider answered the request (an answered synthesis is billed, whole or not).
struct TtsResult {
  size_t bytes = 0;
  bool   answered = false;
  bool   unanswered = false;   // sent, no status line: it may have been billed (lane QW4, CUM-776)
};

// Send the request and stream the answer into outPath. Holds the work slot the
// caller took; a clip that is not whole comes back as 0 bytes with why set.
TtsResult requestAndStream(WiFiClientSecure& c, const TtsProvider& prov, const TtsRequest& rq, String* why) {
  TtsResult res;
  const String& body = rq.body;
  const char* outPath = rq.outPath;
  const uint32_t deadline = rq.deadline;
  c.printf("POST %s HTTP/1.0\r\n", prov.path);
  c.printf("Host: %s\r\n", prov.host.c_str());
  c.printf("Authorization: Bearer %s\r\n", prov.key.c_str());
  c.print("Content-Type: application/json\r\n");
  c.printf("Content-Length: %u\r\n", (unsigned)body.length());
  c.print("Connection: close\r\n\r\n");
  c.print(body);
  long contentLength = -1;
  const int status = readStatus(c, deadline, &contentLength);
  if (status < 200 || status >= 300) {
    noteRefusal(c, prov, status, deadline, why);
    if (status == 0) setWhy(why, "the voice provider did not answer in time");
    res.unanswered = status == 0;
    return res;
  }
  res.answered = true;
  gate::noteAnswer(prov.slug.c_str(), nimbus::orch::PaidSeam::Tts, prov.key, status, "");
  File f = LittleFS.open(outPath, FILE_WRITE);
  if (!f) {
    alog("tts: fs open fail");
    setWhy(why, "the clip could not be written to flash");
    return res;
  }
  nimbus::orch::ClipRead rd;
  rd.mistral = prov.mistral;
  rd.contentLength = contentLength;
  if (prov.mistral) streamMistral(c, f, deadline, rd);
  else streamRaw(c, f, deadline, rd);
  f.close();
  const char* name = prov.mistral ? "mistral" : "openai";
  if (!nimbus::orch::clipWhole(rd)) {
    // A cut clip is never used (22 Oct: a clip cut at the 25 s window was logged as
    // written and queued as whole). Remove it so nothing can pick it up.
    LittleFS.remove(outPath);
    alogf("tts: %s %s -> %s, removed", name, nimbus::orch::clipTruncatedText(rd.bytes).c_str(), outPath);
    setWhy(why, nimbus::orch::clipTruncatedText(rd.bytes).c_str());
    return res;
  }
  alogf("tts: %s wrote %u bytes -> %s", name, (unsigned)rd.bytes, outPath);
  res.bytes = rd.bytes;
  return res;
}

}  // namespace

size_t synthesizeToFile(const String& text, const char* outPath,
                        const char* format, const char* voice, String* why) {
  setWhy(why, "");
  if (text.length() == 0 || !outPath || !outPath[0]) { setWhy(why, "nothing to say"); return 0; }
  const bool needWav = (format && strcmp(format, "wav") == 0);
  TtsProvider prov = resolve(voice);
  if (needWav && prov.mistral) {
    // A WAV was requested but Mistral (Voxtral) emits MP3 only. Do NOT synthesize:
    // Mistral ignores response_format and would return MP3, which written into a
    // .wav is exactly the field bug (playWavFile then rejects it). The on-device
    // speak path asks Mistral for "mp3" instead (minimp3 plays it); this guard just
    // protects any stray WAV caller (e.g. a console command) from that mismatch.
    alog("tts: wav requested but provider mistral emits mp3 only - not synthesizing");
    setWhy(why, "this voice provider makes MP3 only");
    return 0;
  }
  if (prov.key.length() == 0) {
    alogf("tts: no key for provider %s", store::ttsProvider().c_str());
    setWhy(why, "no voice provider key is set");
    return 0;
  }
  // L19-8: a provider the one park closed (out of credit or a refused key learned on
  // any seam, or paused) gets no request; the turn's text reply still reaches the owner.
  if (!gate::check(prov.slug.c_str(), nimbus::orch::PaidSeam::Tts, prov.key).open) {
    setWhy(why, "the voice provider is paused");
    return 0;
  }

  JsonDocument doc;
  doc["model"] = prov.model;
  doc["input"] = text;
  doc["voice"] = prov.voice;
  // OpenAI honors response_format (wav for the speaker, mp3 for Telegram). Mistral
  // ignores it and always returns MP3 (base64 in JSON), so don't send the field.
  if (!prov.mistral) doc["response_format"] = (format && format[0]) ? format : "mp3";
  String body;
  serializeJson(doc, body);

  arbiter::WorkSlot slot(10000);
  if (!slot) {
    alog("tts: arbiter busy");
    setWhy(why, "the network was busy");
    return 0;
  }
  WiFiClientSecure c;
  tlsSetup(c);
  c.setHandshakeTimeout(12);
  // F25: bound the socket (setTimeout is inert on this client) + ride ONE wall
  // clock over connect + I/O so a half-open cellular NAT can't wedge the speak
  // turn (this path is what hung tg_poll 10+ min live). The clock is sized to the
  // text (voice_notes.h): a long reply needs longer than the old fixed 25 s.
  const uint32_t windowMs = nimbus::orch::ttsDeadlineMs(nimbus::orch::textChars(text.c_str()));
  const uint32_t deadline = millis() + windowMs;
  c.setConnectionTimeout(windowMs);
  TtsResult res;
  if (!connectTts(c, prov.host, deadline)) {
    gate::noteAnswer(prov.slug.c_str(), nimbus::orch::PaidSeam::Tts, prov.key, 0, "");
    alogf("tts: connect %s failed heap=%u", prov.host.c_str(), ESP.getFreeHeap());
    setWhy(why, String("could not reach ") + prov.host);
  } else {
    res = requestAndStream(c, prov, TtsRequest{body, outPath, deadline}, why);
  }
  tlsClose(c);
  slot.release();
  // Lane DEF: an answered synthesis is billed per character of the text (the answer is
  // audio, with no usage), counted toward the monthly spend ceiling, even when the
  // clip arrived cut and is not used. Lane QW4 (CUM-776): so is a request sent with no
  // answer (the provider may have synthesized and billed it).
  if (res.answered || res.unanswered) nimbus::orch::meterSpeech(prov.slug.c_str(), prov.model, text.length());
  return res.bytes;
}

}  // namespace tts
}  // namespace agent
