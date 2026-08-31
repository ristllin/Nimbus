#include "nimbus/orch/vector_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nimbus/orch/vector_archive.h"   // prune sink (CUM-225)

namespace nimbus {
namespace orch {

namespace {
// Remove the codec's field separator from caller-controlled text.
std::string stripSep(const std::string& v) {
  if (v.find('\x1F') == std::string::npos) return v;
  std::string o;
  o.reserve(v.size());
  for (char c : v) if ((unsigned char)c != 0x1F) o += c;
  return o;
}
}  // namespace

// The effective namespace of a stored entry: an empty ns is legacy/unattributed
// and belongs to the owner (a migration must never widen access), so it reads as
// kOwnerNs everywhere the boundary is enforced.
static std::string effNs(const std::string& ns) {
  return ns.empty() ? std::string(kOwnerNs) : ns;
}

// v3.7.0: may this entry be seen by a reader allowed the given namespaces?
// An EMPTY allow-list is unscoped (maintenance passes); an empty entry ns is
// legacy and belongs to the owner, so it matches only a reader that lists the
// owner namespace explicitly.
static bool nsVisible(const std::string& entryNs, const std::vector<std::string>& allow) {
  if (allow.empty()) return true;
  const std::string& eff = entryNs.empty() ? std::string(kOwnerNs) : entryNs;
  for (const auto& a : allow) if (a == eff) return true;
  return false;
}

// ---- little-endian binary (de)serialization helpers ------------------------
namespace {
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
constexpr char kMagic[3] = {'V', 'M', '1'};
// Sane bound on the per-store vector width read from a blob header. Real embedders
// sit well inside this (Mistral 1024, OpenAI small/large 1536/3072); the ceiling
// only rejects a corrupt or crafted header. dims must be >= 1 - a dims=0 store
// silently accepts only zero-width vectors (cosineRaw treats n==0 as orthogonal),
// so every add/search degrades to a no-op and the store is poisoned, not merely
// empty. Reject the whole blob instead (CUM-223).
constexpr int kMaxVecDims = 4096;

// Cosine distance core over raw int8 buffers (both `n` wide). Allocator-agnostic, so
// it works over both a std::vector<int8_t> query and a PSRAM-allocated stored vec.
float cosineRaw(const int8_t* a, const int8_t* b, size_t n) {
  if (n == 0) return 1.0f;
  long long dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < n; i++) {
    int ai = a[i], bi = b[i];
    dot += (long long)ai * bi;
    na  += (long long)ai * ai;
    nb  += (long long)bi * bi;
  }
  if (na == 0 || nb == 0) return 1.0f;  // a zero vector has no direction
  double sim = (double)dot / (std::sqrt((double)na) * std::sqrt((double)nb));
  if (sim > 1.0) sim = 1.0;
  if (sim < -1.0) sim = -1.0;
  return (float)(1.0 - sim);
}
}  // namespace

std::vector<int8_t> VectorMemory::quantize(const std::vector<float>& v) {
  std::vector<int8_t> out;
  out.reserve(v.size());
  for (float f : v) {
    int q = (int)std::lround(f * 127.0f);
    if (q > 127) q = 127;
    if (q < -127) q = -127;
    out.push_back((int8_t)q);
  }
  return out;
}

float cosineDistanceI8(const std::vector<int8_t>& a, const std::vector<int8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 1.0f;
  return cosineRaw(a.data(), b.data(), a.size());
}

float cosineDistanceRaw(const int8_t* a, const int8_t* b, size_t n) {
  return cosineRaw(a, b, n);
}

VecEntry VectorMemory::toPublic(const Stored& s) {
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
  e.lastRecallHours = s.lastRecallHours;
  e.vec.assign(s.vec.begin(), s.vec.end());   // PSRAM vec -> std::vector<int8_t>
  return e;
}

int VectorMemory::indexOf(const std::string& id) const {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].id == id) return (int)i;
  return -1;
}

int VectorMemory::findNearest(const std::vector<int8_t>& q, float& distOut) const {
  int best = -1;
  float bestD = 2.0f;
  for (size_t i = 0; i < entries_.size(); i++) {
    float d = cosineRaw(q.data(), entries_[i].vec.data(), (size_t)dims_);
    if (d < bestD) { bestD = d; best = (int)i; }
  }
  distOut = bestD;
  return best;
}

