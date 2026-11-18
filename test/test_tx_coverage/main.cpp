// test_tx_coverage - no silently dropped history (PRD sections 2 and 3; layer L1; lane
// RB-B0b writes it). Every stored item after the digests' thru is in the request: whole,
// as its stored stub with its view hint, or inside one named omission of the oldest whole
// turns (never the newest 6 owner turns). Every photo is shown or named. Compaction never
// leaves a row uncovered: digests through thru plus every item after it, whatever fails.
//
// The v5.1.1 product sent a summary plus one exchange on 148 of 150 turns; this suite is
// the test that would have caught it. Wire level (RB-W1a, RB-W2, RB-W3, RB-W4) at every
// pointer value; engine level (RB-E1a); the ladder on an 85,000 B head (RB-W3); compaction
// (RB-C2). The checker is counter-tested against a reference renderer and ten wrong ones.
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

// Ten owner turns of every result class, the last one open.
txb::Convo tenTurns() {
  txb::Convo c;
  for (int i = 0; i < 9; ++i) {
    c.openTurn();
    c.owner("question " + std::to_string(i), i == 3 ? std::vector<std::string>{"pab12cd34"} : std::vector<std::string>{});
    const std::string id = c.call("read_file", "{\"path\":\"notes/" + std::to_string(i) + ".md\"}");
    if (i % 3 == 0) c.receipt(id, "read_file", "small " + std::to_string(i));
    if (i % 3 == 1) c.bulkResult(id, "read_file", txb::payload("BULK" + std::to_string(i), 5000));
    if (i % 3 == 2) c.outside(id, "web_search", "{}", txb::payload("OUT" + std::to_string(i), 3000));
    c.say("answer " + std::to_string(i));
    c.close();
  }
  c.openTurn();
  c.owner("the open question");
  const std::string id = c.call("big_read", "{}");
  c.bulkResult(id, "big_read", txb::payload("OPENBULK", 9000));
  return c;
}

txb::CoverInput coverOf(const txb::Convo& c, uint64_t pointer, uint64_t thru = 0) {
  txb::CoverInput in;
  in.items = &c.items;
  in.bulk = &c.bulk;
  in.pointer = pointer;
  in.thru = thru;
  in.openTurn = c.turn;
  return in;
}

// ---- counter-tests: a reference renderer passes, each wrong renderer fails ---------------
void test_checker_passes_the_reference_renderer_at_every_pointer() {
  const txb::Convo c = tenTurns();
  for (uint64_t p = 0; p <= c.q; ++p) {
    const txb::CoverInput in = coverOf(c, p);
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::covers(txb::toyRender(in), in, why), why.c_str());
    why.clear();
    TEST_ASSERT_TRUE_MESSAGE(txb::covers(txb::toyRender(in, txb::Mutation::None, 3), in, why), why.c_str());
  }
}

void test_checker_fails_every_wrong_renderer() {
  const txb::Convo c = tenTurns();
  const txb::CoverInput in = coverOf(c, 0);
  const txb::Mutation wrong[] = {txb::Mutation::DropOwnerMessage, txb::Mutation::DropResult, txb::Mutation::DropCall,
                                 txb::Mutation::DuplicateResult,  txb::Mutation::LeakOutside, txb::Mutation::StubOpenBulk,
                                 txb::Mutation::OmitSilently,     txb::Mutation::OmitNewest,  txb::Mutation::HidePhoto};
  for (txb::Mutation m : wrong) {
    std::string why;
    char msg[48];
    std::snprintf(msg, sizeof msg, "mutation %d passed the coverage check", (int)m);
    TEST_ASSERT_FALSE_MESSAGE(txb::covers(txb::toyRender(in, m), in, why), msg);
    TEST_ASSERT_FALSE(why.empty());
  }
  // A stub without its view hint, at a pointer where bulk is stubbed.
  const txb::CoverInput at = coverOf(c, c.q - 4);
  std::string why;
  TEST_ASSERT_FALSE(txb::covers(txb::toyRender(at, txb::Mutation::StubWithoutHint), at, why));
}

