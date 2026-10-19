#pragma once
// errlog_fs - the device-side filesystem sink for the durable error log (CUM-401).
//
// This is the ONLY errlog file that touches Arduino/FS. The write path is fed the
// SAME already-redacted line the RAM ring gets (agent_log.h routes both through
// core::emitRedacted), so no secret can reach the durable log. The rotation, size
// caps, line format and path validation are the portable, host-tested policy in
// errlog.h; this module is the thin glue that opens SD (agent::memory::dataFs())
// or the LittleFS flash fallback and applies that policy.
//
// Header stays light on purpose (std::string only): agent_log.h includes it into
// many TUs, so the heavy FS/memory includes live in the .cpp.
//
// Serialization: every FS op runs under agent::memory::Lock, the recursive mutex
// that serializes ALL card I/O across every task in this firmware (main, poll,
// AsyncTCP). The lock exists from the top of setup() (memory::lockInit), in both
// modes. The active log file is the one handle held open between calls (each line is
// a write plus a flush; see LogWriter in errlog.h); it is only ever touched under the
// lock, and no other FS handle is held across a network send.

#include <cstddef>
#include <string>

namespace fs { class FS; }   // fwd-decl keeps this header Arduino-light

namespace nimbus::errlog {

// Attach a MOUNTED filesystem as the durable log's storage. Until the first call the
// tier is "not ready": lines queue in a bounded early backlog (24 lines / 4 KB) instead
// of failing against an unmounted FS. Called by main.cpp right after the SD card mounts
// (both modes), and by memory::begin() right after it mounts LittleFS when there is no
// card. The first call reads and bumps the boot counter (/log/boot.n), stamps it into
// the queued boot record, flushes the backlog in order, then writes the tier line.
// Runs on the boot task under the card lock.
void attachStorage(::fs::FS& fs, bool isSd);

// Called once, on the boot task, right after the first attachStorage() finished (the
// crash reporter reads the core dump and dedupes against /log/crash.seen there, so
// its lines land as early as storage allows, even in a boot that dies right after).
void onAttached(void (*fn)());

// True once storage has attached.
bool storageAttached();

// End of setup(): when no storage ever attached (Notifier mode without a card) the
// early backlog is freed; later lines stay in the RAM tail and on serial only.
void bootComplete();

// The storage under the held log file is about to be unmounted, reformatted or
// remounted: close the handle now (the next line reopens it). Call before
// solide::storage::end()/format() - a handle kept across a remount could write into
// whatever file the remounted volume hands its descriptor to next.
void releaseHandle();

// This boot's number from /log/boot.n (0 until storage attaches, or when the counter
// could not be written).
uint32_t bootNumber();

// Small state files kept beside the log in /log (the crash dedupe record). Read and
// write under the card lock; "" / false before storage attaches.
std::string readState(const char* name, size_t maxBytes);
bool writeState(const char* name, const std::string& data);

// Append one already-redacted line to the durable log, rotating within the tier's
// size cap. `cat` is a short class tag from errlog::cat (nullptr / "" -> "-").
// Never throws; a full or absent FS degrades to the RAM fallback silently (the
// point of the log is to survive failures, not to become one).
void append(const std::string& redacted, const char* cat);

// Factory-erase suspension: while true, append() keeps the RAM tail (and the
// caller's Serial print) but drops the durable flash write, so the sweep's own
// progress lines cannot recreate the /log tree it just erased. Suspending also
// closes the held log file so the sweep can delete it. Set from the main-loop
// factory-reset drain; cleared again only on the refusal path (a successful erase
// restarts the device).
void setSuspended(bool on);

// Agent-read seam: the most recent <= maxBytes of the durable log, line-aligned
// and reboot-surviving (read from the card/flash, newest last). Falls back to the
// in-RAM tail when the FS is unavailable. This is what lets Nimbus self-report
// "I hit X" - an in-process getter the agent (or a device tool) can call.
std::string readRecent(size_t maxBytes);

// Retrieval support (used by src/net/errlog_routes). listJson() returns
// {"tier":"sd|flash|none","files":[{"name","bytes"}...]} for the current + rotated
// files that exist. fileBytes()/streaming are done by the route directly against
// activeFs() so a large pull never buffers the whole file in RAM.
std::string listJson();

// True when the durable log is currently on the SD card (vs the flash fallback).
bool onSdTier();

// Durable lines dropped since boot because the shared card lock was contended when
// the line was emitted (CUM-407). Lock-free (an atomic read), so any task - a web
// handler, the health builder - can read it cheaply. The count is also persisted
// into the durable log itself at the next successful write, so it is never a silent
// loss. Reported on /api/log (meta) and the health row.
uint32_t durableSkipped();

// Bytes written to the durable log since boot (CUM-409 flash-wear watch). The single
// source of truth is the write engine's own counter (host-tested), read under the card
// lock that serializes every write, so the reported value is exactly the tested one.
// Reported on /api/log (meta) and the health row so the fleet can see the wear a
// device's logging actually incurs.
size_t durableBytes();

// The attached filesystem (SD card or LittleFS; LittleFS before anything attached, so
// a caller's open simply fails on an unmounted FS). The retrieval route opens and
// chunk-streams log files directly against this (under agent::memory::Lock) so a full
// pull never buffers the whole file in RAM.
::fs::FS& activeFs();

}  // namespace nimbus::errlog
