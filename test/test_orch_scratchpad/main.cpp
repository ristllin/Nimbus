#include <cstring>
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/scratchpad.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- Scratchpad: tiers, caps, rendering, round-trip ------------------------

static void test_active_task_set_and_cap() {
  Scratchpad sp;
  TEST_ASSERT_TRUE(sp.setActiveTask("wiring the ring driver"));
  TEST_ASSERT_EQUAL_STRING("wiring the ring driver", sp.activeTask().c_str());
  // over-cap active task is truncated (returns false = did not fully fit)
  std::string big(kScratchActiveMax + 50, 'x');
  TEST_ASSERT_FALSE(sp.setActiveTask(big));
  TEST_ASSERT_LESS_OR_EQUAL(kScratchActiveMax, (int)sp.activeTask().size());
  // empty clears
  TEST_ASSERT_TRUE(sp.setActiveTask(""));
  TEST_ASSERT_TRUE(sp.activeTask().empty());
}

// CUM-28 /clear semantics: clearing the ACTIVE TASK must leave the goal tiers
// intact (that is what lets /clear drop the conversation's focus while keeping
// long-term memory). Contrast with clearAll(), which wipes everything.
static void test_clear_active_preserves_tiers() {
  Scratchpad sp;
  sp.setActiveTask("debugging the SD mount");
  sp.add(Tier::Short, "check the card seating");
  sp.add(Tier::Long, "ship v4.3.0");
  sp.setActiveTask("");                     // the /clear active-task drop
  TEST_ASSERT_TRUE(sp.activeTask().empty());
  TEST_ASSERT_EQUAL_INT(1, sp.count(Tier::Short));   // tiers untouched
  TEST_ASSERT_EQUAL_INT(1, sp.count(Tier::Long));
  TEST_ASSERT_FALSE(sp.empty());
  // clearAll() is the heavier op /clear deliberately does NOT use.
  sp.clearAll();
  TEST_ASSERT_TRUE(sp.empty());
}

static void test_tier_add_and_count_cap() {
  Scratchpad sp;
  for (int i = 0; i < kScratchTierItems; i++)
    TEST_ASSERT_TRUE(sp.add(Tier::Short, "task " + std::to_string(i)));
  TEST_ASSERT_EQUAL_INT(kScratchTierItems, sp.count(Tier::Short));
  // one past the cap is refused
  TEST_ASSERT_FALSE(sp.add(Tier::Short, "overflow"));
  TEST_ASSERT_EQUAL_INT(kScratchTierItems, sp.count(Tier::Short));
  // empty/whitespace item refused, not counted
  TEST_ASSERT_FALSE(sp.add(Tier::Mid, "   "));
  TEST_ASSERT_EQUAL_INT(0, sp.count(Tier::Mid));
}

static void test_item_byte_cap_and_newline_strip() {
  Scratchpad sp;
  std::string big(kScratchItemMax + 40, 'y');
  TEST_ASSERT_TRUE(sp.add(Tier::Long, big));  // truncated, still accepted
  TEST_ASSERT_LESS_OR_EQUAL(kScratchItemMax, (int)sp.items(Tier::Long)[0].size());
  // embedded newlines are flattened so serialization stays line-safe
  sp.add(Tier::Long, "line1\nline2");
  TEST_ASSERT_EQUAL_STRING("line1 line2", sp.items(Tier::Long)[1].c_str());
}

static void test_replace_applies_both_caps() {
  Scratchpad sp;
  std::vector<std::string> many;
  for (int i = 0; i < kScratchTierItems + 5; i++) many.push_back("g" + std::to_string(i));
  int kept = sp.replace(Tier::Mid, many);
  TEST_ASSERT_EQUAL_INT(kScratchTierItems, kept);  // count cap on replace
  TEST_ASSERT_EQUAL_INT(kScratchTierItems, sp.count(Tier::Mid));
}

