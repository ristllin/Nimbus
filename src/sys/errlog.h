#pragma once
// errlog (portable policy) - the durable, verbose, rotating error/event log that
// rides beside the RAM agent-log ring (src/sys/agent_log.h). This header is
// deliberately Arduino-free so the exact policy the device runs is host-tested
// under `pio test -e native` (test/test_errlog). The filesystem glue - the actual
// SD/LittleFS open/append/rotate - lives in src/sys/errlog_fs.{h,cpp}, which is
// the ONLY part that pulls in Arduino.
//
// Redaction: lines reach the durable sink ALREADY redacted. alog()/alogf() feed
// their Serial writer, RAM ring AND this sink through the single core::emitRedacted
// choke point, so whatever the durable log stores is the identical masked string
// the ring stores - there is no second redaction path that could leak a key. This
// header therefore never redacts; it only sanitizes for the on-disk line format
// (control-char stripping + a length cap), never re-introducing a secret.
//
// Layering: portable logic only (std:: types). No Arduino, no FS, no I/O.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/sys/boot_record.h"   // stampBootNo + the boot counter codec

namespace nimbus::errlog {

// ---- on-disk sizing / rotation policy --------------------------------------
//
// Two tiers. The SD tier is generous (a card has gigabytes); the flash fallback
// is deliberately small so the durable log can NEVER fill the LittleFS partition
// that also holds config, voice clips and the degraded /data memory store. Total
// bytes on disk are bounded by maxFileBytes * (maxFiles) in either tier.
struct TierCaps {
  size_t maxFileBytes;   // rotate the active file once it would exceed this
  size_t maxFiles;       // retain this many files total (active + rotated)
};

// SD present: 256 KB per file x 4 files = up to 1 MB of history. Flash fallback:
// 24 KB per file x 2 files = up to 48 KB, small enough to be a rounding error
// against the LittleFS partition yet still hold thousands of recent event lines.
inline constexpr TierCaps kSdCaps    = {262144, 4};
inline constexpr TierCaps kFlashCaps = {24576, 2};

inline TierCaps capsFor(bool haveSd) { return haveSd ? kSdCaps : kFlashCaps; }

// The single per-line cap. A pathological log call (e.g. a whole provider error
// body) is truncated so one line cannot blow past a rotation boundary or a reader
// buffer. Matches alogf()'s own 256-byte format buffer with headroom for the
// timestamp/sequence prefix.
inline constexpr size_t kMaxLineBytes = 320;

// ---- severity level + durable-persistence policy (CUM-409) ------------------
//
// Every log line carries a severity. Durable (on-disk) persistence is scoped to
// warn/error and to category-tagged key events; a routine info line stays in the
// RAM ring + Serial only, to bound internal-flash write wear over the fleet
// lifetime (CUM-401 wrote EVERY line to the durable FS on the caller task, adding
// per-line flash writes and GC/wear-levelling stalls for info-level chatter). The
// predicate is portable so the routing is host-tested (test_errlog).
enum class Level : uint8_t { Info = 0, Warn = 1, Error = 2 };

// True when a line at `lvl` tagged `cat` should be written to the durable log.
// Warn/Error always persist; a non-empty category tag marks a key event that
// persists even at info level; a plain info line does not (the RAM ring + Serial
// keep the full verbose stream).
inline bool persistsDurably(Level lvl, const char* cat) {
  return lvl != Level::Info || (cat && cat[0]);
}

// ---- rotated-file naming ----------------------------------------------------
//
// The active file is always kBaseName. On rotation it becomes ".1", the old ".1"
// becomes ".2", and so on; the oldest (index == maxFiles-1) is dropped. Readers
// enumerate index 0 (newest rotated) .. maxFiles-2 plus the active file.
inline constexpr char kDir[]      = "/log";
inline constexpr char kBaseName[] = "nimbus.log";

// Name of the rotated file at `index` (1-based: 1 == most-recently rotated).
// index 0 is reserved for the active file (kBaseName) and returns kBaseName.
inline std::string rotatedName(size_t index) {
  if (index == 0) return kBaseName;
  return std::string(kBaseName) + "." + std::to_string(index);
}

// Full "/log/<name>" path for a bare log file name.
inline std::string pathFor(const std::string& name) {
  return std::string(kDir) + "/" + name;
}

// True when adding `incomingLen` bytes to a file already `curSize` bytes long
// would exceed the tier's per-file cap AND the file already holds something (so a
// single over-long line - already truncated to kMaxLineBytes - never triggers an
// infinite rotate on an empty file).
inline bool shouldRotate(size_t curSize, size_t incomingLen, const TierCaps& caps) {
  if (curSize == 0) return false;
  return curSize + incomingLen > caps.maxFileBytes;
}

// ---- line formatting --------------------------------------------------------
//
// A durable line is:   "<seq> <ms> <cat> <message>\n"
// seq: monotonic per-boot counter (so a reader can spot dropped/rotated spans).
// ms:  millis() at write. cat: a short lowercase class tag (see Cat below) or "".
// message: the ALREADY-redacted text, control chars stripped, whole line capped.
//
// sanitizeInline() strips CR/LF/other control bytes to a single space so one log
// call can never inject extra lines into the file (log-forging) or smuggle a
// terminal escape into a reader. It does NOT alter the visible content otherwise.
inline std::string sanitizeInline(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (c == '\n' || c == '\r' || c == '\t') { out.push_back(' '); continue; }
    if (c < 0x20 || c == 0x7f) { out.push_back(' '); continue; }
    out.push_back((char)c);
  }
  return out;
}

