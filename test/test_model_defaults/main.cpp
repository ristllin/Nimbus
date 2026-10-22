#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "../../src/agent/agent_config.h"   // the head defaults the device names (pinned below)
#include "nimbus/harness/output_caps.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/provider_slots.h"

// The role-default table (lane DEF, owner requirement 21 Oct 2026; ruling R7: picked by
// price class from the catalog, with named fallbacks). Class tests over every provider
// slot and every role, the release-time agreement with the recorded provider catalogs,
// the device's named head defaults, and the background turn role.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static uint64_t listPrice(const char* slot, const char* id) {
  const BuiltinPrice* p = builtinModelPrice(slot, id);
  return p ? (uint64_t)p->microIn + p->microOut : 0;
}

// Every first-class provider has a row for every role: a new provider (or role) with
// no defensive default fails here instead of shipping with "".
static void test_every_slot_has_every_role() {
  for (const ProviderSlot& s : kProviderSlots)
    for (int r = 0; r < kDefaultRoleCount; ++r) {
      const RoleDefault* row = roleDefaultRow(s.slug, (DefaultRole)r);
      TEST_ASSERT_NOT_NULL_MESSAGE(row, s.slug);
      TEST_ASSERT_TRUE(std::strlen(row->fallback) > 0);
      TEST_ASSERT_TRUE(std::strlen(row->tier) > 0);
    }
  TEST_ASSERT_NULL(roleDefaultRow("custom", DefaultRole::Head));   // the owner's own model
  TEST_ASSERT_NULL(roleDefaultRow(nullptr, DefaultRole::Head));
  TEST_ASSERT_EQUAL_STRING("background", defaultRoleSlug(DefaultRole::Background));
}

// Every named fallback has a price of its own and sits in its own class; every
// non-head role is no pricier than its provider's head default.
static void test_fallbacks_are_priced_and_in_class() {
  for (const RoleDefault& r : kRoleDefaults) {
    TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice(r.slot, r.fallback), r.fallback);
    TEST_ASSERT_TRUE_MESSAGE(inPriceClass(r, r.fallback), r.fallback);
    const RoleDefault* head = roleDefaultRow(r.slot, DefaultRole::Head);
    TEST_ASSERT_TRUE_MESSAGE(listPrice(r.slot, r.fallback) <= listPrice(r.slot, head->fallback),
                             r.fallback);
  }
}

// The owner's shape (PRD addendum C2): Anthropic head sonnet (never opus or fable),
// the rest haiku-5-5; OpenAI head mini class, subs nano class (the cheapest priced
// OpenAI chat model); Mistral medium head, small subs; Z.ai flash for subs.
static void test_the_owner_shape() {
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", roleDefaultRow("anthropic", DefaultRole::Head)->fallback);
  for (DefaultRole r : {DefaultRole::Sub, DefaultRole::Caption, DefaultRole::Background})
    TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", roleDefaultRow("anthropic", r)->fallback);
  for (const RoleDefault& r : kRoleDefaults) {
    if (std::strcmp(r.slot, "anthropic") != 0) continue;
    TEST_ASSERT_NULL(std::strstr(r.fallback, "opus"));
    TEST_ASSERT_NULL(std::strstr(r.fallback, "fable"));
  }
  TEST_ASSERT_EQUAL_STRING("mistral-medium-latest", roleDefaultRow("mistral", DefaultRole::Head)->fallback);
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", roleDefaultRow("mistral", DefaultRole::Sub)->fallback);
  TEST_ASSERT_EQUAL_STRING("glm-5.3-flash", roleDefaultRow("zai", DefaultRole::Sub)->fallback);
  // OpenAI: the sub default is the cheapest priced OpenAI chat row; the head is the
  // next class up, still cheaper than every standard and flagship row.
  uint64_t cheapest = UINT64_MAX;
  for (const BuiltinPrice& p : kBuiltinPrices) {
    if (std::strcmp(p.slot, "openai") != 0 || std::strcmp(p.family, "*") == 0) continue;
    if (std::strstr(p.family, "embedding") || std::strstr(p.family, "deep-research") ||
        std::strcmp(p.family, "gpt-4o-mini") == 0)   // retired by owner ruling R1
      continue;
    const uint64_t c = (uint64_t)p.microIn + p.microOut;
    if (c < cheapest) cheapest = c;
  }
  TEST_ASSERT_EQUAL_UINT64(cheapest, listPrice("openai", roleDefaultRow("openai", DefaultRole::Sub)->fallback));
  TEST_ASSERT_TRUE(listPrice("openai", roleDefaultRow("openai", DefaultRole::Head)->fallback) <
                   listPrice("openai", "gpt-5.6-terra"));
}

