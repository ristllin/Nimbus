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

// ---- self-healing rotating write engine -------------------------------------
//
// Templated on a filesystem adapter so the EXACT rotation + late-mount self-heal
// logic runs unchanged on device (Arduino File) and is host-tested (test_errlog)
// against a fake FS. This is where the boot-order trap is handled: the first log
// line is emitted long before LittleFS/SD is mounted (mounting happens later in
// memory::begin(), inside beginWeb), so early writes must fail soft and the engine
// must persist automatically once the FS becomes ready, with no re-init and no lost
// size accounting.
//
// FsT must provide (all take/return std::string paths, bytes):
//   long   fileSize(const std::string& path);                 // bytes, or -1 if unreadable/unmounted
//   size_t appendFile(const std::string& path, const char* d, size_t n);  // bytes written (0 = not writable)
//   void   makeDir(const std::string& path);
//   void   removeFile(const std::string& path);
//   void   renameFile(const std::string& from, const std::string& to);
template <class FsT>
class LogWriter {
 public:
  // Point the engine at a filesystem for the given tier. Forces a size re-sync so
  // the next write reads the real on-disk size (a late mount, or a tier switch).
  void setTier(FsT* fs, bool haveSd) { fs_ = fs; haveSd_ = haveSd; sizeSynced_ = false; }

  bool haveSd() const { return haveSd_; }
  TierCaps caps() const { return capsFor(haveSd_); }

  // Append one already-formatted line (already ending in '\n'). Returns true iff it
  // reached the filesystem. On failure the caller keeps the line in its RAM tail.
  bool write(const std::string& line) {
    if (!fs_) return false;
    const TierCaps c = capsFor(haveSd_);
    if (!sizeSynced_) {
      const long sz = fs_->fileSize(pathFor(kBaseName));   // -1 if absent/unmounted
      curSize_ = sz > 0 ? (size_t)sz : 0;                  // absent -> 0 (a fresh file)
    }
    if (shouldRotate(curSize_, line.size(), c)) rotate(c);
    size_t w = fs_->appendFile(pathFor(kBaseName), line.data(), line.size());
    if (w == 0) {
      // Open/write failed: the FS may have just mounted, or /log may not exist yet
      // (neither FS auto-creates it). Create the dir and retry ONCE.
      fs_->makeDir(kDir);
      w = fs_->appendFile(pathFor(kBaseName), line.data(), line.size());
    }
    if (w == 0) { sizeSynced_ = false; return false; }     // still not ready: re-sync next time
    sizeSynced_ = true;                                     // a real write happened; size now tracked
    curSize_ += w;
    bytesWritten_ += w;                                     // flash-wear watch (CUM-409)
    return true;
  }

  // Total bytes appended to the durable log since boot (a rotation renames, it does
  // not re-append, so this is the true flash-write volume). Exposed as the CUM-409
  // wear counter; host-tested against the fake FS.
  size_t bytesWritten() const { return bytesWritten_; }

 private:
  // Shift files down and clear the active file: base -> .1 -> .2 ..., dropping the
  // oldest (index maxFiles-1). Bounds total on-disk bytes to maxFileBytes*maxFiles.
  void rotate(const TierCaps& c) {
    fs_->removeFile(pathFor(rotatedName(c.maxFiles - 1)));          // drop oldest (no-op if absent)
    for (size_t i = c.maxFiles - 1; i >= 2; --i)
      fs_->renameFile(pathFor(rotatedName(i - 1)), pathFor(rotatedName(i)));
    fs_->renameFile(pathFor(kBaseName), pathFor(rotatedName(1)));
    curSize_ = 0;
  }

  FsT*   fs_          = nullptr;
  bool   haveSd_      = false;
  size_t curSize_     = 0;
  size_t bytesWritten_ = 0;
  bool   sizeSynced_  = false;
};

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
// itself, bounded to at most one marker per successful write.
inline std::string dropMarkerLine(uint32_t seq, uint32_t ms, uint32_t dropped) {
  if (dropped == 0) return {};
  return formatLine(seq, ms, cat::kStorage,
                    std::string("durable log dropped ") + std::to_string(dropped) +
                    " line(s) under card-lock contention (best-effort)");
}

}  // namespace nimbus::errlog