// Build the on-disk line (including the trailing '\n'). The whole line is capped
// at kMaxLineBytes; if the prefix alone were somehow that long the message is
// dropped but the newline is always kept so the file stays line-structured.
inline std::string formatLine(uint32_t seq, uint32_t ms, const char* cat,
                              const std::string& redactedMsg) {
  std::string line = std::to_string(seq);
  line += ' ';
  line += std::to_string(ms);
  line += ' ';
  line += (cat && cat[0]) ? cat : "-";
  line += ' ';
  line += sanitizeInline(redactedMsg);
  if (line.size() > kMaxLineBytes - 1) line.resize(kMaxLineBytes - 1);
  line += '\n';
  return line;
}

// ---- line-aligned tail (agent-read seam + retrieval helper) -----------------
//
// Return the last <= maxBytes of `content`, trimmed forward to the next line
// boundary so the result never starts mid-line. Empty content -> empty string.
inline std::string tailOf(const std::string& content, size_t maxBytes) {
  if (content.size() <= maxBytes) return content;
  size_t start = content.size() - maxBytes;
  // Advance to just past the next newline so we begin on a whole line.
  size_t nl = content.find('\n', start);
  if (nl != std::string::npos && nl + 1 < content.size()) start = nl + 1;
  return content.substr(start);
}

// ---- retrieval-request planning (path-traversal safe) -----------------------
//
// The HTTP retrieval route (src/net/errlog_routes) turns query params into one of
// these actions. All name validation lives HERE so it is host-tested: a client can
// only ever address a file whose name is exactly one of our known log names, so
// "?file=../secrets" or an absolute path can never escape /log.
enum class RetrievalKind { List, File, Current, Reject };

struct RetrievalPlan {
  RetrievalKind kind;
  std::string   file;    // for File: the validated bare name (no dir)
  std::string   reason;  // for Reject: a short machine reason
};

// True when `name` is a well-formed bare log file name we produce (kBaseName or
// "nimbus.log.<n>"), with no directory separators and no traversal.
inline bool isKnownLogName(const std::string& name, size_t maxFiles) {
  if (name.empty()) return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find("..") != std::string::npos) return false;
  if (name == kBaseName) return true;
  const std::string prefix = std::string(kBaseName) + ".";
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  const std::string idx = name.substr(prefix.size());
  if (idx.empty() || idx.size() > 3) return false;
  for (char c : idx) if (c < '0' || c > '9') return false;
  size_t n = 0;
  for (char c : idx) n = n * 10 + (size_t)(c - '0');
  return n >= 1 && n <= maxFiles - 1;   // 1..maxFiles-1 are the rotated files
}

// Plan a retrieval from the parsed query. `wantList` = the ?list param present;
// `fileParam` = the ?file value ("" if absent). maxFiles bounds valid indices.
inline RetrievalPlan planRetrieval(bool wantList, const std::string& fileParam,
                                   size_t maxFiles) {
  if (wantList) return {RetrievalKind::List, "", ""};
  if (fileParam.empty()) return {RetrievalKind::Current, "", ""};
  if (!isKnownLogName(fileParam, maxFiles))
    return {RetrievalKind::Reject, "", "bad-name"};
  return {RetrievalKind::File, fileParam, ""};
}

