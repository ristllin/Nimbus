#include <unity.h>

#include <string>

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

// ---- trim: stubs payloads, NEVER orphans a pair ------------------------------
static void test_trim_stubs_oldest_and_keeps_pairing() {
  Transcript t;
  t.addUser("u");
  for (int i = 0; i < 4; i++) {
    const std::string id = "id" + std::to_string(i);
    t.addToolCall(call(id.c_str(), "web_search", "{}"), i, "anthropic");
    t.addToolResult(res(id.c_str(), "web_search", std::string(1000, 'z')), i);
  }
  TEST_ASSERT_EQUAL_UINT32(4000, (uint32_t)t.toolBytes());
  const size_t before = t.size();
  size_t freed = t.trimToolOutputs(1500);
  TEST_ASSERT_TRUE(freed > 0);
  TEST_ASSERT_TRUE(t.toolBytes() <= 1500);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)before, (uint32_t)t.size());   // nothing removed
  // Every ToolUse still has its ToolResult.
  for (const auto& a : t.entries()) {
    if (a.kind != TranscriptItem::Kind::ToolUse) continue;
    bool answered = false;
    for (const auto& b : t.entries())
      if (b.kind == TranscriptItem::Kind::ToolResult && b.id == a.id) answered = true;
    TEST_ASSERT_TRUE_MESSAGE(answered, "trim orphaned a tool call");
  }
  // Oldest was stubbed; the newest survived verbatim.
  TEST_ASSERT_TRUE(t.entries()[2].text.find("[trimmed 1000 B]") == 0);
  TEST_ASSERT_EQUAL_UINT32(1000, (uint32_t)t.entries()[8].text.size());
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
  RUN_TEST(test_trim_stubs_oldest_and_keeps_pairing);
  RUN_TEST(test_trim_noop_under_budget);
  RUN_TEST(test_render_brief_bounded_and_marked);
  RUN_TEST(test_render_brief_marks_errors);
  return UNITY_END();
}
