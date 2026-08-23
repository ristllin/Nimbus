#pragma once
#include <cstdint>
#include <string>
#include <vector>

// rbac - who may do what, and how much (v3.7.0, owner-specified).
//
// The device grew a multi-user surface (Telegram members) with an AUTH boundary
// but no DATA boundary and no roles: one "owner" bool decided everything. This
// module is the portable policy: a role per tenant, a permission table, and
// per-tenant quotas enforced at the WRITE seam - a quota that only prunes later
// lets a tenant win the race.
//
// Deliberate asymmetry (owner's call): FILES may be shared; VECTOR MEMORY never
// is. Recall stays strictly per-principal, so device-level facts live in the
// admin namespace rather than a shared one.
namespace nimbus {
namespace orch {

enum class Role : uint8_t {
  Unknown = 0,  // messaged the device, not yet approved - no data, no control
  Guest   = 1,  // approved, tightest quotas, no permanent pins
  User    = 2,  // approved, own namespace, small pin budget
  Admin   = 3,  // every namespace, every file, tenant management
};

const char* roleName(Role r);
bool        roleFromName(const std::string& s, Role& out);

// ---- permissions -------------------------------------------------------------
// One table, consulted by every rail, so a new surface cannot invent its own
// answer. `Unknown` is denied everything on purpose: an unapproved chat has no
// namespace to write into and nothing to read.
struct Perms {
  bool readOwn        = false;   // own namespace (vectors, episodic, files)
  bool writeOwn       = false;
  bool readShared     = false;   // FILES marked shared (never vectors)
  bool shareOwn       = false;   // may mark one's own file shared
  bool readAll        = false;   // every tenant's data (admin)
  bool manageTenants  = false;   // roles, approvals, quotas
  bool pinPermanent   = false;   // `permanent` exempts from eviction AND prune
};

Perms permsFor(Role r);

// May this role ARM a wake-up (wakeup.set / an agent Once loop)? A wake-up fires
// an unattended turn that replies into the owner's channel, so it is an
// admin-only control action, gated on the same `manageTenants` permission the
// tool handler enforces (CUM-27). One predicate so the tool rail, the web
// surface, and the tests can never disagree on the gate.
inline bool mayArmWakeup(Role r) { return permsFor(r).manageTenants; }

// ---- quotas ------------------------------------------------------------------
// Per-tenant ceilings. 0 means "unset" and falls back to the role default, so an
// admin can raise one tenant without editing every other. Admins are unquotaed
// (they own the device); the defaults below apply to User/Guest.
struct Quota {
  uint32_t maxVectors = 0;      // entries in this tenant's namespace
  uint32_t maxBytes   = 0;      // artifact-store bytes
  uint32_t maxTtlHours = 0;     // a longer write is CLAMPED, not refused
  uint16_t maxPins    = 0;      // `permanent` entries allowed
};

Quota defaultQuotaFor(Role r);

// Merge an explicit per-tenant quota over the role default (0 = inherit).
Quota effectiveQuota(Role r, const Quota& explicitQ);

// Clamp a requested TTL to what this tenant may have. Returns the allowed value;
// `clamped` reports whether it was reduced, so the caller can say so honestly
// instead of silently shortening a memory's life.
int32_t clampTtl(Role r, const Quota& q, int32_t requestedHours, bool& clamped);

// May this tenant pin another entry permanently? Counts what it already has.
bool pinAllowed(Role r, const Quota& q, uint32_t currentPins);

// ---- tenants -----------------------------------------------------------------
// One record per chat the device knows about. `ns` is the data namespace
// (kOwnerNs for every admin - one household account across channels).
struct Tenant {
  std::string chatId;
  std::string label;            // display name from the channel ("Roy")
  Role        role = Role::Unknown;
  Quota       quota;            // explicit overrides (0 = role default)
  uint32_t    firstSeen = 0;    // epoch seconds
};

// The tenant table: tolerant load, bounded, with the LAST-ADMIN rule enforced
// here rather than at each surface (web and conversation must agree).
class TenantStore {
 public:
  static constexpr size_t kMaxTenants = 32;

  void        load(const std::string& blob);   // tolerant: bad rows dropped
  std::string dump() const;

  Role roleOf(const std::string& chatId) const;
  // Is there a row for this chat AT ALL? Distinct from roleOf() returning
  // Unknown, which collapses "never seen" and "explicitly revoked" into one
  // value. Callers with an allow-list fallback MUST ask this first: a revoked
  // tenant is still on the Telegram allowlist by design, so treating their
  // explicit Unknown as "no opinion" silently restored their access.
  bool known(const std::string& chatId) const { return find(chatId) != nullptr; }
  const Tenant* find(const std::string& chatId) const;
  const std::vector<Tenant>& all() const { return tenants_; }

  // Upsert a tenant's role. Refuses to remove the last Admin (returns false with
  // `err` set) - the device must never become unadministrable.
  bool setRole(const std::string& chatId, Role r, std::string& err);
  bool setQuota(const std::string& chatId, const Quota& q, std::string& err);
  bool remove(const std::string& chatId, std::string& err);

  size_t adminCount() const;

  // Migration: today's single "owner" list becomes Admin and the rest of the
  // allow-list becomes User, so an upgrade changes nobody's access by surprise.
  void adoptLegacy(const std::vector<std::string>& ownerChatIds,
                   const std::vector<std::string>& allowedChatIds);

 private:
  Tenant* findMut(const std::string& chatId);
  std::vector<Tenant> tenants_;
};

}  // namespace orch
}  // namespace nimbus
