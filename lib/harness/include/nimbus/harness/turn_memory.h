#pragma once
#include <cstdint>
#include <functional>
#include <string>

// turn_memory - a turn never spends the device's last internal SRAM (the S2 L24
// combined-starvation panic). Portable and host-tested (test_turn_memory).
//
// What happened (24 Oct, reproduced on unit A 26 Oct): a turn admitted at ~17 KB of
// internal free spent ~16 KB of it on its own setup before the provider request.
// Each per-turn copy of the 50 tool specs, the tool-name map, the closures and the
// prompt pieces is a heap block under 128 B, and the device routes only blocks of
// 128 B and up to PSRAM (main.cpp, heap_caps_malloc_extmem_enable(128)), so they all
// landed in internal SRAM first and spilled to PSRAM only once internal was EMPTY.
// Measured phase by phase: the tool specs copied from the registry took 3.8 KB, the
// prompt's tool list 1.7 KB and the head loop's tool set 8 KB. Internal reached
// 772 B free (20 B largest) at the provider request, the Wi-Fi task's own 32 B
// internal allocation (the PHY's PLL timer at a modem-sleep wake) failed, and IDF
// aborts on that: a panic and a restart. Every normal turn paid the same dip.
//
// The rule now, in four parts:
//   - PsramFirstScope: while a turn runs, small blocks go to PSRAM too (the device
//     drops its spill limit to 0 for the turn and puts 128 B back after). IDF
//     already places a small block in PSRAM whenever internal is empty, so no code
//     meets a placement it does not meet today; the spill only starts before
//     internal is gone instead of after.
//   - TurnMemoryGate: the turn samples internal SRAM at each setup phase and checks
//     the dispatch floor (one new TLS connection: the relay-proven 8000 B free and
//     5000 B largest) before it asks a provider anything. Under it the turn ends
//     with one honest reply (kShortOfMemoryReply), recorded in the chat history, and
//     no provider request.
//   - The admission floor covers the dispatch floor plus the setup's measured worst
//     case (kTurnSetupCostBytes): admissionCoversSetup() is static_asserted against
//     the device's ORCH_TURN_HARD_FLOOR.
//   - A failed C++ allocation inside a turn (std::bad_alloc) ends the turn the same
//     honest way instead of aborting the device.
namespace agent {

// The dispatch floor: what one new TLS connection needs from internal SRAM at the
// provider request. The relay dial's per-connection floors (relay_heap.h,
// kRelayHeapFloorFree / kRelayHeapFloorLargest); the CUM-404 turn floor is this plus
// the setup's allowance.
inline constexpr uint32_t kDispatchFloorFree = 8000;
inline constexpr uint32_t kDispatchFloorLargest = 5000;

// The turn setup's worst-case internal cost from admission to the provider request,
// with the turn's small blocks in PSRAM, measured on unit A (26 Oct, the L24 recipe:
// the ballast, a grown chat history, 50 tools, recall on). What is left is the
// recall embed's network transient and the card reads.
inline constexpr uint32_t kTurnSetupCostBytes = 4000;

// The admission floor must leave the dispatch floor after the setup's worst case.
inline constexpr bool admissionCoversSetup(uint32_t admitFloorFree) {
  return admitFloorFree >= kDispatchFloorFree + kTurnSetupCostBytes;
}

// The one owner-facing reply when a turn ends for memory after it was admitted
// (the setup went under the dispatch floor, or an allocation failed). It asks for
// a retry later because the device frees the memory by itself.
extern const char kShortOfMemoryReply[];

// One turn's internal-SRAM samples, admission to dispatch.
class TurnMemoryGate {
 public:
  static constexpr int kMaxMarks = 14;

  explicit TurnMemoryGate(uint32_t floorFree = kDispatchFloorFree,
                          uint32_t floorLargest = kDispatchFloorLargest)
      : floorFree_(floorFree), floorLargest_(floorLargest) {}

  // Start a turn's record with its admission sample.
  void begin(uint32_t freeB, uint32_t largest);
  // Record one setup phase's sample. The first phase under the floor is remembered
  // and the gate stays refused for the rest of the turn.
  void phase(const char* name, uint32_t freeB, uint32_t largest);
  // True while every sample so far held the floor.
  bool ok() const { return refusedAt_ == nullptr; }
  // The first phase under the floor ("" while ok).
  const char* refusedAt() const { return refusedAt_ ? refusedAt_ : ""; }
  // Lowest free seen, and the setup's cost so far (admission free minus it).
  uint32_t lowFree() const;
  uint32_t setupCost() const;
  // One line per turn: "turn memory: admit=16936/7156 low=772/20 at tools cost=16164
  // floor=8000/5000 ok". full = every phase instead (the refusal's evidence line).
  std::string line(bool full = false) const;

 private:
  struct Mark {
    const char* name;
    uint32_t freeB;
    uint32_t largest;
  };
  uint32_t floorFree_;
  uint32_t floorLargest_;
  Mark marks_[kMaxMarks] = {};
  int n_ = 0;
  int dropped_ = 0;
  const char* refusedAt_ = nullptr;
};

// PSRAM-first small allocations for the length of a turn. The device installs the
// switch once (orchestrator.cpp: the spill limit 0 while on, 128 B when off); with
// none installed (host builds) the scope only counts. Scopes nest: the outermost
// one switches. Turn task only (turns are serialized).
void setPsramFirstSwitch(std::function<void(bool on)> sw);
class PsramFirstScope {
 public:
  PsramFirstScope();
  ~PsramFirstScope();
  PsramFirstScope(const PsramFirstScope&) = delete;
  PsramFirstScope& operator=(const PsramFirstScope&) = delete;
  // Live scopes (0 = small allocations prefer internal again).
  static int depth();
};

}  // namespace agent
