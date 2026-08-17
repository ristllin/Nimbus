#include "nimbus/orch/rbac.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace nimbus {
namespace orch {

const char* roleName(Role r) {
  switch (r) {
    case Role::Admin:   return "admin";
    case Role::User:    return "user";
    case Role::Guest:   return "guest";
    default:            return "unknown";
  }
}

bool roleFromName(const std::string& s, Role& out) {
  if (s == "admin") { out = Role::Admin; return true; }
  if (s == "user")  { out = Role::User;  return true; }
  if (s == "guest") { out = Role::Guest; return true; }
  if (s == "unknown" || s == "none" || s == "revoked") { out = Role::Unknown; return true; }
  return false;
}

Perms permsFor(Role r) {
  Perms p;
  switch (r) {
    case Role::Admin:
      p.readOwn = p.writeOwn = p.readShared = p.shareOwn = true;
      p.readAll = p.manageTenants = p.pinPermanent = true;
      break;
    case Role::User:
      p.readOwn = p.writeOwn = p.readShared = p.shareOwn = true;
      // A User may pin, but only within its (counted) budget - see pinAllowed.
      p.pinPermanent = true;
      break;
    case Role::Guest:
      p.readOwn = p.writeOwn = p.readShared = true;
      // No sharing and NO permanent pins: `permanent` exempts an entry from
      // eviction AND prune, so an unbounded pin is memory a quota can never
      // reclaim.
      break;
    case Role::Unknown:
    default:
      break;   // denied everything until an admin approves the chat
  }
  return p;
}

Quota defaultQuotaFor(Role r) {
  Quota q;
  switch (r) {
    case Role::Admin:
      // The device's owner is not quotaed by their own device; the global
      // store caps still apply.
      q.maxVectors = 0; q.maxBytes = 0; q.maxTtlHours = 0; q.maxPins = 0;
      break;
    case Role::User:
      q.maxVectors  = 500;
      q.maxBytes    = 8u * 1024 * 1024;
      q.maxTtlHours = 24 * 365;      // a year
      q.maxPins     = 10;
      break;
    case Role::Guest:
    default:
      q.maxVectors  = 100;
      q.maxBytes    = 1u * 1024 * 1024;
      q.maxTtlHours = 24 * 30;       // a month - a guest's facts are not forever
      q.maxPins     = 0;
      break;
  }
  return q;
}

Quota effectiveQuota(Role r, const Quota& e) {
  Quota d = defaultQuotaFor(r);
  Quota q;
  q.maxVectors  = e.maxVectors  ? e.maxVectors  : d.maxVectors;
  q.maxBytes    = e.maxBytes    ? e.maxBytes    : d.maxBytes;
  q.maxTtlHours = e.maxTtlHours ? e.maxTtlHours : d.maxTtlHours;
  q.maxPins     = e.maxPins     ? e.maxPins     : d.maxPins;
  return q;
}

int32_t clampTtl(Role r, const Quota& explicitQ, int32_t requested, bool& clamped) {
  clamped = false;
  const Quota q = effectiveQuota(r, explicitQ);
  if (q.maxTtlHours == 0) return requested;          // unquotaed (admin)
  // -1 means "never expires" - for a quotaed tenant that IS the ceiling case.
  if (requested < 0 || requested > (int32_t)q.maxTtlHours) {
    clamped = true;
    return (int32_t)q.maxTtlHours;
  }
  return requested;
}

bool pinAllowed(Role r, const Quota& explicitQ, uint32_t currentPins) {
  if (!permsFor(r).pinPermanent) return false;
  const Quota q = effectiveQuota(r, explicitQ);
  if (q.maxPins == 0) return r == Role::Admin;       // 0 = unlimited for admin, none otherwise
  return currentPins < q.maxPins;
}

// ---- TenantStore -------------------------------------------------------------
// One record per line: chatId \x1F role \x1F vec \x1F bytes \x1F ttl \x1F pins
//                      \x1F firstSeen \x1F label \x1E
// Label LAST so its free text can never be read as a numeric field; control
// characters are stripped from stored strings (the codec rule used elsewhere).
namespace {
std::string stripCtl(std::string v) {
  std::string o;
  o.reserve(v.size());
  for (char c : v)
    if ((unsigned char)c != 0x1E && (unsigned char)c != 0x1F) o += c;
  return o;
}
uint32_t toU32(const std::string& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 10); }
}  // namespace

