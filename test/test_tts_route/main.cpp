// N12 (CUM-134 #1): the on-device spoken-reply seam.
//
// Two things this suite pins, both of which the shipped firmware got wrong and which
// left the device unable to ever speak a reply:
//
//   1. FORMAT ROUTING - core::speakerTtsFormat maps the configured TTS provider to the
//      format the speaker path must synthesize + how to play it. OpenAI emits WAV
//      (played directly); Mistral/Voxtral emits MP3 (played via minimp3). The bug was
//      forcing "wav" for every provider, so a Mistral-only device synthesized nothing
//      and stayed silent.
//   2. THE MP3-TO-SPEAKER DECODE - the vendored CC0 minimp3 decoder (the same one
//      music::streamMp3File feeds to the I2S TX) actually turns real MP3 bytes into
//      PCM on the host, and the stereo->mono downmix (core::downmixStereoToMono) does
//      the averaging the mono speaker needs.
//
// All host-tested (no Arduino, no hardware): the decoder is pure C, the routing +
// downmix live in lib/core, so the device and this test can never drift.

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/tts_catalog.h"
#include "nimbus/orch/voice_route.h"   // CUM-376: cumulo voice routing helpers
#include "minimp3.h"
#include "mp3_fixture.h"   // kToneMp3 / kToneMp3Len - a real 16 kHz mono MP3

using nimbus::orch::VoiceKind;
using nimbus::orch::VoiceRouteInfo;
using nimbus::orch::voiceActiveProvider;
using nimbus::orch::voiceRefusalStatus;
using nimbus::orch::voiceRouteFor;

void setUp(void) {}
void tearDown(void) {}

// ---- format routing ---------------------------------------------------------

static void test_openai_routes_to_wav(void) {
  bool mp3 = true;
  const char* fmt = core::speakerTtsFormat("openai", &mp3);
  TEST_ASSERT_EQUAL_STRING("wav", fmt);
  TEST_ASSERT_FALSE(mp3);   // WAV plays directly on the speaker, no decode
}

static void test_mistral_routes_to_mp3(void) {
  bool mp3 = false;
  const char* fmt = core::speakerTtsFormat("mistral", &mp3);
  TEST_ASSERT_EQUAL_STRING("mp3", fmt);
  TEST_ASSERT_TRUE(mp3);    // Mistral emits MP3 -> minimp3 decodes it. THE fix.
}

static void test_unknown_provider_defaults_to_mp3(void) {
  // A provider the map does not know (or an empty string) must default to the
  // decodable MP3 path, never to a WAV the provider might not emit.
  bool mp3 = false;
  TEST_ASSERT_EQUAL_STRING("mp3", core::speakerTtsFormat("", &mp3));
  TEST_ASSERT_TRUE(mp3);
  TEST_ASSERT_EQUAL_STRING("mp3", core::speakerTtsFormat("some-future-tts", nullptr));
}

static void test_cumulo_routes_to_wav(void) {
  // CUM-376: cumulo TTS routes through the OpenAI upstream (/router/openai/...), so
  // its response is the same WAV-capable shape as a direct OpenAI call - not MP3.
  bool mp3 = true;
  const char* fmt = core::speakerTtsFormat("cumulo", &mp3);
  TEST_ASSERT_EQUAL_STRING("wav", fmt);
  TEST_ASSERT_FALSE(mp3);
}

// ---- CUM-376: the pure voice route table (provider x route x key state) --------

// Direct BYOK routes are unchanged: the provider's own /v1/audio/* path, direct
// host+key (not the router), and the Mistral wire shape only for Mistral.
static void test_route_openai_direct(void) {
  VoiceRouteInfo s = voiceRouteFor("openai", VoiceKind::Stt);
  TEST_ASSERT_TRUE(s.known);
  TEST_ASSERT_FALSE(s.viaCumuloRouter);
  TEST_ASSERT_FALSE(s.mistralShape);
  TEST_ASSERT_EQUAL_STRING("/v1/audio/transcriptions", s.path);
  TEST_ASSERT_EQUAL_STRING("gpt-4o-mini-transcribe", s.model);
  VoiceRouteInfo t = voiceRouteFor("openai", VoiceKind::Tts);
  TEST_ASSERT_FALSE(t.viaCumuloRouter);
  TEST_ASSERT_FALSE(t.mistralShape);
  TEST_ASSERT_EQUAL_STRING("/v1/audio/speech", t.path);
  TEST_ASSERT_EQUAL_STRING("gpt-4o-mini-tts", t.model);
  TEST_ASSERT_EQUAL_STRING("alloy", t.voiceDefault);
}

