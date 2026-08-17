#pragma once

// sfx_sync - keeps the SD card's sound pack in sync with the public
// asset repo (nimbus-sounds), fully in the background and fully fail-soft:
// play-time NEVER depends on sync state (sound_fx falls through SD -> embedded
// basic -> silence), so a missing card, deleted files, mid-sync WiFi loss or a
// yanked SD only ever degrade the VARIETY, never the device.
//
// Mechanism (a dedicated low-priority task, one bounded step per wake):
//   1. Gate: SD mounted + !FAULT(sd) + STA up + heap above floor.
//   2. GET dist/manifest.json from raw.githubusercontent.com (TLS via the work
//      arbiter, HTTP/1.0 + Connection: close - the proven request pattern).
//   3. Diff vs the local copy: version bump OR missing/size-mismatched files.
//   4. Download a few files per wake (stream -> /sfx/.tmp with streaming
//      SHA-256 -> verify hash+size -> rename). Idempotent: a reboot or WiFi
//      loss just re-diffs and continues; per-file retries back off, repeated
//      network failure backs the whole task off (max ~16 min).
//   5. On a complete set: persist the manifest locally, sfx::rescan(), fire
//      SyncDone (heavy tier), settle into a slow re-check cadence.
namespace sfxsync {

// One bounded sync step, called from the SFX playback task whenever it is idle
// (queue timeout) and the step is due - NO dedicated task: a second 8 KB stack
// measurably starved the ~25 KB resting internal heap (heapMin crashed to
// triple digits, live 2026-07-11). Cheap when nothing is due.
void tick();

// Status for STATUS / /api/state / the web Device tab:
//   "idle" (gate closed / nothing to do yet), "syncing k/n", "full" (set
//   complete + verified), "error" (backing off after repeated failures).
const char* statusStr();

}  // namespace sfxsync
