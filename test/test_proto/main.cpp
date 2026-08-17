#include <unity.h>

#include <cstring>

#include "nimbus/nsn_proto.h"
#include "nsn_vectors.h"

using namespace nimbus::nsn;

void setUp() {}
void tearDown() {}

static Frame frameFromVector(const NsnVector& v) {
  Frame f;
  f.seq = v.seq;
  f.brightness = v.brightness;
  f.count = v.segCount;
  for (uint8_t i = 0; i < v.segCount; ++i)
    f.segs[i] = Segment{v.segs[i].state, v.segs[i].hue, v.segs[i].anim, v.segs[i].span};
  return f;
}

static void assertFrameMatchesVector(const Frame& f, const NsnVector& v) {
  TEST_ASSERT_EQUAL_UINT8(v.seq, f.seq);
  TEST_ASSERT_EQUAL_UINT8(v.brightness, f.brightness);
  TEST_ASSERT_EQUAL_UINT8(v.segCount, f.count);
  for (uint8_t i = 0; i < v.segCount; ++i) {
    TEST_ASSERT_EQUAL_UINT8(v.segs[i].state, f.segs[i].state);
    TEST_ASSERT_EQUAL_UINT8(v.segs[i].hue, f.segs[i].hue);
    TEST_ASSERT_EQUAL_UINT8(v.segs[i].anim, f.segs[i].anim);
    TEST_ASSERT_EQUAL_UINT8(v.segs[i].span, f.segs[i].span);
  }
}

