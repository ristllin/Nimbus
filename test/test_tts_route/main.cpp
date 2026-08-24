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
#include "minimp3.h"
#include "mp3_fixture.h"   // kToneMp3 / kToneMp3Len - a real 16 kHz mono MP3

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
  RUN_TEST(test_downmix_averages_lr);
  RUN_TEST(test_downmix_zero_frames_is_noop);
  RUN_TEST(test_minimp3_decodes_fixture_to_pcm);
  RUN_TEST(test_minimp3_rejects_garbage);
  return UNITY_END();
}
