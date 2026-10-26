#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// heap_watch - evidence for the scarce internal SRAM (the portable half).
//
// Every heap number the device used to log was a point sample at a phase boundary,
// so the real low points (inside a TLS write, a handshake, another task's serve)
// were never seen, and an internal allocation that failed left no trace at all. On
// Lumi the all-time minimum reached 1,180 B free with no log line anywhere near it.
// This module holds the pieces that make those moments visible, each host-tested:
//
//   - FailRing: the failed-allocation recorder. The heap calls the device's
//     callback in the allocating context (any task, possibly an interrupt), so the
//     push path below never allocates, logs or blocks: it copies a fixed record
//     into an 8-entry ring that lives in RTC no-init memory (magic + CRC), so the
//     records survive a panic reset and are reported by the next boot.
//   - Ladder: the low-water ladder. The all-time minimum free is read at 1 Hz; each
//     new low at least 1 KB below the last logged mark gets one line naming what
//     was running (the activity tags), so the 1,180 B moment would have a cause.
//   - Activity: the activity tags (turn, relay dial, relay bulk write, ...), set
//     and cleared by one-line scopes at the call sites; a tag set and cleared
//     between two samples still counts for the next sample (sticky).
//   - RestingLargest: the largest internal block at rest (the minimum of the last
//     idle samples). A single sample taken inside a transient read 7,156 B while
//     the device rested at 15,860 B, which parked the provider verify for good.
//   - spawnVerdict: a worker task may start only with room for its stack plus a
//     margin, and never while a turn is in flight (the turn's own transients need
//     that room).
//
// No Arduino. The device glue is src/sys/heap_watch.{h,cpp}.
namespace nimbus::heapwatch {

// ---- failed-allocation ring ---------------------------------------------------

// "HWF" + layout version: a firmware with another layout reads an invalid ring.
inline constexpr uint32_t kRingMagic = 0x48574601u;
inline constexpr size_t kRingSize = 8;
inline constexpr size_t kTaskNameLen = 8;   // 7 characters + NUL

struct FailRecord {
  uint32_t ms;          // uptime at the failure
  uint32_t size;        // bytes requested
  uint32_t caps;        // MALLOC_CAP_* bits requested
  uint32_t intFree;     // internal free right after the failure (0 = not sampled)
  uint32_t intLargest;  // largest internal block right after (0 = not sampled)
  char task[kTaskNameLen];  // the allocating task, "isr" in an interrupt
};

struct FailRing {
  uint32_t magic;
  uint32_t total;   // failures recorded since the ring was reset (the newest is total-1)
  FailRecord rec[kRingSize];
  uint32_t crc;     // CRC-32 over every byte above
};

// The hot path. Header-inline and table-free on purpose: the device callback runs
// in the allocating context, which can be an interrupt with the flash cache off,
// so nothing here may call into flash or allocate.
#if defined(__GNUC__)
#define NIMBUS_HW_HOT inline __attribute__((always_inline))
#else
#define NIMBUS_HW_HOT inline
#endif

NIMBUS_HW_HOT uint32_t ringCrc(const FailRing& r) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&r);
  const size_t n = offsetof(FailRing, crc);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

NIMBUS_HW_HOT void ringReset(FailRing& r) {
  uint8_t* p = reinterpret_cast<uint8_t*>(&r);
  for (size_t i = 0; i < sizeof(FailRing); ++i) p[i] = 0;
  r.magic = kRingMagic;
  r.crc = ringCrc(r);
}

NIMBUS_HW_HOT bool ringValid(const FailRing& r) {
  return r.magic == kRingMagic && r.crc == ringCrc(r);
}

// One failure's numbers, as the hook samples them.
struct FailSample {
  uint32_t ms, size, caps, intFree, intLargest;
};

// Record one failure. `task` may be null; it is copied up to 7 characters.
NIMBUS_HW_HOT void ringPush(FailRing& r, const FailSample& f, const char* task) {
  FailRecord& e = r.rec[r.total % kRingSize];
  e.ms = f.ms;
  e.size = f.size;
  e.caps = f.caps;
  e.intFree = f.intFree;
  e.intLargest = f.intLargest;
  size_t i = 0;
  for (; task && task[i] && i < kTaskNameLen - 1; ++i) e.task[i] = task[i];
  for (; i < kTaskNameLen; ++i) e.task[i] = 0;
  r.total = r.total + 1;
  r.crc = ringCrc(r);
}

