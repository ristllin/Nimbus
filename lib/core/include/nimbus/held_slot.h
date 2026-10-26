#pragma once
#include <cstdint>

// held_slot - one counted resource held by a scope guard (the device's TLS work slot,
// src/sys/tls_arbiter.h WorkSlot). The hold ends at release() or, at the latest, when
// the guard leaves its scope - also when an exception unwinds through it.
//
// Why (L24 merge review): a turn now catches std::bad_alloc and ends honestly instead
// of aborting the device. The work slot used to be taken and given back by hand, so an
// allocation that failed between the two (a response body growing, a header copy) left
// the single slot taken for good: every later turn, embed, connector and fetch failed
// "tls work-slot busy" until a restart. Under the guard the unwinding gives it back.
//
// Portable and host-tested (test_held_slot); no Arduino.
namespace nimbus {

class HeldSlot {
 public:
  using Acquire = bool (*)(uint32_t timeoutMs);
  using Release = void (*)();

  HeldSlot(Acquire acquire, Release release, uint32_t timeoutMs)
      : release_(release), held_(acquire != nullptr && acquire(timeoutMs)) {}
  ~HeldSlot() { release(); }
  HeldSlot(const HeldSlot&) = delete;
  HeldSlot& operator=(const HeldSlot&) = delete;

  // True while the slot is held (false when the acquire timed out or after release()).
  explicit operator bool() const { return held_; }
  bool held() const { return held_; }
  // Give the slot back now; later calls and the destructor do nothing.
  void release() {
    if (!held_) return;
    held_ = false;
    if (release_) release_();
  }

 private:
  Release release_;
  bool held_;
};

}  // namespace nimbus
