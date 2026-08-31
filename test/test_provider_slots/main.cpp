#include <unity.h>

#include <cstring>
#include <set>
#include <string>

#include "nimbus/orch/provider_slots.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// The gating surface (the /api/orch `providers` emitter in webui.cpp and the model-
// pick validator beside it) is built by looping kProviderSlots. This suite is the
// regression guard for CUM-213: it fails if a provider is dropped from the canonical
// registry, which is the ONLY list those surfaces enumerate - so a provider that is
// in the registry is necessarily emitted WITH its verify/vts gating fields, and a
// provider missing here would be missing from the gate. The exact bug (only Cumulo
// showed "verify the key to unlock") was cumulo/zai being appended by hand beside a
// three-provider table; keeping every provider in one table is what prevents it.

// Every provider that MUST appear in the gate. Adding a real provider is a deliberate
// act: add it to the registry AND to this list. Removing one from the registry (so it
// would show ungated) fails here.
static const char* kExpected[] = {"cumulo", "openai", "anthropic", "mistral", "zai"};
static constexpr size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

// The registry is the single source, so its membership is EXACTLY the expected set -
// no provider silently dropped from the gate, none silently added without a decision.
static void test_registry_is_exactly_the_expected_set() {
  TEST_ASSERT_EQUAL_size_t(kExpectedCount, kProviderSlotCount);
  for (size_t i = 0; i < kExpectedCount; i++) {
    const ProviderSlot* s = findProviderSlot(kExpected[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, kExpected[i]);   // expected provider missing from the gate
    TEST_ASSERT_TRUE_MESSAGE(isProviderSlug(kExpected[i]), kExpected[i]);
  }
  // ...and nothing beyond the expected set slipped in unreviewed.
  for (size_t i = 0; i < kProviderSlotCount; i++) {
    bool found = false;
    for (size_t j = 0; j < kExpectedCount; j++)
      if (!std::strcmp(kProviderSlots[i].slug, kExpected[j])) found = true;
    TEST_ASSERT_TRUE_MESSAGE(found, kProviderSlots[i].slug);   // unreviewed provider in the gate
  }
}

// Cumulo Nimbus is the recommended flagship and renders first (CUM-201).
static void test_cumulo_is_recommended_and_first() {
  TEST_ASSERT_GREATER_THAN_size_t(0, kProviderSlotCount);
  TEST_ASSERT_EQUAL_STRING("cumulo", kProviderSlots[0].slug);
  TEST_ASSERT_TRUE(kProviderSlots[0].recommended);
  // Exactly one recommended slot - two flagships is a UX bug.
  int rec = 0;
  for (size_t i = 0; i < kProviderSlotCount; i++)
    if (kProviderSlots[i].recommended) rec++;
  TEST_ASSERT_EQUAL_INT(1, rec);
}

// Every slot is fully populated; slugs and key-fields are unique (a collision would
// route one provider's key into another's store slot).
static void test_slots_well_formed_and_unique() {
  std::set<std::string> slugs, fields;
  for (size_t i = 0; i < kProviderSlotCount; i++) {
    const ProviderSlot& s = kProviderSlots[i];
    TEST_ASSERT_NOT_NULL(s.slug);
    TEST_ASSERT_NOT_NULL(s.label);
    TEST_ASSERT_NOT_NULL(s.keyField);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(s.slug) > 0, "empty slug");
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(s.label) > 0, s.slug);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(s.keyField) > 0, s.slug);
    TEST_ASSERT_TRUE_MESSAGE(slugs.insert(s.slug).second, s.slug);        // duplicate slug
    TEST_ASSERT_TRUE_MESSAGE(fields.insert(s.keyField).second, s.keyField);  // duplicate key-field
  }
}

// The slug -> key-field mapping is frozen: these field ids are the persisted store
// slots and the front-end input ids. A rename silently misroutes a saved key.
static void test_keyfields_are_frozen() {
  TEST_ASSERT_EQUAL_STRING("cumuloKey", findProviderSlot("cumulo")->keyField);
  TEST_ASSERT_EQUAL_STRING("oaiKey",    findProviderSlot("openai")->keyField);
  TEST_ASSERT_EQUAL_STRING("antKey",    findProviderSlot("anthropic")->keyField);
  TEST_ASSERT_EQUAL_STRING("mistKey",   findProviderSlot("mistral")->keyField);
  TEST_ASSERT_EQUAL_STRING("zaiKey",    findProviderSlot("zai")->keyField);
}

// "custom" is a free-form endpoint with its own UI + verify path, NOT a registry
// slot; unknown and empty slugs are rejected.
static void test_custom_and_unknown_are_not_slots() {
  TEST_ASSERT_FALSE(isProviderSlug("custom"));
  TEST_ASSERT_FALSE(isProviderSlug("bogus"));
  TEST_ASSERT_FALSE(isProviderSlug(""));
  TEST_ASSERT_FALSE(isProviderSlug(nullptr));
  TEST_ASSERT_NULL(findProviderSlot("custom"));
  TEST_ASSERT_NULL(findProviderSlot(nullptr));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_registry_is_exactly_the_expected_set);
  RUN_TEST(test_cumulo_is_recommended_and_first);
  RUN_TEST(test_slots_well_formed_and_unique);
  RUN_TEST(test_keyfields_are_frozen);
  RUN_TEST(test_custom_and_unknown_are_not_slots);
  return UNITY_END();
}
