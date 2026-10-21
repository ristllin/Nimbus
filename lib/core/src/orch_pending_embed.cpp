#include "nimbus/orch/pending_embed.h"

#include <cstring>

#include "nimbus/orch/caps.h"   // kMemModelMax

namespace nimbus {
namespace orch {

namespace {

// ---- little-endian codec helpers (same shape as the vector archive) ---------
void putU16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xff)); s.push_back((char)((v >> 8) & 0xff)); }
void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xff)); }
void putF32(std::string& s, float f) { uint32_t u; std::memcpy(&u, &f, 4); putU32(s, u); }
void putStr(std::string& s, const std::string& v) {
  const size_t n = v.size() > 0xffff ? 0xffff : v.size();
  putU16(s, (uint16_t)n);
  s.append(v, 0, n);
}

struct Reader {
  const char* p; const char* end; bool ok = true;
  Reader(const char* b, const char* e) : p(b), end(e) {}
  bool need(size_t n) { if (!ok || (size_t)(end - p) < n) { ok = false; return false; } return true; }
  uint8_t  u8()  { if (!need(1)) return 0; return (uint8_t)*p++; }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint8_t)p[0] | ((uint8_t)p[1] << 8); p += 2; return v; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)p[i] << (8 * i); p += 4; return v; }
  float    f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
  std::string str() { uint16_t n = u16(); if (!need(n)) return ""; std::string v(p, n); p += n; return v; }
};

constexpr char    kMagic[4] = {'P', 'E', 'Q', '1'};
constexpr uint8_t kVersion = 1;

// One record's body (without its u16 length prefix). New fields are only ever
// APPENDED, and a reader skips what it does not know, so an older build reads a
// newer blob's known prefix of every record.
std::string encodeBody(const PendingEmbed& e) {
  std::string b;
  b.push_back((char)e.op);
  uint8_t flags = 0;
  if (e.permanent) flags |= 1;
  if (e.clockSynced) flags |= 2;
  b.push_back((char)flags);
  putF32(b, e.importance);
  putU32(b, e.createdAtHours);
  putU16(b, e.attempts);
  b.push_back((char)e.lastFail);
  putStr(b, e.id);
  putStr(b, e.ns);
  putStr(b, e.content);
  putStr(b, e.ttlName);
  putStr(b, e.source);
  putStr(b, e.oldId);
  putStr(b, e.oldDesc);
  return b;
}

bool decodeBody(Reader& r, PendingEmbed& e) {
  const uint8_t op = r.u8();
  const uint8_t flags = r.u8();
  e.importance = r.f32();
  e.createdAtHours = r.u32();
  e.attempts = r.u16();
  e.lastFail = r.u8();
  e.id = r.str();
  e.ns = r.str();
  e.content = r.str();
  e.ttlName = r.str();
  e.source = r.str();
  e.oldId = r.str();
  e.oldDesc = r.str();
  if (!r.ok) return false;
  if (op > (uint8_t)PendingOp::Update) return false;   // an op this build cannot run
  e.op = (PendingOp)op;
  e.permanent = flags & 1;
  e.clockSynced = flags & 2;
  if (!(e.importance >= 0.0f && e.importance <= 1.0f)) e.importance = 0.5f;   // NaN-safe
  return !e.id.empty() && !e.content.empty();
}

bool nsAllowed(const std::string& ns, const std::vector<std::string>& allow) {
  if (allow.empty()) return true;
  const std::string eff = ns.empty() ? std::string(kOwnerNs) : ns;
  for (const auto& a : allow) if (a == eff) return true;
  return false;
}

bool isOwnerNs(const std::string& ns) { return ns.empty() || ns == kOwnerNs; }
}  // namespace

// ---- PendingEmbeds ------------------------------------------------------------
size_t PendingEmbeds::entryBytes(const PendingEmbed& e) { return 2 + encodeBody(e).size(); }

size_t PendingEmbeds::bytes() const {
  size_t n = 0;
  for (const auto& e : entries_) n += entryBytes(e);
  return n;
}

int PendingEmbeds::indexOf(const std::string& id) const {
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].id == id) return (int)i;
  return -1;
}