// Copy the records pushed after `since` (a previous `total`), oldest first, into
// `out` (room for kRingSize). Records that the ring has already overwritten are
// counted in `lost`. Returns how many were copied.
size_t ringSince(const FailRing& r, uint32_t since, FailRecord* out, uint32_t& lost);

// "DMA|INTERNAL|8BIT" for the MALLOC_CAP_* bits that name a memory kind; "0" when none.
std::string capsString(uint32_t caps);

// One record as a log line: "heap: alloc FAILED size=1600 caps=DMA|INTERNAL|8BIT
// task=tg_poll free=15204 largest=7156 up=42660s".
std::string failLine(const FailRecord& e);

// ---- activity tags --------------------------------------------------------------

enum Tag : uint8_t {
  kTurn = 0,     // a turn is in flight (sampled from the orchestrator)
  kRelayDial,    // the cloud relay's TLS dial, upgrade and hello
  kRelayBulk,    // a tunneled response frame over 8 KB being written
  kTunnel,       // a tunneled request being served (loopback replay)
  kTgDownload,   // a Telegram file download
  kVision,       // an image description call
  kStt,          // a speech-to-text upload
  kOta,          // an update check or install
  kDream,        // the nightly memory pass
  kVerify,       // a provider verify
  kWorker,       // an on-demand worker task is alive
  kLanBig,       // a web response over 16 KB was started
  kTagCount,
};
static_assert(kTagCount <= 32, "tags fit one mask word");

const char* tagName(Tag t);
// "turn|relay.bulk" for a mask; "-" when no tag is set.
std::string tagsString(uint32_t mask);

// The live activity set. enter/leave are counted per tag (two workers can be alive
// at once), and every enter also marks the tag sticky until the next sample, so a
// burst shorter than the 1 Hz sample period is still attributed.
class Activity {
 public:
  void enter(Tag t);
  void leave(Tag t);
  void touch(Tag t);   // sticky only: "this happened since the last sample"
  uint32_t active() const;
  // The mask for one ladder sample (live tags plus everything sticky), then the
  // sticky set restarts from the live tags.
  uint32_t sample();

 private:
  std::atomic<uint16_t> cnt_[kTagCount] = {};
  std::atomic<uint32_t> sticky_{0};
};

// ---- low-water ladder ---------------------------------------------------------

inline constexpr uint32_t kLadderStepBytes = 1024;
// Lines start once the low-water is under this. Above it the device has the room a
// turn's transients need (docs/memory-model.md), and boot alone walks the minimum
// down through tens of kilobytes.
inline constexpr uint32_t kLadderLogBelow = 24 * 1024;

struct Ladder {
  uint32_t low = UINT32_MAX;     // the low-water mark: lowest internal free seen
  uint32_t lowTags = 0;          // activity when it was set
  uint32_t lowUpMs = 0;          // uptime when it was set
  uint32_t logged = UINT32_MAX;  // the last mark a line was written for
};

struct LadderStep {
  bool newLow = false;  // the mark moved down
  bool log = false;     // write a line for it
  uint32_t was = UINT32_MAX;  // the previous logged mark (UINT32_MAX = none yet)
};

// Fold one reading of the all-time minimum free into the ladder.
LadderStep ladderNote(Ladder& l, uint32_t minFree, uint32_t tags, uint32_t nowMs);

// "heap: new low-water 3908 (was 6664) free=20764 largest=9716 tags=turn|relay.bulk up=345600s"
std::string ladderLine(const Ladder& l, uint32_t was, uint32_t freeNow, uint32_t largestNow);

// ---- rate limit for the drained lines -------------------------------------------

struct LineBudget {
  static constexpr uint32_t kWindowMs = 60000;
  static constexpr uint8_t kPerWindow = 6;
  bool started = false;
  uint32_t windowStart = 0;
  uint8_t used = 0;
  uint32_t dropped = 0;   // lines refused since the last takeDropped()
  bool take(uint32_t nowMs);
  uint32_t takeDropped();
};

// ---- resting largest block ------------------------------------------------------

struct RestingLargest {
  static constexpr uint8_t kSamples = 4;
  uint32_t s[kSamples] = {};
  uint8_t n = 0, next = 0;
  void note(uint32_t largest);
  // The minimum of the last idle samples, or `fallback` before the first one.
  uint32_t resting(uint32_t fallback) const;
};

// ---- worker spawn gate ------------------------------------------------------------

// A worker's stack comes out of internal SRAM; a turn's next exchange needs about
// 8 KB of transients on top of the floors it was admitted at. 16 KB of margin past
// the stack keeps both.
inline constexpr uint32_t kSpawnMarginBytes = 16 * 1024;

