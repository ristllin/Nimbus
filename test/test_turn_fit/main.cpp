#include <unity.h>

#include <string>

#include "../support/fake_platform.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/head_tools.h"
#include "nimbus/harness/turn_fit.h"

// The give-up order (lane H of the history fix, turn_fit.h): one order for every refit, each
// rung measured on the rendered request, and what goes is said - for history with its rows,
// times and the call that reads them back.
using agent::ComposeInputs;
using agent::FitContext;
using agent::HistMsg;
using agent::TurnFit;
using nimbus::orch::FitScope;

void setUp() { harness_test::LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

namespace {

constexpr int32_t kToday = 20407;              // 2025-11-15 (days since 1970-01-01)
constexpr uint32_t kTodayStart = 20407u * 86400u;

// Two days of history: three exchanges yesterday, four today, the summary pair first.
ComposeInputs baseInputs() {
  ComposeInputs ci;
  ci.devName = "Nimbus";
  ci.skillsShelf = "SHELF " + std::string(2000, 's');
  ci.standingPrefs = "PREFS " + std::string(500, 'p');
  ci.recalled = {"[90%] RECALLED " + std::string(1500, 'm')};
  ci.foodPlaybook = "PLAYBOOK " + std::string(1500, 'f');
  ci.history.push_back({true, std::string(agent::kSummaryUserHead) + "SUMMARY"});
  ci.history.push_back({false, agent::kSummaryAck});
  for (int i = 0; i < 7; i++) {
    const bool today = i >= 3;
    const uint32_t at = today ? kTodayStart + 9 * 3600 + (uint32_t)i * 600 : kTodayStart - 6 * 3600 + (uint32_t)i * 600;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(100 + 2 * i));
    HistMsg u(true, "Q" + std::to_string(i) + " " + std::string(1000, 'q'));
    HistMsg a(false, "A" + std::to_string(i) + " " + std::string(1000, 'a'));
    for (HistMsg* m : {&u, &a}) {
      m->firstId = id;
      snprintf(id, sizeof(id), "m%08x", (unsigned)(101 + 2 * i));
      m->lastId = id;
      snprintf(id, sizeof(id), "m%08x", (unsigned)(100 + 2 * i));
      m->localDay = (uint32_t)(today ? kToday : kToday - 1);
    }
    u.atUnix = at;
    u.rows = 2;
    a.atUnix = at + 60;
    ci.history.push_back(u);
    ci.history.push_back(a);
  }
  return ci;
}

FitContext today() {
  FitContext c;
  c.todayLocalDay = kToday;
  return c;
}

const char* const kInput =
    "[ACTIVE SKILLS] Already loaded for this chat\n### meal-photo-science\nSKILLBODY " "xxxxxxxxxxxxxxxx"
    "\n[MEMORY RESULTS] (background)\nMEMBODY\n[FRESH RESULTS] (sub-agents)\n- job1: RESULTHEAD";

std::string inputWithResults() {
  return std::string(kInput) + std::string(6000, 'r') + " RESULTTAIL\n[SYSTEM]\nsynthesize";
}

bool hasQ(const ComposeInputs& ci, int i) {
  for (const auto& m : ci.history)
    if (m.text.rfind("Q" + std::to_string(i) + " ", 0) == 0) return true;
  return false;
}

size_t rendered(const ComposeInputs& ci, const TurnFit& fit, const agent::HeadTools* tools = nullptr) {
  size_t n = agent::composeInstructions(ci).size() + fit.inputs().size();
  if (tools)
    for (const auto& t : tools->specs) n += t.name.size() + t.description.size() + t.schemaJson.size();
  return n;
}

}  // namespace

// The property (plan v3, lane H): every rung strictly shrinks the rendered request, or gives
// nothing and leaves no note. Walked one rung at a time to the end on several shapes,
// including the 15 Nov recall rung that rendered nothing (v5.1.1: +158 B and a note).
static void test_every_rung_strictly_shrinks_or_gives_nothing() {
  for (int shape = 0; shape < 4; shape++) {
    ComposeInputs ci = baseInputs();
    if (shape == 1) ci.budgetBytes = 4096;   // recall cannot render: its rung must give nothing
    if (shape == 2) ci.history.clear();
    if (shape == 3) ci.recalled.clear();
    agent::HeadTools tools;
    tools.specs.push_back({"memory_search", std::string(4000, 'd'), "{}"});
    TurnFit fit(ci, inputWithResults() + std::string(9000, 'i'), &tools, today());
    int rungs = 0;
    for (; rungs < 80; rungs++) {
      const size_t before = rendered(ci, fit, &tools);
      const std::string noteBefore = fit.note();
      const size_t freed = fit.shrink(400, FitScope::NextRung);
      const size_t after = rendered(ci, fit, &tools);
      if (!freed) {
        TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)after);
        TEST_ASSERT_EQUAL_STRING(noteBefore.c_str(), fit.note().c_str());
        break;
      }
      TEST_ASSERT_TRUE_MESSAGE(after < before, (std::to_string(shape) + ": " + std::to_string(before) + " -> " +
                                                std::to_string(after)).c_str());
      TEST_ASSERT_EQUAL_UINT32((uint32_t)(before - after), (uint32_t)freed);
    }
    TEST_ASSERT_TRUE(rungs > 3 && rungs < 80);   // the order ends; it never spins
    if (shape == 1) TEST_ASSERT_TRUE(fit.note().find("recalled") == std::string::npos);
    // The summary pair and the newest exchange are never dropped.
    if (shape != 2) {
      TEST_ASSERT_TRUE(agent::isSummaryMessage(ci.history[0]));
      TEST_ASSERT_TRUE(hasQ(ci, 6));
    }
  }
}

