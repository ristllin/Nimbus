// CUM-222 - MP3 decode scratch relocation guard.
//
// minimp3's mp3dec_decode_frame() puts a ~15 KB mp3dec_scratch_t (grbuf + syn +
// maindata) on the CALLING TASK'S STACK. That single buffer forced the sfx task to a
// 20 KB stack and left the 8 KB music task one MP3 track short of a stack overflow -
// on a chip whose largest contiguous INTERNAL block is the scarce resource that TLS
// and AsyncTCP need. The fix (see src/sfx/music.cpp) routes the audio decode through
// mp3dec_decode_frame_ex(), which takes a caller-provided scratch buffer we allocate
// in PSRAM, and drops the sfx stack to 12 KB (-8 KB internal SRAM).
//
// This host test is the durable guard for that refactor. It proves the two entry
// points are BEHAVIORALLY IDENTICAL: decoding the same real MP3 through the on-stack
// path (mp3dec_decode_frame) and the external-scratch path (mp3dec_decode_frame_ex)
// yields byte-identical PCM and identical frame parsing. If a future edit breaks the
// _ex wiring - or hand-tunes one path - this fails under `pio test -e native` instead
// of shipping garbled audio that only shows on the bench. It also pins the scratch
// size so the premise of the reduction (the buffer really is large) stays true.

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "minimp3.h"
#include "../test_tts_route/mp3_fixture.h"  // kToneMp3 / kToneMp3Len - a real 16 kHz mono MP3

void setUp() {}
void tearDown() {}

namespace {

struct DecodeResult {
  std::vector<int16_t> pcm;          // all decoded samples, concatenated
  std::vector<int> frameBytes;       // info.frame_bytes per call
  std::vector<int> sampleCounts;     // return value per call
  std::vector<int> hz;
  std::vector<int> channels;
};

// Decode the whole fixture one frame at a time. useExt selects the external-scratch
// entry point (scratch on the heap, as the device does with a PSRAM buffer) vs the
// stock on-stack path.
DecodeResult decodeAll(bool useExt) {
  DecodeResult r;
  mp3dec_t dec;
  mp3dec_init(&dec);
  // The external scratch, sized by the decoder itself - exactly the buffer the device
  // moves to PSRAM. std::vector keeps it off THIS test's stack too.
  std::vector<uint8_t> scratch((size_t)mp3dec_scratch_size());

  size_t off = 0;
  int guard = 0;
  while (off < kToneMp3Len && guard++ < 100000) {
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples =
        useExt ? mp3dec_decode_frame_ex(&dec, kToneMp3 + off, (int)(kToneMp3Len - off), pcm, &info, scratch.data())
               : mp3dec_decode_frame(&dec, kToneMp3 + off, (int)(kToneMp3Len - off), pcm, &info);
    if (info.frame_bytes <= 0) break;  // no full frame and no more input
    r.frameBytes.push_back(info.frame_bytes);
    r.sampleCounts.push_back(samples);
    r.hz.push_back(info.hz);
    r.channels.push_back(info.channels);
    for (int i = 0; i < samples * (info.channels ? info.channels : 1); i++) r.pcm.push_back(pcm[i]);
    off += (size_t)info.frame_bytes;
  }
  return r;
}

}  // namespace

// The scratch really is the large buffer we claim it is (the reason to move it off
// the stack). If minimp3 is ever swapped for a build that shrinks this, the whole
// premise - and the sfx/music stack sizing - needs a fresh look.
static void test_scratch_is_large(void) {
  TEST_ASSERT_GREATER_OR_EQUAL_INT(12000, mp3dec_scratch_size());
}

// The two entry points must decode the fixture identically: same frame boundaries,
// same sample counts, same headers, and byte-identical PCM. This is the guarantee the
// refactor rests on.
static void test_ex_matches_stock_on_real_mp3(void) {
  DecodeResult stock = decodeAll(/*useExt=*/false);
  DecodeResult ext = decodeAll(/*useExt=*/true);

  TEST_ASSERT_GREATER_THAN_UINT(0, stock.pcm.size());  // the fixture actually decoded
  TEST_ASSERT_EQUAL_UINT(stock.frameBytes.size(), ext.frameBytes.size());
  TEST_ASSERT_EQUAL_UINT(stock.pcm.size(), ext.pcm.size());
  if (!stock.frameBytes.empty()) {
    TEST_ASSERT_EQUAL_INT_ARRAY(stock.frameBytes.data(), ext.frameBytes.data(), (int)stock.frameBytes.size());
    TEST_ASSERT_EQUAL_INT_ARRAY(stock.sampleCounts.data(), ext.sampleCounts.data(), (int)stock.sampleCounts.size());
    TEST_ASSERT_EQUAL_INT_ARRAY(stock.hz.data(), ext.hz.data(), (int)stock.hz.size());
    TEST_ASSERT_EQUAL_INT_ARRAY(stock.channels.data(), ext.channels.data(), (int)stock.channels.size());
  }
  if (!stock.pcm.empty()) {
    TEST_ASSERT_EQUAL_INT16_ARRAY(stock.pcm.data(), ext.pcm.data(), (int)stock.pcm.size());
  }
}

// Garbage in must behave identically too (both find no frame, report the same skip).
static void test_ex_matches_stock_on_garbage(void) {
  uint8_t junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 37 + 11);

  mp3dec_t d1, d2;
  mp3dec_init(&d1);
  mp3dec_init(&d2);
  std::vector<uint8_t> scratch((size_t)mp3dec_scratch_size());
  mp3dec_frame_info_t i1, i2;
  memset(&i1, 0, sizeof(i1));
  memset(&i2, 0, sizeof(i2));
  mp3d_sample_t p1[MINIMP3_MAX_SAMPLES_PER_FRAME], p2[MINIMP3_MAX_SAMPLES_PER_FRAME];

  int s1 = mp3dec_decode_frame(&d1, junk, (int)sizeof(junk), p1, &i1);
  int s2 = mp3dec_decode_frame_ex(&d2, junk, (int)sizeof(junk), p2, &i2, scratch.data());

  TEST_ASSERT_EQUAL_INT(s1, s2);
  TEST_ASSERT_EQUAL_INT(i1.frame_bytes, i2.frame_bytes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scratch_is_large);
  RUN_TEST(test_ex_matches_stock_on_real_mp3);
  RUN_TEST(test_ex_matches_stock_on_garbage);
  return UNITY_END();
}
