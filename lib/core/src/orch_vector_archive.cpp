#include "nimbus/orch/vector_archive.h"

#include <algorithm>
#include <cstring>

namespace nimbus {
namespace orch {

namespace {
// Strip the field separator from caller-controlled text (kept out of the codec so a
// crafted source/ns can't move a split point). The archive stores ns as its own
// length-prefixed field, so there is no split to poison, but source is still
// free text - keep it clean for symmetry with the live-store codec.
std::string stripSep(const std::string& v) {
  if (v.find('\x1F') == std::string::npos) return v;
  std::string o;
  o.reserve(v.size());
  for (char c : v) if ((unsigned char)c != 0x1F) o += c;
  return o;
}

// The effective namespace of an archived entry: an empty ns is legacy/unattributed
// and belongs to the owner (same rule as the live store - never widen access).
std::string effNs(const std::string& ns) { return ns.empty() ? std::string(kOwnerNs) : ns; }

bool nsVisible(const std::string& entryNs, const std::vector<std::string>& allow) {
  if (allow.empty()) return true;   // unscoped: admin / maintenance
  const std::string eff = effNs(entryNs);
  for (const auto& a : allow) if (a == eff) return true;
  return false;
}

// ---- little-endian binary (de)serialization helpers (mirror vector_memory) --
void putU16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xff)); s.push_back((char)((v >> 8) & 0xff)); }
void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xff)); }
void putF32(std::string& s, float f) { uint32_t u; std::memcpy(&u, &f, 4); putU32(s, u); }
void putStr(std::string& s, const std::string& v) { putU16(s, (uint16_t)v.size()); s += v; }

struct Reader {
  const char* p; const char* end; bool ok = true;
  Reader(const std::string& b) : p(b.data()), end(b.data() + b.size()) {}
  bool need(size_t n) { if (!ok || (size_t)(end - p) < n) { ok = false; return false; } return true; }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint8_t)p[0] | ((uint8_t)p[1] << 8); p += 2; return v; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)p[i] << (8 * i); p += 4; return v; }
  float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
  std::string str() { uint16_t n = u16(); if (!need(n)) return ""; std::string v(p, n); p += n; return v; }
};

constexpr char kMagic[3] = {'V', 'A', '1'};   // vector archive v1
constexpr int  kMaxVecDims = 4096;            // same sane ceiling as the live store
}  // namespace

VecEntry VectorArchive::toPublic(const Stored& s) {
  VecEntry e;
  e.id = s.id;
  e.content = s.content;
  e.importance = s.importance;
  e.ttlHours = s.ttlHours;
  e.createdAtHours = s.createdAtHours;
  e.source = s.source;
  e.ns = s.ns;
  e.creatorFlag = s.creatorFlag;
  e.permanentFlag = s.permanentFlag;
  e.vec.assign(s.vec.begin(), s.vec.end());
  return e;
}

int VectorArchive::indexOf(const std::string& id) const {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].id == id) return (int)i;
  return -1;
}

bool VectorArchive::archive(const VecEntry& e, uint32_t archivedAtHours) {
  if ((int)e.vec.size() != dims_) return false;   // enforce the store's width invariant

  // Same-id replace: a content hash can expire, be restored, and expire again. Drop
  // the stale copy so restore-by-id stays unambiguous and the re-archived entry keeps
  // its most-recent FIFO position.
  int existing = indexOf(e.id);
  if (existing >= 0) entries_.erase(entries_.begin() + existing);

  // FIFO cap: evict from the FRONT (oldest archived) until there is room for one more.
  if (maxEntries_ > 0) {
    while ((int)entries_.size() >= maxEntries_ && !entries_.empty())
      entries_.erase(entries_.begin());
  }

  Stored s;
  s.id = e.id;
  s.content = e.content;
  s.importance = e.importance;
  s.ttlHours = e.ttlHours;
  s.createdAtHours = e.createdAtHours;
  s.archivedAtHours = archivedAtHours;
  s.source = e.source;
  s.ns = e.ns;
  s.creatorFlag = e.creatorFlag;
  s.permanentFlag = e.permanentFlag;
  s.vec.assign(e.vec.begin(), e.vec.end());
  entries_.push_back(std::move(s));
  dirty_ = true;
  return true;
}

