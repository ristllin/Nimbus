// test_tx_overhead - text added around messages cannot pass unseen (PRD section 2 and
// section 11, rule 3 against prompt-patching; layer L1; lane RB-B0b writes it). Two
// properties. Bytes: the rendered body minus the stored text it shows minus the system text
// minus the tools is at most 96 B per owner message and 64 B per photo label, plus a fixed
// framing allowance per tool call, result and extra text block and one 512 B envelope
// (model, max_tokens, thinking, tool_choice, cache marks) that does not grow with the
// conversation. Text: every text block is stored text, so a RUNNING MEMORY, a ledger block
// or a reminder written around messages fails however much framing slack is left. Wire
// level per dialect (RB-W1a, RB-W2, RB-W3, RB-W4); real turns through the engine (RB-E1a).
// Both checkers are counter-tested first.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "../support/tx_battery/render.h"
#include "../support/tx_battery/toy_render.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

// ---- counter-tests ---------------------------------------------------------------------
txb::OverheadInput day(size_t owners, size_t tools) {
  txb::OverheadInput in;
  in.itemBytes = 40000;
  in.systemBytes = 7000;
  in.toolsBytes = 9000;
  in.ownerMessages = owners;
  in.calls = in.results = owners * tools;
  in.textBlocks = owners;
  return in;
}

void test_checker_allows_framing_and_rejects_an_injected_block() {
  txb::OverheadInput in = day(20, 2);
  long over = 0;
  std::string why;
  const long bound = txb::overheadBound(in);
  TEST_ASSERT_EQUAL(20 * 96 + 40 * 96 + 40 * 80 + 20 * 48 + 512, (int)bound);
  in.bodyBytes = 40000 + 7000 + 9000 + (size_t)bound;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
  // Text added around every owner message, beyond each block's framing, is caught.
  for (size_t owners : {2, 6, 30, 150}) {
    in = day(owners, 1);
    in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + (size_t)txb::overheadBound(in) - 512 + owners * 300;
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::overheadWithin(in, over, why), "a 300 B block per message passed");
  }
  // Photos earn their 64 B label, not more.
  in = day(1, 0);
  in.photoLabels = 4;
  in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + 96 + 48 + 4 * 64 + 512;
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
}

void test_json_string_bytes_count_escapes() {
  TEST_ASSERT_EQUAL(2, (int)txb::jsonStringBytes(""));
  TEST_ASSERT_EQUAL(7, (int)txb::jsonStringBytes("hello"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\nb"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\"b"));
}

// Text written around messages that no stored item holds is caught, whatever the slack.
void test_checker_finds_text_no_item_holds() {
  txb::Convo c;
  for (int i = 0; i < 3; ++i) c.ownerTurn("question " + std::to_string(i), "answer " + std::to_string(i));
  c.openTurn();
  c.owner("photo time", {"pab12cd34"});
  txb::CoverInput in;
  in.items = &c.items;
  in.bulk = &c.bulk;
  in.openTurn = c.turn;
  sp::RequestView v = txb::toyRender(in, txb::Mutation::None, 1);
  v.messages.back().blocks.push_back(sp::detail::textBlock("[photo pab12cd34 Tue 17 Nov 13:02 not shown: view id=pab12cd34]"));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(v, c.items, why), why.c_str());
  const char* injected[] = {"RUNNING MEMORY: the owner likes skyr.", "Remember to log every meal.",
                            "[device] recent actions: L00A3 skyr"};
  for (const char* text : injected) {
    sp::RequestView bad = v;
    bad.messages.back().blocks.back().text += std::string(" ") + text;
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::noUnstoredText(bad, c.items, why), text);
    bad = v;
    bad.messages.front().blocks.push_back(sp::detail::textBlock(text));
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::noUnstoredText(bad, c.items, why), text);
  }
}

// ---- wire level ------------------------------------------------------------------------
size_t base64Bytes(size_t n) { return 4 * ((n + 2) / 3); }

void wireOverhead(plain::Dialect d) {
  txb::RenderFixture fx(d);
  const size_t photo = base64Bytes(fx.photoBytes.begin()->second.size());
  for (const txb::Snapshot& s : txb::dayOfFourTurns(d)) {
    txb::Rendered r;
    TXB_OK(txb::renderItems(fx, s.c, 0, r, s.openTurn));
    txb::CoverInput cov;
    cov.items = &s.c.items;
    cov.bulk = &s.c.bulk;
    cov.openTurn = s.openTurn;
    txb::OverheadInput in;
    txb::renderedText(s.c, cov, in);
    // A file reference carries no pixels in the body; an inline photo carries its base64.
    if (r.body.find("file_pab12cd34") == std::string::npos) in.itemBytes += txb::detail::countImages(r.view) * photo;
    in.bodyBytes = r.body.size();
    in.systemBytes = txb::systemJsonBytes(r.view);
    in.toolsBytes = txb::toolsJsonBytes(r.view);
    long over = 0;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), (s.name + ": " + why).c_str());
    TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(r.view, s.c.items, why), (s.name + ": " + why).c_str());
  }
}

void test_wire_overhead_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wireOverhead(plain::Dialect::AnthropicMessages);
}
void test_wire_overhead_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wireOverhead(plain::Dialect::OpenAIResponses);
}
void test_wire_overhead_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wireOverhead(plain::Dialect::ChatCompletions);
}
void test_wire_overhead_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1");
  wireOverhead(plain::Dialect::Flat);
}

// ---- engine level ----------------------------------------------------------------------
// Twelve real turns (tools, outside content, gaps): the last request's framing is bounded
// and every text in it is stored text.
void test_engine_requests_stay_within_the_framing() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  b.world.files["notes/boat.md"] = "Nereid II, 9.5 m";
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 12; ++i) {
    const std::string k = std::to_string(i);
    turns.push_back(i % 3 == 0   ? txb::withTool(60, "boat " + k, "read_file", R"({"path":"notes/boat.md"})", "Nereid " + k)
                    : i % 3 == 1 ? txb::withTool(400, "search " + k, "web_search", "{\"q\":\"s" + k + "\"}", "found " + k)
                                 : txb::chat(30, "chat " + k, "reply " + k));
  }
  turns.push_back(txb::chat(60, "last one", "bye"));
  TXB_OK(txb::runTurns(b, turns));
  txb::Convo c;
  TXB_OK(b.storedBeforeLastFinal(c.items));
  const txb::CoverInput cov = b.coverOf(c.items);
  txb::OverheadInput in;
  txb::renderedText(c, cov, in);
  const sp::Recorded& last = b.model.last();
  in.bodyBytes = last.body.size();
  in.systemBytes = txb::systemJsonBytes(last.view);
  in.toolsBytes = txb::toolsJsonBytes(last.view);
  long over = 0;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  TEST_ASSERT_TRUE_MESSAGE(txb::noUnstoredText(last.view, c.items, why), why.c_str());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_allows_framing_and_rejects_an_injected_block);
  RUN_TEST(test_json_string_bytes_count_escapes);
  RUN_TEST(test_checker_finds_text_no_item_holds);
  RUN_TEST(test_wire_overhead_anthropic);
  RUN_TEST(test_wire_overhead_responses);
  RUN_TEST(test_wire_overhead_chat);
  RUN_TEST(test_wire_overhead_flat);
  RUN_TEST(test_engine_requests_stay_within_the_framing);
  return UNITY_END();
}
