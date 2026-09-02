#pragma once
#include <Arduino.h>

#include <cstdint>
#include <string>

// music::store - the WRITE side of the SD /music folder (CUM-40).
//
// The player (music.cpp) only READS /music; before this there was no user path to
// PUT a track there (the file API writes /mem/files, never /music), so /play always
// found an empty folder. This is the streaming upload seam the web route drives to
// land a validated .wav/.mp3 straight into /music, the exact directory the player
// scans - closing the "no way to add music" gap.
//
// Concurrency contract mirrors files_subsystem: every SD op runs under
// agent::memory::Lock (the one shared SD/SPI bus mutex), held only for the single
// op, never across a network wait. One streaming session at a time (an ESP32 has no
// business running parallel SD uploads). The accept/reject decision is the portable
// nimbus::orch::musicUploadAllowed gate, so an upload can never disagree with what
// the player will list and play.

namespace music {
namespace store {

// Monotonic id of the live upload session (0 = none). The web route scopes its
// disconnect-abort to this id so a stale onDisconnect can't nuke a later upload
// that took the slot.
uint32_t uploadGen();

// Open a streaming write for `name` into /music. Gates on musicUploadAllowed
// (SD present + a valid .wav/.mp3 name); on refusal returns false with `err` set
// to user-facing copy. On success the bytes stream to /music/.part.
bool uploadBegin(const std::string& name, std::string& err);

// Append one chunk to the open session. Returns false on an SD write error or when
// the per-file cap is exceeded (a lying client is still bounded).
bool uploadChunk(const uint8_t* data, size_t len);

// Commit (ok=true) - fsync + rename /music/.part into place - or discard
// (ok=false). Returns true only when the track is on the card under its final
// name; `err` carries the reason on failure.
bool uploadFinish(bool ok, std::string& err);

// Discard the live session and delete the partial (disconnect / error paths).
void uploadAbort();
// Abort only if `gen` still owns the session (scoped disconnect handler).
void uploadAbortGen(uint32_t gen);

// Delete a track from /music. Returns false (err set) when there is no card, the
// name is invalid, or the file is not present.
bool removeTrack(const std::string& name, std::string& err);

}  // namespace store
}  // namespace music
