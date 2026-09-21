#include "nimbus/orch/safety_activity.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace nimbus {
namespace orch {

// ---- small helpers -----------------------------------------------------------

namespace {

std::string trimmed(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') b++;
  while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
  return s.substr(b, e - b);
}

char lc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Case-insensitive full-string equality.
bool ciEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (lc(a[i]) != lc(b[i])) return false;
  return true;
}

// Case-insensitive substring search: index of the first match, or npos.
size_t ciFind(const std::string& hay, const std::string& needle) {
  if (needle.empty() || needle.size() > hay.size()) return std::string::npos;
  for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
    size_t j = 0;
    for (; j < needle.size(); j++)
      if (lc(hay[i + j]) != lc(needle[j])) break;
    if (j == needle.size()) return i;
  }
  return std::string::npos;
}

}  // namespace

// ---- enums -------------------------------------------------------------------

const char* safetyVerdictName(SafetyVerdict v) {
  return v == SafetyVerdict::Suspected ? "suspected" : "blocked";
}
bool safetyVerdictFromName(const std::string& s, SafetyVerdict& out) {
  if (s == "blocked")   { out = SafetyVerdict::Blocked;   return true; }
  if (s == "suspected") { out = SafetyVerdict::Suspected; return true; }
  return false;
}

const char* safetyStatusName(SafetyStatus s) {
  switch (s) {
    case SafetyStatus::Active:    return "active";
    case SafetyStatus::Dismissed: return "dismissed";
    case SafetyStatus::Approved:  return "approved";
  }
  return "active";
}
bool safetyStatusFromName(const std::string& s, SafetyStatus& out) {
  if (s == "active")    { out = SafetyStatus::Active;    return true; }
  if (s == "dismissed") { out = SafetyStatus::Dismissed; return true; }
  if (s == "approved")  { out = SafetyStatus::Approved;  return true; }
  return false;
}

const char* allowScopeName(AllowScope s) {
  switch (s) {
    case AllowScope::Sender:       return "sender";
    case AllowScope::ContentClass: return "content-class";
    case AllowScope::Pattern:      return "pattern";
  }
  return "sender";
}
bool allowScopeFromName(const std::string& s, AllowScope& out) {
  if (s == "sender")        { out = AllowScope::Sender;       return true; }
  if (s == "content-class") { out = AllowScope::ContentClass; return true; }
  if (s == "pattern")       { out = AllowScope::Pattern;      return true; }
  return false;
}

// ---- SafetyEntry -------------------------------------------------------------

bool SafetyEntry::operator==(const SafetyEntry& o) const {
  return id == o.id && tsEpoch == o.tsEpoch && verdict == o.verdict && rule == o.rule &&
         channel == o.channel && sender == o.sender && source == o.source &&
         excerpt == o.excerpt && status == o.status;
}

std::string clampExcerpt(const std::string& in) {
  if (in.size() <= kSafetyExcerptMax) return in;
  return in.substr(0, kSafetyExcerptMax);
}

std::string excerptWindowAround(const std::string& text, const std::string& pattern,
                                size_t maxLen) {
  if (text.size() <= maxLen) return text;
  const size_t pos = ciFind(text, pattern);
  if (pos == std::string::npos) return text.substr(0, maxLen);
  // Center the window on the match: keep roughly equal context on each side, then
  // clamp to the string bounds so the returned window is always exactly maxLen here.
  const size_t lead = (maxLen > pattern.size()) ? (maxLen - pattern.size()) / 2 : 0;
  size_t start = pos > lead ? pos - lead : 0;
  if (start + maxLen > text.size()) start = text.size() - maxLen;
  return text.substr(start, maxLen);
}

// ---- id shape guards ---------------------------------------------------------