// ---- class tags for the "classes we keep missing" ---------------------------
//
// Short, stable, lowercase machine tags (frozen strings: a retrieval reader/grep
// keys off them). Callers pass one to alogc()/alogcf() (agent_log.h). These name
// the failure classes CUM-401 was filed to stop losing.
namespace cat {
inline constexpr char kMem[]     = "mem";      // memory pressure / low heap
inline constexpr char kProvider[]= "provider"; // provider selection + fallbacks
inline constexpr char kRelay[]   = "relay";    // relay / pairing errors
inline constexpr char kOta[]     = "ota";      // OTA / update
inline constexpr char kPanel[]   = "panel";    // panel / touch init
inline constexpr char kNvs[]     = "nvs";      // NVS-full / persistence
inline constexpr char kStorage[] = "storage";  // storage-tier decision (SD absent)
inline constexpr char kNet[]     = "net";      // wifi / network
inline constexpr char kBoot[]    = "boot";     // boot / lifecycle
}  // namespace cat

// CUM-407: the bounded marker line that records how many durable lines were skipped
// under card-lock contention since the last successful write. Returns empty when none
// were dropped (so no marker is emitted). errlog_fs writes it at the next successful
// durable write, so a drop is never a silent loss - it survives reboot in the log
// itself, bounded to at most one marker per successful write. The two drop reasons
// share the one marker: card-lock contention, and the FILE-write context guard (the
// 2026-10-29 Lumi panic class - a write refused at a raised CPU interrupt level
// would otherwise abort inside newlib's recursive FILE lock).
inline std::string dropMarkerLine(uint32_t seq, uint32_t ms, uint32_t dropped) {
  if (dropped == 0) return {};
  return formatLine(seq, ms, cat::kStorage,
                    std::string("durable log dropped ") + std::to_string(dropped) +
                        " line(s) under card-lock contention or an unsafe write context (best-effort)");
}

// ---- the rotating write engine: one held handle -------------------------------
//
// Templated on a filesystem adapter so the EXACT rotation, reopen and degrade logic
// runs unchanged on device (Arduino File) and is host-tested (test_errlog) against a
// fake FS.
//
// One open handle (panic F1.1). The engine keeps the active file open and each line
// is one write plus one flush (fflush + fsync, so a line written right before a panic
// is on the card). It reopens only on rotation, a tier change, a remount, or a write
// error. The old engine opened and closed the file for every line; each open makes
// newlib create a FreeRTOS mutex from internal RAM, and IDF aborts when that fails, so
// the log could turn an out-of-memory moment into a panic through its own line. If an
// open fails the engine degrades to RAM only (the caller's RAM tail keeps every line)
// and retries at most once per kReopenRetryMs - never once per line - and the lines
// it could not write are counted so a marker can say how many were missed.
//
// FsT must provide (std::string paths, bytes):
//   bool   openAppend(const std::string& path);    // open/create for append and HOLD it
//   long   openSize();                              // bytes in the held file (-1 if none)
//   size_t writeOpen(const char* d, size_t n);      // write to the held file; bytes written
//   void   flushOpen();                             // flush + fsync the held file
//   void   closeOpen();                             // close the held file (no-op if none)
//   void   makeDir(const std::string& path);
//   void   removeFile(const std::string& path);
//   void   renameFile(const std::string& from, const std::string& to);
//   std::string readSmall(const std::string& path, size_t maxBytes);   // "" if absent
//   bool   writeSmall(const std::string& path, const std::string& data); // replace the file
template <class FsT>
class LogWriter {
 public:
  static constexpr uint32_t kReopenRetryMs = 10000;

  // Point the engine at a filesystem for the given tier. Closes any held handle; the
  // next write opens the active file there and reads its real size.
  void setTier(FsT* fs, bool haveSd) {
    closeHandle();
    fs_ = fs;
    haveSd_ = haveSd;
    retryArmed_ = false;
  }

