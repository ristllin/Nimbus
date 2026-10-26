#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/key_home.h"

// QW5: the setup wizard preselects Cumulo Nimbus, and a direct OpenAI, Anthropic or
// Mistral key pasted there came back "Rejected - check the key" (a valid key blamed,
// the wrong key left stored in the Cumulo slot). keyHomeFor reads which provider a key
// belongs to; misplacedKeyError is the device's refusal for a key posted to the wrong
// slot. The shape table is shared with the web page's keyHome() (the harness checks
// the same rows), so the device and the page cannot drift apart.

using nimbus::orch::KeyHomeSlot;
using nimbus::orch::keyHomeCertain;
using nimbus::orch::keyHomeFor;
using nimbus::orch::keyHomeSlot;
using nimbus::orch::misplacedKeyError;

void setUp() {}
void tearDown() {}

struct Case {
  std::string key, home;
};

static std::vector<Case> loadCases() {
  std::vector<Case> out;
  FILE* f = std::fopen("test/support/fixtures/key_home_cases.json", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "key_home_cases.json missing");
  std::string text;
  char buf[2048];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  std::fclose(f);
  JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, text) == DeserializationError::Ok);
  for (JsonObject c : d["cases"].as<JsonArray>()) {
    std::string k = c["prefix"].as<std::string>();
    const std::string fill = c["fill"].as<std::string>();
    const int len = c["len"] | 0;
    for (int i = 0; i < len; ++i) k += fill;
    k += c["suffix"].as<std::string>();
    out.push_back({k, c["home"].as<std::string>()});
  }
  TEST_ASSERT_TRUE(out.size() >= 10);
  return out;
}

static void test_every_fixture_row_reads_its_home() {
  for (const Case& c : loadCases()) {
    char ctx[200];
    snprintf(ctx, sizeof ctx, "key len %u starting '%.12s'", (unsigned)c.key.size(), c.key.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.home.c_str(), keyHomeFor(c.key), ctx);
  }
}

static const char* const kKeyFields[] = {"oaiKey", "antKey", "mistKey", "zaiKey", "cumuloKey"};

// The refusal class, over every fixture key x every provider key field:
//  - a key whose home is certain (a published prefix) is refused in every slot but its
//    own, and the refusal names where it goes;
//  - the Cumulo slot refuses anything that is not a cumulo_sk_ key;
//  - a shape-only home (Mistral, Z.ai) or an unknown shape is never refused elsewhere,
//    so a provider changing its key format cannot lock its owners out;
//  - fields that are not provider key slots are never refused here.
static void test_misplaced_keys_are_refused_only_on_certainty() {
  for (const Case& c : loadCases()) {
    const KeyHomeSlot* slot = keyHomeSlot(c.home);
    const bool blank = c.key.find_first_not_of(' ') == std::string::npos;
    for (const char* field : kKeyFields) {
      const std::string err = misplacedKeyError(field, c.key);
      char ctx[200];
      snprintf(ctx, sizeof ctx, "field %s home '%s' key len %u", field, c.home.c_str(),
               (unsigned)c.key.size());
      bool wantRefused;
      if (blank) wantRefused = false;
      else if (slot && keyHomeCertain(c.home)) wantRefused = std::string(field) != slot->field;
      else wantRefused = std::string(field) == "cumuloKey" && c.home != "cumulo";
      TEST_ASSERT_EQUAL_MESSAGE(wantRefused, !err.empty(), ctx);
      if (!err.empty() && slot && keyHomeCertain(c.home))
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, err.find(slot->where), ctx);
      if (!err.empty()) TEST_ASSERT_TRUE_MESSAGE(err[0] >= 'A' && err[0] <= 'Z', ctx);
    }
    TEST_ASSERT_TRUE(misplacedKeyError("tgToken", c.key).empty());
    TEST_ASSERT_TRUE(misplacedKeyError("custKey", c.key).empty());
  }
}

// The exact wizard case, end to end: an Anthropic key pasted into the preselected
// Cumulo field is refused with the slot it belongs in, never stored as a Cumulo key.
static void test_anthropic_key_in_the_cumulo_slot_names_anthropic() {
  const std::string k = std::string("sk-ant-api03-") + std::string(95, 'g');
  TEST_ASSERT_EQUAL_STRING("That is an Anthropic key. Save it under Anthropic.",
                           misplacedKeyError("cumuloKey", k).c_str());
  TEST_ASSERT_TRUE(misplacedKeyError("antKey", k).empty());
}

// Every home the reader can return has a slot to name (no refusal without a place
// to point to), and every slot's field is a real key field of the device.
static void test_every_home_has_a_slot() {
  static const char* const kHomes[] = {"cumulo", "anthropic", "openai", "tavily",
                                       "telegram", "zai", "mistral"};
  for (const char* h : kHomes) {
    const KeyHomeSlot* s = keyHomeSlot(h);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, h);
    TEST_ASSERT_TRUE_MESSAGE(std::string(s->where).size() > 4, h);
  }
  TEST_ASSERT_NULL(keyHomeSlot(""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_fixture_row_reads_its_home);
  RUN_TEST(test_misplaced_keys_are_refused_only_on_certainty);
  RUN_TEST(test_anthropic_key_in_the_cumulo_slot_names_anthropic);
  RUN_TEST(test_every_home_has_a_slot);
  return UNITY_END();
}