bool isValidSafetyEntryId(const std::string& id) {
  if (id.size() != 9 || id[0] != 's') return false;   // "s" + 8 lowercase hex (s%08x)
  for (size_t i = 1; i < id.size(); i++) {
    const char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool isValidAllowRuleId(const std::string& id) {
  if (id.size() < 2 || id[0] != 'a') return false;    // "a" + one or more decimal digits
  for (size_t i = 1; i < id.size(); i++)
    if (id[i] < '0' || id[i] > '9') return false;
  return true;
}

// ---- JSONL codec (entry) -----------------------------------------------------

std::string encodeSafetyLine(const SafetyEntry& e) {
  JsonDocument d;
  d["id"]      = e.id;
  d["ts"]      = e.tsEpoch;
  d["verdict"] = safetyVerdictName(e.verdict);
  d["rule"]    = e.rule;
  d["channel"] = e.channel;
  d["sender"]  = e.sender;
  d["source"]  = e.source;
  d["excerpt"] = clampExcerpt(e.excerpt);
  d["status"]  = safetyStatusName(e.status);
  std::string out;
  serializeJson(d, out);
  return out;
}

bool decodeSafetyLine(const std::string& line, SafetyEntry& out) {
  std::string s = trimmed(line);
  if (s.empty()) return false;
  JsonDocument d;
  if (deserializeJson(d, s) != DeserializationError::Ok) return false;
  if (!d["id"].is<const char*>()) return false;
  SafetyEntry e;
  e.id = d["id"].as<std::string>();
  if (!isValidSafetyEntryId(e.id)) return false;   // reject a hostile/hand-edited id

  e.tsEpoch = d["ts"].as<uint32_t>();
  SafetyVerdict v;
  if (!safetyVerdictFromName(d["verdict"].as<std::string>(), v)) return false;
  e.verdict = v;
  e.rule    = d["rule"].as<std::string>();
  e.channel = d["channel"].as<std::string>();
  e.sender  = d["sender"].as<std::string>();
  e.source  = d["source"].as<std::string>();
  e.excerpt = clampExcerpt(d["excerpt"].as<std::string>());
  SafetyStatus st;
  e.status = safetyStatusFromName(d["status"].as<std::string>(), st) ? st : SafetyStatus::Active;
  out = e;
  return true;
}

// ---- SafetyActivityLog -------------------------------------------------------

const SafetyEntry& SafetyActivityLog::record(SafetyEntry e) {
  e.excerpt = clampExcerpt(e.excerpt);
  entries_.insert(entries_.begin(), std::move(e));
  if ((int)entries_.size() > cap_) entries_.resize(cap_);   // drop the oldest (tail)
  return entries_.front();
}

bool SafetyActivityLog::refreshDuplicateFront(const SafetyEntry& e, uint32_t ts) {
  if (entries_.empty()) return false;
  SafetyEntry& f = entries_.front();
  if (f.status != SafetyStatus::Active) return false;   // never resurrect a reviewed entry
  if (f.verdict == e.verdict && f.rule == e.rule && f.channel == e.channel &&
      f.sender == e.sender && f.source == e.source && f.excerpt == clampExcerpt(e.excerpt)) {
    f.tsEpoch = ts;   // recurring: bump recency, but do not grow the ring
    return true;
  }
  return false;
}

const SafetyEntry* SafetyActivityLog::find(const std::string& id) const {
  for (const auto& e : entries_)
    if (e.id == id) return &e;
  return nullptr;
}

bool SafetyActivityLog::setStatus(const std::string& id, SafetyStatus s) {
  for (auto& e : entries_)
    if (e.id == id) { e.status = s; return true; }
  return false;
}

std::string SafetyActivityLog::serialize() const {
  std::string out;
  for (const auto& e : entries_) {
    out += encodeSafetyLine(e);
    out += '\n';
  }
  return out;
}

void SafetyActivityLog::rebound() {
  // Newest-first: a higher tsEpoch is newer. Stable sort keeps the file order for
  // equal timestamps (the file is already written newest-first). We DON'T reorder
  // when timestamps tie; std::stable_sort with a strict-weak `>` on ts preserves
  // relative order of equal keys, so a same-second burst keeps its stored order.
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const SafetyEntry& a, const SafetyEntry& b) { return a.tsEpoch > b.tsEpoch; });
  if ((int)entries_.size() > cap_) entries_.resize(cap_);
}

int SafetyActivityLog::loadAll(const std::string& blob) {
  entries_.clear();
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t nl = blob.find('\n', pos);
    std::string line = blob.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? blob.size() : nl + 1;
    SafetyEntry e;
    if (decodeSafetyLine(line, e)) entries_.push_back(std::move(e));
  }
  rebound();
  return (int)entries_.size();
}

// ---- AllowRule + allowlist ---------------------------------------------------

bool AllowRule::operator==(const AllowRule& o) const {
  return id == o.id && scope == o.scope && value == o.value && source == o.source &&
         tsEpoch == o.tsEpoch;
}

bool isConcreteAllowValue(const std::string& value) {
  return !trimmed(value).empty();
}

bool contentClassApprovable(const std::string& rule) {
  return isConcreteAllowValue(rule) && trimmed(rule) != kCoarseModerationRule;
}