static void test_route_mistral_direct(void) {
  VoiceRouteInfo s = voiceRouteFor("mistral", VoiceKind::Stt);
  TEST_ASSERT_TRUE(s.known);
  TEST_ASSERT_FALSE(s.viaCumuloRouter);
  TEST_ASSERT_TRUE(s.mistralShape);   // base64 MP3 body, ignores response_format
  TEST_ASSERT_EQUAL_STRING("/v1/audio/transcriptions", s.path);
  TEST_ASSERT_EQUAL_STRING("voxtral-mini-latest", s.model);
  VoiceRouteInfo t = voiceRouteFor("mistral", VoiceKind::Tts);
  TEST_ASSERT_TRUE(t.mistralShape);
  TEST_ASSERT_EQUAL_STRING("/v1/audio/speech", t.path);
  TEST_ASSERT_EQUAL_STRING("voxtral-mini-tts-latest", t.model);
  TEST_ASSERT_EQUAL_STRING("en_paul_neutral", t.voiceDefault);
}

// The cumulo route: through the router at /router/openai/v1/audio/*, openai-shaped
// (mistralShape=false) so the same parse/synthesis code path serves it, host+key
// come from the router (viaCumuloRouter=true).
static void test_route_cumulo_via_router(void) {
  VoiceRouteInfo s = voiceRouteFor("cumulo", VoiceKind::Stt);
  TEST_ASSERT_TRUE(s.known);
  TEST_ASSERT_TRUE(s.viaCumuloRouter);
  TEST_ASSERT_FALSE(s.mistralShape);
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/audio/transcriptions", s.path);
  TEST_ASSERT_EQUAL_STRING("gpt-4o-mini-transcribe", s.model);
  VoiceRouteInfo t = voiceRouteFor("cumulo", VoiceKind::Tts);
  TEST_ASSERT_TRUE(t.viaCumuloRouter);
  TEST_ASSERT_FALSE(t.mistralShape);   // openai upstream: raw binary, honors response_format
  TEST_ASSERT_EQUAL_STRING("/router/openai/v1/audio/speech", t.path);
  TEST_ASSERT_EQUAL_STRING("gpt-4o-mini-tts", t.model);
  TEST_ASSERT_EQUAL_STRING("alloy", t.voiceDefault);
}

static void test_route_unknown_provider_is_not_known(void) {
  VoiceRouteInfo s = voiceRouteFor("zai", VoiceKind::Stt);
  TEST_ASSERT_FALSE(s.known);
  TEST_ASSERT_FALSE(s.viaCumuloRouter);
}

// ---- CUM-376: effective provider is cumulo-aware (the one-key fallback) --------

static void test_voice_active_backcompat_2provider(void) {
  // With no cumulo key, voiceActiveProvider must match the old 2-provider behavior
  // exactly (ttsActiveProvider delegates to it - they can never drift).
  TEST_ASSERT_EQUAL_STRING("mistral", voiceActiveProvider("mistral", true, true, false).c_str());
  TEST_ASSERT_EQUAL_STRING("openai",  voiceActiveProvider("openai", true, true, false).c_str());
  TEST_ASSERT_EQUAL_STRING("openai",  voiceActiveProvider("mistral", true, false, false).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", voiceActiveProvider("openai", false, true, false).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", voiceActiveProvider("", false, false, false).c_str());
}

static void test_voice_active_one_key_device_falls_to_cumulo(void) {
  // THE one-key device (CUM-376): the shipped voice default is "mistral" with NO
  // mistral or openai key, but a cumulo key is present -> route through the router.
  TEST_ASSERT_EQUAL_STRING("cumulo", voiceActiveProvider("mistral", false, false, true).c_str());
  // Same for an openai-configured device that only holds a cumulo key.
  TEST_ASSERT_EQUAL_STRING("cumulo", voiceActiveProvider("openai", false, false, true).c_str());
}

static void test_voice_active_byok_wins_over_cumulo(void) {
  // A direct key for the configured provider is spent first - cumulo is the LAST
  // fallback, never a silent override of the key the owner chose.
  TEST_ASSERT_EQUAL_STRING("mistral", voiceActiveProvider("mistral", false, true, true).c_str());
  TEST_ASSERT_EQUAL_STRING("openai",  voiceActiveProvider("openai", true, false, true).c_str());
  // Configured provider unkeyed, the OTHER BYOK provider keyed -> that one, not cumulo.
  TEST_ASSERT_EQUAL_STRING("openai",  voiceActiveProvider("mistral", true, false, true).c_str());
}

