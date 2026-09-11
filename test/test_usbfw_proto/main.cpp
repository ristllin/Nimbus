// Host tests for the USB serial firmware-update wire protocol and the commit-order
// sequencer (CUM-390). The device runs the SAME code: usb_updater.h is Arduino-free
// so this asserts crc32, START-header + chunk framing round-trips, and the load-
// bearing arm-before-flip commit order without a board.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// src/ is not on the native include path (build_src_filter = -<*>); the header is
// header-only and Arduino-free, so include it directly by path.
#include "../../src/sys/usb_updater.h"

using namespace nimbus::usbfw;

void setUp() {}
void tearDown() {}

// ---- crc32 ------------------------------------------------------------------

static void test_crc32_known_vectors() {
  // Standard IEEE / zlib crc32 check value.
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                          crc32((const uint8_t*)s, std::strlen(s)));
  TEST_ASSERT_EQUAL_HEX32(0u, crc32((const uint8_t*)"", 0));
  const char* fox = "The quick brown fox jumps over the lazy dog";
  TEST_ASSERT_EQUAL_HEX32(0x414FA339u,
                          crc32((const uint8_t*)fox, std::strlen(fox)));
}

static void test_crc32_detects_single_bit_flip() {
  uint8_t a[64];
  for (int i = 0; i < 64; ++i) a[i] = (uint8_t)(i * 7 + 1);
  uint32_t c0 = crc32(a, sizeof a);
  a[31] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL(c0, crc32(a, sizeof a));
}

// ---- little-endian helpers --------------------------------------------------

static void test_le_roundtrip() {
  uint8_t b[4];
  wrU32le(b, 0x11223344u);
  TEST_ASSERT_EQUAL_HEX32(0x11223344u, rdU32le(b));
  TEST_ASSERT_EQUAL_HEX8(0x44, b[0]);   // little-endian on the wire
  TEST_ASSERT_EQUAL_HEX8(0x11, b[3]);
  uint8_t h[2];
  wrU16le(h, 0xBEEFu);
  TEST_ASSERT_EQUAL_HEX16(0xBEEFu, rdU16le(h));
}

// ---- START header pack / parse ---------------------------------------------

static void test_start_header_roundtrip() {
  StartHeader in;
  in.size = 1300000;
  for (int i = 0; i < 32; ++i) in.sha256[i] = (uint8_t)(0xA0 + i);
  uint8_t frame[kStartFrameLen];
  packStart(frame, in);
  // Magic is present, un-terminated.
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(frame, kMagic, kMagicLen));
  StartHeader out;
  TEST_ASSERT_TRUE(parseStart(frame, out));
  TEST_ASSERT_EQUAL_UINT32(in.size, out.size);
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(in.sha256, out.sha256, 32));
}

static void test_start_header_rejects_bad_magic() {
  uint8_t frame[kStartFrameLen] = {0};
  std::memcpy(frame, "NIMBUSFW0", kMagicLen);   // wrong version digit
  StartHeader out;
  TEST_ASSERT_FALSE(parseStart(frame, out));
}

// ---- CHNK frame round-trip --------------------------------------------------

static void test_chunk_roundtrip_and_crc() {
  uint8_t payload[512];
  for (int i = 0; i < 512; ++i) payload[i] = (uint8_t)(i ^ 0x5A);
  uint8_t frame[kTagLen + 2 + 512 + 4];
  size_t n = packChunk(frame, payload, 512);
  TEST_ASSERT_EQUAL_UINT(kTagLen + 2 + 512 + 4, n);
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(frame, kChunkTag, kTagLen));
  uint16_t len = rdU16le(frame + kTagLen);
  TEST_ASSERT_EQUAL_UINT16(512, len);
  const uint8_t* body = frame + kTagLen + 2;
  uint32_t crcField = rdU32le(body + len);
  TEST_ASSERT_TRUE(checkChunkCrc(body, len, crcField));
  // A flipped payload byte breaks the crc check.
  uint8_t corrupt[512];
  std::memcpy(corrupt, body, len);
  corrupt[100] ^= 0xFF;
  TEST_ASSERT_FALSE(checkChunkCrc(corrupt, len, crcField));
}

// ---- hex helpers ------------------------------------------------------------