float VectorMemory::retentionOf(float importance, int32_t ttlHours, uint32_t createdAtHours,
                                bool permanent, bool creator, uint32_t nowHours) {
  if (permanent || creator) return 1e30f;   // never evicted
  if (ttlHours <= 0) return importance;      // never expires -> TTL frac 1
  float remaining = float((int64_t)createdAtHours + ttlHours - (int64_t)nowHours);
  float frac = remaining / float(ttlHours);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return importance * frac;
}

float VectorMemory::retentionScore(const VecEntry& e, uint32_t nowHours) const {
  return retentionOf(e.importance, e.ttlHours, e.createdAtHours, e.permanentFlag,
                     e.creatorFlag, nowHours);
}

bool VectorMemory::isExpiredRaw(float importance, int32_t ttlHours, uint32_t createdAtHours,
                                bool permanent, bool creator, uint32_t nowHours) {
  if (permanent || creator) return false;              // exempt
  if (importance < kMinImportance) return true;        // decayed away
  if (ttlHours <= 0) return false;                     // never age-expires
  uint32_t age = nowHours >= createdAtHours ? nowHours - createdAtHours : 0;
  return (int64_t)age > (int64_t)ttlHours;
}

bool VectorMemory::add(const VecEntry& e, bool dedup) {
  if ((int)e.vec.size() != dims_) return false;  // enforce consistent width

  if (dedup && !entries_.empty()) {
    float d;
    int near = findNearest(e.vec, d);
    if (near >= 0 && d < kDuplicateThreshold) {
      // Dedup: skip the write, bump the existing entry's importance to the max.
      if (e.importance > entries_[near].importance)
        entries_[near].importance = e.importance;
      return false;
    }
  }

  // Cap: evict the lowest-retention-score non-exempt entry before inserting, using
  // the new entry's timestamp as "now". If every entry is exempt (all permanent),
  // no eviction happens and the store grows past the cap - user-pinned data is
  // never dropped for a new memory.
  //
  // CUM-223: eviction is NAMESPACE-SCOPED - only an entry in the SAME namespace as
  // the incoming write may be dropped. The cap (max_vectors) is device-global and
  // eviction was too: a noisy tenant that filled the store could displace another
  // principal's memory (a member evicting the owner's low-importance facts on a
  // full device). Scoping the victim to the writer's own namespace keeps the cap
  // enforcement inside the data boundary. Single-tenant devices - every entry in
  // the owner namespace - are unaffected (the scope is the whole store). If the
  // writer's namespace holds no evictable entry, the store is allowed to grow past
  // the cap rather than cross the boundary, the same spirit as the all-permanent
  // overflow above; per-tenant write quotas (applyWriteQuotas) bound that growth.
  if (maxEntries_ > 0 && (int)entries_.size() >= maxEntries_) {
    const std::string eNs = effNs(e.ns);
    int worst = -1;
    float worstScore = 1e30f;
    for (int i = 0; i < (int)entries_.size(); i++) {
      const Stored& s = entries_[i];
      if (effNs(s.ns) != eNs) continue;   // never evict across the data boundary
      float sc = retentionOf(s.importance, s.ttlHours, s.createdAtHours, s.permanentFlag,
                             s.creatorFlag, e.createdAtHours);
      if (sc < worstScore) { worstScore = sc; worst = i; }
    }
    if (worst >= 0 && worstScore < 1e30f) entries_.erase(entries_.begin() + worst);
  }

  Stored s;
  s.id = e.id;
  s.content = e.content;
  s.importance = e.importance;
  s.ttlHours = e.ttlHours;
  s.createdAtHours = e.createdAtHours;
  s.source = e.source;
  s.ns = e.ns;
  s.creatorFlag = e.creatorFlag;
  s.permanentFlag = e.permanentFlag;
  s.lastRecallHours = e.lastRecallHours;   // survives tier promote/demote rebuilds
  s.vec.assign(e.vec.begin(), e.vec.end());   // std::vector<int8_t> -> PSRAM vec
  entries_.push_back(std::move(s));
  return true;
}

int VectorMemory::boostAccessed(const std::vector<std::string>& ids, float impBoost,
                                uint32_t nowHours) {
  int n = 0;
  for (const auto& id : ids) {
    int idx = indexOf(id);
    if (idx < 0) continue;
    float v = entries_[idx].importance + impBoost;
    entries_[idx].importance = v > 1.0f ? 1.0f : v;
    if (nowHours > 0) entries_[idx].createdAtHours = nowHours;  // reset the TTL clock
    n++;
  }
  return n;
}

