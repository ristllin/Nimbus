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
// AsyncTCP). No FS handle is held across a network send.

#include <cstddef>
#include <string>

namespace fs { class FS; }   // fwd-decl keeps this header Arduino-light

namespace nimbus::errlog {

// Resolve the storage tier and record the decision to the durable log. Optional:
// append() self-initializes lazily on first call, so wiring this into boot is a
// convenience (it makes the tier line appear at a deterministic point) - not a
// requirement. Idempotent.
void begin();

// Append one already-redacted line to the durable log, rotating within the tier's
// size cap. `cat` is a short class tag from errlog::cat (nullptr / "" -> "-").
// Never throws; a full or absent FS degrades to the RAM fallback silently (the
// point of the log is to survive failures, not to become one).
void append(const std::string& redacted, const char* cat);

// Agent-read seam: the most recent <= maxBytes of the durable log, line-aligned
// and reboot-surviving (read from the card/flash, newest last). Falls back to the
// in-RAM tail when the FS is unavailable. This is what lets Nimbus self-report
// "I hit X" - an in-process getter the agent (or a device tool) can call.
std::string readRecent(size_t maxBytes);

// Retrieval support (used by src/net/errlog_routes). listJson() returns
// {"tier":"sd|flash","files":[{"name","bytes"}...]} for the current + rotated
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

// Bytes written to the durable log since boot (CUM-409 flash-wear watch). Lock-free.
// Reported on /api/log (meta) and the health row so the fleet can see the wear a
// device's logging actually incurs.
size_t durableBytes();

// The resolved active filesystem (SD card, else LittleFS). The retrieval route
// opens and chunk-streams log files directly against this (under agent::memory::Lock)
// so a full pull never buffers the whole file in RAM. Initializes the tier lazily.
::fs::FS& activeFs();

}  // namespace nimbus::errlog
