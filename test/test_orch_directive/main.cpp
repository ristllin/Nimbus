#include <unity.h>

#include <string>

#include "nimbus/orch/caps.h"        // kMemDirectiveMax
#include "nimbus/orch/directive.h"   // kOwnerDirectiveDefault, effectiveDirective
#include "nimbus/orch/memory.h"      // OrchMemory::directive() re-cap
#include "nimbus/orch/moderation.h"  // gateApplies (CUM-275 independence)

// Owner directive core (TF-N9). The directive is USER-owned text injected into the
// system prompt. These pin the class rules the feature must never lose:
//  - the shipped default is present when nothing is stored (fresh device / revert),
//  - a stored value overrides it (effective = stored),
//  - the default fits the cap and is copy-clean (no em dash, AGENTS.md section 6),
//  - the byte cap is enforced on the read path (UTF-8-safe), and
//  - moderation of model output is INDEPENDENT of the directive (CUM-275): the
//    outbound gate keys on role + config, never on any prompt text.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Empty stored value => the shipped default (the zero-effort / revert path).
static void test_effective_empty_yields_default() {
  TEST_ASSERT_EQUAL_STRING(kOwnerDirectiveDefault, effectiveDirective("").c_str());
}

// A stored value overrides the default (it is a replacement, not an append).
static void test_effective_stored_overrides_default() {
  TEST_ASSERT_EQUAL_STRING("be terse", effectiveDirective("be terse").c_str());
}

// The default must be non-empty and fit within the cap with real headroom.
static void test_default_fits_the_cap() {
  const size_t len = std::string(kOwnerDirectiveDefault).size();
  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE_MESSAGE(len <= (size_t)kMemDirectiveMax, "default exceeds kMemDirectiveMax");
}

// The default is user-facing copy: no em dash (U+2014, "\xE2\x80\x94"), which the
// project bans everywhere in user copy (AGENTS.md section 6).
static void test_default_is_copy_clean() {
  const std::string d = kOwnerDirectiveDefault;
  TEST_ASSERT_TRUE_MESSAGE(d.find("\xE2\x80\x94") == std::string::npos,
                           "default directive contains an em dash");
}

// The read path caps the stored directive to kMemDirectiveMax (UTF-8-safe). This
// is the last-line enforcement behind the web route + store-layer clamps.
static void test_directive_read_is_capped() {
  OrchMemory m;
  m.begin(nullptr, std::string(kMemDirectiveMax + 500, 'x'));  // oversize, out-of-band
  TEST_ASSERT_TRUE_MESSAGE(m.directive().size() <= (size_t)kMemDirectiveMax,
                           "OrchMemory::directive() did not cap oversize input");
}

// CUM-275 non-interaction: moderation of a model reply is decided by role + owner
// switches ONLY - it takes no directive/prompt text at all - so a directive can
// never turn the outbound gate off. A guest reply is still screened; the owner is
// exempt regardless. (The device self-tags its own system copy by device-name
// prefix, which a model reply never carries - see orchestrator.cpp deliver().)
static void test_moderation_is_independent_of_directive() {
  ModConfig cfg;
  cfg.outbound = true;   // owner enabled outbound screening
  TEST_ASSERT_TRUE(gateApplies(ModGate::OutboundReply, Role::Guest, cfg));
  TEST_ASSERT_TRUE(gateApplies(ModGate::OutboundReply, Role::User, cfg));
  TEST_ASSERT_FALSE(gateApplies(ModGate::OutboundReply, Role::Admin, cfg));  // owner exempt
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_effective_empty_yields_default);
  RUN_TEST(test_effective_stored_overrides_default);
  RUN_TEST(test_default_fits_the_cap);
  RUN_TEST(test_default_is_copy_clean);
  RUN_TEST(test_directive_read_is_capped);
  RUN_TEST(test_moderation_is_independent_of_directive);
  UNITY_END();
  return 0;
}
