#pragma once
#include <Arduino.h>
#include <FS.h>

#include <string>
#include <vector>

// music - the device music player: WAV (and MP3 via the vendored minimp3 decoder)
// from the SD card's /music folder, driven by the portable nimbus::orch::MediaQueue state
// machine (lib/core). Foreground playback: a track is never dropped and plays to
// completion, distinct from the SFX engine's drop-when-full decoration queue.
//
// Playback runs on a DEDICATED low-priority task (like the SFX task), never on
// loopTask/tg_poll: a multi-minute clip would otherwise starve the loop watchdog
// (the reply.speak lesson at 15x the duration). Volume rides the shared master
// (agent::store::sfxVolume via solide::audio::setVolume). WAV and MP3 both play;
// MP3 decode uses the vendored CC0 minimp3 decoder (lib/minimp3).
namespace music {

// Start the player task. Call once from setup() after NVS + solide::begin().
void begin();

// Replace the queue with these SD-relative /music track paths and start playing
// from the first ("play this now"). Returns the number of valid tracks queued.
int playNow(const std::vector<std::string>& tracks);

// Play everything in /music (name order) - the no-argument /play. Returns count.
int playAll();

// Append one /music track to the queue without disturbing what is playing.
bool enqueue(const std::string& track);

void pause();      // hold at the current position
void resume();     // resume from pause / start a stopped-but-loaded queue
void stop();       // halt and clear the queue
bool next();       // skip to the next track

// Stop playback ONLY if `name` is the track currently playing. The upload/delete
// path calls this before it overwrites or removes a /music file, so a track can't
// be replaced or deleted out from under the player's open file handle. Returns
// true if it stopped something.
bool stopIfCurrent(const std::string& name);
void setRepeat(bool on);

// A JSON snapshot for media.list / the web UI: {state,current,index,count,repeat,
// tracks:[...]} - built from the queue under the lock.
String statusJson();

// True when this build can decode MP3 (the vendored minimp3 is always present, so
// this returns true; kept as a seam the UI/tools can report).
bool mp3Supported();

// Decode an MP3 file at `path` on `fs` and play it on the speaker, blocking on the
// CALLING task until it finishes (watchdog-safe: it yields on the I2S DMA between
// frames). This is the reply/SFX seam for a Mistral (MP3-only) spoken reply, which
// the WAV-only playWavFile cannot play. The ~13 KB decode work buffer is allocated
// in PSRAM (never on the caller's stack - the sfx task stack is only 8 KB). Returns
// true if the clip decoded and played to the end. NOTE: the shared I2S TX means a
// caller should stopForSpeech() first if music might be streaming (see below).
bool streamMp3File(fs::FS& fs, const char* path);

// Stop music playback if a track is currently playing, so a spoken reply can take
// the shared I2S TX channel immediately instead of queueing behind the whole track
// (solide::audio serializes the TX with a whole-clip mutex). A no-op when idle.
void stopForSpeech();

// Register the media.play / media.pause / media.stop / media.list tools on the
// orchestrator registry (call from the tool-registration phase).
void registerTools();

// List the /music directory (validated names only), sorted. Device-side (SD).
std::vector<std::string> listMusicDir();

}  // namespace music
