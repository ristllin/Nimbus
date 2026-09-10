#pragma once
// psram_vector - the shared std-allocator + vector alias that routes the relay/cloud-sync
// staging buffers to PSRAM, keeping them OFF the scarce internal SRAM the TLS session and
// the display DMA bounce buffer share (CUM-387, extends the CUM-167/N7 headroom work).
//
// The relay's transient buffers - the decoded response body, the inbound WS frame + its
// reassembly, and the parser staging - can each reach the inbound/response caps (tens to
// hundreds of KB). On internal heap they drove the low-water to ~9 KB during a sync and
// tripped the "Not enough memory right now" dial gate; the S3's 8 MB PSRAM is nearly empty,
// so they belong there. On device (NIMBUS_RELAY_PSRAM_BODY, set for every firmware env in
// platformio.ini) PsVector routes to PSRAM; on host it is a plain std::vector. Only the
// allocator differs; the container API is identical, so host tests are unaffected.
//
// A host test may define NIMBUS_RELAY_PSRAM_BODY together with NIMBUS_PSRAM_ALLOC_TEST and
// supply its own heap_caps_malloc/heap_caps_free + MALLOC_CAP_* before including this header,
// to exercise the PSRAM allocator path against a fake backend (see test/test_relay_psram).
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(NIMBUS_RELAY_PSRAM_BODY)
#  if !defined(NIMBUS_PSRAM_ALLOC_TEST)
#    include <esp_heap_caps.h>
#  endif

namespace nimbus {
namespace cloud {

template <class T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() = default;
  template <class U>
  PsramAlloc(const PsramAlloc<U>&) {}
  T* allocate(std::size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);  // fall back to internal
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) { heap_caps_free(p); }
};
template <class A, class B>
bool operator==(const PsramAlloc<A>&, const PsramAlloc<B>&) { return true; }
template <class A, class B>
bool operator!=(const PsramAlloc<A>&, const PsramAlloc<B>&) { return false; }

template <class T>
using PsVector = std::vector<T, PsramAlloc<T>>;

}  // namespace cloud
}  // namespace nimbus

#else  // host / non-firmware: no PSRAM, plain allocator (identical API)

namespace nimbus {
namespace cloud {
template <class T>
using PsVector = std::vector<T>;
}  // namespace cloud
}  // namespace nimbus

#endif
