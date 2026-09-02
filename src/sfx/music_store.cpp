#include "music_store.h"

#include <FS.h>
#include <solide/storage.h>   // activeFs(): the mounted card (SD_MMC on Freenove, SD on solide)

#include "../agent/memory_subsystem.h"   // memory::Lock (shared SD bus), haveSd()
#include "../sys/agent_log.h"
#include "music.h"                       // stopIfCurrent - fence the player off a file we replace/delete
#include "nimbus/orch/capture.h"         // captureFitsCap - overflow-safe cap check (shared with the sandbox path)
#include "nimbus/orch/media.h"           // musicUploadAllowed - the shared gate

namespace music {
namespace store {

namespace {

constexpr const char* kDir      = "/music";
constexpr const char* kPartPath = "/music/.part";   // single upload session scratch

// Bound a single track so a runaway/hostile upload can't fill the card. Generous
// enough for a multi-minute WAV (44.1 kHz stereo ~10 MB/min) but finite.
constexpr size_t kMaxMusicBytes = 64u * 1024 * 1024;

struct WriteSession {
  bool        active = false;
  uint32_t    gen    = 0;      // monotonic id of THIS session (0 = none)
  size_t      written = 0;
  std::string name;
  File        f;
};

WriteSession g_w;
uint32_t     g_gen = 0;        // never reused; identifies which session a stale
                              // onDisconnect belonged to

std::string fullPath(const std::string& name) { return std::string(kDir) + "/" + name; }

}  // namespace

uint32_t uploadGen() { agent::memory::Lock g; return g_w.gen; }

bool uploadBegin(const std::string& name, std::string& err) {
  agent::memory::Lock g;
  // The portable gate is the single source of truth for "is this an acceptable
  // track" - the same check the player and /play use, so the two can't drift.
  if (!nimbus::orch::musicUploadAllowed(name.c_str(), agent::memory::haveSd(), err))
    return false;
  if (g_w.active) { err = "Another upload is in progress. Try again in a moment."; return false; }
  fs::FS& fs = solide::storage::activeFs();
  fs.mkdir(kDir);
  fs.remove(kPartPath);            // clear a partial stranded by a crash
  g_w.f = fs.open(kPartPath, FILE_WRITE);
  if (!g_w.f) { err = "Couldn't open the card to write. Try again."; return false; }
  g_w.active  = true;
  g_w.gen     = ++g_gen;
  g_w.name    = name;
  g_w.written = 0;
  return true;
}

bool uploadChunk(const uint8_t* data, size_t len) {
  agent::memory::Lock g;   // SHORT hold: one SD write, serialized with the player + files
  if (!g_w.active || !g_w.f) return false;
  if (!nimbus::orch::captureFitsCap(g_w.written, len, kMaxMusicBytes)) return false;   // overflow-safe hard cap
  if (g_w.f.write(data, len) != len) return false;
  g_w.written += len;
  return true;
}

bool uploadFinish(bool ok, std::string& err) {
  agent::memory::Lock g;
  if (!g_w.active) { err = "No upload in progress."; return false; }
  g_w.f.close();
  fs::FS& fs = solide::storage::activeFs();
  if (!ok) { fs.remove(kPartPath); g_w = WriteSession{}; err = "Upload canceled."; return false; }
  if (g_w.written == 0) { fs.remove(kPartPath); g_w = WriteSession{}; err = "That file was empty."; return false; }
  const std::string dst = fullPath(g_w.name);
  music::stopIfCurrent(g_w.name);               // don't replace a track out from under the player
  fs.remove(dst.c_str());                       // overwrite an existing track of the same name
  if (!fs.rename(kPartPath, dst.c_str())) {
    fs.remove(kPartPath); g_w = WriteSession{};
    err = "Couldn't save the track. Try again.";
    return false;
  }
  agent::alogf("music: stored %s (%u B)", dst.c_str(), (unsigned)g_w.written);
  g_w = WriteSession{};
  return true;
}

void uploadAbort() {
  agent::memory::Lock g;
  if (!g_w.active) return;
  g_w.f.close();
  solide::storage::activeFs().remove(kPartPath);
  g_w = WriteSession{};
}

void uploadAbortGen(uint32_t gen) {
  agent::memory::Lock g;
  if (!g_w.active || g_w.gen != gen) return;   // a stale disconnect must not nuke a newer session
  g_w.f.close();
  solide::storage::activeFs().remove(kPartPath);
  g_w = WriteSession{};
}

bool removeTrack(const std::string& name, std::string& err) {
  agent::memory::Lock g;
  if (!agent::memory::haveSd()) { err = "No SD card."; return false; }
  if (!nimbus::orch::validMusicName(name.c_str())) { err = "That is not a valid track name."; return false; }
  fs::FS& fs = solide::storage::activeFs();
  const std::string path = fullPath(name);
  if (!fs.exists(path.c_str())) { err = "No such track."; return false; }
  music::stopIfCurrent(name);                    // don't delete a track out from under the player
  if (!fs.remove(path.c_str())) { err = "Couldn't delete the track. Try again."; return false; }
  agent::alogf("music: removed %s", path.c_str());
  return true;
}

}  // namespace store
}  // namespace music
