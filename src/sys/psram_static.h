#pragma once

// Long-lived buffers in PSRAM instead of internal DRAM (docs/memory.md).
//
// A file-scope `static T g_x;` holds sizeof(T) of the S3's scarce internal DRAM for
// the whole run, which comes straight off the internal heap every turn needs.
// Declaring it as
//
//   static T& g_x = ::nimbus::psramStatic<T>();
//
// keeps every use site and sizeof(g_x) unchanged but puts the object in PSRAM; only
// the 4-byte reference stays in internal .bss. The object starts zeroed, exactly as
// a static would. A buffer may move this way only if all of these hold:
//   - plain data (no constructor or destructor; enforced at compile time);
//   - task context only: never touched from an ISR or IRAM code (PSRAM is
//     unreachable while the flash cache is off);
//   - never a DMA source or target;
//   - it is data, never the lock that guards it (a portMUX or atomic stays internal);
//   - no static initialiser, in this file or another, uses it (init order).
// PSRAM is on the heap before static initialisers run (CONFIG_SPIRAM_BOOT_INIT and
// CONFIG_SPIRAM_USE_MALLOC in the core's sdkconfig). If PSRAM is missing the object
// lands in internal RAM, as before, so behaviour never depends on it.
// tests/hil/test_psram_statics_host.py keeps the moved buffers moved.

#include <esp_heap_caps.h>

#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace nimbus {

// PSRAM first, internal RAM if PSRAM is missing or full; zero-filled either way. The
// one allocation rule every long-lived buffer below (and the provider park's record
// table) follows.
inline void* psramOrInternalCalloc(size_t bytes) {
  void* p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

template <typename T>
T& psramStatic() {
  // Plain data only (char arrays, POD structs, arrays of them): calloc's zero fill is
  // then exactly a static's zero-initialisation, and nothing is ever destroyed.
  static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>,
                "psramStatic: plain data only");
  void* p = psramOrInternalCalloc(sizeof(T));
  if (!p) abort();   // cannot happen at static-init time; a static never fails either
  return *static_cast<T*>(p);
}

// The same for an object that needs its constructor (members that are not plain
// data, built once with arguments where a function-local static was):
//
//   static TurnEngine& turnEngine = ::nimbus::psramObject<TurnEngine>(deps, tuning);
//
// Constructed in place in PSRAM (internal RAM if PSRAM is missing) on the first pass,
// exactly once, and never destroyed: a device never runs exit handlers, so a static
// object's destructor never ran either. The memory is zeroed before the constructor
// runs, as a static's is, so a member a constructor leaves alone still reads 0. The
// rules above hold, plus: no lock or atomic member (they stay in internal RAM; review
// the class when you add one), and nothing reads the object before the declaration
// has run (it is built where the static was, never at static-init time).
template <typename T, typename... Args>
T& psramObject(Args&&... args) {
  void* p = psramOrInternalCalloc(sizeof(T));
  if (!p) abort();   // a few KB at begin(): as fatal as a failed static would have been
  return *new (p) T(std::forward<Args>(args)...);
}

}  // namespace nimbus
