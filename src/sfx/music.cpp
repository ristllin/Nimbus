#include "music.h"

#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <solide/audio.h>

#include <algorithm>

#include <esp_heap_caps.h>

#include "../agent/memory_subsystem.h"      // tool registry
#include "../agent/store.h"                 // sfxVolume (shared master)
#include "../sys/agent_log.h"
#include "minimp3.h"                        // vendored CC0 MP3 decoder (lib/minimp3)
#include "nimbus/fault.h"
#include "nimbus/orch/media.h"
#include "nimbus/orch/tool_registry.h"

namespace music {

using nimbus::orch::MediaFormat;
using nimbus::orch::MediaQueue;
using nimbus::orch::MediaState;

static const char* kDir = "/music";

// The queue is the single source of truth for state + control; the player task and
// the caller both touch it under this mutex (short holds only, never across a play).
static MediaQueue        g_q;
static SemaphoreHandle_t g_mux = nullptr;
static bool              g_begun = false;
// Bumped by playNow/next/stop so a track currently streaming notices the queue
// changed under it and yields immediately (a "play now" must interrupt, not wait
// out the current track). The streaming loop captures it and bails when it moves.
static volatile uint32_t g_gen = 0;

// ⚠ ALL SD access in this file goes through agent::memory::Lock - the one mutex
// this codebase uses to serialize the shared SD/SPI bus (memory_subsystem.h). The
// music task streams for minutes; an unlocked f.read racing a memory/file SD write
// corrupts the bus. Held ONLY around each SD op, never across spkFeedBytes.

struct Lock {
  Lock()  { if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY); }
  ~Lock() { if (g_mux) xSemaphoreGiveRecursive(g_mux); }
};

bool mp3Supported() { return true; }   // minimp3 (CC0) is vendored in lib/minimp3

static std::string fullPath(const std::string& name) {
  return std::string(kDir) + "/" + name;
}

std::vector<std::string> listMusicDir() {
  std::vector<std::string> out;
  agent::memory::Lock sdlk;   // the dir scan is quick; hold the SD-bus lock for it
  File dir = SD.open(kDir);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return out; }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      // f.name() can be a full path on some cores - keep only the basename.
      const char* base = nm;
      for (const char* p = nm; *p; p++) if (*p == '/') base = p + 1;
      if (nimbus::orch::validMusicName(base)) out.emplace_back(base);
    }
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end());
  return out;
}

// Between-chunk playback control: returns false when the track should STOP; blocks
// (yielding) while Paused. A brief recursive-mutex hold reads the queue state; the
// give/take around the delay keeps the lock free while parked so control ops land.
static bool keepPlaying(uint32_t gen) {
  Lock lk;
  if (g_q.state() == MediaState::Stopped || g_gen != gen) return false;
  while (g_q.state() == MediaState::Paused && g_gen == gen) {
    xSemaphoreGiveRecursive(g_mux);
    vTaskDelay(pdMS_TO_TICKS(80));
    xSemaphoreTakeRecursive(g_mux, portMAX_DELAY);
  }
  return g_q.state() != MediaState::Stopped && g_gen == gen;
}

// ---- the streaming WAV player (chunked, watchdog-safe, responsive control) ----
// Returns true if the track played to its end, false if stop was requested.
static bool streamWav(const std::string& path, uint32_t gen) {
  File f;
  uint8_t hdr[12];
  bool badHdr = false;
  { agent::memory::Lock sdlk;
    f = SD.open(path.c_str(), FILE_READ);
    if (f) badHdr = (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)); }
  if (!f) { agent::alogf("music: open failed %s", path.c_str()); return true; }
  if (badHdr) { agent::memory::Lock sdlk; f.close(); agent::alogf("music: not a WAV %s", path.c_str()); return true; }
  uint32_t rate = 16000, dataLen = 0;
  bool haveData = false;
  // Walk chunks for 'fmt ' (sample rate) and 'data' (payload start + length).
  { agent::memory::Lock sdlk;
    while (f.available() >= 8) {
      uint8_t ch[8];
      if (f.read(ch, 8) != 8) break;
      uint32_t clen = (uint32_t)ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
      if (!memcmp(ch, "fmt ", 4)) {
        uint8_t fmt[16] = {0};
        const uint32_t take = clen < 16 ? clen : 16;
        f.read(fmt, take);
        rate = (uint32_t)fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
        if (clen > take) f.seek(f.position() + (clen - take));
      } else if (!memcmp(ch, "data", 4)) {
        dataLen = clen; haveData = true; break;
      } else {
        f.seek(f.position() + clen + (clen & 1));   // skip (chunks are word-aligned)
      }
    }
  }
  if (!haveData || rate < 8000 || rate > 48000) { agent::memory::Lock sdlk; f.close(); return true; }

  solide::audio::setVolume(agent::store::sfxVolume() / 100.0f);
  solide::audio::spkOpen(rate);
  uint8_t buf[1024];
  uint32_t left = dataLen;
  bool finished = true;
  while (left > 0) {
    if (!keepPlaying(gen)) { finished = false; break; }   // stop/replace ends, pause holds
    const uint32_t want = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
    int got;
    { agent::memory::Lock sdlk; got = f.read(buf, want); }   // SD op under the bus lock
    if (got <= 0) break;
    solide::audio::spkFeedBytes(buf, (size_t)got);   // blocks on I2S DMA (yields) - NOT under a lock
    left -= (uint32_t)got;
  }
  solide::audio::spkClose();
  { agent::memory::Lock sdlk; f.close(); }
  return finished;
}

