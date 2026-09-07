// test_upload_reassembly - the device-side chunked-upload state machine (protocol 2).
// Proves ordered byte-for-byte reassembly, monotonic ack offsets, and rejection of
// out-of-order / over-cap / over-total / wrong-id chunks. Pure host test, no hardware.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/cloud/upload_reassembly.h"

using nimbus::cloud::UploadReassembly;
using Outcome = UploadReassembly::ChunkOutcome;
using Status = UploadReassembly::Status;

void setUp() {}
void tearDown() {}

// A sink that collects everything it is handed (the device writes to the loopback here).
struct CollectSink {
  std::vector<uint8_t> bytes;
  std::function<bool(const uint8_t*, size_t)> fn() {
    return [this](const uint8_t* d, size_t n) {
      bytes.insert(bytes.end(), d, d + n);
      return true;
    };
  }
};

// Feed `total` bytes as `chunkLen`-sized ordered chunks; assert byte-identical reassembly
// and that the acked offset advanced monotonically to the total.
static void test_reassembles_in_order_byte_identical() {
  const uint32_t total = 5 * 1024 * 1024 + 123;  // the ~5 MB mp3 case
  const uint32_t chunkLen = 8 * 1024;
  std::vector<uint8_t> src(total);
  for (uint32_t i = 0; i < total; i++) src[i] = (uint8_t)((i * 131) ^ (i >> 5));

  UploadReassembly r;
  CollectSink sink;
  r.begin("up1", total, 64u * 1024 * 1024);
  TEST_ASSERT_TRUE(r.active());

  uint32_t off = 0, seq = 0, lastAck = 0;
  while (off < total) {
    const uint32_t n = (total - off < chunkLen) ? (total - off) : chunkLen;
    Outcome o = r.chunk("up1", seq, off, src.data() + off, n, sink.fn());
    TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)o);
    TEST_ASSERT_TRUE(r.acked() > lastAck);  // strictly monotonic
    lastAck = r.acked();
    off += n;
    seq++;
  }
  TEST_ASSERT_EQUAL_UINT32(total, r.acked());
  TEST_ASSERT_TRUE(r.end("up1"));
  TEST_ASSERT_EQUAL_INT((int)Status::Done, (int)r.status());
  TEST_ASSERT_EQUAL_UINT32(total, (uint32_t)sink.bytes.size());
  TEST_ASSERT_EQUAL_MEMORY(src.data(), sink.bytes.data(), total);
}

static void test_rejects_out_of_order_seq() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 30, 1000);
  uint8_t d[10] = {0};
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)r.chunk("u", 0, 0, d, 10, sink.fn()));
  // Skip seq 1 -> a seq-2 chunk is out of order.
  TEST_ASSERT_EQUAL_INT((int)Outcome::OutOfOrder, (int)r.chunk("u", 2, 10, d, 10, sink.fn()));
}

static void test_rejects_offset_gap() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 30, 1000);
  uint8_t d[10] = {0};
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)r.chunk("u", 0, 0, d, 10, sink.fn()));
  // Correct seq but a gapped offset (should be 10, not 20).
  TEST_ASSERT_EQUAL_INT((int)Outcome::OutOfOrder, (int)r.chunk("u", 1, 20, d, 10, sink.fn()));
}

static void test_rejects_wrong_id() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("right", 20, 1000);
  uint8_t d[10] = {0};
  TEST_ASSERT_EQUAL_INT((int)Outcome::WrongId, (int)r.chunk("wrong", 0, 0, d, 10, sink.fn()));
}

static void test_enforces_cap() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 0 /* totalLen unknown */, 16 /* cap */);
  uint8_t d[10] = {0};
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)r.chunk("u", 0, 0, d, 10, sink.fn()));
  // 10 + 10 = 20 > cap 16.
  TEST_ASSERT_EQUAL_INT((int)Outcome::OverCap, (int)r.chunk("u", 1, 10, d, 10, sink.fn()));
  TEST_ASSERT_EQUAL_INT((int)Status::Failed, (int)r.status());
}

static void test_enforces_declared_total() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 15 /* totalLen */, 1000);
  uint8_t d[10] = {0};
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)r.chunk("u", 0, 0, d, 10, sink.fn()));
  // 10 + 10 = 20 > declared total 15.
  TEST_ASSERT_EQUAL_INT((int)Outcome::OverTotal, (int)r.chunk("u", 1, 10, d, 10, sink.fn()));
}

static void test_end_requires_full_total() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 20, 1000);
  uint8_t d[10] = {0};
  r.chunk("u", 0, 0, d, 10, sink.fn());
  // Only 10 of 20 declared bytes arrived: end must fail (incomplete body).
  TEST_ASSERT_FALSE(r.end("u"));
  TEST_ASSERT_EQUAL_INT((int)Status::Failed, (int)r.status());
}

static void test_sink_failure_fails_upload() {
  UploadReassembly r;
  r.begin("u", 10, 1000);
  uint8_t d[10] = {0};
  auto badSink = [](const uint8_t*, size_t) { return false; };  // SD write failed
  TEST_ASSERT_EQUAL_INT((int)Outcome::SinkFailed, (int)r.chunk("u", 0, 0, d, 10, badSink));
  TEST_ASSERT_EQUAL_INT((int)Status::Failed, (int)r.status());
}

static void test_abort_resets() {
  UploadReassembly r;
  CollectSink sink;
  r.begin("u", 100, 1000);
  uint8_t d[10] = {0};
  r.chunk("u", 0, 0, d, 10, sink.fn());
  r.abort();
  TEST_ASSERT_FALSE(r.active());
  TEST_ASSERT_EQUAL_UINT32(0, r.acked());
  // A chunk after abort is not active.
  TEST_ASSERT_EQUAL_INT((int)Outcome::NotActive, (int)r.chunk("u", 1, 10, d, 10, sink.fn()));
}

static void test_zero_length_chunk_ok_no_sink_call() {
  UploadReassembly r;
  bool called = false;
  auto sink = [&](const uint8_t*, size_t) {
    called = true;
    return true;
  };
  r.begin("u", 0, 1000);
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)r.chunk("u", 0, 0, nullptr, 0, sink));
  TEST_ASSERT_FALSE(called);  // an empty slice never touches the sink
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_reassembles_in_order_byte_identical);
  RUN_TEST(test_rejects_out_of_order_seq);
  RUN_TEST(test_rejects_offset_gap);
  RUN_TEST(test_rejects_wrong_id);
  RUN_TEST(test_enforces_cap);
  RUN_TEST(test_enforces_declared_total);
  RUN_TEST(test_end_requires_full_total);
  RUN_TEST(test_sink_failure_fails_upload);
  RUN_TEST(test_abort_resets);
  RUN_TEST(test_zero_length_chunk_ok_no_sink_call);
  return UNITY_END();
}
