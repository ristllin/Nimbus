// test_clip_probe - reading a speech clip's own framing as it streams in (nimbus/orch/
// clip_probe.h), so a clip cut short by a reset or a proxy can be told from a whole one when
// the server gives no length (CODEREVIEW voice-model-seams, minor 4).
// The class: for a real MP3, the probe reports "ends on a boundary" at exactly the prefix
// lengths where an independent decoder (minimp3) says a frame ends, in any chunking; a
// stream it cannot follow gets no verdict; a WAV is whole only at its declared data size.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "clip_fixtures.h"
#include "minimp3.h"
#include "nimbus/orch/clip_probe.h"

using nimbus::orch::ClipFormat;
using nimbus::orch::ClipProbe;

void setUp() {}
void tearDown() {}

typedef std::vector<uint8_t> Bytes;

static Bytes bytesOf(const uint8_t* p, size_t n) { return Bytes(p, p + n); }

static ClipProbe probeOf(ClipFormat f, const Bytes& b, size_t upTo, size_t chunk) {
  ClipProbe pr(f);
  for (size_t at = 0; at < upTo;) {
    const size_t n = std::min(chunk, upTo - at);
    pr.feed(b.data() + at, n);
    at += n;
  }
  return pr;
}

// The independent oracle: where minimp3 says each frame of `b` ends, and how many samples
// per frame its layer and sampling rate give.
struct Oracle {
  std::vector<size_t> ends;   // byte offset just past each frame
  uint64_t samples = 0;
  uint32_t rate = 0;
};
static Oracle oracleOf(const Bytes& b) {
  Oracle o;
  mp3dec_t dec;
  mp3dec_init(&dec);
  std::vector<uint8_t> scratch((size_t)mp3dec_scratch_size());
  size_t off = 0;
  for (int guard = 0; off < b.size() && guard < 100000; guard++) {
    mp3dec_frame_info_t info;
    std::memset(&info, 0, sizeof(info));
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_decode_frame_ex(&dec, b.data() + off, (int)(b.size() - off), pcm, &info, scratch.data());
    if (info.frame_bytes <= 0) break;
    off += (size_t)info.frame_offset + (size_t)info.frame_bytes;
    o.ends.push_back(off);
    o.rate = (uint32_t)info.hz;
    o.samples += info.hz >= 32000 ? 1152 : 576;   // layer III: MPEG 1 vs MPEG 2 / 2.5
  }
  return o;
}

// Fixture A carries an Info (Xing) frame with a bigger bitrate than the audio after it
// (ffmpeg writes it that way), which minimp3 resyncs past instead of reporting. Its frame ends
// are the packet positions ffprobe -show_entries packet=pos,size reports: the Info frame ends
// at 237 (45 bytes of ID3v2 plus 192), then audio frames of 96 bytes to the end.
static Oracle oracleForA(const Bytes& b) {
  Oracle o;
  for (size_t e = 237; e <= b.size(); e += 96) o.ends.push_back(e);
  o.rate = 24000;
  o.samples = 576 * o.ends.size();
  return o;
}

struct Fixture {
  const char* name;
  Bytes bytes;
  bool minimp3Reports;   // false: the frame ends come from oracleForA
};
static std::vector<Fixture> fixtures() {
  return {{"A: MPEG-2 CBR with ID3v2 and an Info frame", bytesOf(kClipA, kClipALen), false},
          {"B: MPEG-2 CBR bare frames", bytesOf(kClipB, kClipBLen), true},
          {"C: MPEG-1 CBR padded frames", bytesOf(kClipC, kClipCLen), true},
          {"D: MPEG-2 VBR", bytesOf(kClipD, kClipDLen), true}};
}
static Oracle oracleFor(const Fixture& fx) { return fx.minimp3Reports ? oracleOf(fx.bytes) : oracleForA(fx.bytes); }

// THE class: at every prefix length of every real clip, "ends on a boundary" is true
// exactly where the independent decoder says a frame ends (and after at least one frame).
static void test_a_real_clip_ends_on_a_boundary_exactly_at_a_frame_end() {
  for (const Fixture& fx : fixtures()) {
    const Oracle o = oracleFor(fx);
    TEST_ASSERT_TRUE_MESSAGE(o.ends.size() >= 5, fx.name);
    TEST_ASSERT_EQUAL_MESSAGE(fx.bytes.size(), o.ends.back(), fx.name);   // the fixture is whole frames
    for (size_t k = 0; k <= fx.bytes.size(); k++) {
      bool boundary = false;
      for (size_t e : o.ends) boundary = boundary || e == k;
      const ClipProbe pr = probeOf(ClipFormat::Mp3, fx.bytes, k, 97);
      TEST_ASSERT_TRUE_MESSAGE(pr.readable(), fx.name);
      char msg[96];
      std::snprintf(msg, sizeof(msg), "%s, cut at %u of %u", fx.name, (unsigned)k, (unsigned)fx.bytes.size());
      TEST_ASSERT_EQUAL_MESSAGE(boundary, pr.endsOnBoundary(), msg);
    }
    const ClipProbe whole = probeOf(ClipFormat::Mp3, fx.bytes, fx.bytes.size(), 512);
    TEST_ASSERT_EQUAL_MESSAGE((uint64_t)o.ends.size(), whole.units(), fx.name);
    TEST_ASSERT_EQUAL_MESSAGE((uint32_t)(o.samples * 1000u / o.rate), whole.durationMs(), fx.name);
  }
}

