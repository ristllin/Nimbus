// test_tx_overhead - text added around messages cannot pass unseen (PRD section 2 and
// section 11, rule 3 against prompt-patching; layer L1; lane RB-B0b writes it). The
// rendered body minus the stored item bytes minus the system text minus the tools is at
// most 96 B per owner message plus 64 B per photo label, plus one fixed 512 B envelope
// (model, max_tokens, thinking, tool_choice, cache marks) that does not grow with the
// conversation. A renderer that injects a block per message (a RUNNING MEMORY, a ledger
// block, a reminder line) fails here. Wire level per dialect (RB-W1a, RB-W2, RB-W3,
// RB-W4); a real conversation through the engine (RB-E1a). Counter-tested first.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "../support/tx_battery/render.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

// ---- counter-tests ---------------------------------------------------------------------
void test_checker_allows_framing_and_rejects_an_injected_block() {
  txb::OverheadInput in;
  in.itemBytes = 40000;
  in.systemBytes = 7000;
  in.toolsBytes = 9000;
  in.ownerMessages = 20;
  in.photoLabels = 2;
  long over = 0;
  std::string why;
  // 20 x 96 + 2 x 64 + 512 = 2,560 B of framing allowed.
  in.bodyBytes = 40000 + 7000 + 9000 + 2560;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
  TEST_ASSERT_EQUAL(2560, (int)over);
  in.bodyBytes += 1;
  TEST_ASSERT_FALSE(txb::overheadWithin(in, over, why));
  // A 300 B block injected around each owner message is caught from two messages on (the
  // fixed envelope can hide at most 512 B once, never a per-message block).
  for (size_t owners : {2, 6, 30, 150}) {
    in.ownerMessages = owners;
    in.photoLabels = 0;
    in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + owners * (60 + 300);
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::overheadWithin(in, over, why), "a 300 B block per message passed");
  }
  in.ownerMessages = 1;
  in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + 96 + 512 + 1;
  why.clear();
  TEST_ASSERT_FALSE_MESSAGE(txb::overheadWithin(in, over, why), "one byte over the envelope passed");
  // Photos earn their 64 B label, not more.
  in.ownerMessages = 1;
  in.photoLabels = 4;
  in.bodyBytes = in.itemBytes + in.systemBytes + in.toolsBytes + 96 + 4 * 64 + 512;
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
}

void test_json_string_bytes_count_escapes() {
  TEST_ASSERT_EQUAL(2, (int)txb::jsonStringBytes(""));
  TEST_ASSERT_EQUAL(7, (int)txb::jsonStringBytes("hello"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\nb"));
  TEST_ASSERT_EQUAL(6, (int)txb::jsonStringBytes("a\"b"));
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
    TXB_OK(txb::renderedItemBytes(s.c, cov, in.itemBytes));
    const size_t images = txb::detail::countImages(r.view);
    // A file reference carries no pixels in the body; an inline photo carries its base64.
    if (r.body.find("file_pab12cd34") == std::string::npos) in.itemBytes += images * photo;
    in.bodyBytes = r.body.size();
    in.systemBytes = txb::systemJsonBytes(r.view);
    in.toolsBytes = txb::toolsJsonBytes(r.view);
    for (const tx::Item& it : s.c.items) {
      in.ownerMessages += it.kind == tx::ItemKind::User;
      for (const tx::Part& p : it.parts) in.photoLabels += p.kind == tx::PartKind::Image;
    }
    long over = 0;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), (s.name + ": " + why).c_str());
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
// Twelve real turns (tools, outside content, gaps): the last request's framing is bounded.
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
  std::vector<tx::Item> all;
  TXB_OK(b.stored(all));
  // The last request was rendered before the last turn's final text was stored.
  txb::Convo c;
  for (const tx::Item& it : all) {
    if (it.turn == b.turnNo && it.kind == tx::ItemKind::Assistant && it.fin) break;
    c.items.push_back(it);
  }
  txb::CoverInput cov;
  cov.items = &c.items;
  cov.openTurn = b.turnNo;
  for (const tx::Item& it : c.items)
    if (it.kind == tx::ItemKind::PointerMove) cov.pointer = std::max(cov.pointer, it.stub);
  txb::OverheadInput in;
  TXB_OK(txb::renderedItemBytes(c, cov, in.itemBytes));
  const sp::Recorded& last = b.model.last();
  in.bodyBytes = last.body.size();
  in.systemBytes = txb::systemJsonBytes(last.view);
  in.toolsBytes = txb::toolsJsonBytes(last.view);
  for (const tx::Item& it : c.items) in.ownerMessages += it.kind == tx::ItemKind::User;
  long over = 0;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::overheadWithin(in, over, why), why.c_str());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_allows_framing_and_rejects_an_injected_block);
  RUN_TEST(test_json_string_bytes_count_escapes);
  RUN_TEST(test_wire_overhead_anthropic);
  RUN_TEST(test_wire_overhead_responses);
  RUN_TEST(test_wire_overhead_chat);
  RUN_TEST(test_wire_overhead_flat);
  RUN_TEST(test_engine_requests_stay_within_the_framing);
  return UNITY_END();
}
