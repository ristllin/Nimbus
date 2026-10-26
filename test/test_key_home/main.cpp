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
using nimbus::orch::keyHomeFor;
using nimbus::orch::keyHomeSlot;
using nimbus::orch::keyHomeSure;
using nimbus::orch::misplacedKeyError;

void setUp() {}
void tearDown() {}

struct Case {
  std::string key, home;
  bool sure;
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
    out.push_back({k, c["home"].as<std::string>(), c["sure"].as<bool>()});
  }
  TEST_ASSERT_TRUE(out.size() >= 10);
  return out;
}

static void test_every_fixture_row_reads_its_home_and_certainty() {
  for (const Case& c : loadCases()) {
    char ctx[200];
    snprintf(ctx, sizeof ctx, "key len %u starting '%.12s'", (unsigned)c.key.size(), c.key.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.home.c_str(), keyHomeFor(c.key), ctx);
    TEST_ASSERT_EQUAL_MESSAGE(c.sure, keyHomeSure(c.key), ctx);
  }
}

// Every secret slot POST /api/orch guards: the five provider keys, and the two other
// secrets sent raw to a fixed host (the Telegram bot token, the Tavily key).
static const char* const kGuarded[] = {"oaiKey", "antKey", "mistKey", "zaiKey",
                                       "cumuloKey", "tgToken", "tavKey"};

// The slots with a fixed form of their own take nothing else.
static bool ownFormOnly(const std::string& field, const std::string& home) {
  if (field == "cumuloKey") return home != "cumulo";
  if (field == "tgToken") return home != "telegram";
  if (field == "tavKey") return home != "tavily";
  return false;
}

// The refusal class, over every fixture key x every guarded slot:
//  - a key whose home is certain is refused in every slot but its own, and the refusal
//    names where it goes;
//  - the Cumulo, Telegram and Tavily slots refuse anything that is not their own form,
//    so a provider key is never sent to the router, api.telegram.org or Tavily;
//  - a shape-only guess (Mistral, Z.ai, an unfamiliar sk- key) or an unknown shape is
//    never refused in another provider's slot;
//  - the custom endpoint key takes anything.
static void test_misplaced_keys_are_refused_in_every_guarded_slot() {
  int refusals = 0;
  for (const Case& c : loadCases()) {
    const KeyHomeSlot* slot = keyHomeSlot(c.home);
    const bool blank = c.key.find_first_not_of(' ') == std::string::npos;
    for (const char* field : kGuarded) {
      const std::string err = misplacedKeyError(field, c.key);
      char ctx[200];
      snprintf(ctx, sizeof ctx, "field %s home '%s' sure %d key len %u", field, c.home.c_str(),
               (int)c.sure, (unsigned)c.key.size());
      bool wantRefused;
      if (blank) wantRefused = false;
      else if (slot && c.sure && std::string(field) != slot->field) wantRefused = true;
      else wantRefused = ownFormOnly(field, c.home);
      TEST_ASSERT_EQUAL_MESSAGE(wantRefused, !err.empty(), ctx);
      if (!err.empty() && slot) TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, err.find(slot->where), ctx);
      if (!err.empty()) TEST_ASSERT_TRUE_MESSAGE(err[0] >= 'A' && err[0] <= 'Z', ctx);
      if (!c.sure && !err.empty() && slot)
        TEST_ASSERT_EQUAL_MESSAGE(0, err.find("That looks like "), ctx);   // hedged, never "is"
      refusals += !err.empty();
    }
    TEST_ASSERT_TRUE(misplacedKeyError("custKey", c.key).empty());
  }
  TEST_ASSERT_TRUE(refusals > 20);
}

// The finding's exact cases: an Anthropic key in the Telegram field, an OpenAI key in
// the Tavily field, an OpenRouter key under OpenAI (not refused: not ours to judge),
// and a bot token in a provider slot.
static void test_the_reported_cases() {
  const std::string ant = std::string("sk-ant-api03-") + std::string(95, 'g');
  TEST_ASSERT_EQUAL_STRING("That is an Anthropic key. Save it under Anthropic.",
                           misplacedKeyError("tgToken", ant).c_str());
  const std::string oai = std::string("sk-proj-") + std::string(48, 'h');
  TEST_ASSERT_EQUAL_STRING("That is an OpenAI key. Save it under OpenAI.",
                           misplacedKeyError("tavKey", oai).c_str());
  const std::string orKey = std::string("sk-or-v1-") + std::string(64, 'f');
  TEST_ASSERT_TRUE(misplacedKeyError("oaiKey", orKey).empty());
  TEST_ASSERT_TRUE(misplacedKeyError("antKey", orKey).empty());
  const std::string deep = std::string("sk-") + std::string(32, 'a');
  TEST_ASSERT_TRUE(misplacedKeyError("antKey", deep).empty());   // only looks like OpenAI
  TEST_ASSERT_EQUAL(0u, misplacedKeyError("tgToken", deep).find("That looks like an OpenAI key, not a Telegram bot token."));
  const std::string bot = std::string("123456789:") + std::string(35, 'B');
  TEST_ASSERT_EQUAL_STRING("That is a Telegram bot token. Save it under Assistant > Connectors > Telegram.",
                           misplacedKeyError("oaiKey", bot).c_str());
  TEST_ASSERT_TRUE(misplacedKeyError("tgToken", bot).empty());
  TEST_ASSERT_EQUAL(0u, misplacedKeyError("tgToken", std::string(40, 'q')).find("That is not a Telegram bot token"));
  TEST_ASSERT_EQUAL(0u, misplacedKeyError("tavKey", std::string(40, 'q')).find("That is not a Tavily key"));
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
  RUN_TEST(test_every_fixture_row_reads_its_home_and_certainty);
  RUN_TEST(test_misplaced_keys_are_refused_in_every_guarded_slot);
  RUN_TEST(test_the_reported_cases);
  RUN_TEST(test_anthropic_key_in_the_cumulo_slot_names_anthropic);
  RUN_TEST(test_every_home_has_a_slot);
  return UNITY_END();
}
