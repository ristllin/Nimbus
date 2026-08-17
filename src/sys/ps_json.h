#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ps_json - an ArduinoJson allocator that routes a document's node pool to PSRAM.
// (The S3's 8 MB PSRAM is fully usable - heap_caps_malloc(..., MALLOC_CAP_SPIRAM) -
// it's just not in the pool ESP.getFreeHeap()/plain malloc report, which is INTERNAL
// SRAM only. See docs/memory-model.md.) Falls back to internal heap when PSRAM is
// absent/exhausted. Used for the head tool-use loop's accumulating conversation +
// per-round request/response docs, which would otherwise grow the SCARCE internal
// SRAM across rounds (Anthropic Messages is stateless and replays the whole history
// each round). Pass &PsramJsonAllocator::instance() to a JsonDocument ctor. Frees
// via heap_caps_free (handles both regions).
namespace agent {

struct PsramJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);  // fall back to internal
  }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
  }
  static PsramJsonAllocator& instance() {
    static PsramJsonAllocator a;
    return a;
  }
};

}  // namespace agent