// Chunking never matters: bytes arrive in whatever pieces the socket gives.
static void test_chunking_does_not_change_the_verdict() {
  for (const Fixture& fx : fixtures()) {
    const Oracle o = oracleFor(fx);
    for (size_t cut : {fx.bytes.size(), o.ends[2], o.ends[2] + 1, o.ends[3] - 1, (size_t)1, (size_t)5}) {
      const ClipProbe ref = probeOf(ClipFormat::Mp3, fx.bytes, cut, cut ? cut : 1);
      for (size_t chunk : {(size_t)1, (size_t)3, (size_t)64, (size_t)1000}) {
        const ClipProbe pr = probeOf(ClipFormat::Mp3, fx.bytes, cut, chunk);
        TEST_ASSERT_EQUAL_MESSAGE(ref.endsOnBoundary(), pr.endsOnBoundary(), fx.name);
        TEST_ASSERT_EQUAL_MESSAGE(ref.readable(), pr.readable(), fx.name);
        TEST_ASSERT_EQUAL_MESSAGE(ref.units(), pr.units(), fx.name);
        TEST_ASSERT_EQUAL_MESSAGE(ref.durationMs(), pr.durationMs(), fx.name);
      }
    }
  }
}

// An ID3v1 tag after the last frame is not a cut; an ID3v2 tag with no frame after it is
// not a clip.
static void test_tags_are_not_cuts_and_a_tag_alone_is_not_a_clip() {
  Bytes withTag = bytesOf(kClipB, kClipBLen);
  const Oracle o = oracleOf(withTag);
  withTag.push_back('T'); withTag.push_back('A'); withTag.push_back('G');
  withTag.insert(withTag.end(), 125, 0x20);
  ClipProbe pr = probeOf(ClipFormat::Mp3, withTag, withTag.size(), 512);
  TEST_ASSERT_TRUE(pr.readable());
  TEST_ASSERT_TRUE(pr.endsOnBoundary());
  TEST_ASSERT_EQUAL((uint64_t)o.ends.size(), pr.units());
  // A: its ID3v2 tag alone (everything before the first frame) has no audio.
  const Bytes a = bytesOf(kClipA, kClipALen);
  const size_t firstFrameEnd = oracleForA(a).ends[0];
  for (size_t k = 0; k < firstFrameEnd; k += 7) {
    const ClipProbe tagOnly = probeOf(ClipFormat::Mp3, a, k, 64);
    TEST_ASSERT_TRUE(tagOnly.readable());
    TEST_ASSERT_FALSE(tagOnly.endsOnBoundary());
    TEST_ASSERT_EQUAL(0, (int)tagOnly.units());
  }
  // No bytes at all is not a clip either.
  TEST_ASSERT_FALSE(ClipProbe(ClipFormat::Mp3).endsOnBoundary());
}

// A stream the probe cannot follow gets no verdict (readable false), never a "cut".
static void test_an_unreadable_stream_has_no_verdict() {
  const Bytes good = bytesOf(kClipB, kClipBLen);
  const Oracle o = oracleOf(good);
  {   // garbage first
    Bytes b(200, 0x00);
    const ClipProbe pr = probeOf(ClipFormat::Mp3, b, b.size(), 64);
    TEST_ASSERT_FALSE(pr.readable());
    TEST_ASSERT_FALSE(pr.endsOnBoundary());
  }
  {   // a corrupted header mid-stream
    Bytes b = good;
    b[o.ends[1]] = 0x12;
    const ClipProbe pr = probeOf(ClipFormat::Mp3, b, b.size(), 64);
    TEST_ASSERT_FALSE(pr.readable());
  }
  {   // a WAV read as an MP3
    const char riff[] = "RIFF....WAVEfmt ";
    Bytes b(riff, riff + sizeof(riff) - 1);
    TEST_ASSERT_FALSE(probeOf(ClipFormat::Mp3, b, b.size(), 4).readable());
  }
  {   // reserved and invalid header fields
    const uint8_t bad[][4] = {{0xFF, 0xE9, 0x90, 0x00},   // version 01 (reserved)
                              {0xFF, 0xF9, 0x90, 0x00},   // layer 00 (reserved)
                              {0xFF, 0xFB, 0xF0, 0x00},   // bitrate index 15
                              {0xFF, 0xFB, 0x00, 0x00},   // free format
                              {0xFF, 0xFB, 0x9C, 0x00},   // sample rate index 3
                              {0xFE, 0xFB, 0x90, 0x00}};  // no sync
    for (const auto& h : bad) {
      ClipProbe pr(ClipFormat::Mp3);
      pr.feed(h, 4);
      TEST_ASSERT_FALSE(pr.readable());
    }
  }
}