void TenantStore::load(const std::string& raw) {
  tenants_.clear();
  size_t start = 0;
  while (start < raw.size() && tenants_.size() < kMaxTenants) {
    size_t end = raw.find('\x1E', start);
    if (end == std::string::npos) break;             // trailing partial -> dropped
    std::string rec = raw.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    while (f.size() < 7) {
      size_t q = rec.find('\x1F', p);
      if (q == std::string::npos) break;
      f.push_back(rec.substr(p, q - p));
      p = q + 1;
    }
    if (f.size() < 7 || f[0].empty()) continue;      // malformed -> dropped
    Tenant t;
    t.chatId = f[0];
    Role r = Role::Unknown;
    roleFromName(f[1], r);
    t.role = r;
    t.quota.maxVectors  = toU32(f[2]);
    t.quota.maxBytes    = toU32(f[3]);
    t.quota.maxTtlHours = toU32(f[4]);
    t.quota.maxPins     = (uint16_t)toU32(f[5]);
    t.firstSeen = toU32(f[6]);
    t.label = rec.substr(p);
    tenants_.push_back(std::move(t));
  }
}

std::string TenantStore::dump() const {
  std::string out;
  for (const Tenant& t : tenants_) {
    out += stripCtl(t.chatId);          out += '\x1F';
    out += roleName(t.role);            out += '\x1F';
    out += std::to_string(t.quota.maxVectors);  out += '\x1F';
    out += std::to_string(t.quota.maxBytes);    out += '\x1F';
    out += std::to_string(t.quota.maxTtlHours); out += '\x1F';
    out += std::to_string((unsigned)t.quota.maxPins); out += '\x1F';
    out += std::to_string(t.firstSeen); out += '\x1F';
    out += stripCtl(t.label);           out += '\x1E';
  }
  return out;
}

const Tenant* TenantStore::find(const std::string& chatId) const {
  for (const Tenant& t : tenants_) if (t.chatId == chatId) return &t;
  return nullptr;
}
Tenant* TenantStore::findMut(const std::string& chatId) {
  for (Tenant& t : tenants_) if (t.chatId == chatId) return &t;
  return nullptr;
}

Role TenantStore::roleOf(const std::string& chatId) const {
  const Tenant* t = find(chatId);
  return t ? t->role : Role::Unknown;
}

size_t TenantStore::adminCount() const {
  size_t n = 0;
  for (const Tenant& t : tenants_) if (t.role == Role::Admin) n++;
  return n;
}

bool TenantStore::setRole(const std::string& chatId, Role r, std::string& err) {
  Tenant* t = findMut(chatId);
  // Demoting or removing the LAST admin would leave the device unadministrable
  // - the same rule the web surface already enforces for owners.
  if (t && t->role == Role::Admin && r != Role::Admin && adminCount() <= 1) {
    err = "that is the only admin - promote someone else first";
    return false;
  }
  if (!t) {
    if (tenants_.size() >= kMaxTenants) { err = "tenant list is full"; return false; }
    Tenant nt;
    nt.chatId = stripCtl(chatId);
    nt.role = r;
    tenants_.push_back(std::move(nt));
    return true;
  }
  t->role = r;
  return true;
}

bool TenantStore::setQuota(const std::string& chatId, const Quota& q, std::string& err) {
  Tenant* t = findMut(chatId);
  if (!t) { err = "no such tenant"; return false; }
  t->quota = q;
  return true;
}

bool TenantStore::remove(const std::string& chatId, std::string& err) {
  Tenant* t = findMut(chatId);
  if (!t) { err = "no such tenant"; return false; }
  if (t->role == Role::Admin && adminCount() <= 1) {
    err = "that is the only admin - promote someone else first";
    return false;
  }
  tenants_.erase(tenants_.begin() + (t - tenants_.data()));
  return true;
}

void TenantStore::adoptLegacy(const std::vector<std::string>& owners,
                              const std::vector<std::string>& allowed) {
  // Upgrade must not change anyone's access by surprise: today's owners become
  // Admin, every other allow-listed chat becomes a User (it already had full
  // conversational access), and nobody silently becomes Unknown.
  std::string err;
  for (const auto& o : owners) if (!o.empty() && !find(o)) setRole(o, Role::Admin, err);
  for (const auto& a : allowed) {
    if (a.empty() || find(a)) continue;
    setRole(a, Role::User, err);
  }
  // The single-account default: an empty owner list means the FIRST allow-listed
  // chat is the owner (the rule the rest of the firmware already uses).
  if (adminCount() == 0 && !tenants_.empty()) tenants_.front().role = Role::Admin;
}

}  // namespace orch
}  // namespace nimbus