static void test_hex_roundtrip() {
  uint8_t in[32];
  for (int i = 0; i < 32; ++i) in[i] = (uint8_t)(i * 3 + 5);
  char hex[65];
  bytesToHex(in, 32, hex);
  TEST_ASSERT_EQUAL_UINT(64, std::strlen(hex));
  uint8_t out[32];
  TEST_ASSERT_TRUE(hexToBytes(hex, out, 32));
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(in, out, 32));
}

static void test_hex_rejects_non_hex() {
  uint8_t out[4];
  TEST_ASSERT_FALSE(hexToBytes("00zz0011", out, 4));
}

// ---- commit-order sequencer (the arm-before-flip invariant) -----------------

struct Recorder {
  std::vector<std::string> calls;
  int  pending = -1;       // last value written to setPending
  bool flipResult = true;
  bool flipCalled = false;
};

static CommitHooks recorderHooks(Recorder& r) {
  CommitHooks h;
  h.ctx = &r;
  h.setPrevSlot = [](void* c, const char* v) {
    ((Recorder*)c)->calls.push_back(std::string("prev:") + v);
  };
  h.setBootCount = [](void* c, int v) {
    ((Recorder*)c)->calls.push_back("boots:" + std::to_string(v));
  };
  h.setPending = [](void* c, int v) {
    auto* r = (Recorder*)c;
    r->calls.push_back("pending:" + std::to_string(v));
    r->pending = v;
  };
  h.setLastResult = [](void* c, const char* v) {
    ((Recorder*)c)->calls.push_back(std::string("result:") + v);
  };
  h.setPendingNotes = [](void* c, const char* v) {
    ((Recorder*)c)->calls.push_back(std::string("notes:") + v);
  };
  h.endFlip = [](void* c) -> bool {
    auto* r = (Recorder*)c;
    r->flipCalled = true;
    r->calls.push_back("flip");
    return r->flipResult;
  };
  return h;
}

static int indexOf(const std::vector<std::string>& v, const std::string& s) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == s) return (int)i;
  return -1;
}

static void test_commit_arms_guard_before_flip() {
  Recorder r;
  CommitHooks h = recorderHooks(r);
  bool ok = commitInstall(h, "app0", "installing usb", nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(r.flipCalled);
  // Documented order: prev, boots:0, pending:1, result, (notes), flip.
  TEST_ASSERT_EQUAL_INT(0, indexOf(r.calls, "prev:app0"));
  TEST_ASSERT_EQUAL_INT(1, indexOf(r.calls, "boots:0"));
  int iPend = indexOf(r.calls, "pending:1");
  int iFlip = indexOf(r.calls, "flip");
  TEST_ASSERT_TRUE(iPend >= 0);
  TEST_ASSERT_TRUE(iFlip >= 0);
  TEST_ASSERT_TRUE_MESSAGE(iPend < iFlip,
                           "rollback guard must be armed BEFORE the boot flip");
  // A successful flip leaves the guard armed for bootGuard to validate.
  TEST_ASSERT_EQUAL_INT(1, r.pending);
}

static void test_commit_disarms_guard_on_flip_failure() {
  Recorder r;
  r.flipResult = false;   // simulate Update.end() returning false
  CommitHooks h = recorderHooks(r);
  bool ok = commitInstall(h, "app1", "installing usb", nullptr);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_TRUE(r.flipCalled);
  // The final pending write must be a disarm (0), and it must come AFTER the flip.
  int iFlip = indexOf(r.calls, "flip");
  int iDisarm = -1;
  for (size_t i = 0; i < r.calls.size(); ++i)
    if (r.calls[i] == "pending:0") iDisarm = (int)i;
  TEST_ASSERT_TRUE_MESSAGE(iDisarm > iFlip,
                           "a failed flip must disarm the pending guard");
  TEST_ASSERT_EQUAL_INT(0, r.pending);
}

static void test_commit_writes_notes_when_hook_present() {
  Recorder r;
  CommitHooks h = recorderHooks(r);
  commitInstall(h, "app0", "installing v1", "v1|fixed things");
  TEST_ASSERT_TRUE(indexOf(r.calls, "notes:v1|fixed things") >= 0);
}

// ---- framing reader (the device receive state machine) ----------------------

struct RSink {
  std::vector<std::string> events;
  bool startAccept = true;
  bool chunkWriteOk = true;
  bool doneOk = true;
};

static ReaderSink readerSink(RSink& r) {
  ReaderSink s;
  s.ctx = &r;
  s.onStart = [](void* c, uint32_t size, const uint8_t*) -> bool {
    auto* r = (RSink*)c;
    r->events.push_back("start:" + std::to_string(size));
    return r->startAccept;
  };
  s.onChunkOk = [](void* c, uint32_t seq, const uint8_t*, uint16_t len) -> bool {
    auto* r = (RSink*)c;
    r->events.push_back("chunk:" + std::to_string(seq) + ":" + std::to_string(len));
    return r->chunkWriteOk;
  };
  s.onResend = [](void* c, uint32_t seq) {
    ((RSink*)c)->events.push_back("resend:" + std::to_string(seq));
  };
  s.onDone = [](void* c, const uint8_t*) -> bool {
    auto* r = (RSink*)c;
    r->events.push_back("done");
    return r->doneOk;
  };
  s.onError = [](void* c, const char* reason) {
    ((RSink*)c)->events.push_back(std::string("error:") + reason);
  };
  return s;
}

static void feedAll(FrameReader& fr, const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) fr.feed(b);
}