// The frame arithmetic for every version and layer, against sizes known from the MPEG tables.
static Bytes syntheticFrame(unsigned ver, unsigned layer, unsigned brIdx, unsigned srIdx, unsigned pad, size_t len) {
  Bytes f(len, 0);
  f[0] = 0xFF;
  f[1] = (uint8_t)(0xE0 | (ver << 3) | (layer << 1) | 1);   // protection bit off
  f[2] = (uint8_t)((brIdx << 4) | (srIdx << 2) | (pad << 1));
  f[3] = 0xC0;
  return f;
}
static void test_frame_lengths_follow_the_mpeg_tables() {
  struct Row { const char* what; unsigned ver, br, sr, pad; size_t len; };
  const Row rows[] = {   // layer III, as the speech providers send it
      {"MPEG-1 128k 44.1k", 3, 9, 0, 0, 417},
      {"MPEG-1 128k 44.1k padded", 3, 9, 0, 1, 418},
      {"MPEG-1 320k 48k", 3, 14, 1, 0, 960},
      {"MPEG-1 32k 32k", 3, 1, 2, 0, 144},
      {"MPEG-2 64k 22.05k", 2, 8, 0, 0, 208},
      {"MPEG-2 32k 24k", 2, 4, 1, 0, 96},
      {"MPEG-2.5 8k 8k", 0, 1, 2, 0, 72},
  };
  for (const Row& r : rows) {
    const Bytes whole = syntheticFrame(r.ver, 1, r.br, r.sr, r.pad, r.len);
    ClipProbe pr = probeOf(ClipFormat::Mp3, whole, whole.size(), 64);
    TEST_ASSERT_TRUE_MESSAGE(pr.readable(), r.what);
    TEST_ASSERT_TRUE_MESSAGE(pr.endsOnBoundary(), r.what);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)pr.units(), r.what);
    const ClipProbe cut = probeOf(ClipFormat::Mp3, whole, whole.size() - 1, 64);
    TEST_ASSERT_FALSE_MESSAGE(cut.endsOnBoundary(), r.what);
    // Two frames in a row, then a cut one byte into the third.
    Bytes three = whole;
    three.insert(three.end(), whole.begin(), whole.end());
    three.insert(three.end(), whole.begin(), whole.begin() + 1);
    const ClipProbe t = probeOf(ClipFormat::Mp3, three, three.size(), 5);
    TEST_ASSERT_TRUE_MESSAGE(t.readable(), r.what);
    TEST_ASSERT_FALSE_MESSAGE(t.endsOnBoundary(), r.what);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)t.units(), r.what);
  }
  // Layers I and II are not what the providers send: no verdict, never a "cut".
  for (unsigned layer : {2u, 3u}) {
    const Bytes frame = syntheticFrame(3, layer, 9, 0, 0, 417);
    TEST_ASSERT_FALSE(probeOf(ClipFormat::Mp3, frame, frame.size(), 64).readable());
  }
}

// ---- WAV ---------------------------------------------------------------------------------

static void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}
static void put16(Bytes& b, uint16_t v) {
  b.push_back((uint8_t)v);
  b.push_back((uint8_t)(v >> 8));
}
// 16-bit mono 24 kHz PCM with a RIFF header; `declared` is the data chunk size written.
struct WavSpec {
  uint16_t blockAlign = 2;               // bytes a sample takes (2: 16-bit mono, 1: 8-bit mono)
  uint32_t samples = 400;
  uint32_t declared = 0xFFFFFFFFu - 1;   // sentinel: write the true size
  uint32_t fmtSize = 16;
  bool listAfter = false;
  uint32_t dataOdd = 0;                  // extra odd byte of data (and its pad)
};
static Bytes makeWav(const WavSpec& s, size_t* headerLen) {
  Bytes b;
  const uint32_t dataBytes = s.samples * s.blockAlign + s.dataOdd;
  b.insert(b.end(), {'R', 'I', 'F', 'F'});
  put32(b, 0xFFFFFFFFu);
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(b, s.fmtSize);
  put16(b, 1);          // PCM
  put16(b, 1);          // mono
  put32(b, 24000);
  put32(b, 24000u * s.blockAlign);   // byte rate
  put16(b, s.blockAlign);
  put16(b, (uint16_t)(8 * s.blockAlign));
  for (uint32_t i = 16; i < s.fmtSize; i++) b.push_back(0);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  put32(b, s.declared == 0xFFFFFFFFu - 1 ? dataBytes : s.declared);
  if (headerLen) *headerLen = b.size();
  for (uint32_t i = 0; i < dataBytes; i++) b.push_back((uint8_t)(i * 7));
  if (dataBytes & 1u) b.push_back(0);   // pad byte after an odd-sized chunk
  if (s.listAfter) {
    b.insert(b.end(), {'L', 'I', 'S', 'T'});
    put32(b, 4);
    b.insert(b.end(), {'I', 'N', 'F', 'O'});
  }
  return b;
}