enum class SpawnVerdict : uint8_t { Ok, TurnInFlight, LowMemory };
SpawnVerdict spawnVerdict(uint32_t stackBytes, uint32_t intFree, bool turnInFlight);
const char* spawnVerdictName(SpawnVerdict v);

// The orchestrator's one-shot boot task (main.cpp orchBootTask) asks at boot, before
// any turn can exist, so the turn's transient margin does not apply. What the chain
// needs next is the Telegram poll task's 24 KB stack (tg_poll, created inside it) plus
// a few KB of the boot's own transients. Never TurnInFlight.
inline constexpr uint32_t kBootSpawnMarginBytes = 28 * 1024;
SpawnVerdict spawnVerdictAtBoot(uint32_t stackBytes, uint32_t intFree);

// The boot task's bounded restarts (main.cpp): when it cannot start, the boot waits,
// then restarts; the restarts in a row are kept in RTC no-init memory (magic + count)
// so a lasting shortfall is never a restart loop. Power-on garbage fails the magic.
inline constexpr uint32_t kOrchBootRestartMagic = 0x4F42524Eu;   // "OBRN"
inline constexpr uint32_t kOrchBootMaxRestarts = 2;
inline uint32_t orchBootRestartsInARow(uint32_t magic, uint32_t count) {
  return magic == kOrchBootRestartMagic ? count : 0;
}
// After the wait failed: restart again (true), or stop restarting and keep waiting.
inline bool orchBootShouldRestart(uint32_t restartsInARow) { return restartsInARow < kOrchBootMaxRestarts; }

// ---- general-malloc placement ---------------------------------------------------

// The boot's spill limit for plain malloc/new (main.cpp, heap_caps_malloc_extmem_enable):
// a block larger than this goes to PSRAM first, one of this size or smaller prefers
// internal SRAM (IDF compares size <= limit). A turn drops it to 0 for its length (src/sys
// heap_watch psramFirst, the L24 fix in nimbus/harness/turn_memory.h) and puts this value
// back after.
inline constexpr uint32_t kSmallAllocInternalLimit = 128;

// ---- the boot's loopTask stack floor ---------------------------------------------

// The loopTask free-stack floor for the boot ladder, in BYTES (the high-water
// mark is already bytes on the Xtensa port; see telegram.cpp's sample). The
// release-v451x bisect: the last-good boot finished with 88 B of loopTask
// headroom and the next lane's ~100 B of begin-chain depth tipped the canary -
// chronic stack erosion that no host battery can see (a device-only stack
// layout property), caught only by a release-gate boot crash. The floor fails
// the boot LOUDLY so the erosion surfaces at the lane-battery era instead.
// 1 KB: the crashing states (88 B, 56 B) fault, and a healthy boot - whose
// deepest loopTask point sits well above the floor once the orchestrator's
// boot work runs on its own task - passes with margin.
inline constexpr uint32_t kLoopStackFloorBytes = 1024;

// True when the loopTask free-stack watermark drops under the floor.
inline bool loopStackUnderFloor(uint32_t minBytes) { return minBytes < kLoopStackFloorBytes; }

// The boot ladder's one line, healthy or fault (the bench leg greps it):
//   "boot stack: loopTask min free 2890 B, floor 1024 B - ok"
//   "boot stack: BOOT FAULT - loopTask min free 88 B is under the 1024 B floor:
//    the boot path grew past the loopTask stack (see docs/memory.md)"
// `taskPresent` false (no loopTask handle) always reads as a fault.
std::string loopStackFloorLine(uint32_t minBytes, bool taskPresent);

// ---- the orchestrator boot task's stack floor ------------------------------------

// The boot chain (orchestratorBegin) runs on its own one-shot task; its free-stack
// watermark gets the same 1 KB floor, so erosion of that chain shows up as a loud
// boot line in the lane battery era, not as a release-gate panic.
inline constexpr uint32_t kOrchBootStackFloorBytes = 1024;
inline bool orchBootStackUnderFloor(uint32_t minBytes) { return minBytes < kOrchBootStackFloorBytes; }

// The orchboot line of the boot ladder, healthy or fault:
//   "boot stack: orchboot min free 4210 B of 12288 B, floor 1024 B - ok"
//   "boot stack: BOOT FAULT - orchboot min free 600 B of 12288 B is under the 1024 B
//    floor: the orchestrator boot chain grew past its task stack (see docs/memory.md)"
// `measured` false (the task never reported a watermark) always reads as a fault.
std::string orchBootStackFloorLine(uint32_t minBytes, uint32_t stackBytes, bool measured);

}  // namespace nimbus::heapwatch