bool ruleMatches(const AllowRule& r, const SafetyEntry& e) {
  // A rule can only match on its ONE concrete field. An empty target never
  // matches (defense: it should never be stored, but a hand-edited file might).
  if (!isConcreteAllowValue(r.value)) return false;
  // Gate binding: a rule only silences the gate/source it was minted from, so an
  // inbound approval can never quiet the outbound or world gate (CUM-215). Real
  // entries always carry a source; a source-less (hand-edited) rule matches nothing.
  if (r.source != e.source) return false;
  switch (r.scope) {
    case AllowScope::Sender:
      // Trust a specific principal. An entry with no sender (web/world) can never
      // be swept up by a sender rule.
      return !e.sender.empty() && e.sender == r.value;
    case AllowScope::ContentClass:
      // Trust a specific rule/category. An entry with no rule can never match.
      return !e.rule.empty() && e.rule == r.value;
    case AllowScope::Pattern:
      // Trust ONE exact content excerpt (case-insensitive equality, never substring):
      // the approved text embedded in a longer hostile message no longer matches, so a
      // Pattern approve is "this exact content", not a guest-controlled bypass token.
      return !e.excerpt.empty() && ciEquals(e.excerpt, r.value);
  }
  return false;
}

const AllowRule* SafetyAllowlist::find(const std::string& id) const {
  for (const auto& r : rules_)
    if (r.id == id) return &r;
  return nullptr;
}

namespace {
// Parse an auto-assigned "aN" id back to N (0 when it is not that shape), so a
// reload can keep the id counter ahead of every rule it read. Bounded manual parse
// (NEVER std::stoul): a corrupt/hand-edited over-range digit run returns 0 rather
// than throwing std::out_of_range, which on the 32-bit target would abort the device
// on the first ensureLoaded() (CUM-215; the sibling device_identity.cpp guards the
// same way).
uint32_t autoIdSuffix(const std::string& id) {
  if (id.size() < 2 || id[0] != 'a') return 0;
  uint64_t v = 0;
  for (size_t i = 1; i < id.size(); i++) {
    if (id[i] < '0' || id[i] > '9') return 0;
    v = v * 10 + (uint32_t)(id[i] - '0');
    if (v > 0xFFFFFFFFull) return 0;   // over-range: skip, do not overflow the counter
  }
  return (uint32_t)v;
}
}  // namespace

bool SafetyAllowlist::hasRule(AllowScope scope, const std::string& value,
                              const std::string& source) const {
  for (const auto& r : rules_)
    if (r.scope == scope && r.value == value && r.source == source) return true;
  return false;
}

bool SafetyAllowlist::add(AllowScope scope, const std::string& value, const std::string& source,
                          uint32_t tsEpoch, std::string& outId, const std::string& id) {
  const std::string v = trimmed(value);
  if (v.empty()) return false;                       // the anti-global guard
  for (const auto& r : rules_)                        // dup FIRST: an already-allowed
    if (r.scope == scope && r.value == v && r.source == source) {   // item is never "full"
      outId = r.id;
      return false;                                   // outId set: caller reads "already allowed"
    }
  if ((int)rules_.size() >= cap_) { outId.clear(); return false; }  // full: outId EMPTY
  AllowRule r;
  r.scope = scope;
  r.value = v;
  r.source = source;
  r.tsEpoch = tsEpoch;
  if (!id.empty()) {
    r.id = id;
  } else {
    r.id = std::string("a") + std::to_string(nextSfx_++);
  }
  outId = r.id;
  rules_.push_back(std::move(r));
  return true;
}

bool SafetyAllowlist::revoke(const std::string& id) {
  for (auto it = rules_.begin(); it != rules_.end(); ++it)
    if (it->id == id) { rules_.erase(it); return true; }
  return false;
}

bool SafetyAllowlist::allows(const SafetyEntry& e) const {
  for (const auto& r : rules_)
    if (ruleMatches(r, e)) return true;
  return false;
}

std::string encodeAllowLine(const AllowRule& r) {
  JsonDocument d;
  d["id"]     = r.id;
  d["scope"]  = allowScopeName(r.scope);
  d["value"]  = r.value;
  d["source"] = r.source;
  d["ts"]     = r.tsEpoch;
  std::string out;
  serializeJson(d, out);
  return out;
}

bool decodeAllowLine(const std::string& line, AllowRule& out) {
  std::string s = trimmed(line);
  if (s.empty()) return false;
  JsonDocument d;
  if (deserializeJson(d, s) != DeserializationError::Ok) return false;
  AllowRule r;
  r.id = d["id"].as<std::string>();
  if (!isValidAllowRuleId(r.id)) return false;   // reject a hostile/hand-edited id
  AllowScope sc;
  if (!allowScopeFromName(d["scope"].as<std::string>(), sc)) return false;
  r.scope = sc;
  r.value = d["value"].as<std::string>();
  if (!isConcreteAllowValue(r.value)) return false;   // never load a catch-all
  r.source = d["source"].as<std::string>();
  r.tsEpoch = d["ts"].as<uint32_t>();
  out = r;
  return true;
}

