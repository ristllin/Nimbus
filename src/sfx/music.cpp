#include "music.h"

#include <SD.h>
#include <solide/storage.h>   // activeFs(): the mounted card (SD_MMC on Freenove, SD on solide)
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
#include "nimbus/tts_catalog.h"             // core::downmixStereoToMono (host-tested)

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
  File dir = solide::storage::activeFs().open(kDir);
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

// Parse a canonical RIFF/WAVE header: walk chunks for 'fmt ' (sample rate) and
// 'data' (payload length). Returns true with rate+dataLen set when a data chunk is
// found. Caller holds the SD-bus lock. Split out to keep streamWav under the
// complexity gate.
static bool parseWavHeader(File& f, uint32_t& rate, uint32_t& dataLen) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return false;
  while (f.available() >= 8) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) return false;
    const uint32_t clen = (uint32_t)ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t fmt[16] = {0};
      const uint32_t take = clen < 16 ? clen : 16;
      f.read(fmt, take);
      rate = (uint32_t)fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      if (clen > take) f.seek(f.position() + (clen - take));
    } else if (!memcmp(ch, "data", 4)) {
      dataLen = clen;
      return true;
    } else {
      f.seek(f.position() + clen + (clen & 1));   // skip (chunks are word-aligned)
    }
  }
  return false;
}

// ---- the streaming WAV player (chunked, watchdog-safe, responsive control) ----
// Returns true if the track played to its end, false if stop was requested.
static bool streamWav(const std::string& path, uint32_t gen) {
  File f;
  uint32_t rate = 16000, dataLen = 0;
  bool haveData = false;
  { agent::memory::Lock sdlk;
    f = solide::storage::activeFs().open(path.c_str(), FILE_READ);
    if (f) haveData = parseWavHeader(f, rate, dataLen); }
  if (!f) { agent::alogf("music: open failed %s", path.c_str()); return true; }
  if (!haveData || rate < 8000 || rate > 48000) {
    agent::memory::Lock sdlk; f.close(); agent::alogf("music: not a usable WAV %s", path.c_str()); return true;
  }

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
// The decoder state (~7 KB), the input + PCM buffers, AND minimp3's ~15 KB per-frame
// decode scratch all live in PSRAM, not on the task stack (CUM-222). minimp3 normally
// puts that scratch on the caller's stack, which forced the sfx task to 20 KB and left
// the 8 KB music task one MP3 track away from a stack overflow; routing it through
// mp3dec_decode_frame_ex(scratch) keeps the scarce internal SRAM free. Decode one
// frame at a time, downmix stereo to the mono speaker, feed LE16 PCM to I2S, and poll
// the same stop/pause control between frames so a 3-minute clip stays responsive and
// never blocks a watchdog-watched task.
struct Mp3Work {
  mp3dec_t dec;
  uint8_t  in[8192];
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];   // int16 interleaved
  int16_t  mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2];   // downmix scratch
  void*    scratch;                                   // minimp3 per-frame scratch (PSRAM)
};

// Feed one decoded frame to the speaker, downmixing stereo to mono. Split out to
// keep streamMp3 under the complexity gate. The mix math lives in lib/core so it is
// host-tested independent of the I2S sink.
static void feedMp3Frame(Mp3Work* w, int samples, int channels) {
  if (channels == 2) {
    core::downmixStereoToMono(w->pcm, samples, w->mono);
    solide::audio::spkFeedBytes((const uint8_t*)w->mono, (size_t)samples * 2);
  } else {
    solide::audio::spkFeedBytes((const uint8_t*)w->pcm, (size_t)samples * 2);
  }
}

// Top up the sliding MP3 input window from the file. `useSdLock` wraps the read in
// the shared SD-bus lock (SD music path); the LittleFS reply path passes false - it
// is not on the SD/SPI bus, so it must not contend for that mutex.
static void refillMp3(File& f, Mp3Work* w, size_t& avail, bool useSdLock) {
  if (avail >= 2048) return;
  int r;
  if (useSdLock) { agent::memory::Lock sdlk; r = f.read(w->in + avail, (int)(sizeof(w->in) - avail)); }
  else           { r = f.read(w->in + avail, (int)(sizeof(w->in) - avail)); }
  if (r > 0) avail += (size_t)r;
}

// Open the speaker at the stream's sample rate on the first decoded frame.
static void openSpeakerOnce(bool& opened, int hz) {
  if (opened) return;
  uint32_t rate = hz ? (uint32_t)hz : 44100u;
  rate = rate < 8000 ? 8000 : (rate > 48000 ? 48000 : rate);
  solide::audio::setVolume(agent::store::sfxVolume() / 100.0f);
  solide::audio::spkOpen(rate);
  opened = true;
}