int PendingEmbeds::countIn(const std::string& ns) const {
  const bool owner = isOwnerNs(ns);
  int n = 0;
  for (const auto& e : entries_)
    if (owner ? isOwnerNs(e.ns) : e.ns == ns) n++;
  return n;
}

PendingEmbeds::Add PendingEmbeds::enqueue(const PendingEmbed& e) {
  if (e.id.empty() || e.content.empty()) return Add::Empty;
  if (e.content.size() > (size_t)kMemModelMax) return Add::TooLong;
  const int at = indexOf(e.id);
  if (at >= 0) {
    // The same fact again: one entry. Keep the stronger claim on it.
    PendingEmbed& q = entries_[(size_t)at];
    if (e.importance > q.importance) q.importance = e.importance;
    q.permanent = q.permanent || e.permanent;
    // An update intent wins over a plain write of the same text: the old fact it
    // names must still be replaced when the entry is stored.
    if (e.op == PendingOp::Update && q.op == PendingOp::Write) {
      q.op = PendingOp::Update;
      q.oldId = e.oldId;
      q.oldDesc = e.oldDesc;
    }
    dirty_ = true;
    return Add::Coalesced;
  }
  if (entries_.size() >= kMaxEntries) return Add::Full;
  if (bytes() + entryBytes(e) > kMaxBytes) return Add::BytesFull;
  if (!isOwnerNs(e.ns) && countIn(e.ns) >= kMaxPerMember) return Add::NsFull;
  entries_.push_back(e);
  dirty_ = true;
  return Add::Added;
}

const PendingEmbed* PendingEmbeds::front() const { return entries_.empty() ? nullptr : &entries_[0]; }

const PendingEmbed* PendingEmbeds::find(const std::string& id) const {
  const int at = indexOf(id);
  return at < 0 ? nullptr : &entries_[(size_t)at];
}

bool PendingEmbeds::pop(const std::string& id) {
  const int at = indexOf(id);
  if (at < 0) return false;
  entries_.erase(entries_.begin() + at);
  dirty_ = true;
  return true;
}

void PendingEmbeds::noteFailed(const std::string& id, EmbedFail why) {
  const int at = indexOf(id);
  if (at < 0) return;
  PendingEmbed e = entries_[(size_t)at];
  if (e.attempts < 0xffff) e.attempts++;
  e.lastFail = (uint8_t)why;
  entries_.erase(entries_.begin() + at);
  entries_.push_back(std::move(e));
  dirty_ = true;
}

std::vector<PendingEmbed> PendingEmbeds::getAll(const std::vector<std::string>& nsAllow) const {
  std::vector<PendingEmbed> out;
  for (const auto& e : entries_)
    if (nsAllowed(e.ns, nsAllow)) out.push_back(e);
  return out;
}

int PendingEmbeds::removeNs(const std::string& ns) {
  int n = 0;
  for (size_t i = entries_.size(); i-- > 0;)
    if (entries_[i].ns == ns) { entries_.erase(entries_.begin() + (long)i); n++; }
  for (size_t i = notes_.size(); i-- > 0;)
    if (notes_[i].ns == ns) notes_.erase(notes_.begin() + (long)i);
  if (n) dirty_ = true;
  return n;
}

int PendingEmbeds::flushAll() {
  const int n = (int)entries_.size();
  entries_.clear();
  notes_.clear();
  dirty_ = true;
  return n;
}

std::string PendingEmbeds::serialize() const {
  std::string s(kMagic, 4);
  s.push_back((char)kVersion);
  putU16(s, (uint16_t)entries_.size());
  for (const auto& e : entries_) {
    const std::string body = encodeBody(e);
    putU16(s, (uint16_t)body.size());
    s += body;
  }
  return s;
}

