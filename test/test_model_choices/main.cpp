#include <unity.h>

#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/provider_slots.h"

// CUM-486: the ONE model-choice derivation, tested over EVERY provider slot in the
// canonical registry (never a hand list), so a provider added to kProviderSlots
// without a cold-start list fails here instead of reaching a device as "p: ;".

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static std::vector<std::string> split(const std::string& csv) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s <= csv.size()) {
    size_t c = csv.find(',', s);
    if (c == std::string::npos) c = csv.size();
    out.push_back(csv.substr(s, c - s));
    s = c + 1;
  }
  return out;
}

// Every first-class slot ships a non-empty cold-start list with no blank or
// duplicate entries: a never-verified provider must still offer real models.
static void test_every_slot_has_a_cold_start_list() {
  for (const ProviderSlot& slot : kProviderSlots) {
    const std::string csv = staticModelChoices(slot.slug);
    TEST_ASSERT_FALSE_MESSAGE(csv.empty(), slot.slug);
    std::set<std::string> seen;
    for (const std::string& id : split(csv)) {
      TEST_ASSERT_FALSE_MESSAGE(id.empty(), slot.slug);
      TEST_ASSERT_TRUE_MESSAGE(seen.insert(id).second, (std::string(slot.slug) + " dup " + id).c_str());
    }
  }
  TEST_ASSERT_EQUAL_STRING("", staticModelChoices("custom"));   // endpoint-configured, no list
  TEST_ASSERT_EQUAL_STRING("", staticModelChoices(nullptr));
}

// The cold-start lists must agree with the live harvest filter: the harvest
// (provider_verify.cpp kNever) refuses these non-chat families because they
// cannot run the turn contract, so a cold-start id from one of them is offered on
// a fresh device and coerced away on a verified one - the o4-mini-deep-research
// mismatch. Families mirrored from that filter.
static void test_cold_start_lists_hold_no_family_the_harvest_refuses() {
  static const char* kNever[] = {"embed",  "moderation", "audio",   "realtime", "transcribe",
                                 "tts",    "whisper",    "image",   "dall-e",   "ocr",
                                 "voxtral", "davinci",   "babbage", "instruct", "search-",
                                 "chatgpt", "research",  "fim"};
  for (const ProviderSlot& slot : kProviderSlots)
    for (const std::string& id : split(staticModelChoices(slot.slug)))
      for (const char* fam : kNever)
        TEST_ASSERT_TRUE_MESSAGE(id.find(fam) == std::string::npos,
                                 (std::string(slot.slug) + ": " + id + " is a '" + fam +
                                  "' model the harvest refuses").c_str());
}

// THE derivation, for every slot: a live harvest wins; with none, the cold-start
// list. Advertised (the derivation) == spawnable (modelInChoices over the same
// derivation) item for item, and a model outside the list never validates.
static void test_derivation_advertised_equals_spawnable_for_every_slot() {
  for (const ProviderSlot& slot : kProviderSlots) {
    const std::string harvested = std::string("live-") + slot.slug + "-a,live-" + slot.slug + "-b";
    for (const std::string& h : {std::string(), harvested}) {
      const std::string csv = effectiveModelChoices(slot.slug, h);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(h.empty() ? staticModelChoices(slot.slug) : h.c_str(),
                                       csv.c_str(), slot.slug);
      for (const std::string& id : split(csv))
        TEST_ASSERT_TRUE_MESSAGE(modelInChoices(csv, id), (std::string(slot.slug) + ": " + id).c_str());
      TEST_ASSERT_FALSE_MESSAGE(modelInChoices(csv, "not-a-model"), slot.slug);
    }
  }
}

static void test_model_in_choices_exact_and_trimmed() {
  TEST_ASSERT_TRUE(modelInChoices("gpt-5.6, gpt-5.6-terra ,gpt-6-astra", "gpt-5.6-terra"));
  TEST_ASSERT_TRUE(modelInChoices("gpt-5.6, gpt-5.6-terra ,gpt-6-astra", "gpt-6-astra"));
  TEST_ASSERT_TRUE(modelInChoices("a", "a"));
  TEST_ASSERT_FALSE(modelInChoices("gpt-5.6", "gpt-5"));       // no prefix match
  TEST_ASSERT_FALSE(modelInChoices("gpt-5", "gpt-5.6"));       // no extension match
  TEST_ASSERT_FALSE(modelInChoices("a,b", ""));                // empty model never matches
  TEST_ASSERT_FALSE(modelInChoices("", "a"));
  TEST_ASSERT_FALSE(modelInChoices(",,", "a"));
  TEST_ASSERT_TRUE(modelInChoices("openai/gpt-5.6,anthropic/claude-sonnet-5", "anthropic/claude-sonnet-5"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_slot_has_a_cold_start_list);
  RUN_TEST(test_cold_start_lists_hold_no_family_the_harvest_refuses);
  RUN_TEST(test_derivation_advertised_equals_spawnable_for_every_slot);
  RUN_TEST(test_model_in_choices_exact_and_trimmed);
  UNITY_END();
  return 0;
}