// ---- the streaming MP3 player (minimp3 decode -> spkFeedBytes, watchdog-safe) --
// The decoder state (~7 KB) plus the input + PCM buffers live in PSRAM, not on the
// 8 KB task stack. Decode one frame at a time, downmix stereo to the mono speaker,
// feed LE16 PCM to I2S, and poll the same stop/pause control between frames so a
// 3-minute clip stays responsive and never blocks a watchdog-watched task.
struct Mp3Work {
  mp3dec_t dec;
  uint8_t  in[8192];
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   // int16 interleaved
  int16_t  mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2];   // downmix scratch
};

static bool streamMp3(const std::string& path, uint32_t gen) {
  File f;
  { agent::memory::Lock sdlk; f = SD.open(path.c_str(), FILE_READ); }
  if (!f) { agent::alogf("music: open failed %s", path.c_str()); return true; }
  Mp3Work* w = (Mp3Work*)heap_caps_malloc(sizeof(Mp3Work), MALLOC_CAP_SPIRAM);
  if (!w) w = (Mp3Work*)malloc(sizeof(Mp3Work));
  if (!w) { agent::memory::Lock sdlk; f.close(); agent::alogf("music: MP3 out of memory"); return true; }
  mp3dec_init(&w->dec);
  size_t avail = 0;
  bool opened = false, finished = true;
  for (;;) {
    // Refill the sliding input window from the card when it runs low (SD op locked).
    if (avail < 2048) {
      int r; { agent::memory::Lock sdlk; r = f.read(w->in + avail, (int)(sizeof(w->in) - avail)); }
      if (r > 0) avail += (size_t)r;
    }
    if (avail == 0) break;                       // EOF, all frames consumed
    if (!keepPlaying(gen)) { finished = false; break; }
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(&w->dec, w->in, (int)avail, w->pcm, &info);
    if (info.frame_bytes > 0) {
      memmove(w->in, w->in + info.frame_bytes, avail - (size_t)info.frame_bytes);
      avail -= (size_t)info.frame_bytes;
    } else {
      break;   // no full frame in the buffer and no more input -> done
    }
    if (samples <= 0) continue;                  // skipped ID3/junk, keep going
    if (!opened) {
      uint32_t rate = info.hz ? (uint32_t)info.hz : 44100u;
      if (rate < 8000) rate = 8000; if (rate > 48000) rate = 48000;
      solide::audio::setVolume(agent::store::sfxVolume() / 100.0f);
      solide::audio::spkOpen(rate);
      opened = true;
    }
    if (info.channels == 2) {
      for (int i = 0; i < samples; i++)
        w->mono[i] = (int16_t)(((int)w->pcm[2 * i] + (int)w->pcm[2 * i + 1]) / 2);
      solide::audio::spkFeedBytes((const uint8_t*)w->mono, (size_t)samples * 2);
    } else {
      solide::audio::spkFeedBytes((const uint8_t*)w->pcm, (size_t)samples * 2);
    }
  }
  if (opened) solide::audio::spkClose();
  heap_caps_free(w);
  { agent::memory::Lock sdlk; f.close(); }
  return finished;
}

static void playTrack(const std::string& name, uint32_t gen) {
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) { vTaskDelay(pdMS_TO_TICKS(200)); return; }
  const std::string path = fullPath(name);
  // Sniff from the file head so a mislabeled extension is still handled right.
  MediaFormat fmt = MediaFormat::Unknown;
  {
    agent::memory::Lock sdlk;
    File f = SD.open(path.c_str(), FILE_READ);
    if (f) { uint8_t head[12]; int n = f.read(head, sizeof head); f.close();
             fmt = nimbus::orch::sniffFormat(head, n < 0 ? 0 : (size_t)n, name.c_str()); }
  }
  bool finished = true;
  if (fmt == MediaFormat::Wav) {
    finished = streamWav(path, gen);
  } else if (fmt == MediaFormat::Mp3) {
    finished = streamMp3(path, gen);   // minimp3 decode -> speaker (CC0, lib/minimp3)
  } else {
    agent::alogf("music: %s unsupported format, skipping", name.c_str());
  }
  // Advance to the next track only on a NATURAL finish AND if nothing changed the
  // queue under us (a stop/skip/replace bumped g_gen); otherwise leave the queue as
  // the control op set it, so the task's next pass reads the right current track.
  if (finished) { Lock lk; if (g_gen == gen) g_q.trackFinished(); }
}

