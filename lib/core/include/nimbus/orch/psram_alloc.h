#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// psram_alloc - the seam that puts the memory WORKING SET in the 8 MB PSRAM instead
// of the ~300 KB internal SRAM (docs/orchestrator-storage.md §2). The portable core
// is Arduino-free and can't call heap_caps_malloc, so allocations route through a
// pair of global function-pointer HOOKS that default to malloc/free (host tests +
// no-PSRAM boards) and are overridden by the device at boot with PSRAM-backed
// versions (heap_caps_malloc(MALLOC_CAP_SPIRAM)). Only the VDB's hot storage (the
// int8 vector buffers + the entries array - the dominant cost) uses this allocator;
// the small SSO strings stay on the internal heap. Install the device hooks BEFORE
// the first vector is loaded/added so the whole working set lands in PSRAM.
namespace nimbus {
namespace orch {

using AllocFn = void* (*)(std::size_t);
using FreeFn  = void  (*)(void*);

// Install PSRAM-backed hooks (device) or leave the malloc/free default (host). Call
// once at boot before begin(). Passing null for either restores the default.
void  setWorkingAllocators(AllocFn a, FreeFn f);
void* workingAlloc(std::size_t n);
void  workingFree(void* p);

// Stateless STL allocator that routes through the working-set hooks. Applied to the
// VDB's internal containers; host-safe (default hook = malloc) so every VDB unit test
// runs unchanged, and the device gets PSRAM residency for free.
template <class T>
struct WorkingAllocator {
  using value_type = T;
  WorkingAllocator() noexcept = default;
  template <class U>
  WorkingAllocator(const WorkingAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    void* p = workingAlloc(n * sizeof(T));
    if (!p) {
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)
      throw std::bad_alloc();      // host: standard container contract
#else
      std::abort();                // device (-fno-exceptions): OOM is fatal, like ::operator new
#endif
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { workingFree(p); }

  template <class U> bool operator==(const WorkingAllocator<U>&) const noexcept { return true; }
  template <class U> bool operator!=(const WorkingAllocator<U>&) const noexcept { return false; }
};

// Small strings and their containers that outlive a turn (the photo queues, the photo
// row overlay): through the same hooks, so a 20-byte id lands in PSRAM on the device
// instead of the internal heap (malloc keeps anything under 128 B internal, where
// many small long-lived pieces cut the largest free block TLS needs).
using WorkingString = std::basic_string<char, std::char_traits<char>, WorkingAllocator<char>>;
template <class T>
using WorkingVector = std::vector<T, WorkingAllocator<T>>;
inline WorkingString workingCopy(const std::string& s) { return WorkingString(s.data(), s.size()); }
inline std::string plainCopy(const WorkingString& s) { return std::string(s.data(), s.size()); }
inline bool sameText(const WorkingString& a, const std::string& b) {
  return a.size() == b.size() && a.compare(0, a.size(), b.data(), b.size()) == 0;
}

}  // namespace orch
}  // namespace nimbus
