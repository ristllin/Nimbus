#include <unity.h>

#include <string>

#include "nimbus/orch/head_switch.h"
#include "nimbus/orch/provider_slots.h"

// head_switch.h: orch_model never moves the owner's head only because it is paused.
// The apply-level legs (the summary, the reply note, the next turn's feedback, the claim
// check) are in test_harness_apply; these pin the pure pieces.

using namespace nimbus::orch;

void setUp() { headSwitchGuard().reset(); }

static OwnerMessage msg(const char* text, uint32_t number) {
  OwnerMessage m;
  m.text = text;
  m.number = number;
  return m;
}
void tearDown() {}

// Every cause in the owner's words, the pause switch first (it is what the owner set),
// each with the ending that fits it (what makes the provider come back).
static void test_pause_notes() {
  const char* tail = "; replies already fail over automatically and return to ";
  struct Case { const char* label; uint8_t pause; ParkKind park; bool over; const char* why; const char* until; };
  const Case cases[] = {
      {"Anthropic", kPauseProviderLimit, ParkKind::None, true, " is paused by your monthly limit", " when it lifts."},
      {"OpenAI", kPauseSpendCeiling, ParkKind::Billing, false, " is paused by your overall spending limit",
       " when it lifts."},
      {"Mistral", kPauseOwner, ParkKind::None, false, " is paused by you", " when you resume it."},
      {"OpenAI", 0, ParkKind::Billing, false, " is out of credit", " once credit is added."},
      {"Z.ai", 0, ParkKind::Auth, false, " rejected its key", " once its key is replaced."},
      {"Anthropic", 0, ParkKind::None, true, " is over its monthly budget", " when its budget resets."},
  };
  for (const Case& c : cases) {
    const std::string want = std::string(c.label) + c.why + tail + c.label + c.until;
    TEST_ASSERT_EQUAL_STRING(want.c_str(), headPauseNote(c.label, c.pause, c.park, c.over).c_str());
  }
  TEST_ASSERT_EQUAL_STRING("", headPauseNote("Anthropic", 0, ParkKind::None, false).c_str());
}

// The explanation the reply carries: the TASK's sentence, then the way out. No em dash,
// printable ASCII (it also reaches the device screen's log).
static void test_refusal_text() {
  const std::string t = headSwitchRefusal(headPauseNote("Anthropic", kPauseProviderLimit, ParkKind::None, false), "Mistral");
  TEST_ASSERT_EQUAL_STRING(
      "Anthropic is paused by your monthly limit; replies already fail over automatically and return to "
      "Anthropic when it lifts. To make Mistral your main provider anyway, say \"switch to Mistral\" again, "
      "or pick it under Assistant > Models.",
      t.c_str());
  for (unsigned char c : t) TEST_ASSERT_TRUE(c >= 0x20 && c < 0x7f);
}

// Only the owner's own channels and an admin chat speak for the owner.
static void test_owner_channels() {
  TEST_ASSERT_TRUE(ownerChannel("web", false));
  TEST_ASSERT_TRUE(ownerChannel("serial", false));
  TEST_ASSERT_TRUE(ownerChannel("voice", false));
  TEST_ASSERT_TRUE(ownerChannel("6098532481", true));
  TEST_ASSERT_FALSE(ownerChannel("6098532481", false));   // a member or a guest chat
  TEST_ASSERT_FALSE(ownerChannel("system", false));
  TEST_ASSERT_FALSE(ownerChannel("", false));
}