  // The storage under the held handle is about to go away or just came back (an
  // unmount, a remount, a factory sweep): drop the handle. The next write reopens.
  void release() {
    closeHandle();
    retryArmed_ = false;
  }

  bool haveSd() const { return haveSd_; }
  bool isOpen() const { return open_; }
  TierCaps caps() const { return capsFor(haveSd_); }

  // Append one already-formatted line (ending in '\n'). Returns true iff it reached
  // the file. On false the caller keeps the line in its RAM tail; the line is counted
  // as missed unless `countMiss` is false (a caller that keeps and retries it).
  bool write(const std::string& line, uint32_t nowMs, bool countMiss = true) {
    if (!fs_) return false;
    const uint32_t miss = countMiss ? 1 : 0;
    if (!open_ && !openOrDegrade(nowMs)) { unwritten_ += miss; return false; }
    const TierCaps c = capsFor(haveSd_);
    if (shouldRotate(curSize_, line.size(), c) && !rotate(c, nowMs)) { unwritten_ += miss; return false; }
    size_t w = fs_->writeOpen(line.data(), line.size());
    if (w != line.size()) {
      // A write error (card pulled, stale handle after a remount): one reopen, one
      // retry. If that fails too, degrade until the retry window passes.
      closeHandle();
      ++errors_;
      w = (openOrDegrade(nowMs)) ? fs_->writeOpen(line.data(), line.size()) : 0;
      if (w != line.size()) {
        closeHandle();
        arm(nowMs);
        unwritten_ += miss;
        return false;
      }
    }
    if (!batching_) fs_->flushOpen();   // on the card before the call returns
    curSize_ += w;
    bytesWritten_ += w;   // flash-wear watch (CUM-409)
    return true;
  }

  // Write several lines with one flush at the end (the boot backlog: ~10 lines at the
  // SD mount, where one fsync per line would add tens of ms to the boot).
  void beginBatch() { batching_ = true; }
  void endBatch() {
    batching_ = false;
    if (open_) fs_->flushOpen();
  }

  // Lines that could not be written since the last call (the degraded window).
  uint32_t takeUnwritten() {
    const uint32_t n = unwritten_;
    unwritten_ = 0;
    return n;
  }
  // Put a taken count back (its marker did not land), so the next success retries it.
  void restoreUnwritten(uint32_t n) { unwritten_ += n; }

  size_t bytesWritten() const { return bytesWritten_; }   // CUM-409 wear counter
  uint32_t opens() const { return opens_; }               // successful openAppend calls
  uint32_t openAttempts() const { return openAttempts_; }
  uint32_t rotations() const { return rotations_; }
  uint32_t errors() const { return errors_; }

 private:
  void closeHandle() {
    if (open_ && fs_) fs_->closeOpen();
    open_ = false;
  }
  void arm(uint32_t nowMs) {
    retryArmed_ = true;
    failedAtMs_ = nowMs;
  }

  // Open the active file unless a recent failure says to wait. The directory is made
  // first (neither FS creates /log on its own; a fresh device has none).
  bool openOrDegrade(uint32_t nowMs) {
    if (retryArmed_ && (uint32_t)(nowMs - failedAtMs_) < kReopenRetryMs) return false;
    ++openAttempts_;
    fs_->makeDir(kDir);
    if (!fs_->openAppend(pathFor(kBaseName))) {
      arm(nowMs);
      return false;
    }
    retryArmed_ = false;
    open_ = true;
    ++opens_;
    const long sz = fs_->openSize();
    curSize_ = sz > 0 ? (size_t)sz : 0;
    return true;
  }

  // Shift files down and clear the active file: base -> .1 -> .2 ..., dropping the
  // oldest (index maxFiles-1). Bounds total on-disk bytes to maxFileBytes*maxFiles.
  bool rotate(const TierCaps& c, uint32_t nowMs) {
    closeHandle();
    fs_->removeFile(pathFor(rotatedName(c.maxFiles - 1)));   // drop oldest (no-op if absent)
    for (size_t i = c.maxFiles - 1; i >= 2; --i)
      fs_->renameFile(pathFor(rotatedName(i - 1)), pathFor(rotatedName(i)));
    fs_->renameFile(pathFor(kBaseName), pathFor(rotatedName(1)));
    ++rotations_;
    retryArmed_ = false;   // a rotation is a fresh attempt
    return openOrDegrade(nowMs);
  }