void test_checker_requires_every_digest_in_the_system_text() {
  txb::Convo c = tenTurns();
  tx::Item& s = c.add(tx::ItemKind::Digest);
  s.day = "2026-11-16";
  s.x = "Mon 16 Nov: the boat engine, 9.5 m.";
  s.thru = 20;
  const txb::CoverInput in = coverOf(c, 0);
  sp::RequestView v = txb::toyRender(coverOf(c, 0, 20));
  std::string why;
  TEST_ASSERT_FALSE_MESSAGE(txb::compactedCovers(v, in, why), "a digest missing from the system passed");
  v.systemBlocks.push_back(s.x);
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::compactedCovers(v, in, why), why.c_str());
  // Items after thru are still each required.
  sp::RequestView dropped = txb::toyRender(coverOf(c, 0, 20), txb::Mutation::DropOwnerMessage);
  dropped.systemBlocks.push_back(s.x);
  why.clear();
  TEST_ASSERT_FALSE(txb::compactedCovers(dropped, in, why));
}

// ---- wire level ------------------------------------------------------------------------
void wireCoverage(plain::Dialect d) {
  txb::RenderFixture fx(d);
  for (const txb::Snapshot& s : txb::dayOfFourTurns(d))
    for (uint64_t p = 0; p <= s.c.q; ++p) {
      txb::Rendered r;
      TXB_OK(txb::renderItems(fx, s.c, p, r, s.openTurn));
      txb::CoverInput in = coverOf(s.c, p);
      in.openTurn = s.openTurn;
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(txb::covers(r.view, in, why), (s.name + " pointer " + std::to_string(p) + ": " + why).c_str());
    }
}

void test_wire_coverage_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wireCoverage(plain::Dialect::AnthropicMessages);
}
void test_wire_coverage_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wireCoverage(plain::Dialect::OpenAIResponses);
}
void test_wire_coverage_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wireCoverage(plain::Dialect::ChatCompletions);
}
void test_wire_coverage_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1");
  wireCoverage(plain::Dialect::Flat);
}

// ---- engine level ----------------------------------------------------------------------
// The stored items the last request was rendered from: the last turn's final text and
// what follows it are stored after that request.
std::vector<tx::Item> beforeFinalText(std::vector<tx::Item> items, uint32_t turn) {
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].turn == turn && items[i].kind == tx::ItemKind::Assistant && items[i].fin) {
      items.resize(i);
      break;
    }
  return items;
}

txb::CoverInput storedCover(const std::vector<tx::Item>& items, uint32_t openTurn) {
  txb::CoverInput in;
  in.items = &items;
  in.openTurn = openTurn;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::PointerMove) in.pointer = std::max(in.pointer, it.stub);
  return in;
}

void expectLastRequestCovers(txb::Battery& b) {
  std::vector<tx::Item> all;
  TXB_OK(b.stored(all));
  const std::vector<tx::Item> items = beforeFinalText(all, b.turnNo);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::compactedCovers(b.model.last().view, storedCover(items, b.turnNo), why), why.c_str());
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

std::vector<txb::TurnScript> longChat(size_t n, size_t askBytes) {
  const uint32_t gaps[] = {30, 60, 400, 45, 3600, 60, 120, 30, 90000, 60};
  std::vector<txb::TurnScript> turns;
  for (size_t i = 0; i < n; ++i) {
    const std::string k = std::to_string(i), ask = "turn " + k + " " + std::string(askBytes, 'q');
    if (i % 3 == 0) turns.push_back(txb::withTool(gaps[i % 10], ask, "big_read", "{\"tag\":\"t" + k + "\",\"bytes\":12000}", "read " + k));
    else if (i % 3 == 1) turns.push_back(txb::withTool(gaps[i % 10], ask, "web_search", "{\"q\":\"w" + k + "\"}", "found " + k));
    else turns.push_back(txb::chat(gaps[i % 10], ask, "reply " + k));
  }
  return turns;
}

// 30 turns over days with real gaps: the last request still carries every stored item.
void test_the_last_request_covers_a_long_conversation() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  TXB_OK(txb::runTurns(b, longChat(30, 200)));
  expectLastRequestCovers(b);
}

// On a direct head the scripted suite never reaches ladder rung 3 or the size guard.
void test_a_direct_head_never_omits() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  TXB_OK(txb::runTurns(b, longChat(40, 600)));
  for (const auto& r : b.model.requests())
    TEST_ASSERT_TRUE_MESSAGE(txb::detail::omittedTurnsNamed(r.view) < 0, "an omission line on a direct head");
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd) TEST_ASSERT_TRUE_MESSAGE(it.end.cause != "limit:size", "the size guard fired");
  expectLastRequestCovers(b);
}

