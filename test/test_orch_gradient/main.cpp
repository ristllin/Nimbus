#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/gradient.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static TranscriptItem mk(TranscriptItem::Kind k, const char* id, const char* name,
                         const std::string& text, int round, bool pinned = false) {
  TranscriptItem it;
  it.kind = k;
  it.id = id;
  it.name = name;
  it.text = text;
  it.round = (int8_t)round;
  it.pinned = pinned;
  return it;
}

// A 4-round transcript: user + (call,result) per round 0..3.
static std::vector<TranscriptItem> fourRounds() {
  std::vector<TranscriptItem> v;
  v.push_back(mk(TranscriptItem::Kind::User, "", "", "[USER] do the thing", -1, true));
  for (int r = 0; r < 4; r++) {
    v.push_back(mk(TranscriptItem::Kind::ToolUse, ("id" + std::to_string(r)).c_str(),
                   "memory_search", "{\"q\":\"round " + std::to_string(r) + "\"}", r));
    v.push_back(mk(TranscriptItem::Kind::ToolResult, ("id" + std::to_string(r)).c_str(),
                   "memory_search", "result body for round " + std::to_string(r), r));
  }
  return v;
}

// ---- pair folding: old rounds collapse to one User line each -----------------
static void test_pairs_fold_to_one_line() {
  GradientPolicy pol;  // keepRounds=2, always (trigger 0)
  auto out = gradientTrim(fourRounds(), pol);
  // rounds 0,1 fold (2 pairs -> 2 lines); rounds 2,3 stay verbatim (4 items) + user.
  TEST_ASSERT_EQUAL_INT(1 + 2 + 4, (int)out.size());
  TEST_ASSERT_TRUE(out[1].kind == TranscriptItem::Kind::User);
  TEST_ASSERT_TRUE(out[1].text.find("[earlier round 0] memory_search") == 0);
  TEST_ASSERT_TRUE(out[1].text.find("(23 B)") != std::string::npos);
  TEST_ASSERT_TRUE(out[2].text.find("[earlier round 1]") == 0);
}

// ---- newest keepRounds stay verbatim ----------------------------------------
static void test_keep_rounds_verbatim() {
  GradientPolicy pol;
  auto out = gradientTrim(fourRounds(), pol);
  int verbatimCalls = 0;
  for (const auto& it : out)
    if (it.kind == TranscriptItem::Kind::ToolUse) verbatimCalls++;
  TEST_ASSERT_EQUAL_INT(2, verbatimCalls);  // rounds 2 and 3 only
}

// ---- pairing invariant: no orphan ToolUse survives ---------------------------
static void test_no_orphan_tool_use() {
  GradientPolicy pol;
  auto out = gradientTrim(fourRounds(), pol);
  for (const auto& it : out) {
    if (it.kind != TranscriptItem::Kind::ToolUse) continue;
    bool answered = false;
    for (const auto& r : out)
      if (r.kind == TranscriptItem::Kind::ToolResult && r.id == it.id) answered = true;
    TEST_ASSERT_TRUE_MESSAGE(answered, "orphan tool_use survived the trim");
  }
}

// ---- an unanswered old call is kept verbatim, never half-folded --------------
static void test_unanswered_old_call_kept() {
  auto v = fourRounds();
  // remove round 0's RESULT: its call must then survive verbatim.
  for (size_t i = 0; i < v.size(); i++)
    if (v[i].kind == TranscriptItem::Kind::ToolResult && v[i].id == "id0") {
      v.erase(v.begin() + i);
      break;
    }
  GradientPolicy pol;
  auto out = gradientTrim(v, pol);
  bool call0Verbatim = false;
  for (const auto& it : out)
    if (it.kind == TranscriptItem::Kind::ToolUse && it.id == "id0") call0Verbatim = true;
  TEST_ASSERT_TRUE(call0Verbatim);
}