// A provider is named by its slug, display name or a model family only it serves; a
// model by its id, its bare router id or its id without -latest. Case does not matter.
static void test_owner_names_the_target() {
  for (const ProviderSlot& slot : kProviderSlots) {
    TEST_ASSERT_TRUE_MESSAGE(ownerNamesTarget(std::string("use ") + slot.slug, slot.slug, "x"), slot.slug);
    TEST_ASSERT_TRUE_MESSAGE(ownerNamesTarget(std::string("Use ") + slot.label + " please", slot.slug, "x"), slot.label);
  }
  TEST_ASSERT_TRUE(ownerNamesTarget("go back to Claude", "anthropic", "claude-opus-5-5"));
  TEST_ASSERT_TRUE(ownerNamesTarget("try GPT for a while", "openai", "gpt-5.6-sol"));
  TEST_ASSERT_TRUE(ownerNamesTarget("GLM is fine", "zai", "glm-5.3"));
  TEST_ASSERT_TRUE(ownerNamesTarget("run mistral-large", "cumulo", "mistral/mistral-large-latest"));
  TEST_ASSERT_TRUE(ownerNamesTarget("keep using mistral-large-latest", "mistral", "mistral-large-latest"));
  TEST_ASSERT_FALSE(ownerNamesTarget("ok, thanks", "mistral", "mistral-large-latest"));
  TEST_ASSERT_FALSE(ownerNamesTarget("Hmm I thought anthropic is out of budget", "mistral", "mistral-large-latest"));
  TEST_ASSERT_FALSE(ownerNamesTarget("", "mistral", "mistral-large-latest"));
}

// The guard: an open head or the same provider passes; a closed head is refused once,
// the owner's later naming message within the window passes once, the clock wraps.
static void test_guard() {
  HeadSwitchGuard g;
  TEST_ASSERT_TRUE(g.allow("anthropic", "", "mistral", "m", msg("anything", 1), 1));
  TEST_ASSERT_TRUE(g.allow("anthropic", "closed", "anthropic", "claude-opus-5-5", msg("", 0), 1));
  // Refused at the edge of the millis() wrap, confirmed just after it by the next message.
  const uint32_t nearWrap = 0xFFFFFFFFu - 1000u;
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("switch to mistral, anthropic is out", 1), nearWrap));
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("switch to mistral, anthropic is out", 1), 10));   // same message
  TEST_ASSERT_TRUE(g.allow("anthropic", "closed", "mistral", "m", msg("switch to mistral, anthropic is out", 2), 5000));  // same words, next message
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("yes, use mistral for good again", 3), 6000));   // spent
  // A refusal for another target replaces the one waiting.
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "openai", "gpt-5.6-sol", msg("use gpt", 4), 7000));
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("no, mistral", 5), 8000));
  // A turn the owner did not write is refused and records nothing: the owner's next
  // message is a first ask.
  g.reset();
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("", 0), 9000));
  TEST_ASSERT_FALSE(g.allow("anthropic", "closed", "mistral", "m", msg("switch to mistral then", 6), 10000));
  TEST_ASSERT_TRUE(g.allow("anthropic", "closed", "mistral", "m", msg("yes, switch to mistral", 7), 11000));
}

// An ASK: the target named, a switch word, nothing that keeps or declines. The 26 Oct words
// and the owner's natural follow-ups are no ask; an explicit switch is.
static void test_owner_asks_to_switch() {
  for (const char* yes : {"switch to mistral since anthropic is out of budget", "yes, switch to Mistral",
                          "use mistral for now", "make Mistral my main provider", "change the model to mistral-large-latest",
                          "go with Mistral then"})
    TEST_ASSERT_TRUE_MESSAGE(ownerAsksSwitchTo(yes, "mistral", "mistral-large-latest"), yes);
  for (const char* no : {"Hmm I thought anthropic is out of budget", "ok, so mistral is answering for now?",
                         "no, keep anthropic, mistral is fine meanwhile", "don't switch to mistral",
                         "do not change to mistral", "what does mistral cost?", "switch to gpt", ""})
    TEST_ASSERT_FALSE_MESSAGE(ownerAsksSwitchTo(no, "mistral", "mistral-large-latest"), no);
}

