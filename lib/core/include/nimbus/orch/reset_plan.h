#pragma once
#include <cstddef>
#include <cstdint>

// Factory reset: the steps that run once the private-data sweep has succeeded, and
// the erase plan for the internal file area (lane L31, bench 2026-10-13). Pure and
// host-tested (test/test_reset_plan); the device executes the plan step by step in
// main.cpp, so the order the tests pin is the order that runs.
//
// Why the internal file area is erased, not just emptied: LittleFS deletes a file
// by unlinking it, and the old blocks keep their bytes until the file system
// happens to reuse them. Its format() is no better here: it rewrites only the two
// superblock blocks. On the bench a FULL reset left the deleted routines file
// readable in a raw read of the partition (41 hits of the previous owner's routine
// text). So the FULL reset erases every block of the partition and lays down an
// empty file system. Nothing on it needs to survive: it holds only private
// data (routines, chat state, per-chat memory, the member table, usage history,
// logs, staged audio) and rebuildable caches (provider catalogs); the board
// identity the reset keeps lives in NVS.
//
// The order of the FULL tail is the contract the tests pin:
//   1. The internal file area erase runs FIRST, before the settings are touched.
//      It unmounts LittleFS, so no later writer can put bytes back on it; and if
//      the erase cannot finish (an erase error, power lost, a crash), the device
//      still has its settings, so it never reads factory-fresh over readable data.
//      An erase error stops the tail there and the owner is told (the only step
//      that may stop it, and only before the settings are erased).
//   2. Then the settings (NVS) and the crash dump.
//   3. The SD card's final sweep runs LAST, right before the restart, so a late
//      writer during the erase above (it takes several seconds) is mopped up.
//
// The settings-only reset (no storage erase) keeps its behavior: NVS identity
// reset, crash-dump erase, restart.
namespace nimbus {
namespace orch {

enum class ResetStep : uint8_t {
  ResetNvs = 0,      // erase NVS, keep only the board identity, seed setup mode
  EraseCoreDump,     // a crash dump captures task stacks (keys, message text)
  FinalSweep,        // re-sweep the card's file trees a late writer could have refilled
  ScrubInternalFs,   // erase every block of the internal file area, then an empty FS
  Restart,
  Count              // sentinel: a new step must find its place in the plans
};
const char* resetStepStr(ResetStep s);
// True for a step whose failure stops the tail (the owner is told and the device
// keeps running with its settings). Only steps before ResetNvs may stop it.
bool resetStepMayStop(ResetStep s);

struct ResetPlan {
  const ResetStep* steps;
  size_t count;
};

// The steps a factory reset runs after its sweep. eraseData = the FULL reset
// ("Also erase storage" on the web page, always on the device menu).
ResetPlan factoryResetTail(bool eraseData);

// ---- internal file area erase plan ---------------------------------------------
// The partition is erased in chunks so the caller can feed its watchdog between
// them: one 64 KB block erase can take up to ~2 s on the flash part, and the whole
// 3.4 MB area takes several seconds.
inline constexpr uint32_t kFlashSectorBytes = 4096;
inline constexpr uint32_t kScrubChunkBytes = 64u * 1024u;

// How many chunks cover a partition of partSize bytes.
uint32_t scrubChunkCount(uint32_t partSize);
// The index-th chunk: false past the end. Chunks run contiguously from offset 0,
// each at most kScrubChunkBytes, and together cover [0, partSize) exactly, so no
// block is left out (a partition is always a whole number of 4 KB sectors, so
// every chunk is sector-aligned).
bool scrubChunk(uint32_t partSize, uint32_t index, uint32_t& offset, uint32_t& size);

}  // namespace orch
}  // namespace nimbus
