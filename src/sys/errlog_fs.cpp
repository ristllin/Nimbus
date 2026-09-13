#include "errlog_fs.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include "errlog.h"
#include "../agent/memory_subsystem.h"   // dataFs()/haveSd() tier seam + Lock (read-only use)

// Durable error-log filesystem sink. See errlog_fs.h and errlog.h for the design.
// Every entry point takes agent::memory::Lock (recursive) so card I/O is serialized
// with the memory subsystem and every other task; no handle is held across a send.

namespace nimbus::errlog {
namespace {

bool      g_inited  = false;
bool      g_haveSd  = false;
::fs::FS* g_fs      = nullptr;
size_t    g_curSize = 0;      // bytes in the active file (tracked in RAM, seeded on init)
uint32_t  g_seq     = 0;      // monotonic per-boot line sequence

// RAM fallback tail: a fixed byte ring so readRecent() still returns recent lines
// when the card/flash is unavailable (the exact moment the log matters most). Not
// the durable store - just a last-resort mirror. Static internal RAM is scarce on
// the S3, so this stays small.
constexpr size_t kRamTail = 2048;
char             g_ram[kRamTail];
size_t           g_ramTotal = 0;

TierCaps caps() { return capsFor(g_haveSd); }

void ramPush(const std::string& line) {
  for (char c : line) { g_ram[g_ramTotal % kRamTail] = c; ++g_ramTotal; }
}

std::string ramTail() {
  const size_t n     = g_ramTotal < kRamTail ? g_ramTotal : kRamTail;
  const size_t start = g_ramTotal - n;
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) out.push_back(g_ram[(start + i) % kRamTail]);
  return tailOf(out, kRamTail);   // line-align
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

// Shift files down and clear the active file: base -> .1 -> .2 ... dropping the
// oldest. Caller holds the Lock.
void rotate() {
  if (!g_fs) return;
  const size_t maxFiles = caps().maxFiles;
  g_fs->remove(pathFor(rotatedName(maxFiles - 1)).c_str());   // drop oldest (no-op if absent)
  for (size_t i = maxFiles - 1; i >= 2; --i)
    g_fs->rename(pathFor(rotatedName(i - 1)).c_str(), pathFor(rotatedName(i)).c_str());
  g_fs->rename(pathFor(kBaseName).c_str(), pathFor(rotatedName(1)).c_str());
  g_curSize = 0;
}

// Append a fully-formatted line (already ending in '\n') to FS + the RAM tail.
// Caller holds the Lock and has run ensureInit(). FS failure degrades to RAM only.
void writeToFs(const std::string& line) {
  ramPush(line);
  if (!g_fs) return;
  if (shouldRotate(g_curSize, line.size(), caps())) rotate();
  ::File f = g_fs->open(pathFor(kBaseName).c_str(), FILE_APPEND);
  if (!f) return;   // FS full/unmounted: RAM tail already has it; never crash the logger
  const size_t n = f.write((const uint8_t*)line.data(), line.size());
  f.close();
  g_curSize += n;
}

// Resolve the storage tier from the memory subsystem's canonical decision. Caller
// holds the Lock. Re-reads the active file size for the new tier.
void resolveTier() {
  g_haveSd = agent::memory::haveSd();
  g_fs     = g_haveSd ? &agent::memory::dataFs() : (::fs::FS*)&LittleFS;
  if (g_fs) g_fs->mkdir(kDir);
  g_curSize = fileBytes(kBaseName);
}

void ensureInit() {
  if (g_inited) return;
  resolveTier();
  g_inited = true;
  // Record the tier decision directly (not via alog) to avoid re-entering append()
  // through the shared log seam. This is the "storage-tier decision" CUM-401 wants
  // captured, and it ties into the CUM-405 SD-absent story.
  writeToFs(formatLine(g_seq++, millis(), cat::kStorage,
                       g_haveSd ? std::string("durable log on SD ") + kDir
                                : std::string("no SD: durable log on flash fallback ") + kDir));
}

// If the memory subsystem's tier flipped since we resolved (card mounted late, or
// SD lost mid-run), follow it so durability tracks the live store. Caller holds Lock.
void maybeRetier() {
  if (agent::memory::haveSd() == g_haveSd) return;
  const bool wasSd = g_haveSd;
  resolveTier();
  writeToFs(formatLine(g_seq++, millis(), cat::kStorage,
                       wasSd ? std::string("SD lost: durable log now on flash fallback")
                             : std::string("SD available: durable log now on SD")));
}

}  // namespace

void begin() {
  agent::memory::Lock g;
  ensureInit();
}

void append(const std::string& redacted, const char* cat) {
  agent::memory::Lock g;
  ensureInit();
  maybeRetier();
  writeToFs(formatLine(g_seq++, millis(), cat, redacted));
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
  out += "]}";
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
