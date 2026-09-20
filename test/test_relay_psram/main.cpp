// test_relay_psram (CUM-387) - the relay/cloud-sync staging buffers must route to PSRAM
// on device, off the scarce internal SRAM the TLS session + display DMA bounce buffer
// share. On host the firmware flag is off, so the container is a plain std::vector; here
// we define the device flag WITH a fake heap_caps backend (NIMBUS_PSRAM_ALLOC_TEST) so
// the actual PSRAM allocator path is compiled and its routing is asserted - the guard
// that stops the footprint silently regrowing back onto internal heap.
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <type_traits>

// Fake heap_caps backend: record every allocation's caps so we can prove the buffer
// asked for PSRAM. Must be visible before the header that uses it.
#define NIMBUS_RELAY_PSRAM_BODY
#define NIMBUS_PSRAM_ALLOC_TEST
#define MALLOC_CAP_SPIRAM 0x00000400u
#define MALLOC_CAP_8BIT 0x00000004u

static int g_spiramReq = 0;   // allocations that requested PSRAM
static int g_internalReq = 0; // allocations that fell back to (or asked) internal

extern "C" void* heap_caps_malloc(size_t n, uint32_t caps) {
  if (caps & MALLOC_CAP_SPIRAM) g_spiramReq++;
  else g_internalReq++;
  return std::malloc(n ? n : 1);
}
extern "C" void heap_caps_free(void* p) { std::free(p); }

#include "nimbus/cloud/psram_vector.h"
// The ACTUAL relay staging types (not just the generic alias): the HTTP response
// body/parser staging and the inbound WS message payload. Asserting on these is
// what catches a revert of one specific relay buffer back onto the internal heap.
#include "nimbus/cloud/http_replay.h"
#include "nimbus/cloud/relay_ws.h"

using nimbus::cloud::PsVector;
using nimbus::cloud::PsramAlloc;

void setUp() { g_spiramReq = 0; g_internalReq = 0; }
void tearDown() {}

// The aliased vector's allocator is the PSRAM allocator when the device flag is set.
static void test_alias_is_psram_allocated() {
  TEST_ASSERT_TRUE((std::is_same<PsVector<uint8_t>::allocator_type, PsramAlloc<uint8_t>>::value));
}

// A growing buffer routes its backing store to PSRAM (MALLOC_CAP_SPIRAM), never the
// scarce internal heap, so a full inbound frame / response body costs PSRAM not SRAM.
static void test_psvector_routes_to_spiram() {
  {
    PsVector<uint8_t> v;
    v.resize(4096);
    v.push_back(1);  // force at least one growth
  }
  TEST_ASSERT_GREATER_THAN(0, g_spiramReq);   // it asked PSRAM first
  TEST_ASSERT_EQUAL_INT(0, g_internalReq);    // and PSRAM was available, so no SRAM fallback
}

// The response body/staging type used by the parser is the PSRAM-backed alias, so this
// mirrors the header static_assert that fails the firmware build on a regression.
static void test_body_buffer_type_is_psram() {
  // http_replay pulls the same alias; a plain std::vector<uint8_t> would differ here.
  TEST_ASSERT_FALSE((std::is_same<PsVector<uint8_t>, std::vector<uint8_t>>::value));
}

// The ACTUAL type the HTTP response parser stages into (ResponseParser buf_/body_).
// A revert of `using BodyBuf = ...` to a plain std::vector<uint8_t> fails here, not
// just at the generic alias - this is the buffer that reached ~278 KB on a sync.
static void test_http_body_buf_is_psram_backed() {
  using nimbus::cloud::http_replay::BodyBuf;
  TEST_ASSERT_TRUE((std::is_same<BodyBuf::allocator_type, PsramAlloc<uint8_t>>::value));
  {
    BodyBuf b;
    b.resize(4096);
    b.push_back(1);  // force a growth
  }
  TEST_ASSERT_GREATER_THAN(0, g_spiramReq);
  TEST_ASSERT_EQUAL_INT(0, g_internalReq);
}

// The ACTUAL inbound WS message payload type (ws::Message::payload) - the decoded
// tunneled request body that can reach the 16 KB frame cap on the internal heap.
static void test_ws_message_payload_is_psram_backed() {
  using nimbus::cloud::ws::Message;
  TEST_ASSERT_TRUE((std::is_same<decltype(Message::payload)::allocator_type,
                                 PsramAlloc<uint8_t>>::value));
  {
    Message m;
    m.payload.resize(4096);
    m.payload.push_back(1);
  }
  TEST_ASSERT_GREATER_THAN(0, g_spiramReq);
  TEST_ASSERT_EQUAL_INT(0, g_internalReq);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_alias_is_psram_allocated);
  RUN_TEST(test_psvector_routes_to_spiram);
  RUN_TEST(test_body_buffer_type_is_psram);
  RUN_TEST(test_http_body_buf_is_psram_backed);
  RUN_TEST(test_ws_message_payload_is_psram_backed);
  return UNITY_END();
}
