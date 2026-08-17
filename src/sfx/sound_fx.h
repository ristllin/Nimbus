#pragma once
#include <cstdint>

#include "nimbus/sfx_map.h"

// sound_fx - the device SFX engine: sound-cue feedback for device
// operations, the audio counterpart of the ring's Dark/Calm/Full language.
//
// Portable core (event vocabulary, per-mode tier ranks, rate gate) lives in
// lib/core nimbus/sfx_map.*; this layer owns config (NVS via agent::store),
// per-job status EDGE detection, file resolution and playback.
//
// Resolution per event slug (all failures fall through SILENTLY - the device
// is perfect with no SD, no WiFi, no files):
//   SD /sfx/custom/<slug>-<n>.wav   (owner-dropped clips, directory-scanned;
//                                    never synced - the manifest can't touch it)
//   -> SD /sfx/<theme>/<slug>-<n>.wav  (random variant; counts from the synced
//                                       manifest)         [theme = sfxTheme()]
//   -> SD /sfx/general/<slug>-<n>.wav
//   -> embedded basic clip (src/sfx/sfx_basic_data.h, flash rodata, playPcm)
//   -> silence
//
// Playback runs on a dedicated LOW-priority task (solide::audio is synchronous;
// nothing here ever blocks the caller). fire() gates on: level x mode rank
// (sfx_map), nimbus::fault SPEAKER, the voice-capture mute, and the RateGate -
// then enqueues (queue depth 2, drop when full: sounds are decoration, never
// backpressure).
namespace sfx {

// Start the engine. Call once from setup() AFTER NVS/config is readable and
// solide::begin() has run; `orchestratorMode` = the boot-resolved mode (a mode
// switch reboots, so it is fixed for the process lifetime).
void begin(bool orchestratorMode);

// Fire an event through the full gate chain (cheap; safe from any task).
void fire(nimbus::sfx::Ev e);

// JobState feed with per-key EDGE detection: fires AgentDone / Error / NeedsYou
// only when a job's status CHANGES to the trigger status (brokers re-send
// frames; repeats must not re-voice). `status` is solide::ring::Status as int.
// Returns true when this frame was a real EDGE for the key (new job, status
// change, or a live job going Offline) - repeats/heartbeat re-sends return
// false. The screensaver's activity clock keys off this so a chatty broker
// can't keep the panel awake forever.
bool onJobState(uint32_t key, uint8_t status);

// Direct play by slug (console `SFX <slug>` / web test button): bypasses the
// level + rate gates but still respects the speaker fault + voice mute.
// Returns false for an unknown slug (a failed/missing file is still "true" -
// resolution is fall-through by design).
bool play(const char* slug);

// Queue the freshly-synthesized /reply.wav (LittleFS) for playback on the sfx
// task. Returns true when queued. reply.speak MUST use this rather than playing
// inline: on tg_poll the ~8-12 s fetch+playback starved loopTask's watchdog.
bool speakReplyWav();

// Voice capture mute: suppress SFX while the mic records (don't pollute STT).
void setMuted(bool muted);

// Re-read level/theme from NVS (call after a web config save).
void refreshConfig();

// Rebuild the SD variant-count cache from /sfx/manifest.json (call after a
// sync completes or the SD tier changes).
void rescan();

// One-word tier summary for STATUS / /api/state: "off" | "basic" | "sd".
const char* tierStr();
// The active level (0-3) for the booted mode, and the active theme slug.
uint8_t     level();
const char* theme();

// Authoritative "the speaker will make no sound right now" predicate. True when
// the level is 0 (owner set silent), the speaker capability is faulted, OR a
// voice capture has muted playback. The orchestrator's audible self-tests
// (beep / loopback / TTS) gate on this so a device set to silent never blares.
bool        isSilent();

}  // namespace sfx
