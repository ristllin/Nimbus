#include "audio_stt.h"

#include <LittleFS.h>

#include <string>

#include "nimbus/audio_req.h"   // core::parseTranscription (shared, host-tested parse)
#include "nimbus/orch/voice_route.h"   // voiceActiveProvider / voiceRouteFor / voiceRefusalStatus
#include "http_multipart.h"
#include "../agent_config.h"           // CUMULO_HOST_DEFAULT
#include "../store.h"
#include "../../sys/agent_log.h"

// STT diagnostics: log the full transcription path (provider, file size, HTTP
// outcome, raw response, parsed text) through alogf so a silent "Didn't catch that"
// is root-causable over HTTP (GET /api/log) without opening serial (which drops WiFi).
#define STTDIAG(...) ::agent::alogf("[stt] " __VA_ARGS__)

namespace agent {
namespace stt {

// Strip scheme + any path from a stored base, leaving a bare host for connect().
// (A stored cumuloBase may be a full URL; same shape as embeddings::bareEmbedHost.)
static String bareHost(String h) {
  int s = h.indexOf("://");
  if (s >= 0) h = h.substring(s + 3);
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  return h;
}

// The honest one-line status of the LAST transcribe attempt when it failed with a
// router/provider refusal (JSON {error:<code>}); "" when the last attempt succeeded
// or failed some other way. The mic path reads this to surface an honest line
// instead of the generic "Didn't catch that" - never silence (CUM-376).
static String s_lastStatus;

// Resolve the EFFECTIVE STT provider to its host / path / transcription model / key.
// openai + mistral (Voxtral) hit the provider's own /v1/audio/transcriptions; a
// Cumulo-only device (no BYOK key, a cumulo key) routes through the router at
// /router/openai/v1/audio/transcriptions with the single router key, mirroring the
// embeddings viaCumuloRouter pattern (CUM-302). All three POST multipart and return
// {"text":...}, so only host/path/model/key differ.
struct SttProvider { String host; const char* path; const char* model; String key; };
static SttProvider resolve() {
  const std::string eff = nimbus::orch::voiceActiveProvider(
      std::string(store::sttProvider().c_str()), store::hasOpenaiKey(),
      store::hasMistralKey(), store::hasCumuloKey());
  const nimbus::orch::VoiceRouteInfo r =
      nimbus::orch::voiceRouteFor(eff, nimbus::orch::VoiceKind::Stt);
  SttProvider p{String(), r.path, r.model, String()};
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

bool available() {
  // The mic gate keys off this: a cumulo-keyed device resolves to the router and is
  // available, so "Voice needs a speech-to-text key" does not fire (CUM-376).
  return resolve().key.length() > 0;
}

String lastStatus() { return s_lastStatus; }

// The 44-byte canonical RIFF/WAVE header for 16-bit mono PCM. Streamed INLINE as
// the multipart filePrefix (transcribePcm) - the old pcmToWav wrote a second full
// copy of the recording to LittleFS just to prepend these bytes, which halved the
// recordable length (the partition pays twice per capture).
static void wavHeader(uint8_t h[44], uint32_t dataBytes, uint32_t sampleRate) {
  auto p32 = [&](int off, uint32_t v) {
    h[off] = (uint8_t)v; h[off + 1] = (uint8_t)(v >> 8);
    h[off + 2] = (uint8_t)(v >> 16); h[off + 3] = (uint8_t)(v >> 24);
  };
  auto p16 = [&](int off, uint16_t v) { h[off] = (uint8_t)v; h[off + 1] = (uint8_t)(v >> 8); };
  memcpy(h, "RIFF", 4); p32(4, 36 + dataBytes); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); p32(16, 16); p16(20, 1); p16(22, 1);   // PCM, mono
  p32(24, sampleRate); p32(28, sampleRate * 2); p16(32, 2); p16(34, 16);
  memcpy(h + 36, "data", 4); p32(40, dataBytes);
}

// Shared transcription core: multipart POST (optionally with inline prefix bytes
// ahead of the on-disk file) -> parse {"text":...}.
static String transcribeCommon(const char* localPath, const char* fname, const char* mime,
                               const uint8_t* prefix, size_t prefixLen) {
  if (!localPath || !localPath[0]) return String();
  s_lastStatus = String();   // fresh attempt: clear any prior refusal status
  SttProvider prov = resolve();
  if (prov.key.length() == 0) { alogf("stt: no key for provider %s", store::sttProvider().c_str()); return String(); }

  size_t fsz = 0;
  { File f = LittleFS.open(localPath, FILE_READ); if (f) { fsz = f.size(); f.close(); } }
  STTDIAG("host=%s path=%s model=%s file=%s size=%u (+%u prefix) mime=%s",
          prov.host.c_str(), prov.path, prov.model, fname,
          (unsigned)fsz, (unsigned)prefixLen, mime ? mime : "");

  std::vector<httpmp::Field> fields = { {"model", prov.model} };  // Voxtral rejects response_format; text is default
  String resp, err;
  bool ok = httpmp::post(prov.host.c_str(), 443, prov.path,
                         prov.key, fields, "file", fname,
                         mime && mime[0] ? mime : "audio/ogg", localPath, resp, err,
                         /*srcFs=*/nullptr, /*lockSrc=*/false, prefix, prefixLen);
  STTDIAG("http ok=%d err='%s' respLen=%u resp='%.160s'",
          ok ? 1 : 0, err.c_str(), (unsigned)resp.length(), resp.c_str());
  if (!ok) {
    // A refusal is JSON {error:<code>} with 4xx (funding_cap_reached, rate_limited,
    // audio_duration_unknown, unsupported_media_type - the router's contract until
    // the audio route is live, and per-call after). Surface the code as an honest
    // one-line status for the owner instead of a silent generic miss.
    bool jok = false;
    std::string code = core::parseErrorCode(resp.c_str(), &jok);
    if (jok && !code.empty())
      s_lastStatus = String(nimbus::orch::voiceRefusalStatus(code).c_str());
    alogf("stt: transcribe failed (%s): %s%s", store::sttProvider().c_str(), err.c_str(),
          s_lastStatus.length() ? (String(" [") + s_lastStatus + "]").c_str() : "");
    return String();
  }

  // Response: {"text":"..."}. Parse via the shared, host-tested core parser (same
  // code path the regression test exercises). It uses a real JSON decoder so ALL
  // escapes decode correctly - the hand-rolled scanner mangled \uXXXX (accents/emoji
  // -> "u00e9"). jsonOk=false means the body didn't parse - usually a TRUNCATED
  // response, which is exactly what the old 2048-byte read cap produced.
  bool jsonOk = false;
  std::string t = core::parseTranscription(resp.c_str(), &jsonOk);
  if (!jsonOk) {
    alogf("stt: bad JSON (%.80s)", resp.c_str());
    STTDIAG("bad JSON -> empty transcript");
    return String();
  }
  String out(t.c_str());
  STTDIAG("parsed textLen=%u text='%.80s'", (unsigned)out.length(), out.c_str());
  return out;
}

String transcribe(const char* localPath, const char* mime) {
  // Filename extension from the mime so the provider picks the right decoder.
  const char* fname = "audio.ogg";
  if (mime && strstr(mime, "wav"))  fname = "audio.wav";
  else if (mime && strstr(mime, "mp3")) fname = "audio.mp3";
  else if (mime && strstr(mime, "mpeg")) fname = "audio.mp3";
  return transcribeCommon(localPath, fname, mime, nullptr, 0);
}

String transcribePcm(const char* pcmPath, uint32_t sampleRate) {
  size_t dataBytes = 0;
  { File f = LittleFS.open(pcmPath, FILE_READ); if (f) { dataBytes = f.size(); f.close(); } }
  if (dataBytes == 0) { alog("stt: pcm empty/open fail"); return String(); }
  uint8_t hdr[44];
  wavHeader(hdr, (uint32_t)dataBytes, sampleRate);
  return transcribeCommon(pcmPath, "audio.wav", "audio/wav", hdr, sizeof(hdr));
}

}  // namespace stt
}  // namespace agent
