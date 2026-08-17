#include "nimbus/orch/psram_alloc.h"

namespace nimbus {
namespace orch {

namespace {
void* defaultAlloc(std::size_t n) { return std::malloc(n); }
void  defaultFree(void* p)        { std::free(p); }
AllocFn g_alloc = defaultAlloc;   // malloc until the device installs PSRAM hooks
FreeFn  g_free  = defaultFree;
}  // namespace

void setWorkingAllocators(AllocFn a, FreeFn f) {
  g_alloc = a ? a : defaultAlloc;
  g_free  = f ? f : defaultFree;
}

void* workingAlloc(std::size_t n) { return g_alloc(n ? n : 1); }
void  workingFree(void* p)        { if (p) g_free(p); }

}  // namespace orch
}  // namespace nimbus
