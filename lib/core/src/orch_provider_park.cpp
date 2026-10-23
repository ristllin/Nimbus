#include "nimbus/orch/provider_park.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "nimbus/orch/billing_refusal.h"   // isBillingRefusal - the one billing table

namespace nimbus {
namespace orch {

namespace {
constexpr uint8_t kAllSeams = (uint8_t)((1u << kPaidSeamCount) - 1u);

uint8_t seamBit(PaidSeam s) { return (uint8_t)(1u << (uint8_t)s); }

bool laterOrEqual(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }   // wrap-safe

// A provider name the record and the file can carry: 1-15 of [a-z0-9_-], or "*".
bool validName(const char* p) {
  if (!p || !*p) return false;
  const size_t n = std::strlen(p);
  if (n >= sizeof(ParkEntry::provider)) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = p[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string minutesText(uint32_t ms) {
  const uint32_t min = (ms + 59999u) / 60000u;
  if (min <= 1) return "in a minute";
  return "in " + std::to_string(min) + " minutes";
}
}  // namespace

const char* paidSeamSlug(PaidSeam s) {
  switch (s) {
    case PaidSeam::Chat:       return "chat";
    case PaidSeam::Spawn:      return "spawn";
    case PaidSeam::Embeddings: return "embeddings";
    case PaidSeam::Vision:     return "vision";
    case PaidSeam::Stt:        return "stt";
    case PaidSeam::Tts:        return "tts";
    case PaidSeam::UrlFetch:   return "url_fetch";
    case PaidSeam::ImageGen:   return "image_gen";
  }
  return "?";
}

const char* parkKindSlug(ParkKind k) {
  switch (k) {
    case ParkKind::None:    return "none";
    case ParkKind::Billing: return "billing";
    case ParkKind::Auth:    return "auth";
  }
  return "?";
}

ParkKind parkKindFor(int httpStatus, const std::string& text) {
  if (isBillingRefusal(httpStatus, text)) return ParkKind::Billing;
  if (httpStatus == 401 || httpStatus == 403) return ParkKind::Auth;
  return ParkKind::None;
}

std::string parkWords(const char* provider, const ParkVerdict& v) {
  if (v.open) return std::string();
  const std::string p = provider ? provider : "the provider";
  if (v.paused & kPauseSpendCeiling) return "paid requests to " + p + " are paused: the monthly spend limit is reached";
  if (v.paused & kPauseProviderLimit) return "paid requests to " + p + " are paused: its monthly limit is reached";
  if (v.paused) return "paid requests to " + p + " are paused";
  std::string s;
  if (v.kind == ParkKind::Billing) s = p + " says the account is out of credit";
  else s = p + " refused the key";
  if (v.httpStatus > 0) s += " (HTTP " + std::to_string(v.httpStatus) + ")";
  s += "; it is checked again " + minutesText(v.recheckInMs);
  return s;
}

std::string parkShortWords(const char* provider, const ParkVerdict& v) {
  if (v.open) return std::string();
  if (v.paused) return "Paid requests are paused.";
  const std::string p = provider ? provider : "The provider";
  if (v.kind == ParkKind::Billing) return p + " is out of credit. Add credit.";
  return p + " refused the key.";
}

std::string keyNotice(const std::string& provider, int httpStatus, const std::string& next) {
  std::string s = provider + " rejected its key";
  if (httpStatus > 0) s += " (HTTP " + std::to_string(httpStatus) + ")";
  s += ". Update it under Assistant > Models";
  if (!next.empty()) s += "; until then, " + next + " answers";
  return s + ".";
}

std::string keyShortAnswer(const std::string& provider) {
  return "Your message was not answered. " + provider + " rejected its key: update it under Assistant > Models.";
}

uint32_t parkKeyFingerprint(const std::string& key) {
  if (key.empty()) return 0;
  uint32_t h = 2166136261u;
  for (unsigned char c : key) {
    h ^= c;
    h *= 16777619u;
  }
  return h ? h : 1u;   // 0 means "unknown"
}

// ---- ProviderPark -------------------------------------------------------------

void ProviderPark::setStorage(ParkStorage s) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  storage_ = std::move(s);
  loaded_ = false;
}

uint32_t ProviderPark::epochNow() const { return storage_.epochS ? storage_.epochS() : 0u; }

uint32_t ProviderPark::clockNow() {
  if (storage_.nowMs) lastNowMs_ = storage_.nowMs();
  return lastNowMs_;
}

void ProviderPark::setRecordMemory(RecordAlloc alloc) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  recordAlloc_ = alloc;
}

ProviderPark::Span ProviderPark::recs() {
  if (!recs_) {
    const size_t bytes = sizeof(Rec) * kMaxProviders;
    void* p = recordAlloc_ ? recordAlloc_(bytes) : nullptr;
    if (!p) p = std::malloc(bytes);
    if (!p) std::abort();   // a few hundred bytes at the first question: as fatal as a failed static
    recs_ = static_cast<Rec*>(p);
    for (size_t i = 0; i < kMaxProviders; i++) new (&recs_[i]) Rec();
  }
  return Span{recs_, recs_ + kMaxProviders};
}

ProviderPark::Span ProviderPark::recs() const {
  return recs_ ? Span{recs_, recs_ + kMaxProviders} : Span{nullptr, nullptr};
}

ProviderPark::Rec* ProviderPark::find(const char* provider) {
  for (Rec& r : recs())
    if (r.provider[0] && std::strcmp(r.provider, provider) == 0) return &r;
  return nullptr;
}

const ProviderPark::Rec* ProviderPark::find(const char* provider) const {
  for (const Rec& r : recs())
    if (r.provider[0] && std::strcmp(r.provider, provider) == 0) return &r;
  return nullptr;
}

// The record for `provider`, created if new. A full table reuses a record with no
// park and no pause (a stale latch is the cheapest thing to lose), else the first.
ProviderPark::Rec* ProviderPark::ensure(const char* provider) {
  if (Rec* r = find(provider)) return r;
  const Span all = recs();
  Rec* slot = nullptr;
  for (Rec& r : all)
    if (!r.provider[0]) { slot = &r; break; }
  if (!slot)
    for (Rec& r : all)
      if (r.kind == ParkKind::None && !r.paused) { slot = &r; break; }
  if (!slot) slot = &all[0];
  *slot = Rec();
  std::snprintf(slot->provider, sizeof(slot->provider), "%s", provider);
  return slot;
}

void ProviderPark::dropIfEmpty(Rec& r) {
  if (r.kind == ParkKind::None && !r.noticed && !r.answered && !r.warnedDay && !r.paused) r = Rec();
}

void ProviderPark::endEpisode(Rec& r) {
  r.kind = ParkKind::None;
  r.httpStatus = 0;
  r.seamMask = 0;
  r.recheckAtMs = 0;
  r.sinceEpoch = 0;
  r.probing = false;
  r.noticed = false;
  r.answered = false;
}

void ProviderPark::loadLocked(uint32_t nowMs) {
  lastNowMs_ = nowMs;
  if (loaded_ || !storage_.load) return;
  std::string blob;
  const int rc = storage_.load(blob);
  if (rc < 0) return;   // storage not ready yet: the next call tries again
  loaded_ = true;
  if (rc > 0) deserialize(blob, nowMs);
}

void ProviderPark::saveLocked() { dirty_ = true; }

void ProviderPark::flush() {
  std::lock_guard<std::mutex> io(saveMx_);
  std::function<bool(const std::string&)> save;
  std::string blob;
  {
    std::lock_guard<std::recursive_mutex> lk(mx_);
    if (!dirty_ || !storage_.save) return;
    dirty_ = false;
    save = storage_.save;
    blob = serialize(lastNowMs_);
  }
  save(blob);
}

void ProviderPark::parkLocked(Rec& r, PaidSeam seam, ParkKind kind, int httpStatus, uint32_t nowMs) {
  // A billing refusal and a refused key (401) hold for every seam; a 403 names one
  // model or endpoint the key may not use, so it closes only the seam that met it.
  const bool everySeam = kind == ParkKind::Billing || httpStatus != 403;
  if (r.kind == ParkKind::None) r.sinceEpoch = epochNow();
  // The first refusal of a kind names it; a billing park is never downgraded.
  const bool upgrade = r.kind == ParkKind::None || (kind == ParkKind::Billing && r.kind != ParkKind::Billing);
  if (upgrade) {
    r.kind = kind;
    r.httpStatus = httpStatus;
  }
  r.seamMask |= everySeam ? kAllSeams : seamBit(seam);
  r.recheckAtMs = nowMs + kRecheckMs;
  r.probing = false;
}

ParkVerdict ProviderPark::available(const char* provider, PaidSeam seam, uint32_t keyFp, uint32_t nowMs) {
  ParkVerdict v;
  if (!validName(provider)) return v;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(nowMs);
  Rec* r = find(provider);
  v.paused = (uint8_t)(pausedAll_ | (r ? r->paused : 0));
  if (v.paused) {
    v.open = false;
    return v;
  }
  if (!r || r->kind == ParkKind::None) return v;
  if (keyFp && r->keyFp && keyFp != r->keyFp) {   // a new key: a new start
    endEpisode(*r);
    r->keyFp = keyFp;
    dropIfEmpty(*r);
    saveLocked();
    v.keyChanged = true;
    return v;
  }
  if (!(r->seamMask & seamBit(seam))) return v;
  if (r->probing) {
    if (!laterOrEqual(nowMs, r->probeUntilMs)) return v;   // inside the probe's lease
    r->probing = false;                                    // no answer in the lease: a failed probe
    r->recheckAtMs = nowMs + kRecheckMs;
    saveLocked();
  }
  if (laterOrEqual(nowMs, r->recheckAtMs)) {
    r->probing = true;   // one request goes; its answer re-parks, re-arms or ends it
    r->probeUntilMs = nowMs + kProbeLeaseMs;
    v.probe = true;
    return v;
  }
  v.open = false;
  v.kind = r->kind;
  v.httpStatus = r->httpStatus;
  v.recheckInMs = r->recheckAtMs - nowMs;
  return v;
}

void ProviderPark::park(const char* provider, PaidSeam seam, uint32_t keyFp, ParkKind kind, int httpStatus,
                        uint32_t nowMs) {
  if (kind == ParkKind::None || !validName(provider)) return;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(nowMs);
  Rec* r = ensure(provider);
  if (keyFp && r->keyFp && keyFp != r->keyFp) endEpisode(*r);   // the old key's park is over
  if (keyFp) r->keyFp = keyFp;
  parkLocked(*r, seam, kind, httpStatus, nowMs);
  saveLocked();
}

ParkKind ProviderPark::noteAnswer(const char* provider, PaidSeam seam, uint32_t keyFp, int httpStatus,
                                  const std::string& text, uint32_t nowMs) {
  if (httpStatus >= 200 && httpStatus < 300) {
    noteSuccess(provider, seam);
    return ParkKind::None;
  }
  const ParkKind kind = parkKindFor(httpStatus, text);
  if (kind == ParkKind::None) {
    noteFailure(provider, nowMs);
    return kind;
  }
  park(provider, seam, keyFp, kind, httpStatus, nowMs);
  return kind;
}

bool ProviderPark::noteSuccess(const char* provider, PaidSeam seam) {
  if (!validName(provider)) return false;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  Rec* r = find(provider);
  if (!r || (r->kind == ParkKind::None && !r->noticed)) return false;
  if (r->kind != ParkKind::None && !(r->seamMask & seamBit(seam))) return false;
  endEpisode(*r);
  dropIfEmpty(*r);
  saveLocked();
  return true;
}

void ProviderPark::noteFailure(const char* provider, uint32_t nowMs) {
  if (!validName(provider)) return;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(nowMs);
  Rec* r = find(provider);
  if (!r || !r->probing) return;
  r->probing = false;
  r->recheckAtMs = nowMs + kRecheckMs;
  saveLocked();
}

void ProviderPark::pause(const char* provider, uint8_t causes) {
  if (!causes || !provider) return;
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (std::strcmp(provider, kPauseAllProviders) == 0) {
    pausedAll_ |= causes;
    return;
  }
  if (!validName(provider)) return;
  ensure(provider)->paused |= causes;
}

void ProviderPark::resume(const char* provider, uint8_t causes) {
  if (!causes || !provider) return;
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (std::strcmp(provider, kPauseAllProviders) == 0) {
    pausedAll_ &= (uint8_t)~causes;
    return;
  }
  Rec* r = validName(provider) ? find(provider) : nullptr;
  if (!r) return;
  r->paused &= (uint8_t)~causes;
  dropIfEmpty(*r);
}

uint8_t ProviderPark::pausedBy(const char* provider) const {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  const Rec* r = validName(provider) ? find(provider) : nullptr;
  return (uint8_t)(pausedAll_ | (r ? r->paused : 0));
}

bool ProviderPark::noticeDue(const char* provider) {
  if (!validName(provider)) return true;   // an unnamed provider cannot be latched: tell
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  Rec* r = ensure(provider);
  if (r->noticed) return false;
  r->noticed = true;
  saveLocked();
  return true;
}

void ProviderPark::episodeEnd(const char* provider) {
  if (!validName(provider)) return;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  Rec* r = find(provider);
  if (!r || (!r->noticed && !r->answered)) return;
  r->noticed = false;
  r->answered = false;
  dropIfEmpty(*r);
  saveLocked();
}

bool ProviderPark::shortAnswerDue(const char* provider) {
  if (!validName(provider)) return false;   // an unnamed provider cannot be latched: say nothing more
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  Rec* r = ensure(provider);
  if (r->answered) return false;
  r->answered = true;
  r->noticed = true;   // the short answer follows the notice: never the other way round
  saveLocked();
  return true;
}

uint32_t ProviderPark::recheckInMs(const char* provider) {
  if (!validName(provider)) return 0;
  std::lock_guard<std::recursive_mutex> lk(mx_);
  const uint32_t now = clockNow();
  loadLocked(now);
  const Rec* r = find(provider);
  if (!r || r->kind == ParkKind::None || r->probing || laterOrEqual(now, r->recheckAtMs)) return 0;
  return r->recheckAtMs - now;
}

bool ProviderPark::dailyWarningDue(const char* provider, uint32_t dayKey) {
  if (!validName(provider) || !dayKey) return false;
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  Rec* r = ensure(provider);
  if (r->warnedDay == dayKey) return false;
  r->warnedDay = dayKey;
  saveLocked();
  return true;
}

bool ProviderPark::warnedOn(const char* provider, uint32_t dayKey) {
  if (!validName(provider) || !dayKey) return false;
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(clockNow());
  const Rec* r = find(provider);
  return r && r->warnedDay == dayKey;
}

size_t ProviderPark::snapshot(ParkEntry* out, size_t cap, uint32_t nowMs) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(nowMs);
  size_t n = 0;
  for (const Rec& r : recs()) {
    if (!r.provider[0] || n >= cap) continue;
    ParkEntry& e = out[n++];
    e = ParkEntry();
    std::memcpy(e.provider, r.provider, sizeof(e.provider));
    e.kind = r.kind;
    e.httpStatus = r.httpStatus;
    e.seamMask = r.seamMask;
    e.recheckInMs = r.kind != ParkKind::None && !laterOrEqual(nowMs, r.recheckAtMs) ? r.recheckAtMs - nowMs : 0;
    e.sinceEpoch = r.sinceEpoch;
    e.probing = r.probing;
    e.noticed = r.noticed;
    e.answered = r.answered;
    e.warnedDay = r.warnedDay;
    e.paused = (uint8_t)(r.paused | pausedAll_);
  }
  return n;
}

// "PK1\n" then one line per record with durable state:
//   <provider> <kind> <status> <seamMask> <recheckInS> <recheckEpoch> <sinceEpoch> <noticed> <warnedDay> <keyFp hex>
// recheckEpoch is the wall-clock re-check time when the clock is known (0 = not);
// recheckInS is the relative wait, used when either clock is unknown at load.
// <noticed> is 0 (not told), 1 (told) or 2 (told, and the one short answer to a
// refused turn went out): an older image reads any non-zero value as "told".
std::string ProviderPark::serialize(uint32_t nowMs) const {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  std::string out = "PK1\n";
  const uint32_t epoch = epochNow();
  for (const Rec& r : recs()) {
    if (!r.provider[0] || (r.kind == ParkKind::None && !r.noticed && !r.answered && !r.warnedDay)) continue;
    uint32_t inS = 0;
    if (r.kind != ParkKind::None && !r.probing && !laterOrEqual(nowMs, r.recheckAtMs))
      inS = (r.recheckAtMs - nowMs + 999u) / 1000u;
    char line[128];
    std::snprintf(line, sizeof(line), "%s %u %d %u %lu %lu %lu %u %lu %08lx\n", r.provider, (unsigned)r.kind,
                  r.httpStatus, (unsigned)r.seamMask, (unsigned long)inS,
                  (unsigned long)(epoch && inS ? epoch + inS : 0u), (unsigned long)r.sinceEpoch,
                  r.answered ? 2u : (r.noticed ? 1u : 0u), (unsigned long)r.warnedDay, (unsigned long)r.keyFp);
    out += line;
  }
  return out;
}

namespace {
// One saved line: <provider> <kind> <status> <seamMask> <recheckInS> <recheckEpoch>
// <sinceEpoch> <noticed> <warnedDay> <keyFp hex>. False for anything else.
struct SavedRec {
  char prov[16] = {};
  unsigned kind = 0, mask = 0, noticed = 0;
  int status = 0;
  unsigned long inS = 0, recheckEpoch = 0, since = 0, warned = 0, fp = 0;
};
bool parseSaved(const std::string& line, SavedRec& out) {
  const int got = std::sscanf(line.c_str(), "%15s %u %d %u %lu %lu %lu %u %lu %lx", out.prov, &out.kind, &out.status,
                              &out.mask, &out.inS, &out.recheckEpoch, &out.since, &out.noticed, &out.warned, &out.fp);
  return got == 10 && validName(out.prov) && out.kind <= (unsigned)ParkKind::Auth;
}
// What is left of a saved wait after a restart: the wall clock decides when both the
// save and now know it; otherwise the saved wait is used whole (never shorter than the
// truth). Never more than one whole re-check wait.
uint32_t leftAfterRestart(const SavedRec& r, uint32_t epoch) {
  uint32_t leftS = (uint32_t)r.inS;
  if (epoch && r.recheckEpoch) leftS = r.recheckEpoch > epoch ? (uint32_t)(r.recheckEpoch - epoch) : 0u;
  return leftS > ProviderPark::kRecheckMs / 1000u ? ProviderPark::kRecheckMs / 1000u : leftS;
}
}  // namespace

bool ProviderPark::deserialize(const std::string& blob, uint32_t nowMs) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (blob.rfind("PK1\n", 0) != 0) return false;
  const Span all = recs();
  Rec keep[kMaxProviders];   // pauses are RAM only: kept across the load
  for (size_t i = 0; i < kMaxProviders; i++) {
    keep[i] = all[i];
    all[i] = Rec();
  }
  const uint32_t epoch = epochNow();
  bool clean = true;
  size_t at = 4, n = 0;
  while (at < blob.size() && n < kMaxProviders) {
    const size_t eol = blob.find('\n', at);
    const std::string line = blob.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    at = eol == std::string::npos ? blob.size() : eol + 1;
    SavedRec sr;
    if (!parseSaved(line, sr)) {
      clean = false;
      continue;
    }
    Rec& r = all[n++];
    std::snprintf(r.provider, sizeof(r.provider), "%s", sr.prov);
    r.kind = (ParkKind)sr.kind;
    r.httpStatus = sr.status;
    r.seamMask = (uint8_t)(sr.mask & kAllSeams);
    r.recheckAtMs = nowMs + leftAfterRestart(sr, epoch) * 1000u;
    r.sinceEpoch = (uint32_t)sr.since;
    r.noticed = sr.noticed != 0;
    r.answered = sr.noticed >= 2;
    r.warnedDay = (uint32_t)sr.warned;
    r.keyFp = (uint32_t)sr.fp;
  }
  for (const Rec& k : keep)
    if (k.paused && k.provider[0]) ensure(k.provider)->paused |= k.paused;
  return clean;
}