static void test_voice_active_explicit_cumulo_selection(void) {
  // An explicit cumulo selection routes through the router when keyed; if the cumulo
  // key is somehow absent it falls back to a keyed BYOK provider rather than dying.
  TEST_ASSERT_EQUAL_STRING("cumulo",  voiceActiveProvider("cumulo", false, false, true).c_str());
  TEST_ASSERT_EQUAL_STRING("cumulo",  voiceActiveProvider("cumulo", true, true, true).c_str());
  TEST_ASSERT_EQUAL_STRING("openai",  voiceActiveProvider("cumulo", true, false, false).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", voiceActiveProvider("cumulo", false, true, false).c_str());
  // No keys at all: return the configured slug (caller fails at the key check).
  TEST_ASSERT_EQUAL_STRING("cumulo",  voiceActiveProvider("cumulo", false, false, false).c_str());
}

static void test_voice_active_explicit_cumulo_wins_over_present_mistral(void) {
  // CUM-439 acceptance: the whole point of MAKING cumulo selectable is that an
  // explicit cumulo choice beats a present mistral (or openai) key. A device with
  // BOTH a cumulo key AND a mistral key, configured to cumulo, speaks through the
  // router - it must NOT silently fall back to the mistral key it also holds.
  TEST_ASSERT_EQUAL_STRING("cumulo", voiceActiveProvider("cumulo", false, true, true).c_str());
  TEST_ASSERT_EQUAL_STRING("cumulo", voiceActiveProvider("cumulo", true, false, true).c_str());
  TEST_ASSERT_EQUAL_STRING("cumulo", voiceActiveProvider("cumulo", true, true, true).c_str());
}

// ---- CUM-376: refusal code -> honest one-line status (never silence) -----------

static void test_refusal_status_known_codes(void) {
  // Each contract code maps to a distinct, honest, non-empty line.
  const std::string funding = voiceRefusalStatus("funding_cap_reached");
  const std::string rate    = voiceRefusalStatus("rate_limited");
  const std::string dur     = voiceRefusalStatus("audio_duration_unknown");
  const std::string media   = voiceRefusalStatus("unsupported_media_type");
  TEST_ASSERT_TRUE(funding.size() > 0);
  TEST_ASSERT_TRUE(rate.size() > 0);
  TEST_ASSERT_TRUE(dur.size() > 0);
  TEST_ASSERT_TRUE(media.size() > 0);
  // They are genuinely different lines, not one catch-all.
  TEST_ASSERT_TRUE(funding != rate);
  TEST_ASSERT_TRUE(dur != media);
  TEST_ASSERT_TRUE(funding != dur);
}

static void test_refusal_status_unknown_code_has_safe_default(void) {
  // An unknown or empty code must still yield an honest line, never "" (silence).
  TEST_ASSERT_TRUE(voiceRefusalStatus("").size() > 0);
  TEST_ASSERT_TRUE(voiceRefusalStatus("some_new_code").size() > 0);
}

static void test_refusal_status_copy_hygiene(void) {
  // Public-repo copy rules (AGENTS s6): no em dash, no exclamation shouting.
  const char* codes[] = {"funding_cap_reached", "rate_limited", "audio_duration_unknown",
                         "unsupported_media_type", "", "unknown"};
  for (const char* c : codes) {
    const std::string s = voiceRefusalStatus(c);
    TEST_ASSERT_TRUE(s.find("\xe2\x80\x94") == std::string::npos);  // U+2014 em dash
    TEST_ASSERT_TRUE(s.find('!') == std::string::npos);
    // ASCII only (device panel + serial are printable ASCII).
    for (unsigned char ch : s) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7f);
  }
}

// ---- provider key fallback (the regression this lane must not reintroduce) --

static void test_active_provider_prefers_configured(void) {
  // Both keys present: use exactly what the owner configured.
  TEST_ASSERT_EQUAL_STRING("mistral", core::ttsActiveProvider("mistral", true, true).c_str());
  TEST_ASSERT_EQUAL_STRING("openai", core::ttsActiveProvider("openai", true, true).c_str());
}

