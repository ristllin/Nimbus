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

bool TgGuard::valid(uint32_t build) const {
  if (s_.magic != kTgGuardMagic || s_.build != build) return false;
  if (s_.head >= kDupRing || s_.count > kDupRing || s_.turnHead >= kInboundTurnsPerHour) return false;
  return s_.crc == nimbus::sys::crc32(reinterpret_cast<const uint8_t*>(&s_), offsetof(TgGuardState, crc));
}

void TgGuard::clear(uint32_t build) {
  std::memset(&s_, 0, sizeof(s_));
  s_.magic = kTgGuardMagic;
  s_.build = build;
  seal();
}

void TgGuard::seal() {
  s_.crc = nimbus::sys::crc32(reinterpret_cast<const uint8_t*>(&s_), offsetof(TgGuardState, crc));
}

bool TgGuard::boot(BootKind kind, uint32_t build) {
  pending_.clear();
  if (kind == BootKind::PowerOn || !valid(build)) {
    clear(build);
    return false;
  }
  if (kind == BootKind::Crash && s_.inflightFirstId != 0 && s_.tries < 255) ++s_.tries;
  seal();
  return true;
}

// ---- duplicate breaker -----------------------------------------------------------

int TgGuard::answeredIndex(const DupKey& k, DupVerdict& v) const {
  int repeatOf = -1;
  for (size_t i = 0; i < s_.count; ++i) {
    const TgGuardEntry& e = s_.ring[i];
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
  ++s_.dupDrops;
  bool due = false;
  DupVerdict v = DupVerdict::New;
  const int a = answeredIndex(k, v);
  if (a >= 0) {
    due = !(s_.ring[a].flags & kEntryNoticed);
    s_.ring[a].flags |= kEntryNoticed;
  } else {
    const int p = pendingIndex(k, v);
    if (p >= 0) {
      due = !pending_[(size_t)p].noticed;
      pending_[(size_t)p].noticed = true;
    }
  }
  seal();
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
  TgGuardEntry& e = s_.ring[s_.head];
  e.chat = k.chat;
  e.content = k.content;
  e.messageId = (uint32_t)k.messageId;
  e.date = (uint32_t)k.date;
  e.doneAt = nowS;
  e.flags = noticed ? kEntryNoticed : 0;
  s_.head = (uint8_t)((s_.head + 1) % kDupRing);
  if (s_.count < kDupRing) ++s_.count;
  seal();
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
  return recentTurns(s_.turns, nowS, recent) < limitFor(owner);
}

void TgGuard::noteTurn(uint32_t nowS) {
  s_.turns[s_.turnHead] = nowS ? nowS : 1;   // 0 marks an empty slot
  s_.turnHead = (uint8_t)((s_.turnHead + 1) % kInboundTurnsPerHour);
  seal();
}

uint32_t TgGuard::ceilingResumeAt(uint32_t nowS, bool owner) const {
  uint32_t recent[kInboundTurnsPerHour];
  const size_t n = recentTurns(s_.turns, nowS, recent);
  const uint32_t limit = limitFor(owner);
  if (n < limit) return 0;
  return recent[n - limit] + kHourS;   // that many must age out before one more may run
}

bool TgGuard::noteCeilingSkip(uint32_t nowS, bool owner) {
  ++s_.ceilingDrops;
  uint32_t& until = owner ? s_.ceilingNoticeUntil : s_.othersNoticeUntil;
  const bool due = (int64_t)nowS >= (int64_t)until;
  if (due) until = ceilingResumeAt(nowS, owner);
  seal();
  return due;
}

// ---- poison guard ----------------------------------------------------------------

bool TgGuard::poisoned(int32_t id, uint8_t phase) {
  if (id <= 0 || s_.inflightFirstId != id || s_.inflightPhase != phase || s_.tries < kPoisonMaxTries)
    return false;
  ++s_.poisonAcks;
  s_.inflightFirstId = 0;
  s_.inflightPhase = 0;
  s_.tries = 0;
  seal();
  return true;
}

void TgGuard::mark(int32_t id, uint8_t phase) {
  if (id <= 0 || (s_.inflightFirstId == id && s_.inflightPhase == phase)) return;   // keep its tries
  s_.inflightFirstId = id;
  s_.inflightPhase = phase;
  s_.tries = 0;
  seal();
}

void TgGuard::commitPage() {
  if (s_.inflightFirstId == 0 && s_.tries == 0) return;
  s_.inflightFirstId = 0;
  s_.inflightPhase = 0;
  s_.tries = 0;
  seal();
}

}  // namespace nimbus::net
