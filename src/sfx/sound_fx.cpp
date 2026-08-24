#include "sound_fx.h"
#include "sfx_sync.h"
#include "music.h"   // streamMp3File (MP3 reply playback) + stopForSpeech (I2S duck)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <solide/audio.h>
#include <solide/ring.h>   // ring::Status - the JobState vocabulary for edge mapping

#include "../sys/agent_log.h"
#include "../sys/ps_json.h"       // PsramJsonAllocator (manifest parse off internal heap)
#include "../agent/store.h"
#include "../agent/memory_subsystem.h"  // memory::haveSd()
#include "nimbus/fault.h"
#include "nimbus/sfx_paths.h"

#include "sfx_basic_data.h"  // generated: the embedded basic PCM (THE one include site)

namespace sfx {

using nimbus::sfx::Ev;
using nimbus::sfx::RateGate;

namespace {

constexpr int kQueueDepth = 2;
constexpr int kMaxVariants = 8;   // variants per (slug, pool) the resolver considers
// sfx task stack. Sized to hold the minimp3 decoder's ~16 KB stack scratch (used by
// a spoken MP3 reply, music::streamMp3File) plus the sync tick's TLS client, with
// margin. See begin() for the full rationale + the bench validation note.
constexpr uint32_t kSfxStackBytes = 20480;

struct Item {
  uint8_t ev;
  bool    forced;   // console/web test play: skip nothing at play time, it was pre-gated
  bool    speak = false;  // play a synthesized reply (LittleFS) instead of an event
                          // clip - the reply.speak tool queues here so an in-turn TTS
                          // readout never blocks tg_poll for the clip's duration
                          // (a ~12 s inline fetch+playback starved loopTask's
                          // watchdog: task_wdt -> abort, field "harness reset").
  bool    mp3 = false;    // reply format: true => /reply.mp3 (minimp3), false =>
                          // /reply.wav (playWavFile). A Mistral (MP3-only) device
                          // uses the MP3 branch so it can finally speak.
};

bool          g_began = false;
bool          g_orch = false;
volatile bool g_muted = false;
volatile bool g_cfgDirty = false;           // web/menu asked to re-read config; sfx task services it
uint8_t       g_level = 0;
char          g_theme[12] = "pulse";
RateGate      g_gate;                       // 300 ms global gap / 2 s per-event
QueueHandle_t g_q = nullptr;

// Variant counts per (event, pool): [0] = /sfx/custom/ (owner-dropped clips,
// directory-scanned - never synced), [1] = active theme pool, [2] = general.
// Rebuilt by rescan() from the synced /sfx/manifest.json + the custom dir scan;
// zero => tier absent.
constexpr int kPoolCustom  = 0;
constexpr int kPoolTheme   = 1;
constexpr int kPoolGeneral = 2;
uint8_t g_counts[(int)Ev::COUNT][3] = {};

// Per-job status edge table (round-robin): fire only on CHANGE to a trigger
// status - brokers re-send frames and a stuck job must not re-voice every frame.
struct JobEdge { uint32_t key; uint8_t status; bool used; };
JobEdge g_jobs[12] = {};
int     g_jobsNext = 0;

bool sdUsable() {
  return agent::memory::haveSd() && !nimbus::fault::active(nimbus::fault::SD);
}

bool speakerUsable() { return !nimbus::fault::active(nimbus::fault::SPEAKER); }

// Try one pool's WAV for the event (random variant). True if a file PLAYED.
bool playFromPool(Ev e, const char* pool, uint8_t count) {
  if (count == 0) return false;
  const uint8_t k = esp_random() % count;
  char path[64];
  snprintf(path, sizeof(path), "/sfx/%s/%s-%u.wav", pool, nimbus::sfx::slug(e), k);
  return solide::audio::playWavFile(SD, path);
}

bool playEmbedded(Ev e) {
  const char* s = nimbus::sfx::slug(e);
  for (size_t i = 0; i < sfx_basic::kClipCount; i++) {
    if (strcmp(sfx_basic::kClips[i].slug, s) == 0) {
      const auto& c = sfx_basic::kClips[i];
      return solide::audio::playPcm(c.pcm, c.samples, c.rate);
    }
  }
  return false;
}

// The resolver: SD custom pool -> SD theme pool -> SD general pool -> embedded
// basic -> silence. Every step is fall-through; "nothing played" is a valid,
// silent outcome. /sfx/custom/ is the owner's local override folder: drop
// <slug>-<n>.wav files there (numbered from 0) and they win over everything.
void resolveAndPlay(Ev e) {
  if (!speakerUsable() || g_muted) return;   // re-check at play time (queue delay)
  if (sdUsable()) {
    if (playFromPool(e, "custom", g_counts[(int)e][kPoolCustom])) return;
    if (playFromPool(e, g_theme, g_counts[(int)e][kPoolTheme])) return;
    if (playFromPool(e, "general", g_counts[(int)e][kPoolGeneral])) return;
  }
  playEmbedded(e);
}

// Read NVS config into the live engine state and recount SD variants. Runs
// ONLY on the sfx task (or pre-task at boot) so g_level/g_theme/g_counts stay
// single-writer - playback (also on the sfx task) can never read a half-rebuilt
// count table. Post-boot config edits arrive via the g_cfgDirty flag, never by
// mutating this state from the web/menu task.
void applyConfig() {
  g_level = g_orch ? agent::store::sfxLevelOrch() : agent::store::sfxLevelNotif();
  String t = agent::store::sfxTheme();
  strncpy(g_theme, t.c_str(), sizeof(g_theme) - 1);
  g_theme[sizeof(g_theme) - 1] = 0;
  // Master speaker volume is a driver-global (covers SFX + TTS + beep); set it
  // here so it applies at boot and re-applies on any config change.
  solide::audio::setVolume(agent::store::sfxVolume() / 100.0f);
  rescan();
}

void sfxTask(void*) {
  Item it;
  for (;;) {
    if (g_cfgDirty) { g_cfgDirty = false; applyConfig(); }   // serviced on THIS task
    // Wait for a sound; on idle timeouts run the SD sync's bounded tick. One
    // shared task/stack - a second dedicated sync task measurably starved the
    // ~25 KB resting internal heap. A sound queued mid-download plays right
    // after the current bounded step finishes (a few seconds, worst case).
    if (xQueueReceive(g_q, &it, pdMS_TO_TICKS(2000)) == pdTRUE) {
      if (it.speak) {
        // Spoken reply. /reply.wav|/reply.mp3 may be overwritten by a rapid next
        // synth - acceptable for speech (latest wins); the history row was captured
        // at queue time. Muted/faulted checks re-run here, not just at post time.
        if (speakerUsable() && !g_muted) {
          // Both the reply and any music track feed the ONE shared I2S TX (the driver
          // serializes it with a whole-clip mutex), so an active track would make the
          // reply queue behind minutes of audio. Duck music first: a spoken reply wins.
          music::stopForSpeech();
          // Playback runs async on this task, so speakOnDevice already reported
          // "spoken" at queue time (same as the WAV path). Log a decode/playback miss
          // here so a corrupt clip is at least diagnosable in /api/log.
          if (it.mp3) { if (!music::streamMp3File(LittleFS, "/reply.mp3"))
                          agent::alogf("sfx: /reply.mp3 did not play (decode/speaker)"); }
          else        solide::audio::playWavFile(LittleFS, "/reply.wav");
        }
      } else {
        resolveAndPlay((Ev)it.ev);
      }
    } else
      sfxsync::tick();
  }
}

}  // namespace

void begin(bool orchestratorMode) {
  if (g_began) return;
  g_orch = orchestratorMode;
  applyConfig();   // synchronous at boot - single-threaded, no sfx task yet
  g_q = xQueueCreate(kQueueDepth, sizeof(Item));
  if (!g_q) {   // a decorative subsystem must never brick the device - bail quietly
    agent::alogf("sfx: queue alloc failed - sfx disabled");
    return;     // g_began stays false: fire()/play() no-op, tier reports off
  }
  // Low priority, core 0 (main loop + LED render live on core 1): a sound is
  // decoration - it must never contend with rendering or the radio tasks.
  // Stack: playback transients + the sync tick's TLS client (~8 KB) USED to fit in
  // 8 KB, but a spoken MP3 reply now decodes on this task via music::streamMp3File,
  // and minimp3's mp3dec_decode_frame puts a ~16 KB mp3dec_scratch_t on the STACK
  // (grbuf + syn + maindata - it cannot be moved to PSRAM the way Mp3Work is). So
  // the decode path peaks near 17 KB; size the stack to hold it with margin. If a
  // board cannot spare this, xTaskCreate fails and sfx (and on-device voice) degrade
  // gracefully rather than bricking. BENCH: confirm the stack high-water mark keeps
  // >2 KB free on a real MP3 reply and that free internal heap stays healthy.
  if (xTaskCreatePinnedToCore(sfxTask, "sfx", kSfxStackBytes, nullptr, 1, nullptr, 0) != pdPASS) {
    agent::alogf("sfx: task create failed - sfx disabled");
    vQueueDelete(g_q);
    g_q = nullptr;
    return;
  }
  g_began = true;
  agent::alogf("sfx: up (mode=%s level=%u theme=%s tier=%s)",
               g_orch ? "orch" : "notifier", g_level, g_theme, tierStr());
}

void fire(Ev e) {
  if (!g_began || !g_q) return;
  if (!nimbus::sfx::shouldPlay(e, g_level, g_orch)) return;
  if (!speakerUsable() || g_muted) return;
  if (!g_gate.allow(e, millis())) return;
  Item it{(uint8_t)e, false};
  xQueueSend(g_q, &it, 0);   // full queue -> drop; sounds never backpressure
}

bool onJobState(uint32_t key, uint8_t status) {
  using solide::ring::Status;
  // Find / update the edge slot for this key.
  JobEdge* slot = nullptr;
  for (auto& j : g_jobs)
    if (j.used && j.key == key) { slot = &j; break; }
  if (status == (uint8_t)Status::Offline) {
    if (slot) slot->used = false;      // job gone - free the slot, no sound
    return slot != nullptr;            // a LIVE job leaving is an edge; a re-sent
  }                                    // offline for an unknown key is not
  if (slot && slot->status == status) return false;   // repeat frame - not an edge
  if (!slot) {                                   // new key: prefer a free slot,
    for (auto& j : g_jobs)                       // evict a live one only when full
      if (!j.used) { slot = &j; break; }
    if (!slot) {
      slot = &g_jobs[g_jobsNext];
      g_jobsNext = (g_jobsNext + 1) % (int)(sizeof(g_jobs) / sizeof(g_jobs[0]));
    }
    slot->used = true;
    slot->key = key;
  }
  slot->status = status;
  switch ((Status)status) {
    case Status::Done:             fire(Ev::AgentDone); break;
    case Status::Error:            fire(Ev::Error); break;
    case Status::WaitingInput:
    case Status::AwaitingApproval: fire(Ev::NeedsYou); break;
    default: break;   // Idle/Running edges stay silent here (spawn has its own hook)
  }
  return true;
}

bool speakReply(bool mp3) {
  if (!g_began || !g_q) return false;
  if (!speakerUsable()) return false;
  Item it{0, false, true, mp3};
  return xQueueSend(g_q, &it, 0) == pdTRUE;   // full queue -> honest false
}

bool play(const char* s) {
  Ev e;
  if (!g_began || !nimbus::sfx::parseSlug(s, e)) return false;
  if (!speakerUsable() || g_muted) return true;   // known slug, muted - "handled"
  Item it{(uint8_t)e, true};
  xQueueSend(g_q, &it, 0);
  return true;
}

void setMuted(bool m) { g_muted = m; }

void refreshConfig() {
  // Called from the web (config apply) and on-device menu tasks. Do NOT read
  // NVS or rescan the SD manifest here - that would write g_level/g_theme/
  // g_counts from a foreign task while the sfx task plays. Flag it; the sfx
  // task applies it on its next wake. Pre-boot (no task yet) apply inline.
  if (!g_began) { applyConfig(); return; }
  g_cfgDirty = true;
}

// Count the owner's /sfx/custom/ clips by directory scan (there is no manifest
// for that pool - it is never synced; safeRepoPath refuses sd/custom/ entries).
// Files must be "<slug>-<n>.wav" numbered contiguously from 0; a sparse set
// still falls through safely (a missing variant just misses into the theme pool).
void scanCustomDir() {
  File dir = SD.open("/sfx/custom");
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      char slugBuf[24];
      unsigned n = 0;
      Ev e;
      if (nimbus::sfx::parseClipFilename(base, slugBuf, sizeof(slugBuf), &n) &&
          nimbus::sfx::parseSlug(slugBuf, e) && n < kMaxVariants &&
          g_counts[(int)e][kPoolCustom] < kMaxVariants)
        g_counts[(int)e][kPoolCustom]++;
    }
    f.close();
  }
  dir.close();
}