static void test_active_provider_falls_back_when_configured_key_missing(void) {
  // Mistral configured, only an OpenAI key -> speak via OpenAI (the exact regression:
  // dropping the old reroute left this case silent). And the reverse direction.
  TEST_ASSERT_EQUAL_STRING("openai", core::ttsActiveProvider("mistral", true, false).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", core::ttsActiveProvider("openai", false, true).c_str());
}

static void test_active_provider_keeps_configured_when_it_has_the_key(void) {
  // Configured provider has its key; presence/absence of the other never overrides it.
  TEST_ASSERT_EQUAL_STRING("mistral", core::ttsActiveProvider("mistral", false, true).c_str());
  TEST_ASSERT_EQUAL_STRING("openai", core::ttsActiveProvider("openai", true, false).c_str());
}

static void test_active_provider_no_keys_returns_configured(void) {
  // No keys at all: return the configured provider (the caller fails at the key check
  // with a clear log, not a wrong-provider attempt). Unknown slug -> mistral.
  TEST_ASSERT_EQUAL_STRING("mistral", core::ttsActiveProvider("mistral", false, false).c_str());
  TEST_ASSERT_EQUAL_STRING("openai", core::ttsActiveProvider("openai", false, false).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", core::ttsActiveProvider("", false, false).c_str());
}

// ---- stereo -> mono downmix -------------------------------------------------

static void test_downmix_averages_lr(void) {
  // interleaved L,R pairs -> per-frame average.
  const int16_t in[] = {100, 200, -100, -300, 32767, 32767, 0, -2};
  int16_t out[4] = {0};
  core::downmixStereoToMono(in, 4, out);
  TEST_ASSERT_EQUAL_INT16(150, out[0]);      // (100+200)/2
  TEST_ASSERT_EQUAL_INT16(-200, out[1]);     // (-100 + -300)/2
  TEST_ASSERT_EQUAL_INT16(32767, out[2]);    // no overflow: summed as int
  TEST_ASSERT_EQUAL_INT16(-1, out[3]);       // (0 + -2)/2
}

static void test_downmix_zero_frames_is_noop(void) {
  int16_t out[1] = {123};
  core::downmixStereoToMono(nullptr, 0, out);
  TEST_ASSERT_EQUAL_INT16(123, out[0]);      // untouched
}

// ---- the real MP3 decode (minimp3, the vendored CC0 decoder) ----------------

static void test_minimp3_decodes_fixture_to_pcm(void) {
  mp3dec_t dec;
  mp3dec_init(&dec);
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t off = 0;
  int frames = 0, totalSamples = 0, hz = 0, channels = 0;
  while (off < kToneMp3Len) {
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec, kToneMp3 + off, (int)(kToneMp3Len - off), pcm, &info);
    if (info.frame_bytes <= 0) break;   // no full frame left
    off += (size_t)info.frame_bytes;
    if (samples <= 0) continue;         // skipped ID3/junk between frames
    frames++;
    totalSamples += samples;
    hz = info.hz;
    channels = info.channels;
  }
  // A real clip decoded: multiple frames, thousands of PCM samples, at the source
  // rate/mono. This is the exact decode music::streamMp3File runs before feeding I2S.
  TEST_ASSERT_GREATER_THAN_INT(0, frames);
  TEST_ASSERT_GREATER_THAN_INT(1000, totalSamples);
  TEST_ASSERT_EQUAL_INT(16000, hz);
  TEST_ASSERT_EQUAL_INT(1, channels);
}

static void test_minimp3_rejects_garbage(void) {
  // Non-MP3 bytes: the decoder consumes/ skips them and yields no PCM (info.frame_bytes
  // may advance past junk, but samples stay 0) - it must never emit bogus audio.
  mp3dec_t dec;
  mp3dec_init(&dec);
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  unsigned char junk[64];
  memset(junk, 0xAB, sizeof(junk));
  mp3dec_frame_info_t info;
  int samples = mp3dec_decode_frame(&dec, junk, (int)sizeof(junk), pcm, &info);
  TEST_ASSERT_EQUAL_INT(0, samples);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_openai_routes_to_wav);
  RUN_TEST(test_mistral_routes_to_mp3);
  RUN_TEST(test_unknown_provider_defaults_to_mp3);
  RUN_TEST(test_cumulo_routes_to_wav);
  RUN_TEST(test_route_openai_direct);
  RUN_TEST(test_route_mistral_direct);
  RUN_TEST(test_route_cumulo_via_router);
  RUN_TEST(test_route_unknown_provider_is_not_known);
  RUN_TEST(test_voice_active_backcompat_2provider);
  RUN_TEST(test_voice_active_one_key_device_falls_to_cumulo);
  RUN_TEST(test_voice_active_byok_wins_over_cumulo);
  RUN_TEST(test_voice_active_explicit_cumulo_selection);
  RUN_TEST(test_voice_active_explicit_cumulo_wins_over_present_mistral);
  RUN_TEST(test_refusal_status_known_codes);
  RUN_TEST(test_refusal_status_unknown_code_has_safe_default);
  RUN_TEST(test_refusal_status_copy_hygiene);
  RUN_TEST(test_active_provider_prefers_configured);
  RUN_TEST(test_active_provider_falls_back_when_configured_key_missing);
  RUN_TEST(test_active_provider_keeps_configured_when_it_has_the_key);
  RUN_TEST(test_active_provider_no_keys_returns_configured);
  RUN_TEST(test_downmix_averages_lr);
  RUN_TEST(test_downmix_zero_frames_is_noop);
  RUN_TEST(test_minimp3_decodes_fixture_to_pcm);
  RUN_TEST(test_minimp3_rejects_garbage);
  return UNITY_END();
}