// The model's version of the note, for the per-turn line.
static void test_pause_facts_for_the_model() {
  HeadPause hp;
  TEST_ASSERT_EQUAL_STRING("", headPauseFact("Anthropic", hp).c_str());
  hp.causes = kPauseProviderLimit;
  TEST_ASSERT_EQUAL_STRING(
      "Anthropic, the chosen head, is paused by the owner's monthly limit and returns automatically when it lifts.",
      headPauseFact("Anthropic", hp).c_str());
  hp = HeadPause();
  hp.park = ParkKind::Billing;
  TEST_ASSERT_EQUAL_STRING("OpenAI, the chosen head, is out of credit and returns automatically once credit is added.",
                           headPauseFact("OpenAI", hp).c_str());
}

// Review of 4e9cf28f: a 403 parks one seam (the head's Chat), so the catalog, which reads the
// Spawn seam, must not show the provider paused, and the head's words for a 403 say it lifts
// on the re-check, never "once its key is replaced". A park whose re-check is due reads open
// with dueIsOpen (the next request is that re-check).
static void test_park_pause_is_seam_scoped_and_403_lifts_on_recheck() {
  ProviderPark& park = providerPark();
  park.clear();
  const uint32_t t0 = 1000;
  park.park("openai", PaidSeam::Chat, 0, ParkKind::Auth, 403, t0);
  const HeadPause spawn = parkPause("openai", PaidSeam::Spawn, t0 + 1, true);
  TEST_ASSERT_FALSE(pauseHolds(spawn));
  const HeadPause chat = parkPause("openai", PaidSeam::Chat, t0 + 1, false);
  TEST_ASSERT_TRUE(pauseHolds(chat));
  TEST_ASSERT_EQUAL(403, chat.parkStatus);
  const std::string note = headPauseNote("OpenAI", chat);
  TEST_ASSERT_EQUAL_STRING(
      "OpenAI refused a request (HTTP 403); replies already fail over automatically and return to OpenAI when "
      "it is checked again.",
      note.c_str());
  TEST_ASSERT_TRUE(note.find("key is replaced") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("OpenAI refused a request (HTTP 403) and returns automatically when it is checked again.",
                           providerPauseFact("OpenAI", chat).c_str());
  // A 401 holds every seam and keeps the key wording.
  park.clear();
  park.park("openai", PaidSeam::Chat, 0, ParkKind::Auth, 401, t0);
  const HeadPause key = parkPause("openai", PaidSeam::Spawn, t0 + 1, true);
  TEST_ASSERT_TRUE(pauseHolds(key));
  TEST_ASSERT_TRUE(providerPauseFact("OpenAI", key).find("once its key is replaced") != std::string::npos);
  // Its re-check due: open for the Spawn read (dueIsOpen), still held for a Chat read without it.
  const uint32_t due = t0 + ProviderPark::kRecheckMs + 5;
  TEST_ASSERT_FALSE(pauseHolds(parkPause("openai", PaidSeam::Spawn, due, true)));
  TEST_ASSERT_TRUE(pauseHolds(parkPause("openai", PaidSeam::Chat, due, false)));
  // The pause switch holds every seam, due or not.
  park.clear();
  park.pause("anthropic", kPauseProviderLimit);
  TEST_ASSERT_TRUE(pauseHolds(parkPause("anthropic", PaidSeam::Spawn, due, true)));
  park.resume("anthropic", kPauseProviderLimit);
  TEST_ASSERT_FALSE(pauseHolds(parkPause("anthropic", PaidSeam::Spawn, due, true)));
  park.clear();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pause_notes);
  RUN_TEST(test_refusal_text);
  RUN_TEST(test_owner_channels);
  RUN_TEST(test_owner_names_the_target);
  RUN_TEST(test_guard);
  RUN_TEST(test_owner_asks_to_switch);
  RUN_TEST(test_pause_facts_for_the_model);
  RUN_TEST(test_park_pause_is_seam_scoped_and_403_lifts_on_recheck);
  return UNITY_END();
}
