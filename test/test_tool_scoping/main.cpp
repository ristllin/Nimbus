#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/tool_registry.h"

// W14 - principal-scoped tool ADVERTISEMENT.
//
// The composed prompt used to list every registered tool to every conversation,
// so a guest was told the assistant could call skill.save / tenant.set_role /
// loop.create. Those handlers refuse, so nothing unsafe happened - but the
// assistant offered a capability it would then be denied (a failed round and a
// confusing walk-back), and the guest was shown the owner's admin surface.
//
// ⚠ These tests pin VISIBILITY only. The security boundary is the handler's own
// check, and the last test here proves the two are independent: a hidden tool is
// still dispatchable-and-refused, exactly as before.

using nimbus::orch::Principal;
using nimbus::orch::principalForRole;
using nimbus::orch::Role;
using nimbus::orch::ToolRegistry;
using nimbus::orch::ToolResult;

void setUp() {}
void tearDown() {}

static ToolResult okHandler(ArduinoJson::JsonObjectConst, const Principal&) {
  return ToolResult::ok("ran");
}
// An admin-gated handler: the REAL boundary, independent of advertisement.
static ToolResult adminHandler(ArduinoJson::JsonObjectConst, const Principal& who) {
  if (!who.perms().manageTenants) return ToolResult::fail("only an admin can do that");
  return ToolResult::ok("ran");
}

static ToolRegistry makeReg() {
  ToolRegistry r;
  r.add("memory.search", "Search memory.", okHandler);
  r.add("tenant.set_role", "Change a person's role.", adminHandler);
  r.add("skill.save", "Save a reusable skill.", adminHandler);
  r.setAdminOnly("tenant.set_role");
  r.setAdminOnly("skill.save");
  return r;
}

static bool listed(const std::vector<ToolRegistry::Spec>& v, const char* name) {
  for (const auto& s : v) if (s.name == name) return true;
  return false;
}

static void test_admin_sees_everything() {
  ToolRegistry r = makeReg();
  const auto specs = r.toolSpecsFor(principalForRole("c1", Role::Admin));
  TEST_ASSERT_EQUAL(3, (int)specs.size());
  TEST_ASSERT_TRUE(listed(specs, "memory.search"));
  TEST_ASSERT_TRUE(listed(specs, "tenant.set_role"));
  TEST_ASSERT_TRUE(listed(specs, "skill.save"));
}

static void test_user_and_guest_see_only_callable_tools() {
  ToolRegistry r = makeReg();
  for (Role role : {Role::User, Role::Guest, Role::Unknown}) {
    const auto specs = r.toolSpecsFor(principalForRole("c2", role));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)specs.size(), "only the open tool is advertised");
    TEST_ASSERT_TRUE(listed(specs, "memory.search"));
    TEST_ASSERT_FALSE(listed(specs, "tenant.set_role"));
    TEST_ASSERT_FALSE(listed(specs, "skill.save"));
  }
}

// A deny-all / unattributed principal (empty namespace) is the safe default and
// must not be treated as an admin.
static void test_default_principal_is_not_admin() {
  ToolRegistry r = makeReg();
  Principal p;   // no ns, no role
  const auto specs = r.toolSpecsFor(p);
  TEST_ASSERT_EQUAL(1, (int)specs.size());
  TEST_ASSERT_FALSE(listed(specs, "skill.save"));
}

// The unfiltered view is unchanged - the web/debug surfaces that deliberately
// show the whole registry keep working.
static void test_unfiltered_toolspecs_unchanged() {
  ToolRegistry r = makeReg();
  TEST_ASSERT_EQUAL(3, (int)r.toolSpecs().size());
  TEST_ASSERT_EQUAL(3, (int)r.manifest().size());
}

// ⚠ THE LOAD-BEARING TEST: hiding is not the boundary. A guest can still REACH a
// hidden tool (e.g. over /mcp, which lists+dispatches independently) and the
// handler refuses it - visibility and enforcement stay independent, so this
// change cannot become a security regression if a filter is ever bypassed.
static void test_hidden_tool_is_still_dispatchable_and_refused() {
  ToolRegistry r = makeReg();
  ArduinoJson::JsonDocument d;
  const Principal guest = principalForRole("c3", Role::Guest);
  ToolResult res = r.dispatch("skill.save", d.to<ArduinoJson::JsonObject>(), guest);
  TEST_ASSERT_FALSE(res.success);                  // refused by the HANDLER
  TEST_ASSERT_TRUE(res.error.find("admin") != std::string::npos);
  // ...and the same call as an admin succeeds, proving the refusal was the
  // role check and not the hiding.
  ToolResult okRes = r.dispatch("skill.save", d.to<ArduinoJson::JsonObject>(),
                                principalForRole("c4", Role::Admin));
  TEST_ASSERT_TRUE(okRes.success);
}

// An unknown name is a deliberate no-op: a renamed tool degrades to "advertised
// to everyone and refused by its handler", never to "silently missing".
static void test_setadminonly_unknown_name_is_noop() {
  ToolRegistry r = makeReg();
  r.setAdminOnly("does.not.exist");
  TEST_ASSERT_FALSE(r.isAdminOnly("does.not.exist"));
  TEST_ASSERT_EQUAL(1, (int)r.toolSpecsFor(principalForRole("c5", Role::Guest)).size());
}

// Re-registering a tool (add() replaces in place) must not silently clear its
// admin-only mark - that would re-expose it on the next boot path change.
static void test_readd_preserves_admin_only() {
  ToolRegistry r = makeReg();
  r.add("skill.save", "Save a reusable skill (v2).", adminHandler);
  TEST_ASSERT_TRUE(r.isAdminOnly("skill.save"));
  TEST_ASSERT_EQUAL(1, (int)r.toolSpecsFor(principalForRole("c6", Role::Guest)).size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_admin_sees_everything);
  RUN_TEST(test_user_and_guest_see_only_callable_tools);
  RUN_TEST(test_default_principal_is_not_admin);
  RUN_TEST(test_unfiltered_toolspecs_unchanged);
  RUN_TEST(test_hidden_tool_is_still_dispatchable_and_refused);
  RUN_TEST(test_setadminonly_unknown_name_is_noop);
  RUN_TEST(test_readd_preserves_admin_only);
  return UNITY_END();
}