std::vector<VecHit> VectorMemory::search(const std::vector<int8_t>& query, int k,
                                         uint32_t nowHours,
                                         const std::vector<std::string>& nsAllow) const {
  std::vector<VecHit> hits;
  if (entries_.empty() || (int)query.size() != dims_ || k <= 0) return hits;
  struct Scored { VecHit h; int idx; };
  std::vector<Scored> sc;
  sc.reserve(entries_.size());
  for (int i = 0; i < (int)entries_.size(); i++) {
    const Stored& e = entries_[i];
    if (!nsVisible(e.ns, nsAllow)) continue;                 // v3.7.0 read boundary
    // Query-time TTL, same predicate as recall()/pruneExpired (audit 2026-07-24:
    // search() skipped it, so the model's explicit memory.search returned
    // expired-but-unpruned facts that recall correctly hid). nowHours==0 keeps
    // the legacy unfiltered behavior for clockless callers (dedup targeting).
    if (nowHours > 0 && isExpiredRaw(e.importance, e.ttlHours, e.createdAtHours,
                                     e.permanentFlag, e.creatorFlag, nowHours))
      continue;
    sc.push_back(Scored{VecHit{e.id, e.content, e.importance,
                               cosineRaw(query.data(), e.vec.data(), (size_t)dims_)},
                        i});
  }
  std::stable_sort(sc.begin(), sc.end(),
                   [](const Scored& a, const Scored& b) { return a.h.distance < b.h.distance; });
  if ((int)sc.size() > k) sc.resize(k);
  hits.reserve(sc.size());
  for (auto& x : sc) {
    // Usage stamp on the RETURNED hits only (never the also-rans); clockless
    // callers (nowHours==0, dedup probes) don't count as a use.
    if (nowHours > 0) entries_[x.idx].lastRecallHours = nowHours;
    hits.push_back(std::move(x.h));
  }
  return hits;
}

void VectorMemory::decayImportance(float factor) {
  for (auto& e : entries_) {
    if (e.permanentFlag) continue;
    float v = e.importance * factor;
    e.importance = v < kDecayFloor ? kDecayFloor : v;
  }
}

int VectorMemory::pruneExpired(uint32_t nowHours) {
  size_t before = entries_.size();
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [this, nowHours](const Stored& e) {
                       if (!isExpiredRaw(e.importance, e.ttlHours, e.createdAtHours,
                                         e.permanentFlag, e.creatorFlag, nowHours))
                         return false;
                       // CUM-225: with a cold store attached, an expired entry is
                       // MOVED there (embedding preserved) instead of dropped. The
                       // no-SD device leaves the sink null and this stays a delete.
                       if (archiveSink_) archiveSink_->archive(toPublic(e), nowHours);
                       return true;
                     }),
      entries_.end());
  return (int)(before - entries_.size());
}

