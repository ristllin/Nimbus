#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/transcript.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static HeadToolCall call(const char* id, const char* name, const char* args) {
  HeadToolCall c;
  c.id = id; c.name = name; c.argsJson = args;
  return c;
}
static HeadToolResult res(const char* id, const char* name, const std::string& out,
                          bool err = false) {
  HeadToolResult r;
  r.id = id; r.name = name; r.output = out; r.isError = err;
  return r;
}

// ---- append + accounting -----------------------------------------------------
static void test_append_and_tool_bytes() {
  Transcript t;
  t.addUser("do the thing");
  t.addAssistantText("thinking", 0, "anthropic");
  t.addToolCall(call("id0", "memory_search", "{\"q\":\"x\"}"), 0, "anthropic");
  t.addToolResult(res("id0", "memory_search", std::string(500, 'a')), 0);
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)t.size());
  TEST_ASSERT_EQUAL_UINT32(500, (uint32_t)t.toolBytes());   // results only
  TEST_ASSERT_TRUE(t.entries()[0].pinned);                  // seeded user pinned
  TEST_ASSERT_EQUAL_STRING("anthropic", t.entries()[2].provider);
  TEST_ASSERT_TRUE(t.entries()[2].kind == TranscriptItem::Kind::ToolUse);
}

// Empty assistant prose is not recorded (providers emit it on tool-only rounds).
static void test_empty_prose_not_recorded() {
  Transcript t;
  t.addUser("u");
  t.addAssistantText("", 0, "openai");
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)t.size());
}

// ---- trim: append-only, NEVER orphans a pair ---------------------------------
static void requireAnswered(const Transcript& t) {
  for (const auto& a : t.entries()) {
    if (a.kind != TranscriptItem::Kind::ToolUse) continue;
    bool answered = false;
    for (const auto& b : t.entries())
      if (b.kind == TranscriptItem::Kind::ToolResult && b.id == a.id) answered = true;
    TEST_ASSERT_TRUE_MESSAGE(answered, "trim orphaned a tool call");
  }
}

// The class rule: a round that was already sent keeps its bytes whatever the
// budget, so the replayed request only ever grows at its end (prompt_layout.h).
// Run the trim the way every loop does - before each round - and check every
// earlier result is untouched after every later trim.
static void test_trim_never_rewrites_a_sent_round() {
  for (size_t cap : {1500u, 2500u, 4000u, 9000u}) {
    Transcript t;
    t.addUser("u");
    std::vector<std::string> sent;   // each round's result as it went on the wire
    for (int i = 0; i < 6; i++) {
      const std::string id = "id" + std::to_string(i);
      t.addToolCall(call(id.c_str(), "web_search", "{}"), i, "anthropic");
      t.addToolResult(res(id.c_str(), "web_search", std::string(1000, 'a' + i)), i);
      t.trimToolOutputs(cap);
      int seen = 0;
      for (const auto& e : t.entries()) {
        if (e.kind != TranscriptItem::Kind::ToolResult) continue;
        if (seen < (int)sent.size())
          TEST_ASSERT_TRUE_MESSAGE(e.text == sent[seen], "a sent round was rewritten");
        else
          sent.push_back(e.text);
        seen++;
      }
    }
    requireAnswered(t);
  }
}

static void test_trim_clips_the_newest_with_a_visible_marker() {
  Transcript t;
  t.addUser("u");
  t.addToolCall(call("id0", "files_read", "{}"), 0, "openai");
  t.addToolResult(res("id0", "files_read", std::string(1000, 'x')), 0);
  t.trimToolOutputs(1500);
  t.addToolCall(call("id1", "files_read", "{}"), 1, "openai");
  t.addToolResult(res("id1", "files_read", std::string(1000, 'y') + "END"), 1);
  const size_t before = t.size();
  TEST_ASSERT_TRUE(t.trimToolOutputs(1500) > 0);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)t.size());   // nothing removed
  TEST_ASSERT_EQUAL_UINT32(1000, (uint32_t)t.entries()[2].text.size());   // round 0 verbatim
  const std::string& newest = t.entries()[4].text;
  TEST_ASSERT_TRUE(newest.find("[cut: this turn's tool-output budget is spent") != std::string::npos);
  TEST_ASSERT_TRUE(newest.find("of 1003 B") != std::string::npos);   // names what was cut
  TEST_ASSERT_TRUE(newest.find("END") == std::string::npos);
  TEST_ASSERT_TRUE(t.toolBytes() <= 1500);
  requireAnswered(t);
  // With no room left at all, the newest is withheld with a note instead.
  t.addToolCall(call("id2", "files_read", "{}"), 2, "openai");
  t.addToolResult(res("id2", "files_read", std::string(800, 'w')), 2);
  t.trimToolOutputs(1500);
  TEST_ASSERT_TRUE(t.entries()[6].text.find("[not shown: this turn's tool-output budget is spent (800 B)") == 0);
  requireAnswered(t);
}

static void test_trim_noop_under_budget() {
  Transcript t;
  t.addUser("u");
  t.addToolCall(call("i", "t", "{}"), 0, "openai");
  t.addToolResult(res("i", "t", "small"), 0);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)t.trimToolOutputs(100000));
  TEST_ASSERT_EQUAL_STRING("small", t.entries()[2].text.c_str());
}

// ---- renderBrief: bounded, ordered, marks omissions ---------------------------
static void test_render_brief_bounded_and_marked() {
  Transcript t;
  t.addUser("the question");
  for (int i = 0; i < 6; i++) {
    const std::string id = "id" + std::to_string(i);
    t.addToolCall(call(id.c_str(), "memory_search", "{}"), i, "mistral");
    t.addToolResult(res(id.c_str(), "memory_search", "result " + std::to_string(i)), i);
  }
  std::string full = t.renderBrief(100000);
  TEST_ASSERT_TRUE(full.find("[user] the question") == 0);
  TEST_ASSERT_TRUE(full.find("[result] memory_search result 5") != std::string::npos);
  TEST_ASSERT_TRUE(full.find("omitted") == std::string::npos);

  std::string tight = t.renderBrief(120);
  TEST_ASSERT_TRUE(tight.size() <= 160);                       // bounded (+ the marker)
  TEST_ASSERT_TRUE(tight.find("earlier entries omitted") != std::string::npos);
}

static void test_render_brief_marks_errors() {
  Transcript t;
  t.addUser("u");
  t.addToolCall(call("i", "web_search", "{}"), 0, "openai");
  t.addToolResult(res("i", "web_search", "boom", /*err=*/true), 0);
  std::string b = t.renderBrief(100000);
  TEST_ASSERT_TRUE(b.find("(error)") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_and_tool_bytes);
  RUN_TEST(test_empty_prose_not_recorded);
  RUN_TEST(test_trim_never_rewrites_a_sent_round);
  RUN_TEST(test_trim_clips_the_newest_with_a_visible_marker);
  RUN_TEST(test_trim_noop_under_budget);
  RUN_TEST(test_render_brief_bounded_and_marked);
  RUN_TEST(test_render_brief_marks_errors);
  return UNITY_END();
}
