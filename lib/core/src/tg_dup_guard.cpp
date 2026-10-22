#include "nimbus/net/tg_dup_guard.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "nimbus/sys/breadcrumb.h"     // crc32
#include "nimbus/sys/reset_reason.h"   // isCrashReset, ResetReason

// The inbound storm breakers (see tg_dup_guard.h). Pure state over one plain record;
// the device keeps the record in RTC no-init memory and calls in on the poll task.
namespace nimbus::net {

namespace {

constexpr uint32_t kHourS = 3600;

bool sameMessage(const DupKey& a, uint32_t chat, uint32_t messageId, uint32_t date) {
  return a.messageId > 0 && a.chat == chat && (uint32_t)a.messageId == messageId &&
         (uint32_t)a.date == date;
}

// `k` repeats content seen at `seenDate` (answered at `doneAt`, 0 when unknown).
bool repeats(const DupKey& k, uint32_t chat, uint32_t content, uint32_t seenDate, uint32_t doneAt) {
  if (k.command || k.chat != chat || k.content != content) return false;
  const int64_t gap = (int64_t)(uint32_t)k.date - (int64_t)seenDate;
  if (gap > (int64_t)kRepeatWindowS || gap < -(int64_t)kRepeatWindowS) return false;
  return doneAt == 0 || (uint32_t)k.date < doneAt;   // sent before the answer went out
}

DupVerdict verdictFor(const DupKey& k, uint32_t chat, uint32_t content, uint32_t messageId,
                      uint32_t date, uint32_t doneAt) {
  if (sameMessage(k, chat, messageId, date)) return DupVerdict::Replay;
  if (repeats(k, chat, content, date, doneAt)) return DupVerdict::Repeat;
  return DupVerdict::New;
}

}  // namespace

uint32_t dupHash(const char* s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t dupHash(const std::string& s) { return dupHash(s.data(), s.size()); }

uint32_t contentKeyText(const std::string& text) { return dupHash("t:" + text); }

uint32_t contentKeyMedia(char kind, const std::string& fileUniqueId, const std::string& fileId,
                         const std::string& caption) {
  std::string k(1, kind);
  k += ':';
  k += fileUniqueId.empty() ? fileId : fileUniqueId;
  k += '\n';
  k += caption;
  return dupHash(k);
}

const char* dupVerdictName(DupVerdict v) {
  switch (v) {
    case DupVerdict::New: return "new";
    case DupVerdict::Replay: return "replay";
    case DupVerdict::Repeat: return "repeat";
  }
  return "?";
}

DupVerdict compareKeys(const DupKey& seen, uint32_t doneAt, const DupKey& k) {
  return verdictFor(k, seen.chat, seen.content, (uint32_t)seen.messageId, (uint32_t)seen.date, doneAt);
}

BootKind bootKindOf(int resetReason) {
  using nimbus::sys::ResetReason;
  if (resetReason == (int)ResetReason::Unknown || resetReason == (int)ResetReason::PowerOn ||
      resetReason == (int)ResetReason::PwrGlitch || resetReason < 0 ||
      resetReason > nimbus::sys::kLastResetReason)
    return BootKind::PowerOn;
  if (nimbus::sys::isCrashReset(resetReason) || resetReason == (int)ResetReason::Brownout)
    return BootKind::Crash;
  return BootKind::Other;
}

// ---- the record ----------------------------------------------------------------

namespace {

uint32_t crcOf(const TgGuardState& s) {
  return nimbus::sys::crc32(reinterpret_cast<const uint8_t*>(&s), offsetof(TgGuardState, crc));
}

bool slotValid(const TgGuardState& s, uint32_t build) {
  if (s.magic != kTgGuardMagic || s.build != build) return false;
  if (s.head >= kDupRing || s.count > kDupRing || s.turnHead >= kInboundTurnsPerHour) return false;
  return s.crc == crcOf(s);
}

}  // namespace

int currentSlot(const TgGuardStore& st, uint32_t build) {
  const bool v0 = slotValid(st.slot[0], build), v1 = slotValid(st.slot[1], build);
  if (v0 && v1) return (int32_t)(st.slot[1].seq - st.slot[0].seq) > 0 ? 1 : 0;
  if (v0) return 0;
  return v1 ? 1 : -1;
}

bool TgGuard::valid(uint32_t build) const { return slotValid(cur(), build); }

void TgGuard::clear(uint32_t build) {
  std::memset(&st_, 0, sizeof(st_));   // the other copy stays invalid (magic 0)
  TgGuardState& s = st_.slot[0];
  s.magic = kTgGuardMagic;
  s.build = build;
  s.seq = 1;
  s.crc = crcOf(s);
  active_ = 0;
}

TgGuardState& TgGuard::edit() {
  TgGuardState& w = st_.slot[active_ ^ 1];
  std::memcpy(&w, &cur(), sizeof(w));
  return w;
}

void TgGuard::seal(TgGuardState& w) {
  w.seq = cur().seq + 1;
  w.crc = crcOf(w);   // last: until it lands, the current copy stays the valid one
  active_ ^= 1;
}

bool TgGuard::boot(BootKind kind, uint32_t build) {
  pending_.clear();
  const int slot = kind == BootKind::PowerOn ? -1 : currentSlot(st_, build);
  if (slot < 0) {
    clear(build);
    return false;
  }
  active_ = (uint8_t)slot;
  if (kind == BootKind::Crash && cur().inflightFirstId != 0 && cur().tries < 255) {
    TgGuardState& w = edit();
    ++w.tries;
    seal(w);
  }
  return true;
}

// ---- duplicate breaker -----------------------------------------------------------

int TgGuard::answeredIndex(const DupKey& k, DupVerdict& v) const {
  int repeatOf = -1;
  for (size_t i = 0; i < cur().count; ++i) {
    const TgGuardEntry& e = cur().ring[i];
    const DupVerdict d = verdictFor(k, e.chat, e.content, e.messageId, e.date, e.doneAt);
    if (d == DupVerdict::Replay) {
      v = d;
      return (int)i;
    }
    if (d == DupVerdict::Repeat && repeatOf < 0) repeatOf = (int)i;
  }
  if (repeatOf >= 0) v = DupVerdict::Repeat;
  return repeatOf;
}

int TgGuard::pendingIndex(const DupKey& k, DupVerdict& v) const {
  int repeatOf = -1;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const DupVerdict d = compareKeys(pending_[i].key, 0, k);
    if (d == DupVerdict::Replay) {
      v = d;
      return (int)i;
    }
    if (d == DupVerdict::Repeat && repeatOf < 0) repeatOf = (int)i;
  }
  if (repeatOf >= 0) v = DupVerdict::Repeat;
  return repeatOf;
}

// A Replay anywhere wins over a Repeat (it names the very same message).
DupVerdict TgGuard::check(const DupKey& k) const {
  DupVerdict a = DupVerdict::New, p = DupVerdict::New;
  answeredIndex(k, a);
  pendingIndex(k, p);
  if (a == DupVerdict::Replay || p == DupVerdict::Replay) return DupVerdict::Replay;
  return a != DupVerdict::New ? a : p;
}

bool TgGuard::noteDuplicate(const DupKey& k) {
  bool due = false;
  DupVerdict v = DupVerdict::New;
  const int a = answeredIndex(k, v);
  int p = -1;
  if (a < 0) {
    p = pendingIndex(k, v);
    if (p >= 0) {
      due = !pending_[(size_t)p].noticed;
      pending_[(size_t)p].noticed = true;
    }
  }
  TgGuardState& w = edit();
  ++w.dupDrops;
  if (a >= 0) {
    due = !(w.ring[a].flags & kEntryNoticed);
    w.ring[a].flags |= kEntryNoticed;
  }
  seal(w);
  return due;
}

void TgGuard::notePending(const DupKey& k) {
  if (pending_.size() < 64) pending_.push_back({k, false});
}

void TgGuard::noteTurned(const DupKey& k, uint32_t nowS) {
  bool noticed = false;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const DupKey& p = pending_[i].key;
    if (p.chat != k.chat || p.messageId != k.messageId || p.date != k.date || p.content != k.content) continue;
    noticed = pending_[i].noticed;
    pending_.erase(pending_.begin() + (std::ptrdiff_t)i);
    break;
  }
  TgGuardState& w = edit();
  TgGuardEntry& e = w.ring[w.head];
  e.chat = k.chat;
  e.content = k.content;
  e.messageId = (uint32_t)k.messageId;
  e.date = (uint32_t)k.date;
  e.doneAt = nowS;
  e.flags = noticed ? kEntryNoticed : 0;
  w.head = (uint8_t)((w.head + 1) % kDupRing);
  if (w.count < kDupRing) ++w.count;
  seal(w);
}