// The device names its head defaults in agent_config.h (the always-valid pick, the
// adapters' fallback): each equals the table's head row.
static void test_agent_config_names_the_table_heads() {
  TEST_ASSERT_EQUAL_STRING(roleDefaultRow("anthropic", DefaultRole::Head)->fallback, ANT_MODEL);
  TEST_ASSERT_EQUAL_STRING(roleDefaultRow("openai", DefaultRole::Head)->fallback, OPENAI_MODEL);
  TEST_ASSERT_EQUAL_STRING(roleDefaultRow("mistral", DefaultRole::Head)->fallback, MISTRAL_MODEL);
  TEST_ASSERT_EQUAL_STRING(roleDefaultRow("zai", DefaultRole::Head)->fallback, ZAI_MODEL);
  TEST_ASSERT_EQUAL_STRING(roleDefaultRow("cumulo", DefaultRole::Head)->fallback, CUMULO_MODEL);
}

// ---- the catalog pick ---------------------------------------------------------------

static std::string catalogCsv(const char* provider) {
  std::ifstream f(std::string("test/support/fixtures/models_2026-10/") + provider + ".json",
                  std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  JsonDocument doc;
  if (deserializeJson(doc, ss.str())) return std::string();
  std::string csv;
  for (JsonObjectConst m : doc["data"].as<JsonArrayConst>()) {
    const char* id = m["id"] | "";
    if (!*id) continue;
    if (!csv.empty()) csv += ',';
    csv += id;
  }
  return csv;
}

// Release-time agreement (R7): over the recorded provider catalogs, the price-class
// pick IS the named fallback for every direct provider and role. A cheaper model in a
// class (or a named id missing from its provider's list) fails here first.
static void test_class_pick_over_the_recorded_catalogs_is_the_named_fallback() {
  for (const char* p : {"anthropic", "openai", "mistral", "zai"}) {
    const std::string csv = catalogCsv(p);
    TEST_ASSERT_TRUE_MESSAGE(csv.size() > 20, p);
    for (int r = 0; r < kDefaultRoleCount; ++r) {
      const RoleDefault* row = roleDefaultRow(p, (DefaultRole)r);
      TEST_ASSERT_TRUE_MESSAGE(modelInChoices(csv, row->fallback), row->fallback);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(row->fallback,
                                       roleDefaultModel(p, (DefaultRole)r, csv, true).c_str(), p);
    }
  }
}

static void test_cold_start_and_empty_lists_answer_the_named_fallback() {
  for (const RoleDefault& r : kRoleDefaults) {
    TEST_ASSERT_EQUAL_STRING(r.fallback,
                             roleDefaultModel(r.slot, r.role, staticModelChoices(r.slot), false).c_str());
    TEST_ASSERT_EQUAL_STRING(r.fallback, roleDefaultModel(r.slot, r.role, "", true).c_str());
    // The cold-start list carries it (lane DEF g4): a never-verified provider offers and
    // validates the default it runs, so a spawn is never coerced off it.
    TEST_ASSERT_TRUE_MESSAGE(modelInChoices(staticModelChoices(r.slot), r.fallback), r.fallback);
  }
  TEST_ASSERT_EQUAL_STRING("", roleDefaultModel("custom", DefaultRole::Head, "x", true).c_str());
}

static void test_harvested_list_picks_the_cheapest_class_member() {
  // A tie (sonnet-5-5 and sonnet-5 cost the same): the named fallback wins even when
  // listed second.
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5",
                           roleDefaultModel("anthropic", DefaultRole::Head,
                                            "claude-opus-5,claude-sonnet-5-5,claude-sonnet-5", true)
                               .c_str());
  // The named id retired upstream: the cheapest priced member of the class left.
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5-5",
                           roleDefaultModel("anthropic", DefaultRole::Head,
                                            "claude-opus-5,claude-sonnet-5-5,claude-sonnet-4-6", true)
                               .c_str());
  // No class member listed at all (and never a pricier class): the named fallback.
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5",
                           roleDefaultModel("anthropic", DefaultRole::Sub, "claude-opus-5,claude-sonnet-5", true)
                               .c_str());
  // A dated or -latest alias belongs to its family's class.
  TEST_ASSERT_EQUAL_STRING("mistral-small-2603",
                           roleDefaultModel("mistral", DefaultRole::Sub, "mistral-large-latest,mistral-small-2603", true)
                               .c_str());
  // An unpriced id is in no class (fail-closed money: never picked).
  const RoleDefault* sub = roleDefaultRow("zai", DefaultRole::Sub);
  TEST_ASSERT_FALSE(inPriceClass(*sub, "glm-5.3-flashx"));
  TEST_ASSERT_TRUE(inPriceClass(*sub, "glm-5.3-flash"));
}

