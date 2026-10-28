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
  // F6b: every replayed byte - prose (8) + call arguments (9) + result (500).
  TEST_ASSERT_EQUAL_UINT32(517, (uint32_t)t.toolBytes());
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

// The replay as a provider renders it: the seed, then every entry in order.
static std::string body(const Transcript& t) {
  std::string b;
  for (const auto& e : t.entries()) b += std::to_string((int)e.kind) + "|" + e.id + "|" + e.text + "\n";
  return b;
}

// T6, the class rule (H6): run the trim the way every loop does - before each
// round - over 12 rounds whose results total 3x the cap. The request grows only
// at its end except at a fold, and folds come about cap/2 bytes apart, not one
// per round: the rounds whose body is not a byte extension of the previous one
// number at most ceil((total - cap) / (cap / 2)) + 1. Between folds a sent round
// keeps its bytes.
static void test_trim_folds_with_hysteresis() {
  for (size_t cap : {3000u, 6000u, 12000u}) {
    Transcript t;
    t.addUser("u");
    const size_t per = cap * 3 / 12;
    std::string prev;
    size_t breaks = 0, total = 0;
    for (int i = 0; i < 12; i++) {
      const std::string id = "id" + std::to_string(i);
      t.addToolCall(call(id.c_str(), "web_search", "{\"q\":\"x\"}"), i, "anthropic");
      t.addToolResult(res(id.c_str(), "web_search", std::string(per, (char)('a' + i))), i);
      total += per + 9;
      t.trimToolOutputs(cap);
      const std::string b = body(t);
      if (!prev.empty() && b.compare(0, prev.size(), prev) != 0) breaks++;
      prev = b;
      TEST_ASSERT_TRUE(t.toolBytes() <= cap);
      requireAnswered(t);
    }
    const size_t bound = (total - cap + cap / 2 - 1) / (cap / 2) + 1;
    TEST_ASSERT_TRUE_MESSAGE(breaks >= 1, "the replay never folded");
    TEST_ASSERT_TRUE_MESSAGE(breaks <= bound, ("cap " + std::to_string(cap)).c_str());
    // The newest round is always verbatim: the model has not seen it yet.
    const auto& e = t.entries();
    TEST_ASSERT_EQUAL_STRING(std::string(per, (char)('a' + 11)).c_str(), e.back().text.c_str());
    // The folded rounds name what ran, inside the seeded message.
    TEST_ASSERT_TRUE(e.front().text.find(Transcript::kFoldNote) != std::string::npos);
    TEST_ASSERT_TRUE(e.front().text.find("- round 0: web_search") != std::string::npos);
  }
}

// The budget refit's second rung (a CI clip, run 40631977181): foldOlderRounds
// folds older rounds and NEVER clips the newest round, however far the replay
// sits over the cap once the folding is done - trimToolOutputs skips its clip
// only when the fold lands the replay exactly on the cap, so a host whose
// byte accounting sits one byte over would clip the results the model has not
// seen one rung early. The cap path (the third rung's honest last resort, and
// the steady-state replay cap) still clips a newest round that alone exceeds
// the room.
static void test_fold_older_rounds_never_clips_the_newest() {
  Transcript t;
  t.addUser("u");
  for (int i = 0; i < 4; i++) {
    const std::string id = "id" + std::to_string(i);
    t.addToolCall(call(id.c_str(), "web_search", "{\"q\":\"x\"}"), i, "anthropic");
    t.addToolResult(res(id.c_str(), "web_search", std::string(2000, (char)('a' + i))), i);
  }
  const size_t perRound = t.toolBytes() / 4;   // four identical rounds
  TEST_ASSERT_TRUE(t.foldOlderRounds(perRound) > 0);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)perRound, (uint32_t)t.toolBytes(),
                                   "the fold freed the older rounds only");
  TEST_ASSERT_EQUAL_STRING(std::string(2000, (char)('a' + 3)).c_str(),
                           t.entries().back().text.c_str());   // newest verbatim
  requireAnswered(t);
  Transcript c;
  c.addUser("u");
  for (int i = 0; i < 4; i++) {
    const std::string id = "id" + std::to_string(i);
    c.addToolCall(call(id.c_str(), "web_search", "{\"q\":\"x\"}"), i, "anthropic");
    c.addToolResult(res(id.c_str(), "web_search", std::string(2000, (char)('a' + i))), i);
  }
  TEST_ASSERT_TRUE(c.trimToolOutputs(perRound / 2) > 0);   // the newest alone is over half a round
  TEST_ASSERT_TRUE(c.toolBytes() <= perRound / 2);
  TEST_ASSERT_TRUE(c.entries().back().text.size() < 2000);   // the cap path clipped it
}