// ---- hourly ceiling --------------------------------------------------------------

namespace {

uint32_t limitFor(bool owner) { return owner ? kInboundTurnsPerHour : kInboundTurnsPerHour - kOwnerReserveTurns; }

// The counted turns still inside the hour before `nowS`, oldest first (a turn stamped
// after `nowS`, a clock that moved back, counts as recent).
size_t recentTurns(const uint32_t* turns, uint32_t nowS, uint32_t* out) {
  size_t n = 0;
  for (size_t i = 0; i < kInboundTurnsPerHour; ++i)
    if (turns[i] && (int64_t)nowS - (int64_t)turns[i] < (int64_t)kHourS) out[n++] = turns[i];
  std::sort(out, out + n);
  return n;
}

}  // namespace

bool TgGuard::ceilingAllows(uint32_t nowS, bool owner) const {
  uint32_t recent[kInboundTurnsPerHour];
  return recentTurns(cur().turns, nowS, recent) < limitFor(owner);
}

void TgGuard::noteTurn(uint32_t nowS) {
  TgGuardState& w = edit();
  w.turns[w.turnHead] = nowS ? nowS : 1;   // 0 marks an empty slot
  w.turnHead = (uint8_t)((w.turnHead + 1) % kInboundTurnsPerHour);
  seal(w);
}

