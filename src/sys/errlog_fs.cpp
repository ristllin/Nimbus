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

volatile bool g_suspended = false;   // factory erase in progress: drop durable writes
::fs::FS* g_fs = nullptr;            // the attached filesystem (nullptr = not ready yet)
std::atomic<uint32_t> g_seq{0};           // monotonic per-boot line sequence (any task)
std::atomic<uint32_t> g_durableSkipped{0};// TOTAL lines dropped from the DURABLE log under lock contention (reporting)
std::atomic<uint32_t> g_skipUnpersisted{0};// dropped lines not yet recorded to the durable log (CUM-407 marker delta)

// RAM fallback tail: a fixed byte ring so readRecent() still returns recent lines
// when the card/flash is unavailable (the exact moment the log matters most). Not the
// durable store - just a last-resort mirror. Guarded by a leaf spinlock (NOT the card
// lock) so a log call can record into it without ever touching the card, on any task.
// Static internal RAM is scarce on the S3, so this stays small.
constexpr size_t kRamTail = 2048;
char             g_ram[kRamTail];
size_t           g_ramTotal = 0;
portMUX_TYPE     g_ramMux = portMUX_INITIALIZER_UNLOCKED;

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

// Arduino filesystem adapter for the portable engine (errlog.h). The active log file
// is the one HELD handle: opened on attach, rotation, a tier flip, a remount or after
// a write error, and otherwise kept open across lines. Everything else (rotation
// renames, the small state files) opens and closes its own handle. All of it runs
// under the card lock.
struct ArduinoLogFs {
  ::File held;

  bool openAppend(const std::string& path) {
    if (!g_fs) return false;
    held = g_fs->open(path.c_str(), FILE_APPEND);
    return (bool)held;
  }
  long openSize() { return held ? (long)held.size() : -1; }
  // The per-line path: one write, then one flush (fflush + fsync, so the line is on
  // the card before a crash can take it) on the held handle. Neither may open a file
  // (test_errlog source guard): an open per line is what made every log line an
  // abort-capable internal allocation.
  size_t writeOpen(const char* data, size_t len) {
    if (!held) return 0;
    return held.write((const uint8_t*)data, len);
  }
  void flushOpen() {
    if (held) held.flush();
  }
  void closeOpen() {
    if (held) held.close();
    held = ::File();
  }
  void makeDir(const std::string& path)   { if (g_fs) g_fs->mkdir(path.c_str()); }
  void removeFile(const std::string& path){ if (g_fs) g_fs->remove(path.c_str()); }
  void renameFile(const std::string& from, const std::string& to) {
    if (g_fs) g_fs->rename(from.c_str(), to.c_str());
  }
  std::string readSmall(const std::string& path, size_t maxBytes) {
    if (!g_fs) return {};
    ::File f = g_fs->open(path.c_str(), FILE_READ);
    if (!f) return {};
    std::string out;
    uint8_t buf[64];
    while (out.size() < maxBytes) {
      const size_t want = maxBytes - out.size() < sizeof(buf) ? maxBytes - out.size() : sizeof(buf);
      const int n = f.read(buf, want);
      if (n <= 0) break;
      out.append((const char*)buf, (size_t)n);
    }
    f.close();
    return out;
  }
  bool writeSmall(const std::string& path, const std::string& data) {
    if (!g_fs) return false;
    ::File f = g_fs->open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    const size_t n = f.write((const uint8_t*)data.data(), data.size());
    f.close();
    return n == data.size();
  }
};

ArduinoLogFs g_adapter;

LineStamp nextStamp() { return LineStamp{g_seq.fetch_add(1), (uint32_t)millis()}; }

DurableLog<ArduinoLogFs> g_log(nextStamp);
void (*g_onAttached)() = nullptr;   // the crash reporter, run once after the first attach

// Follow a mid-run tier flip (card demoted / lost, or back). Only once the memory
// subsystem has resolved its tier: before that its haveSd() reads false on every boot,
// and following it would log a spurious "SD available" flip (the old seq 7 line).
// Caller holds the Lock.
void maybeRetier() {
  if (!g_log.attached() || !agent::memory::storageResolved()) return;
  const bool sd = agent::memory::haveSd();
  if (sd == g_log.onSd()) return;
  g_fs = sd ? &agent::memory::dataFs() : (::fs::FS*)&LittleFS;
  g_log.attach(&g_adapter, sd, millis());
}

// CUM-407: a durable write just succeeded, so record any lines that were dropped
// under card-lock contention since the last one. Bounded: one marker per success.
// If the marker itself does not land, put the count back so the next success
// retries it - a contention drop is never silently lost. Caller holds the Lock.
void recordContentionDrops() {
  if (g_skipUnpersisted.load() == 0) return;
  // Consume the delta and the seq number only when there is something to record, so
  // an ordinary write never leaves a seq gap that a reader would read as a drop.
  const uint32_t dropped = g_skipUnpersisted.exchange(0);
  const std::string mark = dropMarkerLine(g_seq.fetch_add(1), millis(), dropped);
  ramPush(mark);   // visible in the RAM tail too
  if (!g_log.writer().write(mark, millis())) g_skipUnpersisted.fetch_add(dropped);
}

}  // namespace

