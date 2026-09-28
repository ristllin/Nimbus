#include "audio_stt.h"

#include <LittleFS.h>

#include <string>

#include "nimbus/audio_req.h"   // core::parseTranscription (shared, host-tested parse)
#include "nimbus/orch/voice_route.h"   // voiceActiveProvider / voiceRouteFor / voiceRefusalStatus
#include "nimbus/voice_flow.h"         // SttResult + the hold-to-talk error taxonomy (CUM-456)
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

// Resolve the EFFECTIVE STT provider to its host / path / transcription model / key.
// openai + mistral (Voxtral) hit the provider's own /v1/audio/transcriptions; a
// Cumulo-only device (no BYOK key, a cumulo key) routes through the router at
// /router/openai/v1/audio/transcriptions with the single router key, mirroring the
// embeddings viaCumuloRouter pattern (CUM-302). All three POST multipart and return
// {"text":...}, so only host/path/model/key differ.
struct SttProvider { String host; const char* path; const char* model; String key; std::string slug; };

#ifdef NIMBUS_TEST
// Bench seam (CUM-456, test image only): a RAM-only PLACEHOLDER key so the
// hold-to-talk release path can be driven on a keyless bench board. It is never
// persisted and is not a credential - a provider answers it with HTTP 401, which
// is itself a real leg (the STT HTTP error line). A real stored key always wins.
static bool s_placeholderKey = false;
void setPlaceholderKey(bool on) { s_placeholderKey = on; }
#endif

static SttProvider resolve() {
  const std::string eff = nimbus::orch::voiceActiveProvider(
      std::string(store::sttProvider().c_str()), store::hasOpenaiKey(),
      store::hasMistralKey(), store::hasCumuloKey());
  const nimbus::orch::VoiceRouteInfo r =
      nimbus::orch::voiceRouteFor(eff, nimbus::orch::VoiceKind::Stt);
  SttProvider p{String(), r.path, r.model, String(), eff};
  if (r.viaCumuloRouter) {
    String base = store::cumuloBase(); if (!base.length()) base = CUMULO_HOST_DEFAULT;
    p.host = bareHost(base);
    p.key = store::cumuloKey();
  } else if (eff == "openai") {
    p.host = "api.openai.com"; p.key = store::openaiKey();
  } else {  // mistral
    p.host = "api.mistral.ai"; p.key = store::mistralKey();
  }
#ifdef NIMBUS_TEST
  if (s_placeholderKey && p.key.length() == 0) p.key = "placeholder-not-a-key";
#endif
  return p;
}

bool available() {
  // The mic gate keys off this: a cumulo-keyed device resolves to the router and is
  // available, so "Voice needs a speech-to-text key" does not fire (CUM-376).
  return resolve().key.length() > 0;
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
// ahead of the on-disk file) -> parse {"text":...}. Returns a structured result so
// the mic path can tell the owner WHICH thing failed (CUM-456): no network, a
// provider HTTP error, a named router refusal, an unreadable reply, a busy device,
// or no audio - and only an Ok with empty text is "Didn't catch that".
// connectBudgetMs: 0 = the historical connect; the hold-to-talk path bounds it.
static nimbus::voice::SttResult transcribeCommon(const char* localPath, const char* fname,
                                                 const char* mime, const uint8_t* prefix,
                                                 size_t prefixLen, uint32_t connectBudgetMs) {
  using Kind = nimbus::voice::SttResult::Kind;
  nimbus::voice::SttResult out;
  if (!localPath || !localPath[0]) { out.kind = Kind::NoAudio; return out; }
  SttProvider prov = resolve();
  out.provider = prov.slug;
  if (prov.key.length() == 0) {
    alogf("stt: no key for provider %s", store::sttProvider().c_str());
    out.kind = Kind::Refused;
    out.refusal = "Voice needs a speech-to-text key. Set one in the web app.";
    return out;
  }

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
                         /*srcFs=*/nullptr, /*lockSrc=*/false, prefix, prefixLen,
                         connectBudgetMs);
  STTDIAG("http ok=%d err='%s' respLen=%u resp='%.160s'",
          ok ? 1 : 0, err.c_str(), (unsigned)resp.length(), resp.c_str());
  if (!ok) {
    // A router refusal is JSON {error:<code>} with 4xx (funding_cap_reached,
    // rate_limited, audio_duration_unknown, unsupported_media_type - the CUM-376
    // route contract). Only THOSE are reworded as a refusal line; any other body
    // (a provider's own error, e.g. a rejected key) is reported as its HTTP status
    // so the owner sees what actually failed.
    bool jok = false;
    const std::string code = core::parseErrorCode(resp.c_str(), &jok);
    if (jok && nimbus::voice::namedRefusal(code)) {
      out.kind = Kind::Refused;
      out.refusal = nimbus::orch::voiceRefusalStatus(code);
    } else {
      out.kind = nimbus::voice::sttKindForError(std::string(err.c_str()), &out.http);
    }
    alogf("stt: transcribe failed (%s): %s -> %s", prov.slug.c_str(), err.c_str(),
          nimbus::voice::sentence(nimbus::voice::lineFor(nimbus::voice::classify(out), out)).c_str());
    return out;
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
    STTDIAG("bad JSON -> unreadable reply");
    out.kind = Kind::BadReply;
    return out;
  }
  out.kind = Kind::Ok;
  out.text = t;
  STTDIAG("parsed textLen=%u text='%.80s'", (unsigned)t.size(), t.c_str());
  return out;
}

String transcribe(const char* localPath, const char* mime) {
  // Filename extension from the mime so the provider picks the right decoder.
  const char* fname = "audio.ogg";
  if (mime && strstr(mime, "wav"))  fname = "audio.wav";
  else if (mime && strstr(mime, "mp3")) fname = "audio.mp3";
  else if (mime && strstr(mime, "mpeg")) fname = "audio.mp3";
  return String(transcribeCommon(localPath, fname, mime, nullptr, 0, 0).text.c_str());
}

nimbus::voice::SttResult transcribePcmResult(const char* pcmPath, uint32_t sampleRate,
                                             uint32_t connectBudgetMs) {
  size_t dataBytes = 0;
  { File f = LittleFS.open(pcmPath, FILE_READ); if (f) { dataBytes = f.size(); f.close(); } }
  if (dataBytes == 0) {
    alog("stt: pcm empty/open fail");
    nimbus::voice::SttResult none;
    none.kind = nimbus::voice::SttResult::Kind::NoAudio;
    return none;
  }
  uint8_t hdr[44];
  wavHeader(hdr, (uint32_t)dataBytes, sampleRate);
  return transcribeCommon(pcmPath, "audio.wav", "audio/wav", hdr, sizeof(hdr), connectBudgetMs);
}

String transcribePcm(const char* pcmPath, uint32_t sampleRate) {
  return String(transcribePcmResult(pcmPath, sampleRate, 0).text.c_str());
}

}  // namespace stt
}  // namespace agent