std::vector<VecHit> VectorArchive::search(const std::vector<int8_t>& query, int k,
                                          const std::vector<std::string>& nsAllow) const {
  std::vector<VecHit> hits;
  if (entries_.empty() || (int)query.size() != dims_ || k <= 0) return hits;
  hits.reserve(entries_.size());
  for (const auto& e : entries_) {
    if (!nsVisible(e.ns, nsAllow)) continue;
    hits.push_back(VecHit{e.id, e.content, e.importance,
                          cosineDistanceRaw(query.data(), e.vec.data(), (size_t)dims_), 0.0f});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const VecHit& a, const VecHit& b) { return a.distance < b.distance; });
  if ((int)hits.size() > k) hits.resize(k);
  return hits;
}

bool VectorArchive::idVisible(const std::string& id, const std::vector<std::string>& nsAllow) const {
  int i = indexOf(id);
  return i >= 0 && nsVisible(entries_[i].ns, nsAllow);
}

bool VectorArchive::take(const std::string& id, VecEntry& out,
                         const std::vector<std::string>& nsAllow) {
  int i = indexOf(id);
  if (i < 0 || !nsVisible(entries_[i].ns, nsAllow)) return false;
  out = toPublic(entries_[i]);
  entries_.erase(entries_.begin() + i);
  dirty_ = true;
  return true;
}

bool VectorArchive::remove(const std::string& id) {
  int i = indexOf(id);
  if (i < 0) return false;
  entries_.erase(entries_.begin() + i);
  dirty_ = true;
  return true;
}

std::vector<VecEntry> VectorArchive::getAll(const std::vector<std::string>& nsAllow) const {
  std::vector<VecEntry> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_)
    if (nsVisible(e.ns, nsAllow)) out.push_back(toPublic(e));
  return out;
}

int VectorArchive::flushAll() {
  int n = (int)entries_.size();
  entries_.clear();
  if (n) dirty_ = true;
  return n;
}

std::string VectorArchive::serialize() const {
  std::string s;
  s.append(kMagic, 3);
  putU16(s, (uint16_t)dims_);
  putU32(s, (uint32_t)entries_.size());
  for (const auto& e : entries_) {
    putF32(s, e.importance);
    putU32(s, (uint32_t)e.ttlHours);   // signed stored as bit-pattern; read back as int32
    putU32(s, e.createdAtHours);
    putU32(s, e.archivedAtHours);
    uint8_t flags = (e.creatorFlag ? 1 : 0) | (e.permanentFlag ? 2 : 0);
    s.push_back((char)flags);
    putStr(s, e.id);
    putStr(s, e.content);
    putStr(s, stripSep(e.source));
    putStr(s, e.ns);                   // own field (no source\x1Fns packing needed here)
    putU16(s, (uint16_t)e.vec.size());
    for (int8_t b : e.vec) s.push_back((char)b);
  }
  return s;
}

bool VectorArchive::deserialize(const std::string& blob) {
  entries_.clear();
  Reader r(blob);
  if (!r.need(3) || std::memcmp(r.p, kMagic, 3) != 0) return false;
  r.p += 3;
  const int dims = (int)r.u16();
  // Range-check the width before it becomes the store's invariant (a dims<1 header
  // poisons every width check; dims>ceiling is corrupt/crafted). Refuse the blob.
  if (dims < 1 || dims > kMaxVecDims) return false;
  dims_ = dims;
  uint32_t count = r.u32();
  for (uint32_t i = 0; i < count && r.ok; i++) {
    Stored e;
    e.importance = r.f32();
    e.ttlHours = (int32_t)r.u32();
    e.createdAtHours = r.u32();
    e.archivedAtHours = r.u32();
    uint8_t flags = r.need(1) ? (uint8_t)*r.p : 0; if (r.ok) r.p += 1;
    e.creatorFlag = flags & 1;
    e.permanentFlag = flags & 2;
    e.id = r.str();
    e.content = r.str();
    e.source = r.str();
    e.ns = r.str();
    uint16_t vn = r.u16();
    if (!r.need(vn)) break;
    // Every stored vec MUST be dims_ wide (search reads exactly dims_ bytes/vec with
    // no per-buffer bound); a width mismatch is corruption - keep the clean prefix
    // and stop, so a short vec can never cause an out-of-bounds read later.
    if ((int)vn != dims_) { r.ok = false; break; }
    e.vec.assign((const int8_t*)r.p, (const int8_t*)r.p + vn);
    r.p += vn;
    if (r.ok) entries_.push_back(std::move(e));
  }
  return r.ok;
}

}  // namespace orch
}  // namespace nimbus
