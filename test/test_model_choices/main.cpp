#include <unity.h>

#include <cstdio>

#include <set>
#include <string>
#include <vector>

#include "../../src/agent/agent_config.h"   // CUMULO_MODEL - the router default
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_price_defaults.h"
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

// CUM-530 gate: a Cumulo list an older build harvested and stored (v4.5.9 kept the
// router's slash ids) is what this derivation reads for the prompt's model line,
// the spawn and pick validators and the picker; it drops every selector no
// upstream runs and keeps the rest in order. Nothing left: the cold-start list.
// Other providers' lists are never filtered (their ids are not router selectors).
static void test_cumulo_derivation_drops_what_no_upstream_runs() {
  const std::string v459 =
      "openai/gpt-5.6,anthropic/claude-opus-5-5,mistral/mistral/zai-glm-5-3, mistral/zai-glm-5-3 ,"
      "mistral/mistral/glm-5-2,gpt-5.6-terra";
  const std::string csv = effectiveModelChoices("cumulo", v459);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6,anthropic/claude-opus-5-5,mistral/zai-glm-5-3,gpt-5.6-terra",
                           csv.c_str());
  TEST_ASSERT_FALSE(modelInChoices(csv, "mistral/mistral/zai-glm-5-3"));
  TEST_ASSERT_EQUAL_STRING(staticModelChoices("cumulo"),
                           effectiveModelChoices("cumulo", "mistral/mistral/a,zai//b,openai/").c_str());
  const std::string clean = "openai/gpt-5.6, anthropic/claude-opus-5-5";
  TEST_ASSERT_EQUAL_STRING(clean.c_str(), effectiveModelChoices("cumulo", clean).c_str());   // untouched
  TEST_ASSERT_EQUAL_STRING("ft:open-mistral-7b:acme/x,mistral-small-latest",
                           effectiveModelChoices("mistral", "ft:open-mistral-7b:acme/x,mistral-small-latest").c_str());
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

// The spawn-model fallback's pick (prism gate W1): the cheapest item by the built-in
// price table, items trimmed like modelInChoices reads them, an unpriced item never
// chosen, the earlier item on a tie, "" when nothing is priced.
static void test_cheapest_choice_by_the_price_table() {
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5",
                           cheapestChoice("anthropic", NIMBUS_ANT_MODEL_CHOICES).c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-6-luna", cheapestChoice("openai", NIMBUS_OPENAI_MODEL_CHOICES).c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest",
                           cheapestChoice("mistral", NIMBUS_MISTRAL_MODEL_CHOICES).c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", cheapestChoice("cumulo", NIMBUS_CUMULO_MODEL_CHOICES).c_str());
  TEST_ASSERT_EQUAL_STRING("glm-5.3-flash", cheapestChoice("zai", NIMBUS_ZAI_MODEL_CHOICES).c_str());
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5",
                           cheapestChoice("anthropic", " claude-opus-5 ,\tclaude-haiku-4-5\r\n, x").c_str());
  TEST_ASSERT_EQUAL_STRING("claude-opus-5", cheapestChoice("anthropic", "unpriced-z,claude-opus-5").c_str());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5",   // same price as sonnet-5-5: the earlier item wins
                           cheapestChoice("anthropic", "claude-sonnet-5,claude-sonnet-5-5").c_str());
  TEST_ASSERT_EQUAL_STRING("", cheapestChoice("anthropic", "good-a,good-b").c_str());
  TEST_ASSERT_EQUAL_STRING("", cheapestChoice("anthropic", " , ").c_str());
  TEST_ASSERT_EQUAL_STRING("", cheapestChoice("custom", "llama-3").c_str());
}


// ==== lane F ===========================================================================

// The cold-start lists hold current active ids (claude-haiku-5-5 replaced haiku-4-5,
// whose retirement floor passed on 15 Oct 2026; gpt-5.6-sol is in the OpenAI list,
// critique C1), each priced by its own built-in row, and the Cumulo list always holds
// the router default (agent_config.h CUMULO_MODEL).
static void test_cold_start_lists_are_current_and_priced() {
  TEST_ASSERT_TRUE(modelInChoices(NIMBUS_ANT_MODEL_CHOICES, "claude-haiku-5-5"));
  TEST_ASSERT_FALSE(modelInChoices(NIMBUS_ANT_MODEL_CHOICES, "claude-haiku-4-5"));
  TEST_ASSERT_TRUE(modelInChoices(NIMBUS_OPENAI_MODEL_CHOICES, "gpt-5.6-sol"));
  TEST_ASSERT_TRUE(modelInChoices(NIMBUS_CUMULO_MODEL_CHOICES, CUMULO_MODEL));
  for (const ProviderSlot& slot : kProviderSlots)
    for (const std::string& id : split(staticModelChoices(slot.slug)))
      TEST_ASSERT_NOT_NULL_MESSAGE(builtinModelPrice(slot.slug, id.c_str()),
                                   (std::string(slot.slug) + " has no price row for " + id).c_str());
}

