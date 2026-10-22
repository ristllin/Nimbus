// Ported and adapted from Nuage-Solide src/agent/journal.cpp (Head Orchestrator
// v2). The Preferences NVS I/O + logbuf are hoisted behind the JournalStore seam;
// the record shape, compact-JSON serialization, tag dedupe, seen-eviction, count/
// get/gc, and the serialize-refuses-over-buffer guard are preserved verbatim.
//
// Record JSON (the same in the flash file and in the legacy NVS layout, namespace
// "agjournal", keys "j0".."j<kAgentMaxJobs-1>"): {tag,jid,be,cat,mdl,cid,st,seen}
// (~200 B typical, ~293 B worst case). Finished-ring entries:
// {tag,nm,prj,art,at,ts,q,cid,fail}.
#include "nimbus/orch/journal.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/store/moved_store.h"   // mayRetireLegacy - the moved-store rule

namespace nimbus {
namespace orch {

size_t serializeRecord(const JobRecord& r, char* buf, size_t sz) {
  JsonDocument d;
  d["tag"]  = r.tag;
  d["jid"]  = r.jobId;
  d["be"]   = r.backend;
  d["cat"]  = r.category;
  d["mdl"]  = r.model;
  if (r.name[0]) d["nm"] = r.name;   // omit when empty (older records stay byte-stable)
  if (r.prj[0])  d["prj"] = r.prj;   // v4.0.0 auto-persist target (optional key)
  d["cid"]  = r.chatId;
  d["st"]   = (uint8_t)r.state;
  d["seen"] = r.resultSeen;
  const size_t need = measureJson(d);           // exact bytes, excl. NUL
  if (need + 1 > sz) {                            // would not fit → persist NOTHING
    if (sz) buf[0] = 0;
    return need;
  }
  serializeJson(d, buf, sz);
  return need;
}

bool deserializeRecord(const char* buf, JobRecord& r) {
  JsonDocument d;
  if (deserializeJson(d, buf)) return false;
  std::memset(&r, 0, sizeof(r));
  std::strncpy(r.tag,      d["tag"] | "", sizeof(r.tag)      - 1);
  std::strncpy(r.jobId,    d["jid"] | "", sizeof(r.jobId)    - 1);
  std::strncpy(r.backend,  d["be"]  | "", sizeof(r.backend)  - 1);
  std::strncpy(r.category, d["cat"] | "", sizeof(r.category) - 1);
  std::strncpy(r.model,    d["mdl"] | "", sizeof(r.model)    - 1);
  std::strncpy(r.name,     d["nm"]  | "", sizeof(r.name)     - 1);   // "" = pre-name record
  std::strncpy(r.prj,      d["prj"] | "", sizeof(r.prj)      - 1);   // "" = no persist
  std::strncpy(r.chatId,   d["cid"] | "", sizeof(r.chatId)   - 1);
  r.state      = (JobState)(d["st"] | (uint8_t)JobState::Unknown);
  r.resultSeen = d["seen"] | false;
  r.dispatchedAt = 0;  // advisory; resets on reboot (not persisted)
  return r.tag[0] != 0;
}

size_t serializeFinished(const FinishedJob& f, char* buf, size_t sz) {
  JsonDocument d;
  d["tag"] = f.tag;
  if (f.name[0]) d["nm"] = f.name;
  if (f.prj[0]) d["prj"] = f.prj;
  if (f.artifact[0]) d["art"] = f.artifact;
  if (f.finishedAt[0]) d["at"] = f.finishedAt;
  if (f.finishedEpoch) d["ts"] = f.finishedEpoch;
  d["q"] = f.seq;
  d["cid"] = f.chatId;
  if (f.failed) d["fail"] = true;
  const size_t need = measureJson(d);
  if (need + 1 > sz) {
    if (sz) buf[0] = 0;
    return need;
  }
  serializeJson(d, buf, sz);
  return need;
}

bool deserializeFinished(const char* buf, FinishedJob& f) {
  JsonDocument d;
  if (deserializeJson(d, buf)) return false;
  std::memset(&f, 0, sizeof(f));
  std::strncpy(f.tag,        d["tag"] | "", sizeof(f.tag) - 1);
  std::strncpy(f.name,       d["nm"]  | "", sizeof(f.name) - 1);
  std::strncpy(f.prj,        d["prj"] | "", sizeof(f.prj) - 1);
  std::strncpy(f.artifact,   d["art"] | "", sizeof(f.artifact) - 1);
  std::strncpy(f.finishedAt, d["at"]  | "", sizeof(f.finishedAt) - 1);
  std::strncpy(f.chatId,     d["cid"] | "", sizeof(f.chatId) - 1);
  f.finishedEpoch = d["ts"] | 0u;
  f.seq = d["q"] | 0u;
  f.failed = d["fail"] | false;
  return f.tag[0] != 0;
}

bool Journal::saveSlot(int i) {
  char buf[kJournalSerializeBuf];  // worst-case record ~293 B with jobId[96]
  const size_t need = serializeRecord(records_[i], buf, sizeof(buf));
  if (need + 1 > sizeof(buf)) {
    // Refuse to persist a truncated (thus unparseable, silently-dropped-on-reboot)
    // record - that would orphan a billed remote job.
    return false;
  }
  return store_ ? store_->put(slot_[i], buf) : false;
}

void Journal::begin(JournalStore* store) {
  store_ = store;
  count_ = 0;
  nextSeq_ = 0;
  finCount_ = 0;
  nextFinSeq_ = 0;
  fin_.clear();
  fin_.reserve(kJournalFinishedKeep);
  if (!store_) return;
  loadFinished();
  for (int s = 0; s < kAgentMaxJobs; s++) {
    const std::string val = store_->get(s);
    if (val.empty()) continue;
    JobRecord& r = records_[count_];
    if (!deserializeRecord(val.c_str(), r)) continue;   // malformed: the slot is free
    if (findSlot(r.tag) >= 0) {
      // A duplicate of an earlier slot (an older gc's copy whose source erase did not
      // land): keep the first, drop this one so it cannot re-attach twice.
      (void)store_->remove(s);   // a failed erase only leaves the copy for the next boot
      continue;
    }
    slot_[count_] = s;
    seq_[count_] = nextSeq_++;  // load order == age order for this session
    restored_[count_] = true;
    count_++;
    // Finished and delivered before the restart (the ring entry lands before the
    // seen mark): never poll or deliver it again.
    if (!r.resultSeen && finishedSlotFor(r.tag) >= 0) {
      r.resultSeen = true;
      (void)saveSlot(count_ - 1);   // RAM is authoritative this boot; a later write retries
    }
  }
}

void Journal::loadFinished() {
  for (int s = kJournalFinishedSlot0; s < kJournalSlots; s++) {
    const std::string val = store_->get(s);
    if (val.empty()) continue;
    FinEntry e;
    if (!deserializeFinished(val.c_str(), e.job)) continue;
    e.slot = s;
    e.beforeBoot = true;
    if (e.job.seq >= nextFinSeq_) nextFinSeq_ = e.job.seq + 1;
    fin_.push_back(e);   // capacity reserved: no growth
    finCount_++;
  }
}

int Journal::finishedSlotFor(const char* tag) const {
  for (int i = 0; i < finCount_; i++)
    if (std::strcmp(fin_[i].job.tag, tag) == 0) return i;
  return -1;
}

bool Journal::saveFinished(int i) {
  char buf[kJournalSerializeBuf];
  const size_t need = serializeFinished(fin_[i].job, buf, sizeof(buf));
  if (need + 1 > sizeof(buf)) return false;
  return store_ ? store_->put(fin_[i].slot, buf) : false;
}

// Ring index for a new entry: a free store slot while the ring has room (the lowest
// number), else the oldest entry's.
int Journal::finishedTarget() {
  if (finCount_ >= kJournalFinishedKeep) {
    int i = 0;
    for (int k = 1; k < finCount_; k++)
      if (fin_[k].job.seq < fin_[i].job.seq) i = k;
    return i;
  }
  for (int s = kJournalFinishedSlot0; s < kJournalSlots; s++) {
    bool used = false;
    for (int k = 0; k < finCount_ && !used; k++) used = fin_[k].slot == s;
    if (used) continue;
    if (fin_.capacity() < kJournalFinishedKeep) fin_.reserve(kJournalFinishedKeep);
    FinEntry e;
    e.slot = s;
    fin_.push_back(e);
    return finCount_++;
  }
  return -1;   // unreachable: a ring with room has a free slot
}

bool Journal::finish(const char* tag, const FinishedJob& f, JobState final) {
  const int i = finishedTarget();
  if (i < 0) return false;
  FinEntry& e = fin_[i];
  e.job = f;
  std::strncpy(e.job.tag, tag, sizeof(e.job.tag) - 1);
  e.job.tag[sizeof(e.job.tag) - 1] = 0;
  e.job.seq = nextFinSeq_++;
  e.beforeBoot = false;
  const bool ringLanded = saveFinished(i);
  const int r = findSlot(tag);
  if (r < 0) return ringLanded;
  if (final != JobState::Unknown) records_[r].state = final;
  records_[r].resultSeen = true;
  const bool seenLanded = saveSlot(r);
  return ringLanded && seenLanded;
}

bool Journal::finished(int idx, FinishedJob& out, bool* beforeBoot) const {
  if (idx < 0 || idx >= finCount_) return false;
  // Newest first: the idx-th highest seq (the ring is at most 8 entries).
  bool taken[kJournalFinishedKeep] = {};
  int pick = 0;
  for (int n = 0; n <= idx; n++) {
    pick = -1;
    for (int k = 0; k < finCount_; k++)
      if (!taken[k] && (pick < 0 || fin_[k].job.seq > fin_[pick].job.seq)) pick = k;
    taken[pick] = true;
  }
  out = fin_[pick].job;
  if (beforeBoot) *beforeBoot = fin_[pick].beforeBoot;
  return true;
}

bool Journal::restored(const char* tag) const {
  const int i = findSlot(tag);
  return i >= 0 && restored_[i];
}

static long tagNumber(const char* tag) {
  if (std::strncmp(tag, "job", 3) != 0 || !tag[3]) return -1;
  char* end = nullptr;
  const long n = std::strtol(tag + 3, &end, 10);
  return end && *end == 0 && n >= 0 ? n : -1;
}

long Journal::highestTagNumber() const {
  long hi = -1;
  for (int i = 0; i < count_; i++) {
    const long n = tagNumber(records_[i].tag);
    if (n > hi) hi = n;
  }
  for (int i = 0; i < finCount_; i++) {
    const long n = tagNumber(fin_[i].job.tag);
    if (n > hi) hi = n;
  }
  return hi;
}

int Journal::findSlot(const char* tag) const {
  for (int i = 0; i < count_; i++)
    if (std::strcmp(records_[i].tag, tag) == 0) return i;
  return -1;
}

int Journal::freeStoreSlot() const {
  for (int s = 0; s < kAgentMaxJobs; s++) {
    bool used = false;
    for (int i = 0; i < count_ && !used; i++) used = slot_[i] == s;
    if (!used) return s;
  }
  return -1;
}

bool Journal::write(const JobRecord& r) {
  int slot = findSlot(r.tag);  // dedupe by tag
  if (slot < 0) {
    if (count_ >= kAgentMaxJobs) {
      // Full: evict the OLDEST seen record - the resultSeen slot with the smallest
      // insertion seq_, NOT merely the first in slot order (a prior eviction can
      // leave a newer seen record at a lower index). Eviction only ever targets a
      // resultSeen slot; a full journal of live jobs refuses the write. The new
      // record takes over the evicted record's store slot.
      for (int i = 0; i < count_; i++) {
        if (records_[i].resultSeen && (slot < 0 || seq_[i] < seq_[slot])) slot = i;
      }
      if (slot < 0) return false;  // "journal: full, cannot write"
    } else {
      const int s = freeStoreSlot();
      if (s < 0) return false;     // unreachable while count_ < kAgentMaxJobs
      slot = count_++;
      slot_[slot] = s;
    }
    seq_[slot] = nextSeq_++;  // a NEW record (new slot or eviction) is the newest
    restored_[slot] = false;
  }
  records_[slot] = r;
  return saveSlot(slot);
}

bool Journal::markSeen(const char* tag) {
  const int i = findSlot(tag);
  if (i < 0) return false;
  records_[i].resultSeen = true;
  return saveSlot(i);
}

bool Journal::update(const char* tag, JobState s) {
  const int i = findSlot(tag);
  if (i < 0) return false;
  records_[i].state = s;
  return saveSlot(i);
}

int Journal::count() const {
  int n = 0;
  for (int i = 0; i < count_; i++)
    if (!records_[i].resultSeen) n++;
  return n;
}

bool Journal::get(int idx, JobRecord& out) const {
  int n = 0;
  for (int i = 0; i < count_; i++) {
    if (records_[i].resultSeen) continue;
    if (n == idx) { out = records_[i]; return true; }
    n++;
  }
  return false;
}

void Journal::gc() {
  // Keep every active record where it is; drop a seen record only once its slot is
  // gone from the store. Nothing is copied, so nothing depends on a copy landing.
  int w = 0;
  for (int i = 0; i < count_; i++) {
    const bool drop = records_[i].resultSeen && (!store_ || store_->remove(slot_[i]));
    if (drop) continue;
    if (w != i) {
      records_[w] = records_[i];
      seq_[w] = seq_[i];
      slot_[w] = slot_[i];
      restored_[w] = restored_[i];
    }
    w++;
  }
  count_ = w;
}

void Journal::clearAll() {
  if (store_) store_->clearNs();
  count_ = 0;
  nextSeq_ = 0;
  finCount_ = 0;
  nextFinSeq_ = 0;
  fin_.clear();
}

// ---- the file store ------------------------------------------------------------------

std::string encodeJournalFile(const std::string (&slots)[kJournalSlots]) {
  std::string out = "{";
  bool first = true;
  for (int s = 0; s < kJournalSlots; s++) {
    if (slots[s].empty()) continue;
    if (!first) out += ',';
    first = false;
    out += '"';
    out += std::to_string(s);
    out += "\":";
    out += slots[s];   // the record's own compact JSON
  }
  out += '}';
  return out;
}

bool decodeJournalFile(const std::string& blob, std::string (&slots)[kJournalSlots]) {
  JsonDocument d;
  if (deserializeJson(d, blob) || !d.is<JsonObject>()) return false;
  std::string parsed[kJournalSlots];
  for (JsonPairConst kv : d.as<JsonObjectConst>()) {
    char* end = nullptr;
    const long s = std::strtol(kv.key().c_str(), &end, 10);
    if (!end || *end || s < 0 || s >= kJournalSlots || !kv.value().is<JsonObjectConst>()) continue;
    serializeJson(kv.value(), parsed[s]);
  }
  for (int s = 0; s < kJournalSlots; s++) slots[s] = parsed[s];
  return true;
}

JournalSource FileJournalStore::load() {
  for (auto& v : slots_) v.clear();
  std::string blob;
  onDisk_ = io_.readFile(blob) && decodeJournalFile(blob, slots_);
  if (onDisk_) return JournalSource::File;
  bool any = false;
  for (int s = 0; s < kAgentMaxJobs; s++) {
    slots_[s] = io_.readLegacySlot(s);
    any = any || !slots_[s].empty();
  }
  if (!any) return JournalSource::None;   // a first write creates the file
  durable_ = onDisk_ = io_.writeFileAtomic(encodeJournalFile(slots_));
  return JournalSource::Legacy;
}

bool FileJournalStore::retireLegacy(bool imageValidAtBoot) {
  if (!imageValidAtBoot) return false;
  std::string blob, back[kJournalSlots];
  const bool fileReadsBack = io_.readFile(blob) && decodeJournalFile(blob, back);
  bool legacy = false;
  for (int s = 0; s < kAgentMaxJobs && !legacy; s++) legacy = !io_.readLegacySlot(s).empty();
  if (!nimbus::store::mayRetireLegacy(imageValidAtBoot, fileReadsBack, legacy)) return false;
  return io_.eraseLegacy();
}

std::string FileJournalStore::get(int slot) {
  return slot >= 0 && slot < kJournalSlots ? slots_[slot] : std::string();
}

bool FileJournalStore::commit(int slot, const std::string& v) {
  if (slot < 0 || slot >= kJournalSlots) return false;
  if (slots_[slot] == v && onDisk_) return true;   // the file already holds it
  std::string next[kJournalSlots];
  for (int s = 0; s < kJournalSlots; s++) next[s] = s == slot ? v : slots_[s];
  durable_ = io_.writeFileAtomic(encodeJournalFile(next));
  if (!durable_) return false;   // the file keeps its previous image (tmp + rename)
  slots_[slot] = v;
  onDisk_ = true;
  return true;
}

bool FileJournalStore::put(int slot, const std::string& v) { return commit(slot, v); }

bool FileJournalStore::remove(int slot) { return commit(slot, std::string()); }

void FileJournalStore::clearNs() {
  std::string none[kJournalSlots];
  durable_ = io_.writeFileAtomic(encodeJournalFile(none));
  if (durable_) {
    for (auto& v : slots_) v.clear();
    onDisk_ = true;
  }
  (void)io_.eraseLegacy();   // a debug wipe: the legacy copy must not come back either
}

}  // namespace orch
}  // namespace nimbus
