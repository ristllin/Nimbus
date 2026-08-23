#include "music.h"

#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <solide/audio.h>

#include <algorithm>

#include "../agent/memory_subsystem.h"      // tool registry
#include "../agent/store.h"                 // sfxVolume (shared master)
#include "../sys/agent_log.h"
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

struct Lock {
  Lock()  { if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY); }
  ~Lock() { if (g_mux) xSemaphoreGiveRecursive(g_mux); }
};

bool mp3Supported() {
#ifdef NIMBUS_HAS_MP3
  return true;
#else
  return false;   // the Helix decoder library is not linked in this build
#endif
}

static std::string fullPath(const std::string& name) {
  return std::string(kDir) + "/" + name;
}

std::vector<std::string> listMusicDir() {
  std::vector<std::string> out;
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

// ---- the streaming WAV player (chunked, watchdog-safe, responsive control) ----
// Returns true if the track played to its end, false if stop was requested.
static bool streamWav(const std::string& path) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) { agent::alogf("music: open failed %s", path.c_str()); return true; }  // skip forward
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
    f.close(); agent::alogf("music: not a WAV %s", path.c_str()); return true;
  }
  uint32_t rate = 16000, dataLen = 0;
  bool haveData = false;
  // Walk chunks for 'fmt ' (sample rate) and 'data' (payload start + length).
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
  if (!haveData || rate < 8000 || rate > 48000) { f.close(); return true; }

  solide::audio::setVolume(agent::store::sfxVolume() / 100.0f);
  solide::audio::spkOpen(rate);
  uint8_t buf[1024];
  uint32_t left = dataLen;
  bool finished = true;
  while (left > 0) {
    // Control between chunks: stop ends the track; pause holds without closing TX.
    {
      Lock lk;
      if (g_q.state() == MediaState::Stopped) { finished = false; break; }
      while (g_q.state() == MediaState::Paused) {
        xSemaphoreGiveRecursive(g_mux);
        vTaskDelay(pdMS_TO_TICKS(80));
        xSemaphoreTakeRecursive(g_mux, portMAX_DELAY);
        if (g_q.state() == MediaState::Stopped) break;
      }
      if (g_q.state() == MediaState::Stopped) { finished = false; break; }
    }
    const uint32_t want = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
    const int got = f.read(buf, want);
    if (got <= 0) break;
    solide::audio::spkFeedBytes(buf, (size_t)got);   // blocks on I2S DMA (yields)
    left -= (uint32_t)got;
  }
  solide::audio::spkClose();
  f.close();
  return finished;
}

static void playTrack(const std::string& name) {
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) { vTaskDelay(pdMS_TO_TICKS(200)); return; }
  const std::string path = fullPath(name);
  // Sniff from the file head so a mislabeled extension is still handled right.
  MediaFormat fmt = MediaFormat::Unknown;
  {
    File f = SD.open(path.c_str(), FILE_READ);
    if (f) { uint8_t head[12]; int n = f.read(head, sizeof head); f.close();
             fmt = nimbus::orch::sniffFormat(head, n < 0 ? 0 : (size_t)n, name.c_str()); }
  }
  bool finished = true;
  if (fmt == MediaFormat::Wav) {
    finished = streamWav(path);
  } else if (fmt == MediaFormat::Mp3) {
    if (!mp3Supported()) {
      agent::alogf("music: %s is MP3 - decoder not built in, skipping", name.c_str());
    } else {
#ifdef NIMBUS_HAS_MP3
      finished = streamMp3(path);   // Helix decode loop (device seam; see mp3Supported)
#endif
    }
  } else {
    agent::alogf("music: %s unsupported format, skipping", name.c_str());
  }
  // Advance only on a natural finish; a stop leaves the queue where the user put it.
  if (finished) { Lock lk; g_q.trackFinished(); }
}

static void musicTask(void*) {
  for (;;) {
    std::string cur;
    bool playing = false;
    { Lock lk; playing = g_q.playing(); cur = g_q.current(); }
    if (playing && !cur.empty()) playTrack(cur);
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
  return g_q.playNow(valid);
}

int playAll() { return playNow(listMusicDir()); }

bool enqueue(const std::string& track) {
  if (!nimbus::orch::validMusicName(track.c_str())) return false;
  Lock lk;
  return g_q.enqueue(track);
}

void pause()  { Lock lk; g_q.pause(); }
void resume() { Lock lk; g_q.play(); }
void stop()   { Lock lk; g_q.stop(); }
bool next()   { Lock lk; return g_q.next(); }
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