  FsT* fs_ = nullptr;
  bool haveSd_ = false;
  bool open_ = false;
  bool retryArmed_ = false;
  bool batching_ = false;
  uint32_t failedAtMs_ = 0;
  size_t curSize_ = 0;
  size_t bytesWritten_ = 0;
  uint32_t unwritten_ = 0;
  uint32_t opens_ = 0, openAttempts_ = 0, rotations_ = 0, errors_ = 0;
};

// ---- early backlog: no pre-mount hole (crash forensics F1) ------------------
//
// The first log line of a boot (the boot record) is emitted at the top of setup(),
// about a second before any filesystem is mounted. Those lines used to be dropped:
// every boot lost its first six durable lines, including the reset reason and an
// "[sd] mount FAILED" line. They now wait here, bounded by line count and bytes, and
// are written in order when storage attaches. Overflow is counted and reported by
// one marker line. The memory is freed once the backlog is flushed or released, so
// nothing stays resident after boot.
class EarlyBacklog {
 public:
  static constexpr size_t kMaxLines = 24;
  static constexpr size_t kMaxBytes = 4096;

  // Queue a line. False when it was dropped (counted) or the backlog is closed.
  bool push(const std::string& line) {
    if (closed_) return false;
    if (lines_.size() >= kMaxLines || bytes_ + line.size() > kMaxBytes) {
      ++dropped_;
      return false;
    }
    bytes_ += line.size();
    lines_.push_back(line);
    return true;
  }

  bool closed() const { return closed_; }
  bool empty() const { return lines_.empty() && dropped_ == 0; }
  size_t size() const { return lines_.size(); }
  size_t bytes() const { return bytes_; }
  uint32_t dropped() const { return dropped_; }
  const std::vector<std::string>& lines() const { return lines_; }

  // Write the queued lines in order (each passed through `fix` first), then the drop
  // marker. Stops at the first failed write and keeps the rest for the next attempt.
  // Returns true when everything landed; the backlog is then freed and closed.
  template <class WriteFn, class FixFn, class MarkerFn>
  bool flush(WriteFn write, FixFn fix, MarkerFn marker) {
    while (next_ < lines_.size()) {
      std::string& l = lines_[next_];
      fix(l);
      if (!write(l)) return false;
      ++next_;
    }
    if (dropped_ && !write(marker(dropped_))) return false;
    release();
    return true;
  }

  // Free and close without writing (storage never came this boot).
  void release() {
    std::vector<std::string>().swap(lines_);
    bytes_ = 0;
    next_ = 0;
    dropped_ = 0;
    closed_ = true;
  }

 private:
  std::vector<std::string> lines_;
  size_t bytes_ = 0;
  size_t next_ = 0;
  uint32_t dropped_ = 0;
  bool closed_ = false;
};

// The marker written after the backlog when it overflowed.
inline std::string earlyDropMarkerLine(uint32_t seq, uint32_t ms, uint32_t dropped) {
  return formatLine(seq, ms, cat::kStorage,
                    std::to_string(dropped) + " early line(s) dropped before storage was ready");
}

// The marker written when the log file came back after a failed open.
inline std::string missedMarkerLine(uint32_t seq, uint32_t ms, uint32_t missed) {
  return formatLine(seq, ms, cat::kStorage,
                    std::string("durable log missed ") + std::to_string(missed) +
                        " line(s) while the log file could not be opened");
}

// The boot counter and the crash dedupe state live beside the log.
inline constexpr char kBootCounterName[] = "boot.n";

struct LineStamp {
  uint32_t seq;
  uint32_t ms;
};

// ---- the durable log: backlog, attach, tier flips ----------------------------
//
// The tier is "not ready" until the device attaches a mounted filesystem: the SD card
// right after its mount, or LittleFS right after memory::begin() mounts it. Before
// that, lines queue in the EarlyBacklog (never written to an unmounted FS). The first
// attach reads and bumps the boot counter (/log/boot.n), stamps it into the queued
// boot line, flushes the backlog in order, then writes the tier line. A later attach
// to the other tier is a real mid-run flip (card lost or back) and writes one flip
// line. Following the memory subsystem's tier is the caller's job, and only once that
// subsystem has resolved it, so a normal boot never prints a spurious flip.
template <class FsT>
class DurableLog {
 public:
  using Stamp = LineStamp (*)();
  explicit DurableLog(Stamp stamp) : stamp_(stamp) {}