uint32_t TgGuard::ceilingResumeAt(uint32_t nowS, bool owner) const {
  uint32_t recent[kInboundTurnsPerHour];
  const size_t n = recentTurns(cur().turns, nowS, recent);
  const uint32_t limit = limitFor(owner);
  if (n < limit) return 0;
  return recent[n - limit] + kHourS;   // that many must age out before one more may run
}

bool TgGuard::noteCeilingSkip(uint32_t nowS, bool owner) {
  const uint32_t resume = ceilingResumeAt(nowS, owner);
  TgGuardState& w = edit();
  ++w.ceilingDrops;
  uint32_t& until = owner ? w.ceilingNoticeUntil : w.othersNoticeUntil;
  const bool due = (int64_t)nowS >= (int64_t)until;
  if (due) until = resume;
  seal(w);
  return due;
}

// ---- poison guard ----------------------------------------------------------------

bool TgGuard::poisoned(int32_t id, uint8_t phase) {
  const TgGuardState& c = cur();
  if (id <= 0 || c.inflightFirstId != id || c.inflightPhase != phase || c.tries < kPoisonMaxTries) return false;
  TgGuardState& w = edit();
  ++w.poisonAcks;
  w.inflightFirstId = 0;
  w.inflightPhase = 0;
  w.tries = 0;
  seal(w);
  return true;
}

void TgGuard::mark(int32_t id, uint8_t phase) {
  if (id <= 0 || (cur().inflightFirstId == id && cur().inflightPhase == phase)) return;   // keep its tries
  TgGuardState& w = edit();
  w.inflightFirstId = id;
  w.inflightPhase = phase;
  w.tries = 0;
  seal(w);
}

void TgGuard::commitPage() {
  if (cur().inflightFirstId == 0 && cur().tries == 0) return;
  TgGuardState& w = edit();
  w.inflightFirstId = 0;
  w.inflightPhase = 0;
  w.tries = 0;
  seal(w);
}

}  // namespace nimbus::net
