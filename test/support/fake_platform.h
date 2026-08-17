#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/platform.h"

// Shared host-test fakes for the harness Platform contract + hlog recorder.
// Header-only (PlatformIO compiles each test dir independently); include as
//   #include "../support/fake_platform.h"
namespace harness_test {

// Scripted clock + heap. Advance time manually; set heap to trip gates.
struct FakePlatform {
  uint32_t ms = 1000;
  uint32_t heap = 100000;
  uint64_t epoch = 1752700000;   // ~2026-07-17 (fixed; NEVER wall time in tests)
  std::vector<uint32_t> delays;  // recorded delayMs calls
  // Optional per-read heap script: successive freeHeap() calls return successive
  // values (the LAST repeats once exhausted). Empty => `heap` is returned always.
  // Models the real device where a turn's heap DIPS after recall's TLS embed and
  // recovers a moment later, so a test can prove the loop gate reads the RIGHT
  // moment (turn entry, not the transient trough).
  std::vector<uint32_t> heapScript;
  mutable size_t heapIdx = 0;

  agent::Platform contract() {
    agent::Platform p;
    p.nowMs    = [this] { return ms; };
    p.freeHeap = [this] {
      if (heapScript.empty()) return heap;
      uint32_t v = heapScript[heapIdx < heapScript.size() ? heapIdx : heapScript.size() - 1];
      ++heapIdx;
      return v;
    };
    p.delayMs  = [this](uint32_t d) { delays.push_back(d); ms += d; };
    p.allocLarge = [](size_t n) { return std::malloc(n); };
    p.freeLarge  = [](void* q) { std::free(q); };
    p.nowEpoch = [this] { return epoch; };
    return p;
  }
};

// hlog recorder: captures every harness log line for assertions. Install with
// LogCapture::install() in setUp (the sink is a plain fn pointer, so the
// capture buffer is a static).
struct LogCapture {
  static std::vector<std::string>& lines() {
    static std::vector<std::string> v;
    return v;
  }
  static void install() {
    lines().clear();
    agent::hlog::setSink(+[](const char* l) { lines().push_back(l); });
  }
  static bool contains(const char* needle) {
    for (auto& l : lines())
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

}  // namespace harness_test
