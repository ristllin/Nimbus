#include <unity.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/provider_slots.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static const char* kModelFixDir = "test/support/fixtures/models";

static bool readFile(const char* path, std::string& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

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

// CUM-246: the CLASS guard over the model-pick / verify-harvest surface. The
// per-provider tests in test_model_catalog are INSTANCE tests (one leg each for
// openai / anthropic / mistral / zai / cumulo); this iterates the WHOLE registry
// and asserts every provider is actually KNOWN to the model catalog, so a slot
// added to kProviderSlots without wiring the surface that harvests + classifies
// its models FAILS here rather than shipping a provider whose Models list is empty
// (the CUM-201 / CUM-213 class: a catalog-recommended provider silently unhandled).
static void test_every_catalog_provider_is_model_covered() {
  for (const ProviderSlot& slot : kProviderSlots) {
    if (slot.recommended) {
      // The router (Cumulo): no /v1/models fixture of its own - it inherits its
      // upstream's classification via the "<upstream>/<id>" convention. Assert that
      // path is honored, so the flagship one-key slot is never left unclassified.
      ModelInfo up = classifyCatalogEntry(slot.slug, "openai/gpt-4o-mini");
      TEST_ASSERT_NOT_EQUAL_MESSAGE(0, up.roles, slot.slug);          // classified as a chat model
      TEST_ASSERT_EQUAL_STRING_MESSAGE("openai", up.upstream.c_str(), slot.slug);  // upstream tagged
      continue;
    }
    // A first-class BYOK provider: its recorded /v1/models fixture (the same body
    // the device's verify harvest parses) must classify to a non-empty, role-bearing
    // catalog. A provider the catalog cannot parse or classifies to nothing would
    // present an empty Models list on the device - the exact silent failure.
    std::string body;
    const std::string path = std::string(kModelFixDir) + "/" + slot.slug + ".json";
    TEST_ASSERT_TRUE_MESSAGE(readFile(path.c_str(), body), slot.slug);  // fixture missing
    std::vector<ModelInfo> models;
    const size_t n = parseModelsList(slot.slug, body, models);
    TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(0, n, slot.slug);           // no models parsed
    uint16_t roleUnion = 0;
    for (const ModelInfo& m : models) {
      TEST_ASSERT_TRUE_MESSAGE(!m.id.empty(), slot.slug);              // a nameless model
      roleUnion |= m.roles;
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, roleUnion, slot.slug);            // catalog knows no role for it
  }
}

// CUM-246 (hand-off #2): the fanout guard. store_config's `anyKeyed` and webui's
// onboarding-finish + provider-verified gates now enumerate the registry through
// nimbus::orch::anySlotWhere(pred) - the ONE portable walk both device surfaces share
// (native links no src/, so this shared seam is what a host test can reach). These two
// legs drive that exact walk with a FAKE store (a set of keyed/verified slugs) and pin
// the class rule the recurring bug kept breaking: no registry slot is skipped by the
// fanout, and any single provider - INCLUDING the recommended cumulo and zai, the exact
// b2f4930 (anyKeyed) and 5069ab3 (onboarding gate) misses - is enough to open the gate.
// A slot added to kProviderSlots is covered here with no new code; a fanout that reverts
// to a hand-listed subset (dropping a registry slot) FAILS.

// With an empty fake store the fanout is closed AND must still have visited every slot -
// so a loop that stops short (the "provider missing from the gate" failure) is caught.
static void test_fanout_visits_every_registry_slot() {
  std::set<std::string> visited;
  bool any = anySlotWhere([&](const char* slug) { visited.insert(slug); return false; });
  TEST_ASSERT_FALSE(any);   // nothing keyed -> gate closed
  TEST_ASSERT_EQUAL_size_t(kProviderSlotCount, visited.size());
  for (const ProviderSlot& slot : kProviderSlots)
    TEST_ASSERT_TRUE_MESSAGE(visited.count(slot.slug), slot.slug);   // a slot the fanout skipped
}

// A fake store keyed/verified for exactly ONE provider must open the gate - for EVERY
// provider in turn. This is the direct regression for anyKeyed missing cumulo/zai and
// the onboarding gate missing the recommended cumulo: a device with only that provider
// could not be recognized / could not finish setup.
static void test_each_provider_alone_opens_the_gate() {
  for (const ProviderSlot& target : kProviderSlots) {
    const std::string keyed = target.slug;
    bool open = anySlotWhere([&](const char* slug) { return keyed == slug; });
    TEST_ASSERT_TRUE_MESSAGE(open, target.slug);   // this provider alone did not open the gate
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_registry_is_exactly_the_expected_set);
  RUN_TEST(test_fanout_visits_every_registry_slot);
  RUN_TEST(test_each_provider_alone_opens_the_gate);
  RUN_TEST(test_every_catalog_provider_is_model_covered);
  RUN_TEST(test_cumulo_is_recommended_and_first);
  RUN_TEST(test_slots_well_formed_and_unique);
  RUN_TEST(test_keyfields_are_frozen);
  RUN_TEST(test_custom_and_unknown_are_not_slots);
  return UNITY_END();
}