std::vector<VecHit> VectorMemory::recall(const std::vector<int8_t>& query,
                                         const RecallParams& p, uint32_t nowHours) const {
  std::vector<VecHit> out;
  if (entries_.empty() || (int)query.size() != dims_ || p.k <= 0) return out;

  // 1. Score every non-expired, above-threshold entry.
  struct Cand { int idx; float sim; float score; };
  std::vector<Cand> cands;
  cands.reserve(entries_.size());
  const float halfLife = p.recencyHalfLifeHours > 1.0f ? p.recencyHalfLifeHours : 1.0f;
  for (int i = 0; i < (int)entries_.size(); i++) {
    const Stored& e = entries_[i];
    if (!nsVisible(e.ns, p.nsAllow)) continue;                        // v3.7.0 read boundary
    if (isExpiredRaw(e.importance, e.ttlHours, e.createdAtHours, e.permanentFlag,
                     e.creatorFlag, nowHours)) continue;               // query-time TTL
    float sim = 1.0f - cosineRaw(query.data(), e.vec.data(), (size_t)dims_);
    if (sim < p.relevanceThreshold) continue;
    uint32_t age = nowHours >= e.createdAtHours ? nowHours - e.createdAtHours : 0;
    float rec = RecallParams::kRecencyFloor +
                (1.0f - RecallParams::kRecencyFloor) * std::pow(0.5f, (float)age / halfLife);
    if ((e.permanentFlag || e.creatorFlag) && rec < RecallParams::kPinnedRecencyFloor)
      rec = RecallParams::kPinnedRecencyFloor;
    float imp = 0.5f + 0.5f * e.importance;              // bound stale-importance dominance
    cands.push_back({i, sim, sim * rec * imp});
  }
  if (cands.empty()) return out;

  // 2. Keep the top 2k candidates by score.
  std::stable_sort(cands.begin(), cands.end(),
                   [](const Cand& a, const Cand& b) { return a.score > b.score; });
  int pool = p.k * 2;
  if ((int)cands.size() > pool) cands.resize(pool);

  // 3. Decide MMR: only when the would-be top-k scores are near-tied.
  bool useMmr = false;
  {
    int n = std::min((int)cands.size(), p.k);
    if (n >= 2 && (cands.front().score - cands[n - 1].score) < RecallParams::kTieEpsilon)
      useMmr = true;
  }

  // 4. Greedy selection: skip near-duplicates of already-selected hits; when MMR
  //    is on, penalize similarity to the selected set. maxSim[c] holds each remaining
  //    candidate's max cosine SIMILARITY to the already-selected set, updated
  //    INCREMENTALLY against only the newly-selected item each round - O(k * pool)
  //    cosine calls total, not the O(k^2 * pool) of recomputing the whole selected
  //    set every round (retrieval_count is owner-settable up to 100).
  std::vector<int> selected;
  std::vector<char> used(cands.size(), 0);
  std::vector<float> maxSim(cands.size(), 0.0f);
  auto vecOf = [&](int c) { return entries_[cands[c].idx].vec.data(); };
  const float dupSim = 1.0f - RecallParams::kQueryDupDist;   // near-dup if sim >= this
  while ((int)selected.size() < p.k) {
    int best = -1;
    float bestVal = -1e30f;
    for (int c = 0; c < (int)cands.size(); c++) {
      if (used[c]) continue;
      if (!selected.empty() && maxSim[c] >= dupSim) { used[c] = 1; continue; }  // collapse near-dup
      float val = cands[c].score;
      if (useMmr && !selected.empty())
        val = p.mmrLambda * cands[c].score - (1.0f - p.mmrLambda) * maxSim[c];
      if (val > bestVal) { bestVal = val; best = c; }
    }
    if (best < 0) break;
    used[best] = 1;
    selected.push_back(best);
    // Fold the newly-selected item into every remaining candidate's maxSim.
    const int8_t* bv = vecOf(best);
    for (int c = 0; c < (int)cands.size(); c++) {
      if (used[c]) continue;
      float sm = 1.0f - cosineRaw(vecOf(c), bv, (size_t)dims_);
      if (sm > maxSim[c]) maxSim[c] = sm;
    }
  }

  // 5. Emit in selection order.
  out.reserve(selected.size());
  for (int c : selected) {
    const Stored& e = entries_[cands[c].idx];
    if (nowHours > 0) e.lastRecallHours = nowHours;   // usage stamp (mutable)
    VecHit h{e.id, e.content, e.importance, 1.0f - cands[c].sim, cands[c].score};
    out.push_back(std::move(h));
  }
  return out;
}

std::vector<VecEntry> VectorMemory::getAll() const {
  std::vector<VecEntry> out;
  out.reserve(entries_.size());
  for (const auto& s : entries_) out.push_back(toPublic(s));
  std::stable_sort(out.begin(), out.end(),
                   [](const VecEntry& a, const VecEntry& b) { return a.importance > b.importance; });
  return out;
}

uint32_t VectorMemory::countIn(const std::string& ns) const {
  uint32_t n = 0;
  for (const auto& e : entries_)
    if ((e.ns.empty() ? std::string(kOwnerNs) : e.ns) == ns) n++;
  return n;
}

uint32_t VectorMemory::pinsIn(const std::string& ns) const {
  uint32_t n = 0;
  for (const auto& e : entries_)
    if (e.permanentFlag && (e.ns.empty() ? std::string(kOwnerNs) : e.ns) == ns) n++;
  return n;
}

std::vector<NsUsage> VectorMemory::usageByNamespace() const {
  std::vector<NsUsage> out;
  for (const auto& e : entries_) {
    const std::string key = e.ns.empty() ? std::string(kOwnerNs) : e.ns;
    NsUsage* row = nullptr;
    for (auto& u : out)
      if (u.ns == key) { row = &u; break; }
    if (!row) { out.push_back(NsUsage{key, 0, 0}); row = &out.back(); }
    row->count++;
    if (e.permanentFlag) row->pins++;
  }
  std::stable_sort(out.begin(), out.end(), [](const NsUsage& a, const NsUsage& b) {
    if (a.count != b.count) return a.count > b.count;
    return a.ns < b.ns;
  });
  return out;
}