static std::vector<uint8_t> makeStart(uint32_t size, const uint8_t sha[32]) {
  StartHeader h;
  h.size = size;
  std::memcpy(h.sha256, sha, 32);
  std::vector<uint8_t> v(kStartFrameLen);
  packStart(v.data(), h);
  return v;
}
static std::vector<uint8_t> makeChunk(const uint8_t* payload, uint16_t len) {
  std::vector<uint8_t> v(kTagLen + 2 + len + 4);
  packChunk(v.data(), payload, len);
  return v;
}
static std::vector<uint8_t> makeDone(const uint8_t sha[32]) {
  std::vector<uint8_t> v(kTagLen + 32);
  std::memcpy(v.data(), kDoneTag, kTagLen);
  std::memcpy(v.data() + kTagLen, sha, 32);
  return v;
}

static void test_reader_happy_path() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32];
  for (int i = 0; i < 32; ++i) sha[i] = (uint8_t)i;
  uint8_t payload[1000];
  for (int i = 0; i < 1000; ++i) payload[i] = (uint8_t)(i & 0xFF);

  feedAll(fr, makeStart(1000, sha));
  feedAll(fr, makeChunk(payload, 1000));
  feedAll(fr, makeDone(sha));

  TEST_ASSERT_TRUE(fr.done());
  TEST_ASSERT_EQUAL_UINT(3, r.events.size());
  TEST_ASSERT_EQUAL_STRING("start:1000", r.events[0].c_str());
  TEST_ASSERT_EQUAL_STRING("chunk:0:1000", r.events[1].c_str());
  TEST_ASSERT_EQUAL_STRING("done", r.events[2].c_str());
}

static void test_reader_finds_magic_in_noise() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  // Leading junk, including a false partial "NIMBUS" run, before the real magic.
  std::vector<uint8_t> stream = {'N', 'I', 'M', 'x', 'N', 'N'};
  auto start = makeStart(4, sha);
  stream.insert(stream.end(), start.begin(), start.end());
  feedAll(fr, stream);
  TEST_ASSERT_EQUAL_UINT(1, r.events.size());
  TEST_ASSERT_EQUAL_STRING("start:4", r.events[0].c_str());
}

static void test_reader_resends_on_bad_crc_then_recovers() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  uint8_t payload[64];
  for (int i = 0; i < 64; ++i) payload[i] = (uint8_t)(i + 1);

  feedAll(fr, makeStart(64, sha));
  auto chunk = makeChunk(payload, 64);
  chunk[10] ^= 0xFF;          // corrupt a payload byte -> crc mismatch
  feedAll(fr, chunk);
  auto good = makeChunk(payload, 64);   // host resends the SAME chunk, index 0
  feedAll(fr, good);
  feedAll(fr, makeDone(sha));

  TEST_ASSERT_TRUE(fr.done());
  // resend:0 fired, then the retry acked as chunk:0 (the index did not advance).
  TEST_ASSERT_EQUAL_STRING("start:64", r.events[0].c_str());
  TEST_ASSERT_EQUAL_STRING("resend:0", r.events[1].c_str());
  TEST_ASSERT_EQUAL_STRING("chunk:0:64", r.events[2].c_str());
  TEST_ASSERT_EQUAL_STRING("done", r.events[3].c_str());
}

