#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

// Platform - the harness's view of the machine it runs on. Everything the
// lifted orchestration code used to reach via Arduino globals (millis(),
// ESP.getFreeHeap(), vTaskDelay, heap_caps PSRAM allocs) arrives through this
// one injected struct, so the identical code runs on host tests with a scripted
// clock/heap. Mirrors the HeadLoopHooks convention (nimbus/orch/head_loop.h) -
// plain std::function slots, no framework.
namespace agent {

struct Platform {
  std::function<uint32_t()> nowMs;              // monotonic ms (device: millis)
  std::function<uint32_t()> freeHeap;           // free INTERNAL heap (device: ESP.getFreeHeap)
  std::function<void(uint32_t)> delayMs;        // cooperative delay (device: vTaskDelay)
  std::function<void*(size_t)> allocLarge;      // big buffers -> PSRAM (device: heap_caps_malloc)
  std::function<void(void*)>  freeLarge;

  // Epoch seconds for wall-clock decisions (loops, episodic stamps). 0 = no SNTP yet.
  std::function<uint64_t()> nowEpoch;
};

}  // namespace agent
