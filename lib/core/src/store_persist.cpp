#include "nimbus/store/persist.h"

#include <utility>
#include <vector>

namespace nimbus {
namespace store {

namespace {
// JSON string escape: quotes, backslashes and control bytes (a route's result can
// carry request text, such as a tier name). UTF-8 passes through unchanged.
void appendEscaped(std::string& out, const char* s, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    } else {
      out += (char)c;
    }
  }
}

std::string storageFullHead() {
  std::string out = "{\"error\":\"";
  out += kStorageFullCode;
  out += "\",\"message\":\"";
  appendEscaped(out, kStorageFullMessage, sizeof(kStorageFullMessage) - 1);
  out += "\"";
  return out;
}
}  // namespace

std::string storageFullJson() { return storageFullHead() + "}"; }

std::string storageFullJson(const std::string& msg) {
  std::string out = storageFullHead() + ",\"msg\":\"";
  appendEscaped(out, msg.data(), msg.size());
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

namespace {
std::string trimmed(const std::string& v) {
  const size_t a = v.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  const size_t b = v.find_last_not_of(" \t");
  return v.substr(a, b - a + 1);
}
// Split a comma-separated list into trimmed, non-empty items.
std::vector<std::string> splitList(const std::string& csv) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s <= csv.size()) {
    size_t e = csv.find(',', s);
    if (e == std::string::npos) e = csv.size();
    const std::string item = trimmed(csv.substr(s, e - s));
    if (!item.empty()) out.push_back(item);
    s = e + 1;
  }
  return out;
}
}  // namespace

namespace {
using NameList = std::vector<std::pair<std::string, std::string>>;   // oldest first
bool onList(const std::vector<std::string>& list, const std::string& who) {
  for (const auto& a : list)
    if (a == who) return true;
  return false;
}
// A later entry for the same id replaces an earlier one; "" drops it; an id off the
// allowlist is never kept.
void putName(NameList& kept, const std::vector<std::string>& allow, const std::string& who,
             const std::string& nm) {
  for (auto it = kept.begin(); it != kept.end(); ++it)
    if (it->first == who) { kept.erase(it); break; }
  if (!nm.empty() && onList(allow, who)) kept.emplace_back(who, nm);
}
std::string renderNames(const NameList& kept) {
  std::string out;
  for (const auto& kv : kept) out += (out.empty() ? "" : ",") + kv.first + ":" + kv.second;
  return out;
}
}  // namespace

std::string capTelegramNames(const std::string& names, const std::string& allowlist,
                             const std::string& id, const std::string& name) {
  const std::vector<std::string> allow = splitList(allowlist);
  NameList kept;
  for (const auto& entry : splitList(names)) {
    const size_t colon = entry.find(':');
    if (colon == 0 || colon == std::string::npos) continue;   // no id, or no name
    putName(kept, allow, trimmed(entry.substr(0, colon)), entry.substr(colon + 1));
  }
  if (!id.empty()) putName(kept, allow, id, name);
  std::string out = renderNames(kept);
  while (out.size() > kTgNamesMaxBytes && kept.size() > 1) {   // the newest always stays
    kept.erase(kept.begin());
    out = renderNames(kept);
  }
  return out.size() > kTgNamesMaxBytes ? std::string() : out;
}

}  // namespace store
}  // namespace nimbus