// The shared decode loop for both the SD-music and the LittleFS-reply paths. `f` is
// an already-open MP3 file; `useSdLock` locks each file read on the SD-bus mutex
// (SD path only). When `hasGen`, the SD queue's stop/pause control is honored via
// keepPlaying(gen); the reply path passes hasGen=false and only stops on a speaker
// fault. Returns true if the clip played to its natural end. Watchdog-safe: the
// per-frame spkFeedBytes blocks on the I2S DMA, which yields.
static bool decodeMp3Stream(File& f, Mp3Work* w, bool useSdLock, uint32_t gen, bool hasGen) {
  mp3dec_init(&w->dec);
  size_t avail = 0;
  bool opened = false, finished = true;
  for (;;) {
    refillMp3(f, w, avail, useSdLock);
    if (avail == 0) break;                       // EOF, all frames consumed
    if (hasGen ? !keepPlaying(gen)
               : nimbus::fault::active(nimbus::fault::SPEAKER)) { finished = false; break; }
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame_ex(&w->dec, w->in, (int)avail, w->pcm, &info, w->scratch);
    if (info.frame_bytes <= 0) break;            // no full frame + no more input -> done
    memmove(w->in, w->in + info.frame_bytes, avail - (size_t)info.frame_bytes);
    avail -= (size_t)info.frame_bytes;
    if (samples <= 0) continue;                  // skipped ID3/junk, keep going
    openSpeakerOnce(opened, info.hz);
    feedMp3Frame(w, samples, info.channels);
  }
  if (opened) solide::audio::spkClose();
  return finished;
}

// Allocate the decode work buffer (~13 KB) plus minimp3's per-frame scratch (~15 KB)
// in PSRAM (never on the caller's task stack). Falls back to internal heap only if
// PSRAM is exhausted. Caller frees with freeMp3Work().
static Mp3Work* allocMp3Work() {
  Mp3Work* w = (Mp3Work*)heap_caps_malloc(sizeof(Mp3Work), MALLOC_CAP_SPIRAM);
  if (!w) w = (Mp3Work*)malloc(sizeof(Mp3Work));
  if (!w) return nullptr;
  w->scratch = heap_caps_malloc((size_t)mp3dec_scratch_size(), MALLOC_CAP_SPIRAM);
  if (!w->scratch) w->scratch = malloc((size_t)mp3dec_scratch_size());
  if (!w->scratch) { heap_caps_free(w); return nullptr; }
  return w;
}

// Free a work buffer and its scratch. heap_caps_free handles both the PSRAM and the
// internal-heap fallback allocations.
static void freeMp3Work(Mp3Work* w) {
  if (!w) return;
  heap_caps_free(w->scratch);
  heap_caps_free(w);
}

static bool streamMp3(const std::string& path, uint32_t gen) {
  File f;
  { agent::memory::Lock sdlk; f = solide::storage::activeFs().open(path.c_str(), FILE_READ); }
  if (!f) { agent::alogf("music: open failed %s", path.c_str()); return true; }
  Mp3Work* w = allocMp3Work();
  if (!w) { agent::memory::Lock sdlk; f.close(); agent::alogf("music: MP3 out of memory"); return true; }
  bool finished = decodeMp3Stream(f, w, /*useSdLock=*/true, gen, /*hasGen=*/true);
  freeMp3Work(w);
  { agent::memory::Lock sdlk; f.close(); }
  return finished;
}

bool streamMp3File(fs::FS& fs, const char* path) {
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) return false;
  File f = fs.open(path, FILE_READ);
  if (!f) { agent::alogf("music: mp3 reply open failed %s", path); return false; }
  Mp3Work* w = allocMp3Work();
  if (!w) { f.close(); agent::alogf("music: MP3 reply out of memory"); return false; }
  // No SD-bus lock (LittleFS is off that bus) and no queue gen (a short reply clip
  // is not a controllable track). Natural EOF returns true; a mid-clip speaker fault
  // returns false.
  bool ok = decodeMp3Stream(f, w, /*useSdLock=*/false, /*gen=*/0, /*hasGen=*/false);
  freeMp3Work(w);
  f.close();
  return ok;
}

void stopForSpeech() {
  Lock lk;
  // Stop ANY non-stopped track, not just an actively Playing one. A PAUSED track
  // leaves the music task parked inside keepPlaying() with the driver's whole-clip
  // I2S TX mutex still held (spkClose runs only after the stream loop exits), so a
  // reply's spkOpen would block forever. Bumping g_gen + Stopped forces keepPlaying
  // to return false, the loop exits, spkClose releases the TX, and the reply plays.
  if (g_q.state() != MediaState::Stopped) { g_q.stop(); g_gen++; }
}

static void playTrack(const std::string& name, uint32_t gen) {
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) { vTaskDelay(pdMS_TO_TICKS(200)); return; }
  const std::string path = fullPath(name);
  // Sniff from the file head so a mislabeled extension is still handled right.
  MediaFormat fmt = MediaFormat::Unknown;
  {
    agent::memory::Lock sdlk;
    File f = solide::storage::activeFs().open(path.c_str(), FILE_READ);
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