static void test_prompt_block_skips_empty() {
  Scratchpad sp;
  std::string out;
  sp.appendPromptBlock(out);
  TEST_ASSERT_TRUE(out.empty());  // empty scratchpad renders nothing
  sp.setActiveTask("ship it");
  sp.add(Tier::Short, "run tests");
  sp.appendPromptBlock(out);
  TEST_ASSERT_NOT_NULL(strstr(out.c_str(), "SCRATCHPAD"));
  TEST_ASSERT_NOT_NULL(strstr(out.c_str(), "Now: ship it"));
  TEST_ASSERT_NOT_NULL(strstr(out.c_str(), "- run tests"));
}

static void test_serialize_roundtrip() {
  Scratchpad sp;
  sp.setActiveTask("active thing");
  sp.add(Tier::Short, "s1"); sp.add(Tier::Short, "s2");
  sp.add(Tier::Mid, "m1");
  sp.add(Tier::Long, "l1");
  std::string blob = sp.serialize();

  Scratchpad sp2;
  TEST_ASSERT_TRUE(sp2.deserialize(blob));
  TEST_ASSERT_EQUAL_STRING("active thing", sp2.activeTask().c_str());
  TEST_ASSERT_EQUAL_INT(2, sp2.count(Tier::Short));
  TEST_ASSERT_EQUAL_INT(1, sp2.count(Tier::Mid));
  TEST_ASSERT_EQUAL_INT(1, sp2.count(Tier::Long));
  TEST_ASSERT_EQUAL_STRING("s2", sp2.items(Tier::Short)[1].c_str());
}

static void test_deserialize_tolerates_garbage() {
  Scratchpad sp;
  // unknown tags / blank lines are skipped, valid lines kept - no crash
  TEST_ASSERT_TRUE(sp.deserialize("Ahello\n\n?junk\nSkeep\nZ\n"));
  TEST_ASSERT_EQUAL_STRING("hello", sp.activeTask().c_str());
  TEST_ASSERT_EQUAL_INT(1, sp.count(Tier::Short));
  TEST_ASSERT_EQUAL_STRING("keep", sp.items(Tier::Short)[0].c_str());
}

// ---- MemConfig: defaults + clamps -------------------------------------------

static void test_memconfig_defaults() {
  MemConfig c;
  TEST_ASSERT_EQUAL_INT(10, c.retrievalCount);
  TEST_ASSERT_EQUAL_FLOAT(0.95f, c.decayFactor);
}

static void test_memconfig_clamps() {
  MemConfig c;
  c.setRetrievalCount(9999);
  TEST_ASSERT_EQUAL_INT(MemConfig::kRetrievalMax, c.retrievalCount);
  c.setRetrievalCount(-3);
  TEST_ASSERT_EQUAL_INT(MemConfig::kRetrievalMin, c.retrievalCount);
  c.setDecayFactor(0.1f);
  TEST_ASSERT_EQUAL_FLOAT(MemConfig::kDecayMin, c.decayFactor);
  c.setRelevanceThreshold(2.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, c.relevanceThreshold);
}

static void test_memconfig_apply_named() {
  MemConfig c;
  TEST_ASSERT_TRUE(c.applyInt("retrieval_count", 25));
  TEST_ASSERT_EQUAL_INT(25, c.retrievalCount);
  TEST_ASSERT_TRUE(c.applyFloat("decay_factor", 0.8f));
  TEST_ASSERT_EQUAL_FLOAT(0.8f, c.decayFactor);
  TEST_ASSERT_FALSE(c.applyInt("nonsense", 5));   // unknown field rejected
  TEST_ASSERT_FALSE(c.applyFloat("retrieval_count", 1.0f));  // wrong type bucket
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_active_task_set_and_cap);
  RUN_TEST(test_clear_active_preserves_tiers);
  RUN_TEST(test_tier_add_and_count_cap);
  RUN_TEST(test_item_byte_cap_and_newline_strip);
  RUN_TEST(test_replace_applies_both_caps);
  RUN_TEST(test_prompt_block_skips_empty);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_deserialize_tolerates_garbage);
  RUN_TEST(test_memconfig_defaults);
  RUN_TEST(test_memconfig_clamps);
  RUN_TEST(test_memconfig_apply_named);
  return UNITY_END();
}