// The order: recall, the skills (playbook and [ACTIVE SKILLS]), history before today, today's
// beyond the newest exchange, the newest results, the input's middle, the tools, the shelf.
static void test_the_give_up_order() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = true;
  agent::HeadTools tools;
  tools.specs.push_back({"memory_search", std::string(4000, 'd'), "{}"});
  TurnFit fit(ci, inputWithResults(), &tools, today());
  TEST_ASSERT_TRUE(fit.shrink(100, FitScope::NextRung) > 0);
  TEST_ASSERT_TRUE(ci.recalled.empty());
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.shrink(100, FitScope::NextRung) > 0);
  TEST_ASSERT_TRUE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.inputs().find("SKILLBODY") == std::string::npos);   // the attached skills went too
  TEST_ASSERT_TRUE(hasQ(ci, 0));
  TEST_ASSERT_TRUE(fit.shrink(100000, FitScope::NextRung) > 0);   // all of yesterday, no more
  TEST_ASSERT_FALSE(hasQ(ci, 2));
  TEST_ASSERT_TRUE(hasQ(ci, 3));
  TEST_ASSERT_TRUE(fit.inputs().find("RESULTTAIL") != std::string::npos);
  TEST_ASSERT_TRUE(fit.shrink(100000, FitScope::NextRung) > 0);   // today's, down to the newest exchange
  TEST_ASSERT_FALSE(hasQ(ci, 5));
  TEST_ASSERT_TRUE(hasQ(ci, 6));
  TEST_ASSERT_TRUE(fit.inputs().find("cut to fit") == std::string::npos);   // results still whole
  TEST_ASSERT_TRUE(fit.shrink(3000, FitScope::NextRung) > 0);   // now the results, both ends kept
  const std::string in = fit.inputs();
  TEST_ASSERT_TRUE(in.find("cut to fit this request") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("RESULTHEAD") != std::string::npos && in.find("RESULTTAIL") != std::string::npos);
  TEST_ASSERT_FALSE(tools.specs.empty());
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::Ladder) > 0);   // the rest of the order
  TEST_ASSERT_TRUE(tools.specs.empty());
  TEST_ASSERT_TRUE(ci.standingPrefs.empty());
  TEST_ASSERT_TRUE(hasQ(ci, 6));                                  // never the newest exchange
  TEST_ASSERT_TRUE(agent::isSummaryMessage(ci.history[0]));       // never the summary pair
  TEST_ASSERT_TRUE(fit.inputs().find("Your tools did not fit") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(1000, FitScope::Ladder));   // the order ends
}

// The history cut is named: count, local time range, rows, and a ready memory.episodic call
// whose cursor reads exactly what went (the first row kept is its `before`).
static void test_the_note_names_the_history_cut() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, "the input", nullptr, today());
  TEST_ASSERT_TRUE(fit.shrink(2500, FitScope::OlderHistory) >= 2500);
  TEST_ASSERT_FALSE(hasQ(ci, 1));
  TEST_ASSERT_TRUE(hasQ(ci, 2));
  const std::string in = fit.inputs();
  TEST_ASSERT_TRUE_MESSAGE(in.rfind("[LEFT OUT TO FIT THIS REQUEST] 4 older conversation message(s) from "
                                    "Fri 14 Nov 18:00 to Fri 14 Nov 18:11 (rows m00000064 to m00000067), "
                                    "read them back with memory_episodic {\"kind\":\"message\",\"before\":\"m00000068\"}",
                                    0) == 0,
                           in.c_str());
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)fit.historyRowsLeftOut());
  TEST_ASSERT_EQUAL(4, fit.historyMessagesLeftOut());
  TEST_ASSERT_TRUE(fit.ownerLine().empty());   // older history alone is not the owner's news
  TEST_ASSERT_TRUE(harness_test::LogCapture::contains("turn fit: left out 4 older"));
}