// The first boot of this build (critique C3): the older build's JSON catalog lost
// claude-fable-5-1 in its size trim while its NVS short list still had it; the merged
// store holds both. The NVS list is erased only on the boot AFTER the image was marked
// valid, and only once the store holds the list (an OTA trial boot may still roll back).
static void test_legacy_short_list_merge_then_erase_after_mark_valid() {
  const std::string mcat = "[{\"id\":\"claude-opus-5\",\"roles\":[\"orchestrator\",\"sub-agent\"],\"usable\":true}]";
  const std::string mch = "claude-fable-5-1,claude-opus-5";
  // Boot 1 (OTA trial boot): merge, keep the NVS list.
  const std::string store = mergeLegacyLists("anthropic", "", mcat, mch, "v4.6.0");
  TEST_ASSERT_TRUE(store.find("\nclaude-fable-5-1\t") != std::string::npos);
  TEST_ASSERT_TRUE(store.find("\nclaude-opus-5\t") != std::string::npos);
  TEST_ASSERT_FALSE(legacyListErasable(/*imageValidAtBoot=*/false, true, true));
  // Boot 2 (the image was marked valid during boot 1): nothing new to merge, erase.
  TEST_ASSERT_EQUAL_STRING("", mergeLegacyLists("anthropic", store, mcat, mch, "v4.6.0").c_str());
  TEST_ASSERT_TRUE(legacyListErasable(true, true, true));
  // A failed store write on boot 1 keeps the NVS list on boot 2.
  TEST_ASSERT_FALSE(legacyListErasable(true, false, true));
}

static std::string readSrc(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// A rollback keeps reading /data/mcat_<slug>.json (critique C2): this build never
// writes or deletes it outside the legacy NVS-to-file copy, and never writes the NVS
// short list. A source guard over the files that touch the catalogs.
static void test_rollback_keeps_the_legacy_catalog_readable() {
  const std::string store = readSrc("src/agent/store.cpp");
  const std::string verify = readSrc("src/agent/provider_verify.cpp");
  const std::string web = readSrc("src/net/webui.cpp");
  for (const std::string* src : {&store, &verify, &web}) {
    TEST_ASSERT_TRUE(src->find("setModelCatalogJson") == std::string::npos);
    TEST_ASSERT_TRUE(src->find("setModelChoices") == std::string::npos);
  }
  // writeCatalogFile (the only writer of an mcat_ file) runs only in the legacy copy.
  size_t calls = 0;
  for (size_t p = store.find("writeCatalogFile("); p != std::string::npos; p = store.find("writeCatalogFile(", p + 1))
    ++calls;
  TEST_ASSERT_EQUAL_UINT(2, calls);   // its definition and the one call in migrateCatalogCachesOffNvs
  TEST_ASSERT_TRUE(store.find("LittleFS.remove(jpath") == std::string::npos);
  TEST_ASSERT_TRUE(store.find("LittleFS.remove(nimbus::store::catalogCachePath") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_slot_has_a_cold_start_list);
  RUN_TEST(test_cold_start_lists_hold_no_family_the_harvest_refuses);
  RUN_TEST(test_derivation_advertised_equals_spawnable_for_every_slot);
  RUN_TEST(test_model_in_choices_exact_and_trimmed);
  RUN_TEST(test_cheapest_choice_by_the_price_table);
  RUN_TEST(test_cumulo_derivation_drops_what_no_upstream_runs);
  RUN_TEST(test_cold_start_lists_are_current_and_priced);
  RUN_TEST(test_legacy_short_list_merge_then_erase_after_mark_valid);
  RUN_TEST(test_rollback_keeps_the_legacy_catalog_readable);
  UNITY_END();
  return 0;
}
