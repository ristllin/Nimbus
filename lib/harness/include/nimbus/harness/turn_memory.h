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
// prompt pieces is a heap block of 128 B or less, and the device routes only larger
// blocks to PSRAM (main.cpp, heap_caps_malloc_extmem_enable(128)), so they all landed
// in internal SRAM first and spilled to PSRAM only once internal was EMPTY.
// Measured phase by phase: the tool specs copied from the registry took 3.8 KB, the
// prompt's tool list 1.7 KB and the head loop's tool set 8 KB. Internal reached
// 772 B free (20 B largest) at the provider request, the Wi-Fi task's own 32 B
// internal allocation (the PHY's PLL timer at a modem-sleep wake) failed, and IDF
// aborts on that: a panic and a restart. Every normal turn paid the same dip.
//
// The rule now:
//   - PsramFirstScope: while a turn runs, small blocks go to PSRAM too (the device
//     drops its spill limit to 0 for the turn and puts 128 B back after). The limit
//     is device-wide, so every task's plain malloc spills for the turn's length.
//     IDF already places a small block in PSRAM whenever internal is empty, so code
//     that allocates plain small blocks already meets PSRAM under pressure; the spill
//     now starts before internal is gone instead of after. Explicit INTERNAL/DMA
//     allocations (Wi-Fi, lwIP, drivers, FreeRTOS) are never affected. With it the
//     same turn's setup measured 0 B of internal cost (was ~16 KB), so the admission
//     floor now covers the setup: an admitted turn reaches its request on the floor
//     it was admitted on.
//   - TurnMemoryGate: the turn samples internal SRAM at each setup phase (evidence)
//     and checks it again at the provider request against the floor it was admitted
//     on (the CUM-404 turn floor, the same numbers its later rounds re-gate on). A
//     dip under it (the recall embed's network buffers, work beside the turn) gets a
//     few short waits to recover. Still under it, the turn ends with one honest reply
//     (kShortOfMemoryReply), recorded in the chat history, and no provider request.
//   - A failed C++ allocation inside a turn (std::bad_alloc) ends the turn the same
//     honest way instead of aborting the device.
namespace agent {

// The one owner-facing reply when a turn ends for memory after it was admitted
// (the setup went under the floor, or an allocation failed). It asks for a retry
// later because the device frees the memory by itself.
extern const char kShortOfMemoryReply[];

// A dispatch sample under the floor is taken again after a short wait, this many
// times: the recall embed's network transient frees asynchronously (lwIP releases
// its buffers a few ms after the connection closes), so a dip is not a refusal.
inline constexpr int kDispatchRechecks = 5;
inline constexpr uint32_t kDispatchRecheckMs = 200;

// One turn's internal-SRAM samples, admission to dispatch.
class TurnMemoryGate {
 public:
  static constexpr int kMaxMarks = 16;

  // Start a turn's record with its admission sample and the floor it was admitted on
  // (total free, largest block): the floor its dispatch is checked against.
  void begin(uint32_t freeB, uint32_t largest, uint32_t floorFree, uint32_t floorLargest);
  // Record one setup phase's sample (evidence: it never refuses by itself).
  void phase(const char* name, uint32_t freeB, uint32_t largest);
  // The decision at the provider request: record the sample and return whether it
  // holds the floor. The last call decides the turn.
  bool dispatch(const char* name, uint32_t freeB, uint32_t largest);
  // True unless the last dispatch sample was under the floor.
  bool ok() const { return refusedAt_ == nullptr; }
  // The dispatch sample that refused ("" while ok).
  const char* refusedAt() const { return refusedAt_ ? refusedAt_ : ""; }
  // The first sample under the floor, a dispatch or not ("" if none): evidence.
  const char* firstUnder() const { return firstUnder_ ? firstUnder_ : ""; }
  // Lowest free seen, and the setup's cost so far (admission free minus it).
  uint32_t lowFree() const;
  uint32_t setupCost() const;
  // One line per turn: "turn memory: admit=16936/7156 low=772/20 at tools cost=16164
  // floor=12000/5000 ok". full = every phase instead (the refusal's evidence line).
  std::string line(bool full = false) const;

 private:
  struct Mark {
    const char* name;
    uint32_t freeB;
    uint32_t largest;
  };
  bool holds(uint32_t freeB, uint32_t largest) const {
    return freeB >= floorFree_ && largest >= floorLargest_;
  }
  uint32_t floorFree_ = 0;
  uint32_t floorLargest_ = 0;
  Mark marks_[kMaxMarks] = {};
  int n_ = 0;
  int dropped_ = 0;
  const char* refusedAt_ = nullptr;
  const char* firstUnder_ = nullptr;
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