// The photo fit's scope (RULINGS-v3 item 5): history only, down to the newest exchange, then
// nothing - the photos are read as text only then. Nothing else moves.
static void test_older_history_scope_is_history_only() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr, today());
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::OlderHistory) > 0);
  TEST_ASSERT_TRUE(hasQ(ci, 6));
  TEST_ASSERT_FALSE(hasQ(ci, 5));
  TEST_ASSERT_FALSE(ci.recalled.empty());
  TEST_ASSERT_FALSE(ci.foodPlaybook.empty());
  TEST_ASSERT_TRUE(fit.inputs().find("RESULTTAIL") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(1000, FitScope::OlderHistory));
}

// The head loop's context scope: one of recall, skills, history per call, and never the results
// (the loop clips its newest round only after them).
static void test_context_scope_stops_before_the_results() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr, today());
  int calls = 0;
  while (fit.shrink(1000000, FitScope::Context) > 0 && calls < 10) calls++;
  TEST_ASSERT_EQUAL(4, calls);   // recall, skills, yesterday, today's
  TEST_ASSERT_TRUE(hasQ(ci, 6));
  TEST_ASSERT_TRUE(fit.inputs().find("cut to fit") == std::string::npos);
  TEST_ASSERT_FALSE(ci.skillsShelf.empty());
}

// Without a clock every turn is "before today": the newest exchange still stays.
static void test_no_clock_history_goes_oldest_first() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, "x", nullptr, FitContext());
  TEST_ASSERT_TRUE(fit.shrink(1000000, FitScope::OlderHistory) > 0);
  TEST_ASSERT_EQUAL(4, (int)ci.history.size());   // the summary pair and the newest exchange
  TEST_ASSERT_TRUE(hasQ(ci, 6));
}

// Every history cut is counted (rows, messages, bytes): any of it makes the fold due; other
// cuts count none.
static void test_history_cuts_are_counted() {
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr, today());
  fit.shrink(1500, FitScope::OlderHistory);   // one pair of 1 KB messages
  TEST_ASSERT_TRUE(fit.trimmed());
  TEST_ASSERT_EQUAL(2, fit.historyMessagesLeftOut());
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)fit.historyRowsLeftOut());
  TEST_ASSERT_TRUE(fit.historyBytesLeftOut() >= 2000);

  ComposeInputs other = baseInputs();
  other.history.clear();
  TurnFit otherFit(other, inputWithResults(), nullptr, today());
  otherFit.shrink(3000, FitScope::Ladder);
  TEST_ASSERT_TRUE(otherFit.trimmed());
  TEST_ASSERT_EQUAL(0, otherFit.historyMessagesLeftOut());
}

// An owner hears when today's earlier messages went, or part of the results.
static void test_the_owner_line() {
  ComposeInputs ci = baseInputs();
  ci.speakerPresent = true;
  TurnFit fit(ci, inputWithResults(), nullptr, today());
  fit.shrink(1000000, FitScope::OlderHistory);
  TEST_ASSERT_TRUE_MESSAGE(fit.ownerLine().find("some of today's earlier messages") != std::string::npos,
                           fit.ownerLine().c_str());
}