void ProviderPark::clear() {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  for (Rec& r : recs()) r = Rec();
  pausedAll_ = 0;
}

void ProviderPark::forgetParks(uint32_t nowMs) {
  FlushOnExit flushAfter{*this};   // after the lock below is released
  std::lock_guard<std::recursive_mutex> lk(mx_);
  loadLocked(nowMs);
  for (Rec& r : recs()) {
    if (!r.provider[0]) continue;
    Rec kept;
    std::memcpy(kept.provider, r.provider, sizeof(kept.provider));
    kept.paused = r.paused;
    r = kept;
    dropIfEmpty(r);
  }
  saveLocked();
}

void ProviderPark::forgetStorage() {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  storage_ = ParkStorage();
  loaded_ = false;
  lastNowMs_ = 0;
}

ProviderPark& providerPark() {
  static ProviderPark p;
  return p;
}

ProviderPick pickOpenProvider(const std::vector<std::string>& order,
                              const std::function<std::string(const std::string&)>& keyOf, PaidSeam seam,
                              uint32_t nowMs) {
  ProviderPick pick;
  for (const std::string& p : order) {
    const std::string key = keyOf ? keyOf(p) : std::string();
    if (key.empty()) continue;
    const ParkVerdict v = providerPark().available(p.c_str(), seam, parkKeyFingerprint(key), nowMs);
    if (v.open) {
      pick.provider = p;
      return pick;
    }
    if (pick.closedWhy.empty()) pick.closedWhy = parkWords(p.c_str(), v);
  }
  return pick;
}

bool billingNoticeDue(const char* provider) { return providerPark().noticeDue(provider); }
void billingEpisodeEnd(const char* provider) { providerPark().episodeEnd(provider); }

RefusalTurnAnswer refusalTurnAnswer(const char* provider, bool ownerWrote) {
  if (providerPark().noticeDue(provider)) return RefusalTurnAnswer::Notice;
  if (ownerWrote && providerPark().shortAnswerDue(provider)) return RefusalTurnAnswer::Short;
  return RefusalTurnAnswer::Nothing;
}

}  // namespace orch
}  // namespace nimbus