// A WAV that declares its data size is whole only once every declared byte is there (and a
// trailing chunk or pad byte changes nothing); every shorter prefix is a cut, whole samples
// or not.
static void test_a_wav_is_whole_only_at_its_declared_size() {
  WavSpec specs[3];
  specs[1].fmtSize = 18;
  specs[2].listAfter = true;
  for (const WavSpec& s : specs) {
    size_t head = 0;
    const Bytes wav = makeWav(s, &head);
    const size_t dataEnd = head + s.samples * s.blockAlign;
    for (size_t k = 0; k <= wav.size(); k++) {
      const ClipProbe pr = probeOf(ClipFormat::Wav, wav, k, 33);
      TEST_ASSERT_TRUE(pr.readable());
      TEST_ASSERT_EQUAL_MESSAGE(k >= dataEnd, pr.endsOnBoundary(), "declared size");
    }
    const ClipProbe whole = probeOf(ClipFormat::Wav, wav, wav.size(), 1000);
    TEST_ASSERT_EQUAL((int)(s.samples * s.blockAlign), (int)whole.units());
    TEST_ASSERT_EQUAL(s.samples * 1000 / 24000, (int)whole.durationMs());
  }
  // An odd data size (8-bit samples) is followed by a pad byte and then a trailing chunk: it is
  // whole once its data is, the pad and the chunk change nothing, and nothing short of it is whole.
  WavSpec odd;
  odd.blockAlign = 1;
  odd.samples = 101;
  odd.listAfter = true;
  size_t oddHead = 0;
  const Bytes oddWav = makeWav(odd, &oddHead);
  for (size_t k = 0; k <= oddWav.size(); k++) {
    const ClipProbe pr = probeOf(ClipFormat::Wav, oddWav, k, 7);
    TEST_ASSERT_TRUE(pr.readable());
    TEST_ASSERT_EQUAL_MESSAGE(k >= oddHead + 101, pr.endsOnBoundary(), "odd declared size");
  }
}

// A streamed WAV (the size field 0 or the maximum) cannot be checked against a size: it is
// whole at any whole sample, a cut mid-sample is a cut, and no data is not a clip.
static void test_a_streamed_wav_is_checked_by_whole_samples() {
  for (uint32_t declared : {0u, 0xFFFFFFFFu}) {
    WavSpec s;
    s.declared = declared;
    size_t head = 0;
    const Bytes wav = makeWav(s, &head);
    for (size_t k = 0; k <= wav.size(); k++) {
      const ClipProbe pr = probeOf(ClipFormat::Wav, wav, k, 50);
      TEST_ASSERT_TRUE(pr.readable());
      const bool want = k > head && (k - head) % 2 == 0;
      TEST_ASSERT_EQUAL_MESSAGE(want, pr.endsOnBoundary(), "streamed size");
    }
  }
  // Not a WAV at all: no verdict.
  Bytes junk(64, 0x41);
  TEST_ASSERT_FALSE(probeOf(ClipFormat::Wav, junk, junk.size(), 8).readable());
  // A huge non-data chunk is not understood: no verdict.
  Bytes big = makeWav(WavSpec(), nullptr);
  big.resize(36);
  big.insert(big.end(), {'J', 'U', 'N', 'K'});
  put32(big, 0x7FFFFFFFu);
  TEST_ASSERT_FALSE(probeOf(ClipFormat::Wav, big, big.size(), 8).readable());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_real_clip_ends_on_a_boundary_exactly_at_a_frame_end);
  RUN_TEST(test_chunking_does_not_change_the_verdict);
  RUN_TEST(test_tags_are_not_cuts_and_a_tag_alone_is_not_a_clip);
  RUN_TEST(test_an_unreadable_stream_has_no_verdict);
  RUN_TEST(test_frame_lengths_follow_the_mpeg_tables);
  RUN_TEST(test_a_wav_is_whole_only_at_its_declared_size);
  RUN_TEST(test_a_streamed_wav_is_checked_by_whole_samples);
  return UNITY_END();
}
