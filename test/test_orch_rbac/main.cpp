#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/loop_delivery.h"
#include "nimbus/orch/rbac.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the permission matrix ---------------------------------------------------
static void test_role_permissions_matrix() {
  const Perms admin = permsFor(Role::Admin);
  TEST_ASSERT_TRUE(admin.readAll && admin.manageTenants && admin.pinPermanent &&
                   admin.shareOwn && admin.writeOwn);

  const Perms user = permsFor(Role::User);
  TEST_ASSERT_TRUE(user.readOwn && user.writeOwn && user.readShared && user.shareOwn);
  TEST_ASSERT_FALSE(user.readAll);          // never another tenant's data
  TEST_ASSERT_FALSE(user.manageTenants);    // never roles/quotas

  const Perms guest = permsFor(Role::Guest);
  TEST_ASSERT_TRUE(guest.readOwn && guest.writeOwn && guest.readShared);
  TEST_ASSERT_FALSE(guest.shareOwn);
  TEST_ASSERT_FALSE(guest.pinPermanent);    // pins outlive quotas - guests get none
  TEST_ASSERT_FALSE(guest.readAll || guest.manageTenants);

  // Unknown = messaged the device, not approved: denied everything.
  const Perms none = permsFor(Role::Unknown);
  TEST_ASSERT_FALSE(none.readOwn || none.writeOwn || none.readShared ||
                    none.shareOwn || none.readAll || none.manageTenants ||
                    none.pinPermanent);
}

// ---- quotas ------------------------------------------------------------------
static void test_ttl_is_clamped_not_refused() {
  bool clamped = false;
  Quota none;
  // A guest asking for ten years gets its ceiling - the fact still lands, and
  // the caller can say so honestly rather than silently shortening it.
  int32_t got = clampTtl(Role::Guest, none, 24 * 3650, clamped);
  TEST_ASSERT_TRUE(clamped);
  TEST_ASSERT_EQUAL_INT32((int32_t)defaultQuotaFor(Role::Guest).maxTtlHours, got);

  // "Never expires" (-1) is the same ceiling case for a quotaed tenant.
  clamped = false;
  got = clampTtl(Role::User, none, -1, clamped);
  TEST_ASSERT_TRUE(clamped);
  TEST_ASSERT_EQUAL_INT32((int32_t)defaultQuotaFor(Role::User).maxTtlHours, got);

  // A modest request passes through untouched.
  clamped = false;
  TEST_ASSERT_EQUAL_INT32(72, clampTtl(Role::Guest, none, 72, clamped));
  TEST_ASSERT_FALSE(clamped);

  // An admin is not quotaed by their own device.
  clamped = false;
  TEST_ASSERT_EQUAL_INT32(-1, clampTtl(Role::Admin, none, -1, clamped));
  TEST_ASSERT_FALSE(clamped);
}

static void test_permanent_pins_are_bounded() {
  Quota none;
  TEST_ASSERT_FALSE(pinAllowed(Role::Guest, none, 0));      // never
  TEST_ASSERT_TRUE(pinAllowed(Role::User, none, 0));
  TEST_ASSERT_TRUE(pinAllowed(Role::User, none,
                              defaultQuotaFor(Role::User).maxPins - 1));
  TEST_ASSERT_FALSE(pinAllowed(Role::User, none,
                               defaultQuotaFor(Role::User).maxPins));   // budget spent
  TEST_ASSERT_TRUE(pinAllowed(Role::Admin, none, 9999));    // unquotaed
  TEST_ASSERT_FALSE(pinAllowed(Role::Unknown, none, 0));
}

static void test_explicit_quota_overrides_role_default() {
  Quota q;
  q.maxVectors = 5000;              // an admin raised this one tenant
  const Quota eff = effectiveQuota(Role::Guest, q);
  TEST_ASSERT_EQUAL_UINT32(5000, eff.maxVectors);
  // Unset fields still inherit the role default (0 = inherit, not zero).
  TEST_ASSERT_EQUAL_UINT32(defaultQuotaFor(Role::Guest).maxBytes, eff.maxBytes);
}

// ---- the tenant table --------------------------------------------------------
static void test_last_admin_cannot_be_demoted_or_removed() {
  TenantStore ts;
  std::string err;
  TEST_ASSERT_TRUE(ts.setRole("1001", Role::Admin, err));
  TEST_ASSERT_TRUE(ts.setRole("2002", Role::User, err));

  TEST_ASSERT_FALSE(ts.setRole("1001", Role::User, err));   // the only admin
  TEST_ASSERT_TRUE(err.find("only admin") != std::string::npos);
  TEST_ASSERT_EQUAL(Role::Admin, ts.roleOf("1001"));
  TEST_ASSERT_FALSE(ts.remove("1001", err));

  // With a second admin in place the first may step down.
  TEST_ASSERT_TRUE(ts.setRole("2002", Role::Admin, err));
  TEST_ASSERT_TRUE(ts.setRole("1001", Role::User, err));
  TEST_ASSERT_EQUAL(Role::User, ts.roleOf("1001"));
  TEST_ASSERT_EQUAL(1, (int)ts.adminCount());
}