// encode() must reproduce every reference packet byte-for-byte.
static void test_encode_matches_reference_bytes() {
  for (size_t i = 0; i < kNsnVectorCount; ++i) {
    const NsnVector& v = kNsnVectors[i];
    Frame f = frameFromVector(v);
    uint8_t out[kMaxPacket];
    size_t n = encode(f, out, sizeof(out));
    TEST_ASSERT_EQUAL_MESSAGE(v.packetLen, n, v.name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(v.packet, out, n, v.name);
  }
}

static void test_decode_matches_reference_fields() {
  for (size_t i = 0; i < kNsnVectorCount; ++i) {
    const NsnVector& v = kNsnVectors[i];
    Frame f;
    TEST_ASSERT_TRUE_MESSAGE(decodePacket(v.packet, v.packetLen, f), v.name);
    assertFrameMatchesVector(f, v);
  }
}

// Encoder truncates >16 segments, mirroring the reference.
static void test_encode_truncates_overflow() {
  Frame f;
  f.count = 20;  // > kMaxSegs; only first 16 must be encoded
  for (int i = 0; i < kMaxSegs; ++i) f.segs[i] = Segment{0, uint8_t(10 + i), 1, 1};
  uint8_t out[kMaxPacket];
  size_t n = encode(f, out, sizeof(out));
  TEST_ASSERT_EQUAL(2 + 4 + 16 * 4 + 1, n);
  TEST_ASSERT_EQUAL_UINT8(16, out[2 + 2]);  // payload count byte
}

static void test_decode_rejects_bad_crc() {
  uint8_t pkt[kMaxPacket];
  memcpy(pkt, kNsnVectors[1].packet, kNsnVectors[1].packetLen);
  pkt[kNsnVectors[1].packetLen - 1] ^= 0xFF;
  Frame f;
  TEST_ASSERT_FALSE(decodePacket(pkt, kNsnVectors[1].packetLen, f));
}

static void test_decode_rejects_bad_sof_magic_short() {
  Frame f;
  uint8_t pkt[kMaxPacket];
  size_t len = kNsnVectors[1].packetLen;
  memcpy(pkt, kNsnVectors[1].packet, len);
  pkt[0] = 0xAB;
  TEST_ASSERT_FALSE(decodePacket(pkt, len, f));

  memcpy(pkt, kNsnVectors[1].packet, len);
  pkt[2] = 0x00;  // magic - CRC covers payload, so recompute to isolate the check
  pkt[len - 1] = crc8(pkt + 2, len - 3);
  TEST_ASSERT_FALSE(decodePacket(pkt, len, f));

  TEST_ASSERT_FALSE(decodePacket(kNsnVectors[1].packet, 5, f));
}

// Out-of-range state/anim rejects the frame (defensive divergence from the
// reference, which raises).
static void test_decode_rejects_invalid_enums() {
  Frame f;
  f.count = 1;
  f.segs[0] = Segment{7, 0, 0, 0};  // state 7 > Offline
  uint8_t pkt[kMaxPacket];
  size_t n = encode(f, pkt, sizeof(pkt));
  Frame out;
  TEST_ASSERT_FALSE(decodePacket(pkt, n, out));

  f.segs[0] = Segment{0, 0, 6, 0};  // anim 6 > Fade
  n = encode(f, pkt, sizeof(pkt));
  TEST_ASSERT_FALSE(decodePacket(pkt, n, out));
}

// Stream decoder: garbage prefix, then two back-to-back frames, then a
// corrupted one, then a valid one again (resync).
static void test_stream_decoder_resyncs() {
  Decoder d;
  Frame f;
  const NsnVector& a = kNsnVectors[1];
  const NsnVector& b = kNsnVectors[3];

  const uint8_t garbage[] = {0x00, 0x13, 0x37, 0xAA};  // trailing 0xAA = false SOF
  int decoded = 0;
  for (uint8_t g : garbage)
    if (d.feed(g, f)) ++decoded;
  // False SOF eats the next bytes as LEN/body; a corrupted "packet" must not
  // decode, and the decoder must recover on the next real frame boundary.
  for (size_t i = 0; i < a.packetLen; ++i)
    if (d.feed(a.packet[i], f)) ++decoded;
  d.reset();  // transport-level resync (e.g. after an RX gap)

  for (size_t i = 0; i < a.packetLen; ++i)
    if (d.feed(a.packet[i], f)) ++decoded;
  TEST_ASSERT_EQUAL(1, decoded);
  assertFrameMatchesVector(f, a);

  for (size_t i = 0; i < b.packetLen; ++i)
    if (d.feed(b.packet[i], f)) ++decoded;
  TEST_ASSERT_EQUAL(2, decoded);
  assertFrameMatchesVector(f, b);
}

static void test_stream_decoder_back_to_back() {
  Decoder d;
  Frame f;
  int decoded = 0;
  for (size_t v = 0; v < kNsnVectorCount; ++v)
    for (size_t i = 0; i < kNsnVectors[v].packetLen; ++i)
      if (d.feed(kNsnVectors[v].packet[i], f)) ++decoded;
  TEST_ASSERT_EQUAL(int(kNsnVectorCount), decoded);
}

// v2: harness + title round-trip through encode/decode.
static void test_v2_harness_title_roundtrip() {
  Frame f;
  f.seq = 7; f.brightness = 200; f.count = 2;
  f.segs[0] = Segment{1, 170, 3, 0};
  f.segs[0].harness = kHarnessCodex; std::strcpy(f.segs[0].title, "nimbus");
  f.segs[1] = Segment{2, 213, 2, 0};
  f.segs[1].harness = kHarnessClaude; std::strcpy(f.segs[1].title, "docs sweep");
  uint8_t buf[kMaxPacket];
  size_t n = encode(f, buf, sizeof buf);
  TEST_ASSERT_TRUE(n > 0);
  Frame g;
  TEST_ASSERT_TRUE(decodePacket(buf, n, g));
  TEST_ASSERT_EQUAL_UINT8(2, g.count);
  TEST_ASSERT_EQUAL_UINT8(kHarnessCodex, g.segs[0].harness);
  TEST_ASSERT_EQUAL_STRING("nimbus", g.segs[0].title);
  TEST_ASSERT_EQUAL_STRING("codex", harnessName(g.segs[0].harness));
  TEST_ASSERT_EQUAL_UINT8(kHarnessClaude, g.segs[1].harness);
  TEST_ASSERT_EQUAL_STRING("docs sweep", g.segs[1].title);
  // Base fields still intact.
  TEST_ASSERT_EQUAL_UINT8(1, g.segs[0].state);
  TEST_ASSERT_EQUAL_UINT8(170, g.segs[0].hue);
}

// A v1 frame (no harness/title) encodes byte-identically (no TLV) and decodes with
// empty v2 fields - the byte-locked vectors remain valid.
static void test_v1_frame_has_no_tlv() {
  Frame f; f.seq = 3; f.brightness = 50; f.count = 1;
  f.segs[0] = Segment{1, 170, 3, 0};
  uint8_t buf[kMaxPacket];
  size_t n = encode(f, buf, sizeof buf);
  TEST_ASSERT_EQUAL_UINT32(11u, n);           // 2 + (4 + 4) + 1, no TLV
  Frame g;
  TEST_ASSERT_TRUE(decodePacket(buf, n, g));
  TEST_ASSERT_EQUAL_UINT8(0, g.segs[0].harness);
  TEST_ASSERT_EQUAL_STRING("", g.segs[0].title);
}

// A max-length title (buffer full to the cap) encodes + round-trips at exactly
// kMaxTitle - encode's titleLen() caps at the buffer, never reading past it.
static void test_v2_title_truncates() {
  Frame f; f.count = 1;
  f.segs[0] = Segment{1, 170, 3, 0};
  f.segs[0].harness = kHarnessVibe;
  for (int i = 0; i < kMaxTitle; ++i) f.segs[0].title[i] = 'x';  // fill to cap (buffer is kMaxTitle+1)
  f.segs[0].title[kMaxTitle] = 0;             // NUL terminator (last valid index)
  uint8_t buf[kMaxPacket];
  size_t n = encode(f, buf, sizeof buf);
  Frame g;
  TEST_ASSERT_TRUE(decodePacket(buf, n, g));
  TEST_ASSERT_EQUAL_UINT32((unsigned)kMaxTitle, (unsigned)std::strlen(g.segs[0].title));
}

// Decode-side clamp: a hostile/corrupt packet whose titleLen byte claims MORE than
// kMaxTitle must be clamped (no OOB write into Segment.title[kMaxTitle+1]) and the
// title NUL-terminated at the cap.
static void test_v2_decode_clamps_oversize_title() {
  // Build a v2 packet by hand: 1 segment, base block + TLV [harness][titleLen=kMaxTitle+5][chars]
  const uint8_t tlClaim = uint8_t(kMaxTitle) + 5;
  uint8_t payload[4 + 4 + 2 + 64];
  size_t pi = 0;
  payload[pi++] = 0x4E;            // magic (kMagic)
  payload[pi++] = 1;              // seq
  payload[pi++] = 1;              // count
  payload[pi++] = 100;            // brightness
  payload[pi++] = 1;              // state
  payload[pi++] = 170;           // hue
  payload[pi++] = 1;             // anim
  payload[pi++] = 0;             // span
  payload[pi++] = kHarnessCodex;  // harness
  payload[pi++] = tlClaim;        // titleLen (over-long)
  for (int i = 0; i < tlClaim; ++i) payload[pi++] = 'z';
  const uint8_t payloadLen = uint8_t(pi);
  uint8_t buf[kMaxPacket];
  buf[0] = 0xAA;                  // SOF (kSof)
  buf[1] = payloadLen;
  for (size_t i = 0; i < payloadLen; ++i) buf[2 + i] = payload[i];
  buf[2 + payloadLen] = crc8(payload, payloadLen);
  Frame g;
  TEST_ASSERT_TRUE(decodePacket(buf, 2 + payloadLen + 1, g));
  // Clamped to kMaxTitle chars, NUL-terminated, no overflow.
  TEST_ASSERT_EQUAL_UINT32((unsigned)kMaxTitle, (unsigned)std::strlen(g.segs[0].title));
}

// CROSS-REPO BYTE-LOCK (v2): the broker (../nsnotify frame.py) encode of
// [Running/codex/"nimbus", WaitingInput/claude/"docs sweep"] @ seq7 bright90. The
// device must decode these EXACT bytes to the right harness+title, AND its own
// encoder must reproduce them. Regenerate this literal if frame.py's v2 format changes
// (python: notify.broker.frame.encode_frame(...).hex()).
static void test_v2_byte_locked_to_broker() {
  static const uint8_t pkt[] = {
      0xAA, 0x20, 0x4E, 0x07, 0x02, 0x5A, 0x01, 0xAA, 0x03, 0x00, 0x02, 0xD5,
      0x02, 0x00, 0x02, 0x06, 0x6E, 0x69, 0x6D, 0x62, 0x75, 0x73, 0x01, 0x0A,
      0x64, 0x6F, 0x63, 0x73, 0x20, 0x73, 0x77, 0x65, 0x65, 0x70, 0x1B};
  Frame g;
  TEST_ASSERT_TRUE(decodePacket(pkt, sizeof pkt, g));
  TEST_ASSERT_EQUAL_UINT8(2, g.count);
  TEST_ASSERT_EQUAL_UINT8(90, g.brightness);
  TEST_ASSERT_EQUAL_UINT8(7, g.seq);
  TEST_ASSERT_EQUAL_UINT8(kHarnessCodex, g.segs[0].harness);
  TEST_ASSERT_EQUAL_STRING("nimbus", g.segs[0].title);
  TEST_ASSERT_EQUAL_UINT8(kHarnessClaude, g.segs[1].harness);
  TEST_ASSERT_EQUAL_STRING("docs sweep", g.segs[1].title);

  Frame f;
  f.seq = 7; f.brightness = 90; f.count = 2;
  f.segs[0] = Segment{1, 170, 3, 0};
  f.segs[0].harness = kHarnessCodex; std::strcpy(f.segs[0].title, "nimbus");
  f.segs[1] = Segment{2, 213, 2, 0};
  f.segs[1].harness = kHarnessClaude; std::strcpy(f.segs[1].title, "docs sweep");
  uint8_t buf[kMaxPacket];
  size_t n = encode(f, buf, sizeof buf);
  TEST_ASSERT_EQUAL_UINT32((unsigned)sizeof pkt, (unsigned)n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pkt, buf, sizeof pkt);   // device encode == broker encode
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_matches_reference_bytes);
  RUN_TEST(test_decode_matches_reference_fields);
  RUN_TEST(test_encode_truncates_overflow);
  RUN_TEST(test_decode_rejects_bad_crc);
  RUN_TEST(test_decode_rejects_bad_sof_magic_short);
  RUN_TEST(test_decode_rejects_invalid_enums);
  RUN_TEST(test_stream_decoder_resyncs);
  RUN_TEST(test_stream_decoder_back_to_back);
  RUN_TEST(test_v2_harness_title_roundtrip);
  RUN_TEST(test_v1_frame_has_no_tlv);
  RUN_TEST(test_v2_title_truncates);
  RUN_TEST(test_v2_decode_clamps_oversize_title);
  RUN_TEST(test_v2_byte_locked_to_broker);
  return UNITY_END();
}