// ---- the background turn role --------------------------------------------------------

static void test_background_scope_is_thread_local_and_nests() {
  TEST_ASSERT_TRUE(currentTurnRole() == TurnRole::Owner);
  {
    BackgroundTurnScope outer;
    TEST_ASSERT_TRUE(currentTurnRole() == TurnRole::Background);
    TurnRole other = TurnRole::Background;
    std::thread t([&other] { other = currentTurnRole(); });   // a web request elsewhere
    t.join();
    TEST_ASSERT_TRUE(other == TurnRole::Owner);
    {
      BackgroundTurnScope inner;
      TEST_ASSERT_TRUE(currentTurnRole() == TurnRole::Background);
    }
    TEST_ASSERT_TRUE(currentTurnRole() == TurnRole::Background);
  }
  TEST_ASSERT_TRUE(currentTurnRole() == TurnRole::Owner);
}

// A background turn pins low effort (the silent-routine cost, 22 Oct); an owner turn
// keeps medium; the fold stays low either way.
static void test_background_turn_pins_low_effort() {
  using agent::providers::outputCapFor;
  using agent::providers::RoundKind;
  TEST_ASSERT_EQUAL_STRING("medium", outputCapFor("anthropic", "claude-haiku-5-5", RoundKind::ToolRound).effort);
  {
    BackgroundTurnScope bg;
    for (RoundKind k : {RoundKind::ToolRound, RoundKind::FinalRound, RoundKind::SingleShot, RoundKind::Fold})
      TEST_ASSERT_EQUAL_STRING("low", outputCapFor("anthropic", "claude-haiku-5-5", k).effort);
    // A model that takes no effort field still sends none.
    TEST_ASSERT_EQUAL_STRING("", outputCapFor("anthropic", "claude-haiku-4-5", RoundKind::ToolRound).effort);
  }
  TEST_ASSERT_EQUAL_STRING("low", outputCapFor("anthropic", "claude-sonnet-5-5", RoundKind::Fold).effort);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_slot_has_every_role);
  RUN_TEST(test_fallbacks_are_priced_and_in_class);
  RUN_TEST(test_the_owner_shape);
  RUN_TEST(test_agent_config_names_the_table_heads);
  RUN_TEST(test_class_pick_over_the_recorded_catalogs_is_the_named_fallback);
  RUN_TEST(test_cold_start_and_empty_lists_answer_the_named_fallback);
  RUN_TEST(test_harvested_list_picks_the_cheapest_class_member);
  RUN_TEST(test_background_scope_is_thread_local_and_nests);
  RUN_TEST(test_background_turn_pins_low_effort);
  return UNITY_END();
}