bool VectorMemory::idVisible(const std::string& id,
                             const std::vector<std::string>& nsAllow) const {
  const int i = indexOf(id);
  if (i < 0) return false;
  return nsVisible(entries_[(size_t)i].ns, nsAllow);
}

bool VectorMemory::markPermanent(const std::string& id) {
  int i = indexOf(id);
  if (i < 0) return false;
  entries_[i].permanentFlag = true;
  entries_[i].ttlHours = -1;
  return true;
}

bool VectorMemory::markTemporary(const std::string& id, int32_t defTtlHours, uint32_t nowHours) {
  int i = indexOf(id);
  if (i < 0) return false;
  entries_[i].permanentFlag = false;
  entries_[i].ttlHours = defTtlHours > 0 ? defTtlHours : 720;
  if (nowHours > 0) entries_[i].createdAtHours = nowHours;  // reset the TTL clock
  return true;
}

bool VectorMemory::remove(const std::string& id) {
  int i = indexOf(id);
  if (i < 0) return false;
  entries_.erase(entries_.begin() + i);
  return true;
}

int VectorMemory::flushAll() {
  int n = (int)entries_.size();
  entries_.clear();
  return n;
}

int VectorMemory::flushNonPermanent() {
  size_t before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [](const Stored& e) { return !e.permanentFlag; }),
                 entries_.end());
  return (int)(before - entries_.size());
}

int VectorMemory::restampPreSync(uint32_t threshold, uint32_t nowHours) {
  int n = 0;
  for (auto& e : entries_) {
    if (e.createdAtHours < threshold) { e.createdAtHours = nowHours; n++; }
    // A recall stamped in the boot-relative window (millis/3600, a tiny number)
    // would render as "last used ~56y ago" and then PERSIST via the LR1 block.
    // Reset those to 0 ("never") - the honest floor pre-sync.
    if (e.lastRecallHours && e.lastRecallHours < threshold) e.lastRecallHours = 0;
  }
  return n;
}

int VectorMemory::deduplicate(int windowLimit) {
  // Pairwise scan: for each entry, drop later near-duplicates, keeping the
  // higher-importance one. windowLimit bounds the scan to the newest entries
  // (Release C2 - see the header); entries_ is insertion-ordered so the window
  // is simply the tail.
  std::vector<bool> dead(entries_.size(), false);
  int removed = 0;
  const size_t begin =
      (windowLimit > 0 && entries_.size() > (size_t)windowLimit)
          ? entries_.size() - (size_t)windowLimit : 0;
  for (size_t i = begin; i < entries_.size(); i++) {
    if (dead[i]) continue;
    for (size_t j = i + 1; j < entries_.size(); j++) {
      if (dead[j]) continue;
      if (cosineRaw(entries_[i].vec.data(), entries_[j].vec.data(), (size_t)dims_) < kDuplicateThreshold) {
        // keep the higher-importance twin; mark the other dead
        size_t victim = entries_[i].importance >= entries_[j].importance ? j : i;
        dead[victim] = true;
        removed++;
        if (victim == i) break;  // i itself died - stop comparing from it
      }
    }
  }
  std::vector<Stored, WorkingAllocator<Stored>> kept;
  kept.reserve(entries_.size() - removed);
  for (size_t i = 0; i < entries_.size(); i++)
    if (!dead[i]) kept.push_back(std::move(entries_[i]));
  entries_.swap(kept);
  return removed;
}

