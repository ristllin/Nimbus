// Ported and adapted from Nuage-Solide src/agent/journal.cpp (Head Orchestrator
// v2). The Preferences NVS I/O + logbuf are hoisted behind the JournalStore seam;
// the record shape, compact-JSON serialization, tag dedupe, seen-eviction, count/
// get/gc, and the serialize-refuses-over-buffer guard are preserved verbatim.
//
// Original NVS layout (now owned by the device JournalStore impl): namespace
// "agjournal", keys "j0".."j<kAgentMaxJobs-1>", each value a compact JSON string
// {tag,jid,be,cat,mdl,cid,st,seen} (~200 B typical, ~293 B worst case).
#include "nimbus/orch/journal.h"

#include <ArduinoJson.h>

#include <cstring>

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

void Journal::saveSlot(int i) {
  char buf[kJournalSerializeBuf];  // worst-case record ~293 B with jobId[96]
  const size_t need = serializeRecord(records_[i], buf, sizeof(buf));
  if (need + 1 > sizeof(buf)) {
    // Refuse to persist a truncated (thus unparseable, silently-dropped-on-reboot)
    // record - that would orphan a billed remote job.
    return;
  }
  if (store_) store_->put(i, buf);
}

void Journal::begin(JournalStore* store) {
  store_ = store;
  count_ = 0;
  nextSeq_ = 0;
  if (!store_) return;
  for (int i = 0; i < kAgentMaxJobs; i++) {
    const std::string val = store_->get(i);
    if (val.empty()) continue;
    if (deserializeRecord(val.c_str(), records_[count_])) {
      seq_[count_] = nextSeq_++;  // load order == age order for this session
      count_++;
    }
  }
}

int Journal::findSlot(const char* tag) const {
  for (int i = 0; i < count_; i++)
    if (std::strcmp(records_[i].tag, tag) == 0) return i;
  return -1;
}

bool Journal::write(const JobRecord& r) {
  int slot = findSlot(r.tag);  // dedupe by tag
  if (slot < 0) {
    if (count_ >= kAgentMaxJobs) {
      // Full: evict the OLDEST seen record - the resultSeen slot with the smallest
      // insertion seq_, NOT merely the first in slot order (a prior eviction can
      // leave a newer seen record at a lower index). Eviction only ever targets a
      // resultSeen slot; a full journal of live jobs refuses the write.
      for (int i = 0; i < count_; i++) {
        if (records_[i].resultSeen && (slot < 0 || seq_[i] < seq_[slot])) slot = i;
      }
      if (slot < 0) return false;  // "journal: full, cannot write"
    } else {
      slot = count_++;
    }
    seq_[slot] = nextSeq_++;  // a NEW record (new slot or eviction) is the newest
  }
  records_[slot] = r;
  saveSlot(slot);
  return true;
}

bool Journal::markSeen(const char* tag) {
  const int i = findSlot(tag);
  if (i < 0) return false;
  records_[i].resultSeen = true;
  saveSlot(i);
  return true;
}

bool Journal::update(const char* tag, JobState s) {
  const int i = findSlot(tag);
  if (i < 0) return false;
  records_[i].state = s;
  saveSlot(i);
  return true;
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
  // Compact: keep only the active records, shifting them down into slots 0..w-1.
  int w = 0;
  for (int i = 0; i < count_; i++) {
    if (!records_[i].resultSeen) {
      if (w != i) { records_[w] = records_[i]; seq_[w] = seq_[i]; saveSlot(w); }
      w++;
    }
  }
  // Clear the vacated tail slots.
  if (store_)
    for (int i = w; i < count_; i++) store_->remove(i);
  count_ = w;
}

void Journal::clearAll() {
  if (store_) store_->clearNs();
  count_ = 0;
  nextSeq_ = 0;
}

}  // namespace orch
}  // namespace nimbus