// ---- pinned + prose survive --------------------------------------------------
static void test_pinned_and_prose_survive() {
  auto v = fourRounds();
  v.push_back(mk(TranscriptItem::Kind::AssistantText, "", "", "thinking prose r0", 0));
  GradientPolicy pol;
  auto out = gradientTrim(v, pol);
  TEST_ASSERT_TRUE(out[0].pinned);
  TEST_ASSERT_EQUAL_STRING("[USER] do the thing", out[0].text.c_str());
  bool proseSurvived = false;
  for (const auto& it : out)
    if (it.kind == TranscriptItem::Kind::AssistantText) proseSurvived = true;
  TEST_ASSERT_TRUE(proseSurvived);
}

// ---- below trigger: byte-identical passthrough -------------------------------
static void test_below_trigger_is_identity() {
  GradientPolicy pol;
  pol.triggerBytes = 1 << 20;  // far above the transcript size
  auto in = fourRounds();
  auto out = gradientTrim(in, pol);
  TEST_ASSERT_EQUAL_INT((int)in.size(), (int)out.size());
  for (size_t i = 0; i < in.size(); i++) {
    TEST_ASSERT_TRUE(in[i].kind == out[i].kind);
    TEST_ASSERT_EQUAL_STRING(in[i].text.c_str(), out[i].text.c_str());
  }
}

// ---- UTF-8-safe fold clipping ------------------------------------------------
static void test_fold_line_utf8_safe() {
  // ⚠ MUTATION-CHECKED (prism 2026-08-05): the previous version of this test was
  // GREEN with the UTF-8 backup deleted - it picked a cap that happened to land
  // on a boundary and only asserted the ellipsis was present. Now the cap
  // deliberately lands MID-character and we validate the kept bytes.
  //   "tag: " is 5 bytes, then 3-byte '€' chars -> a cap of 51 cuts 1 byte into
  //   the 16th char (5 + 15*3 = 50), which a naive substr would split.
  std::string s;
  for (int i = 0; i < 100; i++) s += "\xE2\x82\xAC";  // €
  std::string line = foldLine("tag", s, 51);
  const std::string ell = "\xE2\x80\xA6";             // the appended ellipsis
  TEST_ASSERT_TRUE(line.size() >= ell.size());
  TEST_ASSERT_EQUAL_STRING(ell.c_str(), line.substr(line.size() - ell.size()).c_str());
  const std::string kept = line.substr(0, line.size() - ell.size());
  // Every byte sequence in the kept prefix must be a COMPLETE code point: walk it.
  size_t i = 0;
  while (i < kept.size()) {
    unsigned char c = (unsigned char)kept[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    TEST_ASSERT_TRUE_MESSAGE(len > 0, "lead byte invalid - the clip split a character");
    TEST_ASSERT_TRUE_MESSAGE(i + len <= kept.size(), "trailing partial character survived the clip");
    for (size_t k = 1; k < len; k++)
      TEST_ASSERT_TRUE_MESSAGE(((unsigned char)kept[i + k] & 0xC0) == 0x80, "broken sequence");
    i += len;
  }
  TEST_ASSERT_TRUE(kept.size() <= 51);
}

// ---- whitespace collapses to one line ---------------------------------------
static void test_fold_line_collapses_newlines() {
  std::string line = foldLine("t", "a\nb\r\n  c\td", 100);
  TEST_ASSERT_EQUAL_STRING("t: a b c d", line.c_str());
}

// ---- determinism -------------------------------------------------------------
static void test_deterministic() {
  GradientPolicy pol;
  auto a = gradientTrim(fourRounds(), pol);
  auto b = gradientTrim(fourRounds(), pol);
  TEST_ASSERT_EQUAL_INT((int)a.size(), (int)b.size());
  for (size_t i = 0; i < a.size(); i++)
    TEST_ASSERT_EQUAL_STRING(a[i].text.c_str(), b[i].text.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pairs_fold_to_one_line);
  RUN_TEST(test_keep_rounds_verbatim);
  RUN_TEST(test_no_orphan_tool_use);
  RUN_TEST(test_unanswered_old_call_kept);
  RUN_TEST(test_pinned_and_prose_survive);
  RUN_TEST(test_below_trigger_is_identity);
  RUN_TEST(test_fold_line_utf8_safe);
  RUN_TEST(test_fold_line_collapses_newlines);
  RUN_TEST(test_deterministic);
  return UNITY_END();
}