bool PendingEmbeds::deserialize(const std::string& blob) {
  entries_.clear();
  dirty_ = false;
  Reader r(blob.data(), blob.data() + blob.size());
  if (!r.need(4) || std::memcmp(r.p, kMagic, 4) != 0) return false;
  r.p += 4;
  const uint8_t ver = r.u8();
  if (!r.ok || ver < 1) return false;
  const uint16_t count = r.u16();
  for (uint16_t i = 0; i < count && r.ok; i++) {
    const uint16_t len = r.u16();
    if (!r.need(len)) break;
    Reader rec(r.p, r.p + len);
    r.p += len;   // skip the whole record, whatever this build understood of it
    PendingEmbed e;
    if (!decodeBody(rec, e)) { r.ok = false; break; }
    // The bounds hold on load too: a crafted or foreign blob cannot grow the queue
    // past them, and a duplicate id keeps its first copy.
    if (indexOf(e.id) >= 0) continue;
    if (entries_.size() >= kMaxEntries) { r.ok = false; break; }
    entries_.push_back(std::move(e));
  }
  return r.ok;
}

void PendingEmbeds::addNote(const std::string& ns, const std::string& text) {
  if (notes_.size() >= kMaxNotes) notes_.erase(notes_.begin());
  notes_.push_back(Note{ns, text});
}

std::string PendingEmbeds::takeNotes(const std::string& ns) {
  std::string out;
  const bool owner = isOwnerNs(ns);
  for (size_t i = 0; i < notes_.size();) {
    const bool mine = owner ? isOwnerNs(notes_[i].ns) : notes_[i].ns == ns;
    if (mine) {
      out += notes_[i].text + "\n";
      notes_.erase(notes_.begin() + (long)i);
    } else {
      i++;
    }
  }
  return out;
}

// ---- EmbedPark ----------------------------------------------------------------
ParkRule parkRuleFor(EmbedFail kind) {
  // Indexed by EmbedFail value (append-only); the host test walks the enum.
  static const ParkRule kRules[] = {
      ParkRule::Clear,     // None
      ParkRule::Long,      // NoKey
      ParkRule::Long,      // KeyRejected
      ParkRule::Retry,     // Busy: the local TLS slot, not the provider
      ParkRule::Backoff,   // Unreachable
      ParkRule::Backoff,   // Timeout
      ParkRule::Long,      // OutOfCredit
      ParkRule::Backoff,   // RateLimited
      ParkRule::Long,      // RouteNotAllowed
      ParkRule::Backoff,   // ProviderError
      ParkRule::Backoff,   // BadResponse
      ParkRule::Long,      // WidthMismatch
      ParkRule::Long,      // NoModel
      ParkRule::NoChange,  // BadRequest: nothing to embed says nothing about the provider
      ParkRule::Backoff,   // Unknown
  };
  static_assert(sizeof kRules / sizeof kRules[0] == (size_t)EmbedFail::Unknown + 1,
                "every EmbedFail needs a park rule");
  const size_t i = (size_t)kind;
  return i < sizeof kRules / sizeof kRules[0] ? kRules[i] : ParkRule::Backoff;
}

bool EmbedPark::blocked(uint32_t nowMs) const {
  if (reason_ == EmbedFail::None) return false;
  return (int32_t)(nowMs - retryAtMs_) < 0;   // wrap-safe
}

bool EmbedPark::onResult(EmbedFail kind, uint32_t nowMs, const std::string& detail) {
  const ParkRule rule = parkRuleFor(kind);
  if (rule == ParkRule::Clear) { clear(); return false; }
  if (rule == ParkRule::Retry || rule == ParkRule::NoChange) return false;
  const bool started = reason_ != kind;
  if (reason_ == EmbedFail::None) sinceMs_ = nowMs;
  reason_ = kind;
  detail_ = detail;
  if (streak_ < 30) streak_++;
  uint32_t wait = kMaxParkMs;
  if (rule == ParkRule::Backoff) {
    wait = kBackoffBaseMs;
    for (int i = 1; i < streak_ && wait < kMaxParkMs; i++) wait *= 2;
    if (wait > kMaxParkMs) wait = kMaxParkMs;
  }
  retryAtMs_ = nowMs + wait;
  return started;
}

void EmbedPark::clear() {
  reason_ = EmbedFail::None;
  detail_.clear();
  sinceMs_ = 0;
  retryAtMs_ = 0;
  streak_ = 0;
}

}  // namespace orch
}  // namespace nimbus