static void test_reader_rejects_bad_frame_tag() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  feedAll(fr, makeStart(16, sha));
  std::vector<uint8_t> badtag = {'X', 'X', 'X', 'X'};
  feedAll(fr, badtag);
  TEST_ASSERT_FALSE(fr.done());
  TEST_ASSERT_EQUAL_STRING("error:badframe", r.events.back().c_str());
  TEST_ASSERT_TRUE(fr.scanning());   // reset back to hunting the magic
}

static void test_reader_rejects_sha_mismatch() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha1[32], sha2[32];
  for (int i = 0; i < 32; ++i) { sha1[i] = (uint8_t)i; sha2[i] = (uint8_t)(i + 1); }
  uint8_t payload[32] = {0};
  feedAll(fr, makeStart(32, sha1));
  feedAll(fr, makeChunk(payload, 32));
  feedAll(fr, makeDone(sha2));   // DONE hash disagrees with START
  TEST_ASSERT_FALSE(fr.done());
  TEST_ASSERT_EQUAL_STRING("error:shamismatch", r.events.back().c_str());
}

static void test_reader_rejects_zero_length_chunk() {
  RSink r;
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  feedAll(fr, makeStart(16, sha));
  std::vector<uint8_t> zero(kTagLen + 2 + 4, 0);
  std::memcpy(zero.data(), kChunkTag, kTagLen);      // len = 0, then a crc field
  feedAll(fr, zero);
  TEST_ASSERT_EQUAL_STRING("error:chunklen", r.events.back().c_str());
}

static void test_reader_start_refusal_returns_to_scan() {
  RSink r;
  r.startAccept = false;   // device refused (busy / confirm / slot)
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  feedAll(fr, makeStart(16, sha));
  TEST_ASSERT_EQUAL_UINT(1, r.events.size());
  TEST_ASSERT_EQUAL_STRING("start:16", r.events[0].c_str());
  TEST_ASSERT_TRUE(fr.scanning());   // no streaming after a refusal
  // A following chunk is ignored (treated as magic-scan noise), never written.
  uint8_t payload[8] = {0};
  feedAll(fr, makeChunk(payload, 8));
  TEST_ASSERT_EQUAL_UINT(1, r.events.size());
}

static void test_reader_write_failure_aborts() {
  RSink r;
  r.chunkWriteOk = false;   // localWrite fault
  FrameReader fr;
  fr.sink = readerSink(r);
  uint8_t sha[32] = {0};
  uint8_t payload[16];
  for (int i = 0; i < 16; ++i) payload[i] = (uint8_t)i;
  feedAll(fr, makeStart(16, sha));
  feedAll(fr, makeChunk(payload, 16));
  TEST_ASSERT_FALSE(fr.done());
  TEST_ASSERT_TRUE(fr.scanning());   // reset after the write error
  TEST_ASSERT_EQUAL_STRING("chunk:0:16", r.events.back().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_known_vectors);
  RUN_TEST(test_crc32_detects_single_bit_flip);
  RUN_TEST(test_le_roundtrip);
  RUN_TEST(test_start_header_roundtrip);
  RUN_TEST(test_start_header_rejects_bad_magic);
  RUN_TEST(test_chunk_roundtrip_and_crc);
  RUN_TEST(test_hex_roundtrip);
  RUN_TEST(test_hex_rejects_non_hex);
  RUN_TEST(test_commit_arms_guard_before_flip);
  RUN_TEST(test_commit_disarms_guard_on_flip_failure);
  RUN_TEST(test_commit_writes_notes_when_hook_present);
  RUN_TEST(test_reader_happy_path);
  RUN_TEST(test_reader_finds_magic_in_noise);
  RUN_TEST(test_reader_resends_on_bad_crc_then_recovers);
  RUN_TEST(test_reader_rejects_bad_frame_tag);
  RUN_TEST(test_reader_rejects_sha_mismatch);
  RUN_TEST(test_reader_rejects_zero_length_chunk);
  RUN_TEST(test_reader_start_refusal_returns_to_scan);
  RUN_TEST(test_reader_write_failure_aborts);
  return UNITY_END();
}