void attachStorage(::fs::FS& fs, bool isSd) {
  bool first = false;
  {
    agent::memory::Lock g;
    first = !g_log.attached();
    g_fs = &fs;
    g_log.attach(&g_adapter, isSd, millis());
  }
  if (first && g_onAttached) g_onAttached();
}

void onAttached(void (*fn)()) { g_onAttached = fn; }

bool storageAttached() { return g_log.attached(); }

void bootComplete() {
  agent::memory::Lock g;
  g_log.releaseBacklog();
}

void releaseHandle() {
  agent::memory::Lock g;
  g_log.writer().release();
}

uint32_t bootNumber() { return g_log.bootNo(); }

std::string readState(const char* name, size_t maxBytes) {
  agent::memory::Lock g;
  return g_log.readState(name, maxBytes);
}

bool writeState(const char* name, const std::string& data) {
  agent::memory::Lock g;
  return g_log.writeState(name, data);
}

void setSuspended(bool on) {
  g_suspended = on;
  if (on) releaseHandle();   // the sweep deletes /log: never hold its file open
}

void append(const std::string& redacted, const char* cat) {
  // Non-blocking phase: format + RAM tail, guarded only by a leaf spinlock. This part
  // never touches the card, so a log call is cheap even while the card is busy.
  std::string line = formatLine(g_seq.fetch_add(1), millis(), cat, redacted);
  ramPush(line);        // RAM tail always gets it (the readRecent() fallback + /api/log ring)

  // Factory-erase suspension (release-gate finding 2026-10-04): while the sweep
  // runs, a durable append would recreate the just-swept /log tree - including
  // the sweep's own progress lines - so a "factory fresh" board carried a log.
  // RAM tail + Serial keep every line; only the flash write is dropped.
  if (g_suspended) return;

  // The 2026-10-29 Lumi panic class (fw v4.6.1, tg_poll, the dream-end row):
  // every newlib stdio operation on a FILE takes the FILE's recursive lock, and
  // IDF newlib's lock_acquire_generic ABORTS when that acquire runs with the
  // CPU's interrupt level raised (its "recursive mutexes make no sense in ISR
  // context" abort - reached from task code when a critical section's unmask
  // leaked earlier on this task's stack, or from an exception frame). The
  // durable window between this line's last heap use and the FILE write is
  // exactly where that abort landed. Refuse the FILE touch in that context:
  // the line is already in the RAM tail + Serial, and the same drop counters
  // make the next successful write record the miss - never an abort.
  if (!xPortCanYield()) {
    g_durableSkipped.fetch_add(1);    // total, for reporting
    g_skipUnpersisted.fetch_add(1);   // delta, recorded to disk at the next successful write
    return;
  }

  // Durable phase: best-effort. Take the shared card lock only briefly; if the card is
  // contended (e.g. a big memory persist in flight) SKIP rather than stall the caller
  // (loop/AsyncTCP) and risk a WDT reset. The line is already captured in RAM + Serial.
  if (!agent::memory::tryLock(kDurableLockMs)) {
    g_durableSkipped.fetch_add(1);    // total, for reporting
    g_skipUnpersisted.fetch_add(1);   // delta, recorded to disk at the next successful write
    return;
  }
  // Given back on every path out: a turn logs, and a turn catches std::bad_alloc (L24),
  // so a failed allocation inside the write must not leave the card lock taken.
  struct Held {
    ~Held() { agent::memory::unlock(); }
  } held;
  maybeRetier();
  // Before storage attaches this queues the line in the early backlog; after, it is
  // one write + flush on the held handle (degrading to RAM only if the file is gone).
  if (g_log.append(line, millis()) && g_log.attached()) recordContentionDrops();
}

std::string readRecent(size_t maxBytes) {
  if (maxBytes == 0) return {};
  if (maxBytes > 32768) maxBytes = 32768;   // clamp: bound the PSRAM/heap scratch
  agent::memory::Lock g;
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
  std::string out = "{\"tier\":\"";
  out += !g_log.attached() ? "none" : (g_log.onSd() ? "sd" : "flash");
  out += "\",\"files\":[";
  const size_t maxFiles = capsFor(g_log.onSd()).maxFiles;
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
  out += std::to_string(g_durableSkipped.load());   // durable lines dropped under card contention (CUM-407)
  out += ",\"bytesWritten\":";
  out += std::to_string(g_log.writer().bytesWritten());   // bytes written to durable this boot (CUM-409 wear)
  out += '}';
  return out;
}

// Durable lines skipped under card-lock contention: a lock-free atomic (it is bumped
// from the tryLock-FAILED path, i.e. when the card lock is NOT held), so it is read
// without the lock.
uint32_t durableSkipped() { return g_durableSkipped.load(); }
// Bytes written to the durable log this boot: the single source of truth is the engine's
// own counter (host-tested), read under the card lock that serializes every write, so the
// value reported here is exactly the value the tests pin.
size_t   durableBytes()   { agent::memory::Lock g; return g_log.writer().bytesWritten(); }

bool onSdTier() {
  agent::memory::Lock g;
  return g_log.attached() && g_log.onSd();
}

::fs::FS& activeFs() {
  agent::memory::Lock g;
  return g_fs ? *g_fs : (::fs::FS&)LittleFS;
}

}  // namespace nimbus::errlog