std::string SafetyAllowlist::serialize() const {
  std::string out;
  for (const auto& r : rules_) {
    out += encodeAllowLine(r);
    out += '\n';
  }
  return out;
}

int SafetyAllowlist::loadAll(const std::string& blob) {
  rules_.clear();
  nextSfx_ = 1;
  size_t pos = 0;
  while (pos < blob.size() && (int)rules_.size() < cap_) {   // bounded on load too
    size_t nl = blob.find('\n', pos);
    std::string line = blob.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? blob.size() : nl + 1;
    AllowRule r;
    if (!decodeAllowLine(line, r)) continue;
    if (hasRule(r.scope, r.value, r.source)) continue;       // drop a duplicate scope+value+source
    uint32_t n = autoIdSuffix(r.id);                         // keep the id counter ahead
    if (n >= nextSfx_) nextSfx_ = n + 1;
    rules_.push_back(std::move(r));
  }
  return (int)rules_.size();
}

// ---- report payload builder + gate -------------------------------------------

std::string buildSafetyReportJson(const SafetyReportInput& in) {
  // EXACTLY the contract fields, in a stable order. Nothing else is ever added:
  // the test parses this and asserts the top-level key set is {deviceId,
  // reportedAt, verdict, rule, channel, excerpt, meta} and meta is {fw, source}.
  JsonDocument d;
  d["deviceId"]   = in.deviceId;
  d["reportedAt"] = in.reportedAtIso;
  d["verdict"]    = safetyVerdictName(in.verdict);
  d["rule"]       = in.rule;
  d["channel"]    = in.channel;
  d["excerpt"]    = clampExcerpt(in.excerpt);
  JsonObject meta = d["meta"].to<JsonObject>();
  meta["fw"]     = in.fw;
  meta["source"] = in.source;
  std::string out;
  serializeJson(d, out);
  return out;
}

SafetyReportInput reportInputFromEntry(const SafetyEntry& e, const std::string& deviceId,
                                       const std::string& reportedAtIso, const std::string& fw) {
  SafetyReportInput in;
  in.deviceId      = deviceId;
  in.reportedAtIso = reportedAtIso;
  in.verdict       = e.verdict;
  in.rule          = e.rule;
  in.channel       = e.channel;
  in.excerpt       = clampExcerpt(e.excerpt);
  in.fw            = fw;
  in.source        = e.source;
  return in;
}

std::string cumuloHostFromBase(const std::string& base, const std::string& deflt) {
  std::string h = trimmed(base);
  if (h.empty()) h = deflt;
  const size_t sch = h.find("://");
  if (sch != std::string::npos) h = h.substr(sch + 3);
  const size_t sl = h.find('/');
  if (sl != std::string::npos) h = h.substr(0, sl);
  return h;
}

bool reportAvailable(bool hasCumuloKey) { return hasCumuloKey; }

const char* reportUnavailableCopy() { return "Reporting needs a Cumulo subscription."; }

const char* reportOutcomeName(ReportOutcome o) {
  switch (o) {
    case ReportOutcome::Sent:          return "sent";
    case ReportOutcome::NoEntitlement: return "no-entitlement";
    case ReportOutcome::TooLarge:      return "too-large";
    case ReportOutcome::RateLimited:   return "rate-limited";
    case ReportOutcome::Failed:        return "failed";
  }
  return "failed";
}

ReportOutcome reportOutcomeFromHttp(int httpStatus) {
  if (httpStatus == 202) return ReportOutcome::Sent;
  if (httpStatus == 401 || httpStatus == 403) return ReportOutcome::NoEntitlement;
  if (httpStatus == 413) return ReportOutcome::TooLarge;
  if (httpStatus == 429) return ReportOutcome::RateLimited;
  return ReportOutcome::Failed;
}

const char* reportOutcomeCopy(ReportOutcome o) {
  switch (o) {
    case ReportOutcome::Sent:          return "Reported to Cumulo.";
    case ReportOutcome::NoEntitlement: return reportUnavailableCopy();
    case ReportOutcome::TooLarge:      return "That item is too large to report.";
    case ReportOutcome::RateLimited:   return "Too many reports. Try again later.";
    case ReportOutcome::Failed:        return "Couldn't reach Cumulo. Try again.";
  }
  return "Couldn't reach Cumulo. Try again.";
}

}  // namespace orch
}  // namespace nimbus