static void test_store_roundtrip_and_tolerant_load() {
  TenantStore ts;
  std::string err;
  ts.setRole("1001", Role::Admin, err);
  ts.setRole("2002", Role::Guest, err);
  Quota q; q.maxVectors = 42; q.maxPins = 3;
  TEST_ASSERT_TRUE(ts.setQuota("2002", q, err));

  TenantStore back;
  back.load(ts.dump());
  TEST_ASSERT_EQUAL(Role::Admin, back.roleOf("1001"));
  TEST_ASSERT_EQUAL(Role::Guest, back.roleOf("2002"));
  TEST_ASSERT_EQUAL_UINT32(42, back.find("2002")->quota.maxVectors);
  TEST_ASSERT_EQUAL_UINT32(3, back.find("2002")->quota.maxPins);

  // A malformed record is dropped, never fatal, and the good ones survive.
  TenantStore tolerant;
  tolerant.load(std::string("garbage-no-separators\x1E") + ts.dump());
  TEST_ASSERT_EQUAL(Role::Admin, tolerant.roleOf("1001"));

  // An unknown chat is Unknown - the closed default.
  TEST_ASSERT_EQUAL(Role::Unknown, back.roleOf("9999"));
}

static void test_legacy_adoption_changes_nobody_by_surprise() {
  TenantStore ts;
  ts.adoptLegacy({"1001"}, {"1001", "2002", "3003"});
  TEST_ASSERT_EQUAL(Role::Admin, ts.roleOf("1001"));
  // Already-allow-listed chats keep conversational access as Users - an
  // upgrade must not silently demote them to Unknown.
  TEST_ASSERT_EQUAL(Role::User, ts.roleOf("2002"));
  TEST_ASSERT_EQUAL(Role::User, ts.roleOf("3003"));

  // The single-account default: no explicit owner => the FIRST allow-listed
  // chat is the admin, matching the rest of the firmware.
  TenantStore ts2;
  ts2.adoptLegacy({}, {"5005", "6006"});
  TEST_ASSERT_EQUAL(Role::Admin, ts2.roleOf("5005"));
  TEST_ASSERT_EQUAL(1, (int)ts2.adminCount());
}

// CUM-27: only an Admin may ARM a wake-up. A wake-up fires an unattended turn into
// the owner's channel, so it is gated on manageTenants, the admin-only permission.
// This is the single predicate the tool rail and the web surface both consult.
static void test_only_admin_may_arm_wakeup() {
  TEST_ASSERT_TRUE (mayArmWakeup(Role::Admin));
  TEST_ASSERT_FALSE(mayArmWakeup(Role::User));
  TEST_ASSERT_FALSE(mayArmWakeup(Role::Guest));
  TEST_ASSERT_FALSE(mayArmWakeup(Role::Unknown));
  // The gate must track the manageTenants permission exactly (one source of truth).
  for (Role r : {Role::Unknown, Role::Guest, Role::User, Role::Admin})
    TEST_ASSERT_EQUAL(permsFor(r).manageTenants, mayArmWakeup(r));
}