std::string VectorMemory::serialize() const {
  std::string s;
  s.append(kMagic, 3);
  putU16(s, (uint16_t)dims_);
  putU32(s, (uint32_t)entries_.size());
  for (const auto& e : entries_) {
    putF32(s, e.importance);
    putU32(s, (uint32_t)e.ttlHours);   // signed stored as bit-pattern; read back as int32
    putU32(s, e.createdAtHours);
    uint8_t flags = (e.creatorFlag ? 1 : 0) | (e.permanentFlag ? 2 : 0);
    s.push_back((char)flags);
    putStr(s, e.id);
    putStr(s, e.content);
    // `source` is written straight from the model's arguments and the namespace
  // rides the same field behind a 0x1F separator. A source containing 0x1F would
  // move the split point and rewrite the entry's OWN namespace on the next load -
  // the memory would vanish from its owner's recall and stop counting against
  // their quota. Strip the separator, the way every other codec here does.
  putStr(s, e.ns.empty() ? stripSep(e.source) : (stripSep(e.source) + "\x1F" + e.ns));
    putU16(s, (uint16_t)e.vec.size());
    for (int8_t b : e.vec) s.push_back((char)b);
  }
  // Optional TRAILING usage block (v4.1): "LR1" + one u32 lastRecallHours per
  // entry, in entry order. Wire-compatible both ways: deserialize() reads
  // exactly `count` entries and ignores trailing bytes, so an OLD image loads
  // a NEW blob untouched (it just drops the stamps on its next save), and a
  // NEW image detects the marker's absence in an OLD blob (zeros = never).
  // Same spirit as the source\x1Fns pairing: never bump the magic (a magic
  // bump would clear the store on the next load - see the ns note above).
  s.append("LR1", 3);
  for (const auto& e : entries_) putU32(s, e.lastRecallHours);
  return s;
}

bool VectorMemory::deserialize(const std::string& blob) {
  entries_.clear();
  Reader r(blob);
  if (!r.need(3) || std::memcmp(r.p, kMagic, 3) != 0) return false;
  r.p += 3;
  const int dims = (int)r.u16();
  // Range-check the width from the header BEFORE it becomes the store's invariant.
  // dims<1 (esp. 0) poisons the store silently: every entry's width check then
  // passes only for zero-width vecs and all similarity math collapses. dims>ceiling
  // is a corrupt/crafted header. Either way, refuse the blob and keep the store
  // empty rather than adopt a broken width (CUM-223).
  if (dims < 1 || dims > kMaxVecDims) return false;
  dims_ = dims;
  uint32_t count = r.u32();
  for (uint32_t i = 0; i < count && r.ok; i++) {
    Stored e;
    e.importance = r.f32();
    e.ttlHours = (int32_t)r.u32();
    e.createdAtHours = r.u32();
    uint8_t flags = r.need(1) ? (uint8_t)*r.p : 0; if (r.ok) r.p += 1;
    e.creatorFlag = flags & 1;
    e.permanentFlag = flags & 2;
    e.id = r.str();
    e.content = r.str();
    e.source = r.str();
    // Split the wire-compatible "source\x1Fns" pairing (v3.7.0). A blob written
    // by a pre-v3.7 image has no separator: those entries are LEGACY and are
    // adopted into the owner's namespace by the caller - never into shared,
    // which would make every pre-existing memory world-readable exactly when
    // the boundary lands.
    {
      const size_t sep = e.source.find('\x1F');
      if (sep != std::string::npos) {
        e.ns = e.source.substr(sep + 1);
        e.source.resize(sep);
      }
    }
    uint16_t vn = r.u16();
    if (!r.need(vn)) break;
    // Enforce the store's width invariant: every stored vec MUST be dims_ wide, since
    // search/dedup/findNearest read exactly dims_ bytes per vec (cosineRaw has no
    // per-buffer size check). A blob whose entry width disagrees with dims_ (corrupt
    // header, or a cross-firmware/mixed-embed-config blob) is treated as corruption -
    // keep the clean prefix + return false - so a short vec can never cause an
    // out-of-bounds heap read later. (add() already enforces this for the RAM path.)
    if ((int)vn != dims_) { r.ok = false; break; }
    e.vec.assign((const int8_t*)r.p, (const int8_t*)r.p + vn);
    r.p += vn;
    if (r.ok) entries_.push_back(std::move(e));
  }
  // Optional trailing usage block (see serialize): only parse when every entry
  // loaded cleanly AND the marker is present; an old blob simply ends here and
  // every stamp stays 0 ("never recalled").
  // ⚠ probe WITHOUT Reader::need - need() latches ok=false on a shortfall, and
  // an OLD blob legitimately ends right here (that poisoned the whole load).
  if (r.ok && entries_.size() == count && (size_t)(r.end - r.p) >= 3 &&
      std::memcmp(r.p, "LR1", 3) == 0) {
    r.p += 3;
    for (uint32_t i = 0; i < count && r.ok; i++) {
      uint32_t v = r.u32();
      if (r.ok) entries_[i].lastRecallHours = v;
    }
    // A torn LR1 block is non-fatal: the entries themselves are intact.
    r.ok = true;
  }
  return r.ok;
}

}  // namespace orch
}  // namespace nimbus