// An 85,000 B head (chat completions, unqualified): when the ladder omits, the request
// names exactly what it left out and keeps the newest 6 owner turns.
void test_a_floor_head_names_what_the_ladder_omits() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-E1a");
  txb::Battery b(plain::Dialect::ChatCompletions);
  TXB_OK(txb::runTurns(b, longChat(40, 4000)));
  bool omitted = false;
  for (const auto& r : b.model.requests()) omitted |= txb::detail::omittedTurnsNamed(r.view) > 0;
  TEST_ASSERT_TRUE_MESSAGE(omitted, "160 KB of owner text on an 85,000 B head and the ladder never omitted");
  expectLastRequestCovers(b);
}

// ---- compaction ------------------------------------------------------------------------
const char* kDigest =
    "Day digest: the owner asked about the boat, read files and searched the web; nothing is pending. "
    "He prefers short answers. Facts: Nereid II, 9.5 m.";

void answerSummariesWith(txb::Battery& b, std::function<sp::Step()> step) {
  b.model.respondWith([step](const sp::Recorded&, size_t) { return step(); });
}

std::vector<txb::TurnScript> threeDays() {
  std::vector<txb::TurnScript> turns;
  for (int day = 0; day < 3; ++day)
    for (int i = 0; i < 10; ++i) {
      const std::string k = std::to_string(day) + "." + std::to_string(i);
      turns.push_back(i == 0 ? txb::withTool(86400, "day " + k + std::string(3000, 'd'), "read_file",
                                             R"({"path":"notes/boat.md"})", "boat " + k)
                             : txb::chat(60, "ask " + k + std::string(3000, 'a'), "reply " + k));
    }
  return turns;
}

// After every compaction the next request is digests through thru plus every item after it.
void test_compaction_covers_every_row() {
  TXB_WAITS_FOR("PENDING RB-C2", "RB-E1a");
  txb::Battery b;
  b.world.files["notes/boat.md"] = "Nereid II";
  TXB_OK(txb::runTurns(b, threeDays()));
  answerSummariesWith(b, [] { return sp::say(kDigest); });
  plain::IdleOutcome out;
  TXB_OK(b.idle(out));
  TEST_ASSERT_TRUE_MESSAGE(out.worked, "90 KB resident above H=60,000 B and no compaction ran");
  b.model.respondWith(nullptr);
  TXB_OK(txb::runTurns(b, {txb::chat(60, "what did we talk about?", "the boat")}));
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  size_t digests = 0;
  for (const tx::Item& it : items) digests += it.kind == tx::ItemKind::Digest;
  TEST_ASSERT_TRUE_MESSAGE(digests > 0, "the compaction stored no digest");
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::itemsPaired(items, why), why.c_str());
  expectLastRequestCovers(b);
}

// A summarizer timeout, a thin answer, a 4xx, or a cut between the digest append and the
// head rename: whatever state is left, the next request covers every row.
void test_a_failed_compaction_leaves_every_row_covered() {
  TXB_WAITS_FOR("PENDING RB-C2", "RB-E1a");
  for (int mode = 0; mode < 4; ++mode) {
    txb::Battery b;
    b.world.files["notes/boat.md"] = "Nereid II";
    TXB_OK(txb::runTurns(b, threeDays()));
    if (mode == 0) answerSummariesWith(b, [] { return sp::transportCut(45000); });
    if (mode == 1) answerSummariesWith(b, [] { return sp::say("Talked."); });
    if (mode == 2) answerSummariesWith(b, [] { return sp::httpError(400); });
    if (mode == 3) {
      answerSummariesWith(b, [] { return sp::say(kDigest); });
      b.files.killAtRename = (long)b.files.renames;
    }
    plain::IdleOutcome out;
    const tx::Status st = b.idle(out);
    if (st.notImplemented()) TXB_OK(st);
    if (b.killed) TXB_OK(b.restartAfterCut());
    b.model.respondWith(nullptr);
    TXB_OK(txb::runTurns(b, {txb::chat(60, "still there?", "yes")}));
    expectLastRequestCovers(b);
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_passes_the_reference_renderer_at_every_pointer);
  RUN_TEST(test_checker_fails_every_wrong_renderer);
  RUN_TEST(test_checker_requires_every_digest_in_the_system_text);
  RUN_TEST(test_wire_coverage_anthropic);
  RUN_TEST(test_wire_coverage_responses);
  RUN_TEST(test_wire_coverage_chat);
  RUN_TEST(test_wire_coverage_flat);
  RUN_TEST(test_the_last_request_covers_a_long_conversation);
  RUN_TEST(test_a_direct_head_never_omits);
  RUN_TEST(test_a_floor_head_names_what_the_ladder_omits);
  RUN_TEST(test_compaction_covers_every_row);
  RUN_TEST(test_a_failed_compaction_leaves_every_row_covered);
  return UNITY_END();
}