// Counter-test: under the cap nothing is rewritten at all, however many rounds.
static void test_trim_under_cap_is_append_only() {
  Transcript t;
  t.addUser("u");
  std::string prev;
  for (int i = 0; i < 10; i++) {
    const std::string id = "id" + std::to_string(i);
    t.addToolCall(call(id.c_str(), "files_read", "{}"), i, "openai");
    t.addToolResult(res(id.c_str(), "files_read", std::string(100, 'x')), i);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)t.trimToolOutputs(100000));
    const std::string b = body(t);
    TEST_ASSERT_TRUE(b.compare(0, prev.size(), prev) == 0);
    prev = b;
  }
  TEST_ASSERT_TRUE(t.entries().front().text == "u");
}

// F6b: a long call argument counts like a long result. A 40 KB artifact_save
// written in round 0 is folded once the next round's results arrive, instead of
// riding every later round unbudgeted.
static void test_trim_counts_call_arguments() {
  Transcript t;
  t.addUser("write the plan");
  const std::string big = "{\"text\":\"" + std::string(40000, 'p') + "\"}";
  t.addToolCall(call("id0", "artifact_save", big.c_str()), 0, "mistral");
  t.addToolResult(res("id0", "artifact_save", "saved plan.md"), 0);
  TEST_ASSERT_TRUE(t.toolBytes() > 40000);
  t.addToolCall(call("id1", "files_list", "{}"), 1, "mistral");
  t.addToolResult(res("id1", "files_list", "plan.md 40 KB"), 1);
  TEST_ASSERT_TRUE(t.trimToolOutputs(24576) > 40000);
  TEST_ASSERT_TRUE(t.toolBytes() < 24576 / 2);
  TEST_ASSERT_TRUE(t.entries().front().text.find("- round 0: artifact_save") != std::string::npos);
  TEST_ASSERT_TRUE(t.entries().front().text.find("saved plan.md") != std::string::npos);
  requireAnswered(t);
}

static void test_trim_clips_the_newest_with_a_visible_marker() {
  Transcript t;
  t.addUser("u");
  t.addToolCall(call("id0", "files_read", "{}"), 0, "openai");
  t.addToolResult(res("id0", "files_read", std::string(1000, 'x')), 0);
  t.trimToolOutputs(1500);
  t.addToolCall(call("id1", "files_read", "{}"), 1, "openai");
  t.addToolResult(res("id1", "files_read", std::string(2000, 'y') + "END"), 1);
  TEST_ASSERT_TRUE(t.trimToolOutputs(1500) > 0);
  // Round 0 folded into the seed; round 1 alone is over the cap: clipped.
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)t.size());
  const std::string& newest = t.entries()[2].text;
  TEST_ASSERT_TRUE(newest.find("[cut: this turn's tool-output budget is spent") != std::string::npos);
  TEST_ASSERT_TRUE(newest.find("of 2003 B") != std::string::npos);   // names what was cut
  TEST_ASSERT_TRUE(newest.find("END") == std::string::npos);
  TEST_ASSERT_TRUE(t.toolBytes() <= 1500);
  requireAnswered(t);
  // With no room left at all, the newest is withheld with a note instead.
  Transcript u;
  u.addUser("u");
  const std::string huge = "{\"a\":\"" + std::string(1400, 'z') + "\"}";
  u.addToolCall(call("id2", "files_read", huge.c_str()), 0, "openai");
  u.addToolResult(res("id2", "files_read", std::string(800, 'w')), 0);
  u.trimToolOutputs(1500);
  TEST_ASSERT_TRUE(u.entries()[2].text.find("[not shown: this turn's tool-output budget is spent (800 B)") == 0);
  requireAnswered(u);
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
  RUN_TEST(test_trim_folds_with_hysteresis);
  RUN_TEST(test_trim_under_cap_is_append_only);
  RUN_TEST(test_fold_older_rounds_never_clips_the_newest);
  RUN_TEST(test_trim_counts_call_arguments);
  RUN_TEST(test_trim_clips_the_newest_with_a_visible_marker);
  RUN_TEST(test_trim_noop_under_budget);
  RUN_TEST(test_render_brief_bounded_and_marked);
  RUN_TEST(test_render_brief_marks_errors);
  return UNITY_END();
}