void rescan() {
  memset(g_counts, 0, sizeof(g_counts));
  if (!sdUsable()) { agent::alogf("sfx: rescan - no SD"); return; }
  scanCustomDir();
  File f = SD.open("/sfx/manifest.json", FILE_READ);
  if (!f) { agent::alogf("sfx: rescan - no /sfx/manifest.json"); return; }
  JsonDocument filter;
  filter["files"].add<JsonObject>()["path"] = true;
  JsonDocument doc(&agent::PsramJsonAllocator::instance());
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  if (err) { agent::alogf("sfx: manifest parse: %s", err.c_str()); return; }
  for (JsonObjectConst fo : doc["files"].as<JsonArrayConst>()) {
    const char* p = fo["path"] | "";
    if (strncmp(p, "sd/", 3) != 0) continue;      // basic tier is embedded, skip
    const char* pool = p + 3;
    const char* sl = strchr(pool, '/');
    if (!sl) continue;
    const size_t poolLen = (size_t)(sl - pool);
    int poolIdx = -1;
    if (strncmp(pool, g_theme, poolLen) == 0 && g_theme[poolLen] == 0) poolIdx = kPoolTheme;
    else if (poolLen == 7 && strncmp(pool, "general", 7) == 0)         poolIdx = kPoolGeneral;
    if (poolIdx < 0) continue;
    // "<slug>-<n>.wav" -> slug (shared, host-tested parser)
    char slugBuf[24];
    unsigned n = 0;
    if (!nimbus::sfx::parseClipFilename(sl + 1, slugBuf, sizeof(slugBuf), &n)) continue;
    Ev e;
    if (!nimbus::sfx::parseSlug(slugBuf, e)) continue;
    if (g_counts[(int)e][poolIdx] < kMaxVariants) g_counts[(int)e][poolIdx]++;
  }
  int custN = 0, themeN = 0, genN = 0;
  for (const auto& row : g_counts) {
    custN += row[kPoolCustom];
    themeN += row[kPoolTheme];
    genN += row[kPoolGeneral];
  }
  agent::alogf("sfx: rescan - custom=%d, theme(%s)=%d, general=%d clips",
               custN, g_theme, themeN, genN);
}

const char* tierStr() {
  if (g_level == 0) return "off";
  if (sdUsable())
    for (const auto& row : g_counts)
      if (row[0] || row[1] || row[2]) return "sd";
  return "basic";
}

uint8_t     level() { return g_level; }
const char* theme() { return g_theme; }

bool isSilent() { return g_level == 0 || !speakerUsable() || g_muted; }

}  // namespace sfx
