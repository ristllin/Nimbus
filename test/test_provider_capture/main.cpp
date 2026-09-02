#include <unity.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/capture.h"

// CUM-49: the code-sandbox SD round-trip. When the assistant's sandbox writes a
// file, the device streams it from the provider's Files API onto the SD card. The
// TLS + SD glue is device code (HIL/live-proven, see the issue), but the two safety
// decisions are portable and pinned here:
//   - captureFitsCap  : the streaming byte LIMIT (sandbox exec limit)
//   - captureVerdict  : the truncation/integrity gate on the finished download
// plus a byte-level write->read->verify using the same content hash the device
// write path uses to prove the bytes that landed are the bytes that were sent.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the streaming byte cap (sandbox exec limit), as a class ---------------

static void test_cap_boundary_and_overflow() {
  const size_t cap = kProviderFileCapBytes;
  TEST_ASSERT_TRUE(captureFitsCap(0, cap, cap));        // exactly to the cap is allowed
  TEST_ASSERT_TRUE(captureFitsCap(cap - 1, 1, cap));    // last byte
  TEST_ASSERT_FALSE(captureFitsCap(cap, 1, cap));       // one past the cap
  TEST_ASSERT_FALSE(captureFitsCap(cap - 1, 2, cap));   // straddling the cap
  TEST_ASSERT_TRUE(captureFitsCap(0, 0, cap));          // a zero-length chunk is fine
  // A hostile huge `add` must be refused WITHOUT integer overflow wrapping to a
  // small sum that would sneak past a naive `have + add > cap` check.
  TEST_ASSERT_FALSE(captureFitsCap(1000, std::numeric_limits<size_t>::max(), cap));
  TEST_ASSERT_FALSE(captureFitsCap(std::numeric_limits<size_t>::max(), 1, cap));
}

// ---- the download integrity verdict, as a class ----------------------------

static const size_t NONE = std::numeric_limits<size_t>::max();

static void test_verdict_accepts_clean_download() {
  // Declared length, exact match, clean EOF -> saved.
  CaptureVerdict v = captureVerdict(200, 16, 16, /*eof=*/true, /*writeErr=*/false);
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_TRUE(v.reason.empty());
  // No Content-Length (server sent none), clean EOF, non-empty -> saved.
  CaptureVerdict v2 = captureVerdict(200, 16, NONE, true, false);
  TEST_ASSERT_TRUE(v2.ok);
}

static void test_verdict_refuses_every_failure_mode() {
  // One case per reason - a new failure path with no guard fails here.
  TEST_ASSERT_FALSE(captureVerdict(200, 5, 10, true, false).ok);     // short of declared length
  TEST_ASSERT_FALSE(captureVerdict(200, 10, 10, false, false).ok);   // deadline, not a clean EOF
  TEST_ASSERT_FALSE(captureVerdict(200, 0, NONE, true, false).ok);   // empty body
  TEST_ASSERT_FALSE(captureVerdict(500, 0, NONE, false, false).ok);  // HTTP error
  TEST_ASSERT_FALSE(captureVerdict(0, 0, NONE, false, false).ok);    // no response
  TEST_ASSERT_FALSE(captureVerdict(200, 999, 10, true, true).ok);    // write error / over cap

  // The reasons are specific enough to diagnose from a log.
  TEST_ASSERT_NOT_NULL(strstr(captureVerdict(200, 5, 10, true, false).reason.c_str(), "truncated"));
  TEST_ASSERT_NOT_NULL(strstr(captureVerdict(404, 0, NONE, false, false).reason.c_str(), "404"));
  TEST_ASSERT_NOT_NULL(strstr(captureVerdict(0, 0, NONE, false, false).reason.c_str(), "no response"));
  TEST_ASSERT_NOT_NULL(strstr(captureVerdict(200, 9, 9, true, true).reason.c_str(), "too large"));
}

static void test_writeerr_dominates() {
  // A write error / cap hit is fatal even if the status + counts otherwise look ok.
  CaptureVerdict v = captureVerdict(200, 8, 8, true, /*writeErr=*/true);
  TEST_ASSERT_FALSE(v.ok);
}

// ---- byte round-trip: write -> read -> verify (the SD integrity primitive) --

// The device write path (files_subsystem writeChunk) content-addresses a captured
// file with BlobHasher as it streams; a re-read must reproduce the identical digest,
// which is exactly how the round-trip is proven byte-for-byte. Model that here,
// independent of chunk boundaries and of the physical SD.
static std::string streamHash(const std::vector<uint8_t>& bytes, size_t chunk) {
  BlobHasher h;
  for (size_t i = 0; i < bytes.size(); i += chunk)
    h.update(bytes.data() + i, std::min(chunk, bytes.size() - i));
  return h.hex();
}

static void test_byte_roundtrip_hash_matches() {
  std::vector<uint8_t> payload;
  for (int i = 0; i < 5000; i++) payload.push_back((uint8_t)((i * 37 + 11) & 0xFF));

  // "Write" side (small streaming buffer) vs "read-back" side (different buffer):
  // the digest is identical, so the bytes that landed are the bytes that were sent.
  const std::string written = streamHash(payload, 512);
  const std::string readBack = streamHash(payload, 1024);
  TEST_ASSERT_EQUAL_STRING(written.c_str(), readBack.c_str());
  TEST_ASSERT_EQUAL_STRING(blobHash(std::string(payload.begin(), payload.end())).c_str(),
                           written.c_str());

  // A single corrupted byte on read-back is caught (the exact guard finishWrite
  // relies on to refuse a flaky-card write it didn't actually keep).
  std::vector<uint8_t> corrupt = payload;
  corrupt[2500] ^= 0x01;
  TEST_ASSERT_TRUE(streamHash(corrupt, 512) != written);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cap_boundary_and_overflow);
  RUN_TEST(test_verdict_accepts_clean_download);
  RUN_TEST(test_verdict_refuses_every_failure_mode);
  RUN_TEST(test_writeerr_dominates);
  RUN_TEST(test_byte_roundtrip_hash_matches);
  return UNITY_END();
}
