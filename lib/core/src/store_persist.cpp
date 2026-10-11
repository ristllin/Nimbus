#include "nimbus/store/persist.h"

namespace nimbus {
namespace store {

namespace {
// Minimal JSON string escape for the fixed message (quotes + backslashes only; the
// message is a compile-time constant with neither, but the builder stays honest).
void appendEscaped(std::string& out, const char* s) {
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += *s;
  }
}
}  // namespace

std::string storageFullJson() {
  std::string out = "{\"error\":\"";
  out += kStorageFullCode;
  out += "\",\"message\":\"";
  appendEscaped(out, kStorageFullMessage);
  out += "\"}";
  return out;
}

SaveReply saveReply(bool persisted, int okStatus, const std::string& okBody) {
  if (!persisted) return {kStorageFullStatus, storageFullJson()};
  return {okStatus, okBody};
}

void PersistLedger::noteFailure(uintptr_t task, const char* key) {
  ++seq_;
  Rec& r = ring_[(seq_ - 1) % kRing];
  r.seq  = seq_;
  r.task = task;
  size_t n = 0;
  if (key)
    for (; key[n] && n < kKeyMax - 1; ++n) r.key[n] = key[n];
  r.key[n] = 0;
}

bool PersistLedger::failedSince(uint32_t sinceSeq, uintptr_t task) const {
  if (seq_ == sinceSeq) return false;            // nothing failed anywhere since
  if (seq_ - sinceSeq > kRing) return true;      // overflowed: cannot attribute, fail closed
  for (const Rec& r : ring_)
    if (r.seq > sinceSeq && r.task == task) return true;
  return false;
}

const char* PersistLedger::lastKey() const {
  if (seq_ == 0) return "";
  return ring_[(seq_ - 1) % kRing].key;
}

NvsUsage nvsUsageFromEntries(size_t usedEntries, size_t availableEntries,
                             size_t totalEntries) {
  NvsUsage u;
  u.usedBytes  = (uint32_t)(usedEntries * kNvsEntryBytes);
  u.freeBytes  = (uint32_t)(availableEntries * kNvsEntryBytes);
  u.totalBytes = (uint32_t)(totalEntries * kNvsEntryBytes);
  u.low        = u.freeBytes < kOwnerHeadroomBytes;
  return u;
}

bool isCatalogSlug(const char* slug) {
  if (!slug || !slug[0]) return false;
  size_t n = 0;
  for (; slug[n]; ++n) {
    const char c = slug[n];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!ok || n >= 10) return false;
  }
  return true;
}

std::string catalogCachePath(const char* slug) {
  if (!isCatalogSlug(slug)) return {};
  return std::string("/data/mcat_") + slug + ".json";
}

std::string legacyCatalogKey(const char* slug) {
  if (!isCatalogSlug(slug)) return {};
  return std::string("mcat_") + slug;
}

}  // namespace store
}  // namespace nimbus
