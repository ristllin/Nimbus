#pragma once
// moved_store - a value that moved out of NVS into a flash file, and the migration
// rule both moved stores follow (the usage ledger and the scratchpad, v4.5.14).
//
// Both were rewritten in NVS on every turn; on a full partition the writes failed and
// most of a month's spend and every scratchpad change lived in RAM only. They now
// live in LittleFS files. The rule (owner decision, Oct 2026), host-tested in
// test/test_nvs_full over every leg (NVS only, file only, both, neither):
//
//   load:    read the file; when there is no file, read the legacy NVS key.
//   persist: write the file atomically (tmp + rename), then read it back; the write
//            counts only when the read-back equals what was written. NVS is never
//            written again.
//   retire:  erase the legacy NVS copy only on a boot whose image was already marked
//            valid (not an OTA trial boot), and only once the file reads back. A
//            rollback from a trial boot to the previous release therefore still finds
//            that release's last values in NVS.
//
// `Io` binds the device (LittleFS + Preferences) or a host fake. It needs:
//   bool readFile(std::string& out);            // true when the file exists
//   bool writeFileAtomic(const std::string& v); // tmp + rename
//   bool readNvs(std::string& out);             // true when the legacy key exists
//   bool eraseNvs();                            // true when the key is gone

#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace store {

enum class MovedSource : uint8_t { None = 0, File, Nvs };

struct MovedLoad {
  MovedSource src = MovedSource::None;
  std::string value;
};

template <typename Io>
MovedLoad loadMoved(Io& io) {
  MovedLoad m;
  if (io.readFile(m.value)) {
    m.src = MovedSource::File;
    return m;
  }
  m.value.clear();
  if (io.readNvs(m.value)) {
    m.src = MovedSource::Nvs;
    return m;
  }
  m.value.clear();
  return m;
}

template <typename Io>
bool persistMoved(Io& io, const std::string& v) {
  if (!io.writeFileAtomic(v)) return false;
  std::string back;
  return io.readFile(back) && back == v;
}

// True when the legacy NVS copy may be erased now (see the rule above).
inline bool mayRetireLegacy(bool imageValidAtBoot, bool fileReadsBack, bool nvsPresent) {
  return imageValidAtBoot && fileReadsBack && nvsPresent;
}

// Retire the legacy copy if the rule allows it. Returns true when it was erased.
template <typename Io>
bool retireLegacy(Io& io, bool imageValidAtBoot) {
  if (!imageValidAtBoot) return false;
  std::string file, nvs;
  const bool haveFile = io.readFile(file);
  const bool haveNvs = io.readNvs(nvs);
  if (!mayRetireLegacy(imageValidAtBoot, haveFile, haveNvs)) return false;
  return io.eraseNvs();
}

// ---- the dirty flag and its retry --------------------------------------------------
// Set by a persist that did not land (the RAM copy holds data the file does not),
// cleared by one that did. While dirty, the main loop retries at most once per
// retryMs (every mutation also persists, so a busy device retries sooner). saved() is
// what /api/state shows.
class DirtyRetry {
 public:
  explicit DirtyRetry(uint32_t retryMs = 60000) : retryMs_(retryMs) {}
  void noteWrite(bool landed, uint32_t nowMs) {
    dirty_ = !landed;
    lastMs_ = nowMs;
  }
  void markDirty(uint32_t nowMs) {   // data only RAM holds, not yet tried (a legacy load)
    dirty_ = true;
    lastMs_ = nowMs - retryMs_;      // due at the next check
  }
  bool saved() const { return !dirty_; }
  bool retryDue(uint32_t nowMs) const {
    return dirty_ && (uint32_t)(nowMs - lastMs_) >= retryMs_;
  }

 private:
  uint32_t retryMs_;
  uint32_t lastMs_ = 0;
  bool     dirty_ = false;
};

// ---- persist only on a change ------------------------------------------------------
// The scratchpad is persisted two or three times per turn whether or not it changed.
// LandedFingerprint remembers the bytes the file last took (a 64-bit FNV-1a hash and
// the length), so a persist of the same bytes writes nothing; a write that did not
// land leaves the old fingerprint, so the next persist tries again.
class LandedFingerprint {
 public:
  bool changed(const std::string& v) const { return !known_ || len_ != v.size() || hash_ != hashOf(v); }
  void noteLanded(const std::string& v) {
    known_ = true;
    len_ = v.size();
    hash_ = hashOf(v);
  }

 private:
  static uint64_t hashOf(const std::string& v) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (unsigned char c : v) h = (h ^ c) * 1099511628211ull;
    return h;
  }
  bool     known_ = false;
  size_t   len_ = 0;
  uint64_t hash_ = 0;
};

enum class PersistOutcome : uint8_t { Unchanged = 0, Landed, Failed };

// persistMoved, skipped when the bytes equal what last landed.
template <typename Io>
PersistOutcome persistIfChanged(Io& io, LandedFingerprint& last, const std::string& v) {
  if (!last.changed(v)) return PersistOutcome::Unchanged;
  if (!persistMoved(io, v)) return PersistOutcome::Failed;
  last.noteLanded(v);
  return PersistOutcome::Landed;
}

// ---- a failure line at most once a minute, with a count ----------------------------
// noteFailure returns how many failures the line due now reports (this one and every
// one held back since the last line), or 0 while the window holds it back.
class FailureLog {
 public:
  explicit FailureLog(uint32_t windowMs = 60000) : windowMs_(windowMs) {}
  uint32_t noteFailure(uint32_t nowMs) {
    ++pending_;
    if (logged_ && (uint32_t)(nowMs - lastMs_) < windowMs_) return 0;
    logged_ = true;
    lastMs_ = nowMs;
    const uint32_t n = pending_;
    pending_ = 0;
    return n;
  }

 private:
  uint32_t windowMs_;
  uint32_t lastMs_ = 0;
  uint32_t pending_ = 0;
  bool     logged_ = false;
};

}  // namespace store
}  // namespace nimbus
