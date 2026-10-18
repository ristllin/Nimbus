#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/wav_clip.h"

// wav_clip - the header reader behind the test console's AUDIOINJECT (a recorded
// clip fed through the hold-to-talk path in place of the mic). Pinned: a valid
// mic-format clip, extra chunks and odd-size padding before the data, and every
// refusal, so a wrong clip fails with a reason instead of reaching a paid STT call.

using nimbus::voice::parseWavHeader;
using nimbus::voice::WavErr;
using nimbus::voice::WavInfo;
using nimbus::voice::wavMatchesMic;

void setUp() {}
void tearDown() {}

static void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
static void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xFF);
}
static void tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// A WAV with the given fmt fields, optional extra chunks before "data", and
// `samples` bytes of sample data.
static std::vector<uint8_t> wav(uint16_t fmt, uint16_t ch, uint32_t rate, uint16_t bits,
                                const std::vector<std::vector<uint8_t>>& extra, uint32_t samples) {
  std::vector<uint8_t> b;
  tag(b, "RIFF");
  put32(b, 0);   // RIFF size: not consulted by the reader
  tag(b, "WAVE");
  tag(b, "fmt ");
  put32(b, 16);
  put16(b, fmt);
  put16(b, ch);
  put32(b, rate);
  put32(b, rate * ch * (bits / 8));
  put16(b, ch * (bits / 8));
  put16(b, bits);
  for (const auto& e : extra) b.insert(b.end(), e.begin(), e.end());
  tag(b, "data");
  put32(b, samples);
  b.insert(b.end(), samples, 0x11);
  return b;
}

static std::vector<uint8_t> chunk(const char* id, uint32_t size) {
  std::vector<uint8_t> c;
  tag(c, id);
  put32(c, size);
  c.insert(c.end(), size + (size & 1u), 0x00);   // odd sizes carry one pad byte
  return c;
}

static void test_mic_format_clip_parses_and_matches() {
  auto b = wav(1, 1, 16000, 16, {}, 3200);
  WavInfo w;
  TEST_ASSERT_EQUAL(int(WavErr::Ok), int(parseWavHeader(b.data(), b.size(), w)));
  TEST_ASSERT_EQUAL_UINT32(44, w.dataOffset);
  TEST_ASSERT_EQUAL_UINT32(3200, w.dataBytes);
  TEST_ASSERT_TRUE(wavMatchesMic(w));
}

// macOS `say` and most editors put a LIST/FLLR chunk before the data, and an
// odd-sized chunk carries a pad byte: the reader must skip both exactly.
static void test_extra_chunks_and_odd_padding_are_skipped() {
  auto b = wav(1, 1, 16000, 16, {chunk("LIST", 26), chunk("FLLR", 7)}, 640);
  WavInfo w;
  TEST_ASSERT_EQUAL(int(WavErr::Ok), int(parseWavHeader(b.data(), b.size(), w)));
  TEST_ASSERT_EQUAL_UINT32(44 + (8 + 26) + (8 + 8), w.dataOffset);
  TEST_ASSERT_EQUAL_UINT8(0x11, b[w.dataOffset]);   // lands on the first sample byte
  TEST_ASSERT_EQUAL_UINT32(640, w.dataBytes);
}

// The class: every non-mic format parses but is refused by wavMatchesMic.
static void test_every_non_mic_format_is_refused() {
  struct { uint16_t ch; uint32_t rate; uint16_t bits; } bad[] = {
      {2, 16000, 16}, {1, 8000, 16}, {1, 44100, 16}, {1, 48000, 16}, {1, 16000, 8}, {1, 16000, 24},
  };
  for (const auto& c : bad) {
    auto b = wav(1, c.ch, c.rate, c.bits, {}, 64);
    WavInfo w;
    TEST_ASSERT_EQUAL(int(WavErr::Ok), int(parseWavHeader(b.data(), b.size(), w)));
    TEST_ASSERT_FALSE(wavMatchesMic(w));
  }
}

static void test_refusals_name_the_reason() {
  WavInfo w;
  const uint8_t tiny[4] = {'R', 'I', 'F', 'F'};
  TEST_ASSERT_EQUAL(int(WavErr::TooShort), int(parseWavHeader(tiny, sizeof tiny, w)));
  TEST_ASSERT_EQUAL(int(WavErr::TooShort), int(parseWavHeader(nullptr, 0, w)));

  auto b = wav(1, 1, 16000, 16, {}, 16);
  auto notRiff = b;
  memcpy(notRiff.data(), "RIFX", 4);
  TEST_ASSERT_EQUAL(int(WavErr::NotRiff), int(parseWavHeader(notRiff.data(), notRiff.size(), w)));
  auto notWave = b;
  memcpy(notWave.data() + 8, "AVI ", 4);
  TEST_ASSERT_EQUAL(int(WavErr::NotWave), int(parseWavHeader(notWave.data(), notWave.size(), w)));

  auto adpcm = wav(2, 1, 16000, 16, {}, 16);   // format 2 = MS ADPCM
  TEST_ASSERT_EQUAL(int(WavErr::NotPcm), int(parseWavHeader(adpcm.data(), adpcm.size(), w)));

  // A data chunk before any fmt chunk.
  std::vector<uint8_t> noFmt;
  tag(noFmt, "RIFF"); put32(noFmt, 0); tag(noFmt, "WAVE"); tag(noFmt, "data"); put32(noFmt, 4);
  noFmt.insert(noFmt.end(), 4, 0);
  TEST_ASSERT_EQUAL(int(WavErr::NoFmt), int(parseWavHeader(noFmt.data(), noFmt.size(), w)));

  // The header window ends before the data chunk starts.
  auto cut = wav(1, 1, 16000, 16, {chunk("LIST", 200)}, 16);
  TEST_ASSERT_EQUAL(int(WavErr::NoData), int(parseWavHeader(cut.data(), 60, w)));
  for (int e = 0; e <= int(WavErr::NoData); e++)
    TEST_ASSERT_TRUE(std::strlen(nimbus::voice::wavErrName(WavErr(e))) > 0);
}

// A hostile chunk size must not walk past the buffer or wrap the offset.
static void test_hostile_chunk_size_stays_in_bounds() {
  std::vector<uint8_t> b;
  tag(b, "RIFF"); put32(b, 0); tag(b, "WAVE");
  tag(b, "JUNK"); put32(b, 0xFFFFFFF0u);
  b.insert(b.end(), 8, 0);
  WavInfo w;
  TEST_ASSERT_EQUAL(int(WavErr::NoFmt), int(parseWavHeader(b.data(), b.size(), w)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mic_format_clip_parses_and_matches);
  RUN_TEST(test_extra_chunks_and_odd_padding_are_skipped);
  RUN_TEST(test_every_non_mic_format_is_refused);
  RUN_TEST(test_refusals_name_the_reason);
  RUN_TEST(test_hostile_chunk_size_stays_in_bounds);
  return UNITY_END();
}