static void musicTask(void*) {
  for (;;) {
    std::string cur;
    bool playing = false;
    uint32_t gen = 0;
    { Lock lk; playing = g_q.playing(); cur = g_q.current(); gen = g_gen; }
    if (playing && !cur.empty()) playTrack(cur, gen);
    else vTaskDelay(pdMS_TO_TICKS(120));
  }
}

void begin() {
  if (g_begun) return;
  g_begun = true;
  g_mux = xSemaphoreCreateRecursiveMutex();
  // Low priority on core 0 (same as the SFX task); 8 KB stack covers SD + decode.
  xTaskCreatePinnedToCore(musicTask, "music", 8192, nullptr, 1, nullptr, 0);
}

// ---- control API -----------------------------------------------------------

int playNow(const std::vector<std::string>& tracks) {
  std::vector<std::string> valid;
  for (const auto& t : tracks) if (nimbus::orch::validMusicName(t.c_str())) valid.push_back(t);
  Lock lk;
  int n = g_q.playNow(valid);
  g_gen++;   // interrupt whatever is streaming so the new queue plays NOW
  return n;
}

int playAll() { return playNow(listMusicDir()); }

bool enqueue(const std::string& track) {
  if (!nimbus::orch::validMusicName(track.c_str())) return false;
  Lock lk;
  return g_q.enqueue(track);   // append only - does not disturb the current track
}

void pause()  { Lock lk; g_q.pause(); }              // holds position; NOT an interrupt
void resume() { Lock lk; g_q.play(); }
void stop()   { Lock lk; g_q.stop(); g_gen++; }       // interrupt + halt
bool next()   { Lock lk; bool r = g_q.next(); g_gen++; return r; }   // interrupt + skip
void setRepeat(bool on) { Lock lk; g_q.setRepeat(on); }

String statusJson() {
  Lock lk;
  String j = "{\"state\":\"";
  j += nimbus::orch::mediaStateName(g_q.state());
  j += "\",\"current\":\"";
  j += g_q.current().c_str();
  j += "\",\"index\":" + String(g_q.index());
  j += ",\"count\":" + String(g_q.size());
  j += ",\"repeat\":" + String(g_q.repeat() ? "true" : "false");
  j += ",\"tracks\":[";
  const auto& ts = g_q.tracks();
  for (size_t i = 0; i < ts.size(); i++) {
    if (i) j += ",";
    j += "\""; j += ts[i].c_str(); j += "\"";
  }
  j += "]}";
  return j;
}

// ---- media.* tools ---------------------------------------------------------

static std::string argStr(ArduinoJson::JsonObjectConst a, const char* k) {
  const char* v = a[k] | (const char*)nullptr;
  return v ? std::string(v) : std::string();
}

void registerTools() {
  auto& reg = agent::memory::registry();

  reg.add("media.play",
          "Play music from the device's SD /music folder on the speaker. With no "
          "argument, plays every track. 'path' plays one track now; add 'queue':true "
          "to append it instead of replacing. WAV plays today; MP3 needs the decoder.",
          [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            const std::string path = argStr(a, "path");
            const bool queue = a["queue"] | false;
            if (path.empty()) {
              int n = playAll();
              return nimbus::orch::ToolResult::ok(n ? statusJson().c_str()
                                                    : std::string("{\"error\":\"no tracks in /music\"}"));
            }
            if (!nimbus::orch::validMusicName(path.c_str()))
              return nimbus::orch::ToolResult::fail("invalid track name (SD /music, .wav or .mp3)");
            if (queue) { enqueue(path); resume(); }
            else       { playNow({path}); }
            return nimbus::orch::ToolResult::ok(std::string(statusJson().c_str()));
          },
          R"({"type":"object","properties":{"path":{"type":"string"},"queue":{"type":"boolean"}}})");

  reg.add("media.pause", "Pause music playback (resume with media.play).",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            pause();
            return nimbus::orch::ToolResult::ok(std::string(statusJson().c_str()));
          }, R"({"type":"object","properties":{}})");

  reg.add("media.stop", "Stop music playback and clear the queue.",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            stop();
            return nimbus::orch::ToolResult::ok(std::string(statusJson().c_str()));
          }, R"({"type":"object","properties":{}})");

  reg.add("media.list",
          "List the tracks in SD /music and the current playback state.",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            String j = "{\"available\":[";
            auto tracks = listMusicDir();
            for (size_t i = 0; i < tracks.size(); i++) { if (i) j += ","; j += "\""; j += tracks[i].c_str(); j += "\""; }
            j += "],\"player\":"; j += statusJson(); j += "}";
            return nimbus::orch::ToolResult::ok(std::string(j.c_str()));
          }, R"({"type":"object","properties":{}})");
}

}  // namespace music