  bool attached() const { return attached_; }
  bool onSd() const { return writer_.haveSd(); }
  uint32_t bootNo() const { return bootNo_; }
  LogWriter<FsT>& writer() { return writer_; }
  const EarlyBacklog& backlog() const { return backlog_; }

  // Queue (before attach) or write (after). True when the line reached storage or
  // was queued for it.
  bool append(const std::string& line, uint32_t nowMs) {
    if (!attached_) return backlog_.push(line);
    if (!backlog_.closed()) flushBacklog(nowMs);
    const bool ok = writer_.write(line, nowMs);
    if (ok) noteMissed(nowMs);
    return ok;
  }

  // Attach a mounted filesystem. The first call is the boot attach; a later call with
  // the other tier is a flip. Re-attaching the same tier only releases the handle (a
  // remount), so a normal boot never logs a flip.
  void attach(FsT* fs, bool isSd, uint32_t nowMs) {
    if (!fs) return;
    if (!attached_) {
      attached_ = true;
      fs_ = fs;
      writer_.setTier(fs, isSd);
      bumpBootCounter();
      flushBacklog(nowMs);
      writeStamped(isSd ? std::string("durable log on SD ") + kDir
                        : std::string("no SD: durable log on flash fallback ") + kDir,
                   nowMs);
      return;
    }
    if (fs == fs_ && isSd == writer_.haveSd()) {
      writer_.release();
      return;
    }
    const bool wasSd = writer_.haveSd();
    fs_ = fs;
    writer_.setTier(fs, isSd);
    if (wasSd != isSd)
      writeStamped(wasSd ? "SD lost: durable log now on flash fallback"
                         : "SD available: durable log now on SD",
                   nowMs);
  }

  // Storage never came this boot (Notifier mode with no card): free the backlog.
  void releaseBacklog() {
    if (!attached_) backlog_.release();
  }

  // Small state files beside the log (/log/<name>). "" / false before attach.
  std::string readState(const std::string& name, size_t maxBytes) {
    return fs_ ? fs_->readSmall(pathFor(name), maxBytes) : std::string();
  }
  bool writeState(const std::string& name, const std::string& data) {
    if (!fs_) return false;
    fs_->makeDir(kDir);
    return fs_->writeSmall(pathFor(name), data);
  }

 private:
  void bumpBootCounter() {
    fs_->makeDir(kDir);
    uint32_t n = nimbus::sys::parseBootCounter(fs_->readSmall(pathFor(kBootCounterName), 32)) + 1;
    if (n == 0) n = 1;   // wrapped
    bootNo_ = fs_->writeSmall(pathFor(kBootCounterName), nimbus::sys::renderBootCounter(n)) ? n : 0;
  }

  void flushBacklog(uint32_t nowMs) {
    const uint32_t n = bootNo_;
    writer_.beginBatch();
    backlog_.flush([&](const std::string& l) { return writer_.write(l, nowMs, false); },
                   [n](std::string& l) { nimbus::sys::stampBootNo(l, n); },
                   [this](uint32_t dropped) {
                     const LineStamp s = stamp_();
                     return earlyDropMarkerLine(s.seq, s.ms, dropped);
                   });
    writer_.endBatch();
  }

  void writeStamped(const std::string& msg, uint32_t nowMs) {
    const LineStamp s = stamp_();
    writer_.write(formatLine(s.seq, s.ms, cat::kStorage, msg), nowMs);
  }

  // After a successful write: record the lines a degraded window could not write.
  void noteMissed(uint32_t nowMs) {
    const uint32_t missed = writer_.takeUnwritten();
    if (!missed) return;
    const LineStamp s = stamp_();
    // A marker that does not land puts the count back: the miss is never silent.
    if (!writer_.write(missedMarkerLine(s.seq, s.ms, missed), nowMs, false)) writer_.restoreUnwritten(missed);
  }

  Stamp stamp_;
  FsT* fs_ = nullptr;
  bool attached_ = false;
  uint32_t bootNo_ = 0;
  LogWriter<FsT> writer_;
  EarlyBacklog backlog_;
};

}  // namespace nimbus::errlog
