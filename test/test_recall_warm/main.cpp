// test_recall_warm - the recall embedding made during the settle hold (nimbus/orch/recall_warm.h).
// The class: a vector made ahead is used only by the turn whose recall would have asked for
// exactly that text with the same embedder, at most once and while fresh; every page whose
// turn text could differ from the held text is not embedded ahead at all.
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/msg_batch.h"
#include "nimbus/orch/recall_warm.h"

using nimbus::orch::RecallWarm;
using nimbus::orch::warmableHeldText;
using nimbus::tg::Update;

void setUp() {}
void tearDown() {}

static Update text(const char* chat, const char* words) {
  Update u;
  u.updateId = 7;
  u.chatId = chat;
  u.from = "Sam";
  u.text = words;
  u.date = 1763532000;
  return u;
}

static const Update* pick(const std::vector<Update>& page) {
  return warmableHeldText(
      page, [](const std::string& c) { return c == "42"; }, [](int64_t d) { return d < 1763531000; });
}

static void test_one_plain_allowed_text_is_embedded_ahead() {
  const std::vector<Update> page = {text("42", "What is on today?")};
  TEST_ASSERT_EQUAL_PTR(&page[0], pick(page));
}

static void test_every_page_whose_turn_text_could_differ_is_not() {
  std::vector<std::vector<Update>> pages;
  pages.push_back({});
  pages.push_back({text("42", "a"), text("42", "b")});   // a burst: the turn joins them
  pages.push_back({text("99", "not allowed")});
  pages.push_back({text("42", "/budget")});              // a command runs no turn recall
  Update late = text("42", "late");
  late.date = 1763530000;                                 // the turn carries "(sent ...)"
  pages.push_back({late});
  Update fwd = text("42", "forwarded");
  fwd.forwarded = true;                                   // the turn labels it
  pages.push_back({fwd});
  Update photo = text("42", "caption");
  photo.attachment.kind = nimbus::tg::Attachment::Kind::Photo;
  photo.textIsCaption = true;
  pages.push_back({photo});
  Update edit = text("42", "");
  edit.edited = true;
  pages.push_back({edit});
  pages.push_back({text("42", "")});
  for (const auto& p : pages) TEST_ASSERT_NULL(pick(p));
}

// The premise of the rule: one on-time text renders as itself, so the turn's query is
// the held text byte for byte; two do not.
static void test_one_on_time_text_renders_as_itself() {
  nimbus::orch::MessageBatcher b;
  b.add("42", "Sam", "What is on today?");
  TEST_ASSERT_EQUAL_STRING("What is on today?", b.render(0).c_str());
  b.clear();
  b.add("42", "Sam", "one");
  b.add("42", "Sam", "two");
  TEST_ASSERT_TRUE(std::string(b.render(0).c_str()) != "one");
}

static void test_the_vector_is_taken_once_for_its_exact_text_and_embedder() {
  RecallWarm w;
  std::vector<int8_t> out;
  w.put("hello", "openai|m|256", std::vector<int8_t>{1, 2, 3}, 1000);
  TEST_ASSERT_TRUE(w.has("hello"));
  TEST_ASSERT_TRUE(w.take("hello", "openai|m|256", 2000, out));
  TEST_ASSERT_EQUAL(3, out.size());
  out.clear();
  TEST_ASSERT_FALSE(w.take("hello", "openai|m|256", 2000, out));   // used once
  TEST_ASSERT_FALSE(w.has("hello"));
}

static void test_any_difference_is_a_miss_and_empties_the_slot() {
  struct Case {
    const char* query;
    const char* embedder;
    uint32_t now;
  } cases[] = {
      {"hello!", "openai|m|256", 2000},                                    // another text
      {"hello", "cumulo|m|256", 2000},                                     // another embedder
      {"hello", "openai|m|512", 2000},                                     // another width
      {"hello", "openai|m|256", 1000 + nimbus::orch::kRecallWarmMaxAgeMs + 1},   // stale
  };
  for (const Case& c : cases) {
    RecallWarm w;
    std::vector<int8_t> out;
    w.put("hello", "openai|m|256", std::vector<int8_t>{1, 2, 3}, 1000);
    TEST_ASSERT_FALSE(w.take(c.query, c.embedder, c.now, out));
    TEST_ASSERT_TRUE(out.empty());
    TEST_ASSERT_FALSE(w.has("hello"));   // the next turn cannot pick it up either
  }
  RecallWarm w;
  std::vector<int8_t> out;
  w.put("hello", "openai|m|256", std::vector<int8_t>{1}, 4000000000u);
  TEST_ASSERT_TRUE(w.take("hello", "openai|m|256", 4000000000u + 10, out));   // near the wrap
  w.put("hello", "openai|m|256", std::vector<int8_t>{1}, 4294967000u);
  TEST_ASSERT_TRUE(w.take("hello", "openai|m|256", 200u, out));   // across the millis wrap
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_one_plain_allowed_text_is_embedded_ahead);
  RUN_TEST(test_every_page_whose_turn_text_could_differ_is_not);
  RUN_TEST(test_one_on_time_text_renders_as_itself);
  RUN_TEST(test_the_vector_is_taken_once_for_its_exact_text_and_embedder);
  RUN_TEST(test_any_difference_is_a_miss_and_empties_the_slot);
  return UNITY_END();
}
