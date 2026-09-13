#include "errlog_fs.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include <atomic>

#include "errlog.h"
#include "../agent/memory_subsystem.h"   // dataFs()/haveSd() tier seam + tryLock (read-only use)

// Durable error-log filesystem sink. See errlog_fs.h and errlog.h for the design.
//
// Hot-path contract (CUM-401 ruling): a log call must NEVER block the caller (the
// WDT-guarded loop task, or an AsyncTCP handler) waiting on a slow/contended card.
// So append() formats + pushes to the RAM tail with only a leaf spinlock, then takes
// the shared card lock with a SHORT timed acquire; if the card is busy it SKIPS the
// durable write (the RAM ring + Serial still captured the line) and bumps a counter.
// The read/retrieval paths (readRecent/listJson/activeFs) are request-driven and keep
// the blocking Lock, like every other on-demand card read in this firmware.

namespace nimbus::errlog {
namespace {

// Longest the durable write may wait for the shared card lock before giving up. A few
// ms covers the common uncontended case; under a big concurrent memory persist we skip
// rather than stall the loop (best-effort durability is correct for a diagnostic log).
constexpr uint32_t kDurableLockMs = 8;

bool      g_inited    = false;
bool      g_haveSd    = false;  // tier for reporting (listJson/onSdTier); engine tracks its own
::fs::FS* g_fs        = nullptr;
bool      g_tierNoted = false;  // the storage-tier decision line has landed on disk
std::atomic<uint32_t> g_seq{0};           // monotonic per-boot line sequence (any task)
std::atomic<uint32_t> g_durableSkipped{0};// lines dropped from the DURABLE log under lock contention
std::string g_pendingTier;      // tier line built once, re-attempted until it lands (no seq churn)

// RAM fallback tail: a fixed byte ring so readRecent() still returns recent lines
// when the card/flash is unavailable (the exact moment the log matters most). Not the
// durable store - just a last-resort mirror. Guarded by a leaf spinlock (NOT the card
// lock) so a log call can record into it without ever touching the card, on any task.
// Static internal RAM is scarce on the S3, so this stays small.
constexpr size_t kRamTail = 2048;
char             g_ram[kRamTail];
size_t           g_ramTotal = 0;
portMUX_TYPE     g_ramMux = portMUX_INITIALIZER_UNLOCKED;

TierCaps caps() { return capsFor(g_haveSd); }

// Copy bytes into the RAM ring under the spinlock. Only byte copies here (no heap),
// so it is safe inside the no-heap critical section and never blocks on the card.
void ramPush(const std::string& line) {
  const char* p = line.c_str();
  portENTER_CRITICAL(&g_ramMux);
  for (; *p; ++p) { g_ram[g_ramTotal % kRamTail] = *p; ++g_ramTotal; }
  portEXIT_CRITICAL(&g_ramMux);
}

std::string ramTail() {
  // Snapshot under the spinlock into a stack buffer, then build the String OUTSIDE the
  // critical section (the no-heap-under-spinlock rule).
  static_assert(kRamTail <= 4096, "ramTail snapshot lives on the stack");
  char   snap[kRamTail];
  size_t n;
  portENTER_CRITICAL(&g_ramMux);
  n = g_ramTotal < kRamTail ? g_ramTotal : kRamTail;
  const size_t start = g_ramTotal - n;
  for (size_t i = 0; i < n; ++i) snap[i] = g_ram[(start + i) % kRamTail];
  portEXIT_CRITICAL(&g_ramMux);
  return tailOf(std::string(snap, n), kRamTail);   // line-align
}

// Read the last <= want bytes of a log file (empty if absent / FS down). Caller
// holds the Lock.
std::string readFileTail(const std::string& name, size_t want) {
  if (!g_fs || want == 0) return {};
  ::File f = g_fs->open(pathFor(name).c_str(), FILE_READ);
  if (!f) return {};
  const size_t size = f.size();
  const size_t off  = size > want ? size - want : 0;
  if (off) f.seek(off);
  std::string out;
  out.reserve(size - off);
  uint8_t buf[256];
  for (;;) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.append((const char*)buf, (size_t)n);
  }
  f.close();
  return out;
}

size_t fileBytes(const std::string& name) {
  if (!g_fs) return 0;
  ::File f = g_fs->open(pathFor(name).c_str(), FILE_READ);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

// Arduino filesystem adapter for the portable LogWriter engine. All card I/O goes
// through g_fs (SD or LittleFS); the engine holds the rotation + self-heal logic and
// is host-tested against a fake adapter (test_errlog).
struct ArduinoLogFs {
  long fileSize(const std::string& path) {
    if (!g_fs) return -1;
    ::File f = g_fs->open(path.c_str(), FILE_READ);
    if (!f) return -1;                    // absent or FS not mounted
    const long n = (long)f.size();
    f.close();
    return n;
  }
  size_t appendFile(const std::string& path, const char* data, size_t len) {
    if (!g_fs) return 0;
    ::File f = g_fs->open(path.c_str(), FILE_APPEND);
    if (!f) return 0;                     // dir missing / FS not mounted / full
    const size_t n = f.write((const uint8_t*)data, len);
    f.close();
    return n;
  }
  void makeDir(const std::string& path)   { if (g_fs) g_fs->mkdir(path.c_str()); }
  void removeFile(const std::string& path){ if (g_fs) g_fs->remove(path.c_str()); }
  void renameFile(const std::string& from, const std::string& to) {
    if (g_fs) g_fs->rename(from.c_str(), to.c_str());
  }
};

ArduinoLogFs          g_adapter;
LogWriter<ArduinoLogFs> g_writer;

std::string tierMsg() {
  return g_haveSd ? std::string("durable log on SD ") + kDir
                  : std::string("no SD: durable log on flash fallback ") + kDir;
}

// Ensure the storage-tier decision is the first durable line, once the FS is writable.
// The line is built ONCE (cached) and re-attempted every append until it lands, so a
// long not-yet-mounted window costs no sequence churn and no duplicate RAM lines.
void noteTierIfNeeded() {
  if (g_tierNoted) return;
  if (g_pendingTier.empty()) {
    g_pendingTier = formatLine(g_seq.fetch_add(1), millis(), cat::kStorage, tierMsg());
    ramPush(g_pendingTier);   // visible in the RAM tail immediately, even before it persists
  }
  if (g_writer.write(g_pendingTier)) { g_tierNoted = true; g_pendingTier.clear(); }
}

// Resolve the storage tier from the memory subsystem's canonical decision. Caller
// holds the Lock. Points the engine at the tier's FS (which re-syncs size on the
// next write and self-heals if the FS mounts later).
void resolveTier() {
  g_haveSd = agent::memory::haveSd();
  g_fs     = g_haveSd ? &agent::memory::dataFs() : (::fs::FS*)&LittleFS;
  if (g_fs) g_fs->mkdir(kDir);   // best-effort; the engine retries if the FS mounts later
  g_writer.setTier(&g_adapter, g_haveSd);
}

void ensureInit() {
  if (g_inited) return;
  resolveTier();
  g_inited = true;
}

// If the memory subsystem's tier flipped since we resolved (card mounted late, or
// SD lost mid-run), follow it so durability tracks the live store. Caller holds Lock.
void maybeRetier() {
  if (agent::memory::haveSd() == g_haveSd) return;
  const bool wasSd = g_haveSd;
  resolveTier();
  g_tierNoted = false;   // re-announce the tier on the new store
  g_pendingTier.clear();
  std::string line = formatLine(g_seq.fetch_add(1), millis(), cat::kStorage,
                       wasSd ? std::string("SD lost: durable log now on flash fallback")
                             : std::string("SD available: durable log now on SD"));
  ramPush(line);
  g_writer.write(line);
}

}  // namespace

void begin() {
  agent::memory::Lock g;
  ensureInit();
}

void append(const std::string& redacted, const char* cat) {
  // Non-blocking phase: format + RAM tail, guarded only by a leaf spinlock. This part
  // never touches the card, so a log call is cheap even while the card is busy.
  std::string line = formatLine(g_seq.fetch_add(1), millis(), cat, redacted);
  ramPush(line);        // RAM tail always gets it (the readRecent() fallback + /api/log ring)

  // Durable phase: best-effort. Take the shared card lock only briefly; if the card is
  // contended (e.g. a big memory persist in flight) SKIP rather than stall the caller
  // (loop/AsyncTCP) and risk a WDT reset. The line is already captured in RAM + Serial.
  if (!agent::memory::tryLock(kDurableLockMs)) {
    g_durableSkipped.fetch_add(1);
    return;
  }
  ensureInit();
  maybeRetier();
  noteTierIfNeeded();   // lands the tier line first once the FS is writable
  g_writer.write(line); // one bounded append; degrades to RAM-only if the FS is not ready
  agent::memory::unlock();
}

std::string readRecent(size_t maxBytes) {
  if (maxBytes == 0) return {};
  if (maxBytes > 32768) maxBytes = 32768;   // clamp: bound the PSRAM/heap scratch
  agent::memory::Lock g;
  ensureInit();
  std::string cur = readFileTail(kBaseName, maxBytes);
  std::string out;
  if (cur.size() < maxBytes) {
    std::string prev = readFileTail(rotatedName(1), maxBytes - cur.size());
    out = prev + cur;
  } else {
    out = cur;
  }
  if (out.empty()) return ramTail();   // FS unavailable / nothing durable yet
  return tailOf(out, maxBytes);
}

std::string listJson() {
  agent::memory::Lock g;
  ensureInit();
  std::string out = "{\"tier\":\"";
  out += g_haveSd ? "sd" : "flash";
  out += "\",\"files\":[";
  const size_t maxFiles = caps().maxFiles;
  bool first = true;
  for (size_t i = 0; i < maxFiles; ++i) {
    const std::string name = rotatedName(i);   // i==0 -> active (kBaseName)
    const size_t bytes = fileBytes(name);
    if (bytes == 0 && i != 0) continue;         // skip rotated files that don't exist
    if (!first) out += ',';
    first = false;
    out += "{\"name\":\"";
    out += name;
    out += "\",\"bytes\":";
    out += std::to_string(bytes);
    out += '}';
  }
  out += "],\"skipped\":";
  out += std::to_string(g_durableSkipped.load());   // durable lines dropped under card contention
  out += '}';
  return out;
}

bool onSdTier() {
  agent::memory::Lock g;
  ensureInit();
  return g_haveSd;
}

::fs::FS& activeFs() {
  agent::memory::Lock g;
  ensureInit();
  return *g_fs;
}

}  // namespace nimbus::errlog