static void test_clips_stay_valid_utf8() {
  ComposeInputs ci = baseInputs();
  ci.recalled.clear();
  ci.foodPlaybook.clear();
  ci.history.clear();
  ci.speakerPresent = true;
  std::string input = "[FRESH RESULTS]\n";
  for (int i = 0; i < 3000; i++) input += "\xE2\x82\xAC";
  TurnFit fit(ci, input, nullptr, today());
  TEST_ASSERT_TRUE(fit.shrink(4000, FitScope::Ladder) > 0);
  const std::string in = fit.inputs();
  for (size_t i = 0; i < in.size();) {
    const unsigned char c = (unsigned char)in[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    TEST_ASSERT_TRUE(n > 0 && i + n <= in.size());
    for (size_t k = 1; k < n; k++) TEST_ASSERT_TRUE(((unsigned char)in[i + k] & 0xC0) == 0x80);
    i += n;
  }
}

// Release review v5.1.2, finding 4 (RULINGS-v3 item 5): a photo round's history cut stops at
// the head's keep floor, whatever it asks for, and returns 0 there so the photos go as text;
// then the history it gave up comes back whole, with no LEFT OUT note and no row left out.
// The class: every floor from none to more than the whole history, every request size.
static void test_photo_history_cut_stops_at_the_keep_floor_and_comes_back() {
  const size_t live = [] {
    ComposeInputs ci = baseInputs();
    size_t n = 0;
    for (size_t i = 2; i < ci.history.size(); i++) n += ci.history[i].text.size();
    return n;
  }();
  for (size_t floor : {(size_t)0, (size_t)2000, (size_t)5000, (size_t)9000, live + 1}) {
    for (size_t want : {(size_t)1500, (size_t)4000, (size_t)1000000}) {
      ComposeInputs ci = baseInputs();
      const size_t messages = ci.history.size();
      const std::string composed = agent::composeInstructions(ci);
      TurnFit fit(ci, "hello", nullptr, today());
      fit.setHistoryKeepFloor(floor);
      const std::string what = "floor " + std::to_string(floor) + " want " + std::to_string(want);
      for (int i = 0; i < 20 && fit.shrink(want, FitScope::OlderHistory) > 0; i++) {
      }
      size_t left = 0;
      for (size_t i = 2; i < ci.history.size(); i++) left += ci.history[i].text.size();
      if (floor) TEST_ASSERT_TRUE_MESSAGE(left >= std::min(floor, live), what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(hasQ(ci, 6), what.c_str());   // the newest exchange stays
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, (uint32_t)fit.shrink(want, FitScope::OlderHistory), what.c_str());
      // Given back: as it was, nothing left out, nothing said.
      const bool cut = ci.history.size() < messages;
      TEST_ASSERT_EQUAL_MESSAGE(cut, fit.shrink(0, FitScope::GiveBackHistory) > 0, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)messages, (uint32_t)ci.history.size(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, (uint32_t)fit.historyRowsLeftOut(), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", fit.note().c_str(), what.c_str());
      TEST_ASSERT_FALSE_MESSAGE(fit.trimmed(), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(composed.c_str(), agent::composeInstructions(ci).c_str(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, (uint32_t)fit.shrink(0, FitScope::GiveBackHistory), what.c_str());
    }
  }
  // A cut another scope made (a body over budget) stays: nothing is given back over it, and
  // what another rung gave up stays said.
  ComposeInputs ci = baseInputs();
  TurnFit fit(ci, inputWithResults(), nullptr, today());
  TEST_ASSERT_TRUE(fit.shrink(1500, FitScope::OlderHistory) > 0);
  TEST_ASSERT_TRUE(fit.shrink(100, FitScope::NextRung) > 0);    // recall
  TEST_ASSERT_TRUE(fit.shrink(100000, FitScope::Ladder) > 0);   // more history, and beyond
  const size_t after = ci.history.size();
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(0, FitScope::GiveBackHistory));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)after, (uint32_t)ci.history.size());
  TEST_ASSERT_TRUE(fit.note().find("recalled") != std::string::npos);
}

// Release review v5.1.2, finding 5: a newest message over its string budget gives up only
// what that message holds (recall, the skills, the results, the input's middle), never the
// history, the tools or the shelf, and one call meets what it asks for.
static void test_newest_message_scope_never_touches_history_tools_or_shelf() {
  ComposeInputs ci = baseInputs();
  const size_t messages = ci.history.size();
  const std::string shelf = ci.skillsShelf;
  agent::HeadTools tools;
  tools.specs.push_back({"memory_search", std::string(4000, 'd'), "{}"});
  TurnFit fit(ci, inputWithResults() + std::string(40000, 'i'), &tools, today());
  const size_t want = 30000;
  TEST_ASSERT_TRUE(fit.shrink(want, FitScope::NewestMessage) + 512 >= want);   // one call, one pass (less a marker)
  TEST_ASSERT_EQUAL_UINT32((uint32_t)messages, (uint32_t)ci.history.size());
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)tools.specs.size());
  TEST_ASSERT_EQUAL_STRING(shelf.c_str(), ci.skillsShelf.c_str());
  TEST_ASSERT_TRUE(fit.note().find("input") != std::string::npos);
  // With nothing of its own left to give, it gives nothing (no history, no tools).
  for (int i = 0; i < 10 && fit.shrink(want, FitScope::NewestMessage) > 0; i++) {
  }
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)fit.shrink(want, FitScope::NewestMessage));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)messages, (uint32_t)ci.history.size());
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)tools.specs.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_rung_strictly_shrinks_or_gives_nothing);
  RUN_TEST(test_the_give_up_order);
  RUN_TEST(test_the_note_names_the_history_cut);
  RUN_TEST(test_older_history_scope_is_history_only);
  RUN_TEST(test_context_scope_stops_before_the_results);
  RUN_TEST(test_no_clock_history_goes_oldest_first);
  RUN_TEST(test_history_cuts_are_counted);
  RUN_TEST(test_the_owner_line);
  RUN_TEST(test_clips_stay_valid_utf8);
  RUN_TEST(test_photo_history_cut_stops_at_the_keep_floor_and_comes_back);
  RUN_TEST(test_newest_message_scope_never_touches_history_tools_or_shelf);
  return UNITY_END();
}
