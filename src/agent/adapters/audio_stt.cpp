#include "audio_stt.h"

#include <LittleFS.h>

#include <string>

#include "nimbus/audio_req.h"   // core::parseTranscription (shared, host-tested parse)
#include "http_multipart.h"
#include "../store.h"
#include "../../sys/agent_log.h"

// STT diagnostics: log the full transcription path (provider, file size, HTTP
// outcome, raw response, parsed text) through alogf so a silent "Didn't catch that"
// is root-causable over HTTP (GET /api/log) without opening serial (which drops WiFi).
#define STTDIAG(...) ::agent::alogf("[stt] " __VA_ARGS__)

namespace agent {
namespace stt {

// Resolve the configured STT provider to its host / transcription model / key.
// Both OpenAI and Mistral (Voxtral) expose /v1/audio/transcriptions as multipart
// and return {"text":...}, so only these three fields differ.
struct SttProvider { const char* host; const char* model; String key; };
static SttProvider resolve() {
  String p = store::sttProvider();
  if (p == "openai") return {"api.openai.com", "gpt-4o-mini-transcribe", store::openaiKey()};
  return {"api.mistral.ai", "voxtral-mini-latest", store::mistralKey()};  // default
}

bool available() {
  String p = store::sttProvider();
  return (p == "openai") ? store::hasOpenaiKey() : store::hasMistralKey();
}

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
  SttProvider prov = resolve();
  if (prov.key.length() == 0) { alogf("stt: no key for provider %s", store::sttProvider().c_str()); return String(); }

  size_t fsz = 0;
  { File f = LittleFS.open(localPath, FILE_READ); if (f) { fsz = f.size(); f.close(); } }
  STTDIAG("provider=%s model=%s file=%s size=%u (+%u prefix) mime=%s",
          store::sttProvider().c_str(), prov.model, fname,
          (unsigned)fsz, (unsigned)prefixLen, mime ? mime : "");

  std::vector<httpmp::Field> fields = { {"model", prov.model} };  // Voxtral rejects response_format; text is default
  String resp, err;
  bool ok = httpmp::post(prov.host, 443, "/v1/audio/transcriptions",
                         prov.key, fields, "file", fname,
                         mime && mime[0] ? mime : "audio/ogg", localPath, resp, err,
                         /*srcFs=*/nullptr, /*lockSrc=*/false, prefix, prefixLen);
  STTDIAG("http ok=%d err='%s' respLen=%u resp='%.160s'",
          ok ? 1 : 0, err.c_str(), (unsigned)resp.length(), resp.c_str());
  if (!ok) { alogf("stt: transcribe failed (%s): %s", store::sttProvider().c_str(), err.c_str()); return String(); }

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