// An approval writes the chat's row (it used to write none, and the chat resolved by the
// allowlist fallback from then on). What the row buys: a later change to who is "first on
// the allowlist" can never promote this chat.
static void test_approval_writes_the_row() {
  TenantStore t;
  std::string err;
  // A plain approval, an admin on the table: a User row.
  TEST_ASSERT_TRUE(t.setRole("owner", Role::Admin, err));
  TEST_ASSERT_TRUE(noteApproved(t, "alice", false, err));
  TEST_ASSERT_TRUE(t.known("alice"));
  TEST_ASSERT_EQUAL((int)Role::User, (int)t.roleOf("alice"));
  // Idempotent: nothing to change means no write to make.
  TEST_ASSERT_FALSE(noteApproved(t, "alice", false, err));
  // An explicit role stands: an approval never demotes or promotes one.
  TEST_ASSERT_TRUE(t.setRole("carla", Role::Guest, err));
  TEST_ASSERT_FALSE(noteApproved(t, "carla", false, err));
  TEST_ASSERT_EQUAL((int)Role::Guest, (int)t.roleOf("carla"));
  TEST_ASSERT_FALSE(noteApproved(t, "owner", false, err));
  TEST_ASSERT_EQUAL((int)Role::Admin, (int)t.roleOf("owner"));
  // A revoked row: approving again is an explicit grant (the hosted rule, now shared).
  TEST_ASSERT_TRUE(t.setRole("bob", Role::Unknown, err));
  TEST_ASSERT_TRUE(noteApproved(t, "bob", false, err));
  TEST_ASSERT_EQUAL((int)Role::User, (int)t.roleOf("bob"));

  // A fresh device, no admin yet: the chat the Telegram owner rule names keeps that rule
  // (a User row would lock the owner out of their own device), and gets no row.
  TenantStore fresh;
  TEST_ASSERT_FALSE(noteApproved(fresh, "first", true, err));
  TEST_ASSERT_FALSE(fresh.known("first"));
  // Anyone else on that fresh device is a User, in writing: the escalation this closes.
  TEST_ASSERT_TRUE(noteApproved(fresh, "second", false, err));
  TEST_ASSERT_EQUAL((int)Role::User, (int)fresh.roleOf("second"));
  // Counter-test, the row is what holds: with the first chat gone, the second becomes
  // the one the owner rule names, and its row still says User.
  const Role resolved = fresh.known("second") ? fresh.roleOf("second")
                                              : roleWithoutRow(true, true, fresh.adminCount() > 0);
  TEST_ASSERT_EQUAL((int)Role::User, (int)resolved);
  TEST_ASSERT_EQUAL((int)Role::Admin, (int)roleWithoutRow(true, true, fresh.adminCount() > 0));

  // A full table refuses in words; the allowlist fallback still answers User, no worse.
  TenantStore full;
  for (size_t i = 0; i < TenantStore::kMaxTenants; i++)
    TEST_ASSERT_TRUE(full.setRole("c" + std::to_string(i), Role::User, err));
  err.clear();
  TEST_ASSERT_FALSE(noteApproved(full, "late", false, err));
  TEST_ASSERT_FALSE(full.known("late"));
  TEST_ASSERT_FALSE(err.empty());
}

// The device's approval judges the owner rule from the lists it just wrote, not from the
// poll task's copy (which does not hold this chat yet): on a fresh device the first chat
// approved IS the owner and keeps that rule. It used to be written down as a User: the
// table then had no admin, and the owner could not change a setting from Telegram.
static void test_first_approval_on_a_fresh_device_keeps_the_owner_rule() {
  TenantStore t;
  std::string err;
  std::string allow = "111";   // what the approval just stored
  TEST_ASSERT_TRUE(telegramOwnerOf(allow, "", "111"));
  TEST_ASSERT_FALSE(noteApproved(t, "111", telegramOwnerOf(allow, "", "111"), err));
  TEST_ASSERT_FALSE(t.known("111"));
  TEST_ASSERT_EQUAL((int)Role::Admin, (int)roleWithoutRow(true, true, t.adminCount() > 0));
  allow += ",222";             // the second chat is a member, in writing
  TEST_ASSERT_FALSE(telegramOwnerOf(allow, "", "222"));
  TEST_ASSERT_TRUE(noteApproved(t, "222", telegramOwnerOf(allow, "", "222"), err));
  TEST_ASSERT_EQUAL((int)Role::User, (int)t.roleOf("222"));
  // An explicit owners list decides, and only for a chat that is on the allowlist.
  TEST_ASSERT_TRUE(telegramOwnerOf("111,222", "222", "222"));
  TEST_ASSERT_FALSE(telegramOwnerOf("111,222", "222", "111"));
  TEST_ASSERT_FALSE(telegramOwnerOf("111", "222", "222"));
  TEST_ASSERT_FALSE(telegramOwnerOf("", "", "111"));
  TEST_ASSERT_FALSE(telegramOwnerOf("111", "", ""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_role_permissions_matrix);
  RUN_TEST(test_only_admin_may_arm_wakeup);
  RUN_TEST(test_ttl_is_clamped_not_refused);
  RUN_TEST(test_permanent_pins_are_bounded);
  RUN_TEST(test_explicit_quota_overrides_role_default);
  RUN_TEST(test_last_admin_cannot_be_demoted_or_removed);
  RUN_TEST(test_store_roundtrip_and_tolerant_load);
  RUN_TEST(test_legacy_adoption_changes_nobody_by_surprise);
  RUN_TEST(test_approval_writes_the_row);
  RUN_TEST(test_first_approval_on_a_fresh_device_keeps_the_owner_rule);
  return UNITY_END();
}
