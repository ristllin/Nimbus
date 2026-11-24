// test_tx_prefix - byte-stable request prefixes (PRD section 2, "Prefix property"; layer
// L1; lane RB-B0b writes it). Request N+1 extends request N - system, tools and every
// earlier message unchanged - inside a turn and across warm turns. The only allowed breaks
// are the named change points (pointer move, outside stubbing, superseded read, photo
// leaving view, dropped thinking, compaction, notes edit, connector change, recent files at
// a cold open, the ladder), each logged in the e item as "change:<slug>" and counted.
//
// Wire level (each renderer, RB-W1a, RB-W2, RB-W3, RB-W4): render is a pure function of
// stored state, it extends round by round, a restart renders the same bytes, and the one
// outside change point breaks only the outside result. Engine level (RB-E1a): real turns
// with real gaps through the scripted provider. The checker is counter-tested first.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <cstdint>
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

sp::Message msg(const char* role, const std::string& text) {
  sp::Message m;
  m.role = role;
  m.blocks.push_back(sp::detail::textBlock(text));
  return m;
}

sp::RequestView view(std::vector<sp::Message> ms, const char* system = "core") {
  sp::RequestView v;
  v.parsed = true;
  v.systemBlocks = {system};
  v.tools = {"read_file"};
  v.toolsJson = {R"({"name":"read_file"})"};
  v.messages = std::move(ms);
  return v;
}

// ---- counter-tests: the checker bites -------------------------------------------------
void test_checker_accepts_extension_and_rejects_a_silent_rewrite() {
  const sp::RequestView r1 = view({msg("user", "hi")});
  const sp::RequestView r2 = view({msg("user", "hi"), msg("assistant", "hello"), msg("user", "and?")});
  const sp::RequestView rw = view({msg("user", "hi (edited)"), msg("assistant", "hello"), msg("user", "and?")});
  txb::PrefixReport rep;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::prefixHolds({{&r1, 1}, {&r2, 2}}, {}, rep, why), why.c_str());
  TEST_ASSERT_EQUAL(0, (int)rep.breaks);
  why.clear();
  TEST_ASSERT_FALSE(txb::prefixHolds({{&r1, 1}, {&rw, 2}}, {}, rep, why));
  TEST_ASSERT_TRUE(why.find("no unspent change point") != std::string::npos);
  // A system edit, a dropped earlier message and a tools change are rewrites too.
  const sp::RequestView sys = view({msg("user", "hi"), msg("user", "and?")}, "core v2");
  const sp::RequestView dropped = view({msg("user", "and?")});
  sp::RequestView tools = r2;
  tools.tools = {"read_file", "web_search"};
  for (const sp::RequestView* later : {&sys, &dropped, (const sp::RequestView*)&tools}) {
    why.clear();
    TEST_ASSERT_FALSE(txb::prefixHolds({{&r1, 1}, {later, 2}}, {}, rep, why));
  }
}

void test_checker_counts_named_change_points() {
  const sp::RequestView r1 = view({msg("user", "hi"), msg("user", "[stub web_search r2 ...]")});
  const sp::RequestView r2 = view({msg("user", "hi"), msg("user", "[stub web_search(q) r2 outside content]")});
  txb::PrefixReport rep;
  std::string why;
  // Across a turn boundary a logged change point allows the break.
  TEST_ASSERT_TRUE_MESSAGE(txb::prefixHolds({{&r1, 1}, {&r2, 2}}, {{2, {"change:outside"}}}, rep, why), why.c_str());
  TEST_ASSERT_EQUAL(1, (int)rep.breaks);
  TEST_ASSERT_EQUAL(1, (int)rep.changePoints);
  // Inside one turn only the ladder may break the prefix.
  why.clear();
  TEST_ASSERT_FALSE(txb::prefixHolds({{&r1, 3}, {&r2, 3}}, {{3, {"change:outside"}}}, rep, why));
  why.clear();
  TEST_ASSERT_TRUE(txb::prefixHolds({{&r1, 3}, {&r2, 3}}, {{3, {"change:ladder"}}}, rep, why));
  // A slug outside the closed list is refused, and breaks never outnumber the points.
  why.clear();
  TEST_ASSERT_FALSE(txb::prefixHolds({{&r1, 1}, {&r2, 2}}, {{2, {"change:because"}}}, rep, why));
  // One logged point excuses one break: two boundary breaks around turn 2 need two points.
  const sp::RequestView r3 = view({msg("user", "hi!"), msg("user", "x")});
  why.clear();
  TEST_ASSERT_FALSE(txb::prefixHolds({{&r1, 1}, {&r2, 2}, {&r3, 3}}, {{2, {"change:pointer"}}}, rep, why));
  TEST_ASSERT_TRUE(why.find("no unspent change point") != std::string::npos);
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(
      txb::prefixHolds({{&r1, 1}, {&r2, 2}, {&r3, 3}}, {{2, {"change:pointer", "change:outside"}}}, rep, why),
      why.c_str());
  TEST_ASSERT_EQUAL(2, (int)rep.breaks);
}

// ---- wire level ---------------------------------------------------------------------
void expectSameMessagesExceptResult(const sp::RequestView& a, const sp::RequestView& b, const std::string& id) {
  TEST_ASSERT_TRUE(a.systemBlocks == b.systemBlocks && a.toolsJson == b.toolsJson);
  TEST_ASSERT_TRUE(a.messages.size() <= b.messages.size());
  for (size_t i = 0; i < a.messages.size(); ++i) {
    if (a.messages[i] == b.messages[i]) continue;
    for (size_t k = 0; k < a.messages[i].blocks.size(); ++k) {
      const sp::Block& x = a.messages[i].blocks[k];
      const bool sameOrOutside = (k < b.messages[i].blocks.size() && x == b.messages[i].blocks[k]) ||
                                 (x.kind == sp::BlockKind::ToolResult && x.id == id);
      TEST_ASSERT_TRUE_MESSAGE(sameOrOutside, "the outside change point changed more than the outside result");
    }
  }
}

void wirePrefix(plain::Dialect d) {
  txb::RenderFixture fx(d);
  const std::vector<txb::Snapshot> snaps = txb::dayOfFourTurns(d);
  std::vector<txb::Rendered> r(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i) {
    TXB_OK(txb::renderItems(fx, snaps[i].c, 0, r[i], snaps[i].openTurn));
    TEST_ASSERT_TRUE_MESSAGE(r[i].view.parsed, (snaps[i].name + ": " + r[i].view.parseError).c_str());
    // Pure: the same stored state renders the same bytes.
    txb::Rendered again;
    TXB_OK(txb::renderItems(fx, snaps[i].c, 0, again, snaps[i].openTurn));
    TEST_ASSERT_TRUE_MESSAGE(again.body == r[i].body, (snaps[i].name + ": render is not pure").c_str());
  }
  // Round by round and across warm turns the request only grows (the last snapshot is the
  // outside change point, checked apart).
  for (size_t i = 1; i + 1 < snaps.size(); ++i) {
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(sp::isPrefix(r[i - 1].view, r[i].view, why), (snaps[i].name + ": " + why).c_str());
  }
  const txb::Snapshot& last = snaps.back();
  std::string outsideId;
  for (const tx::Item& it : last.c.items)
    if (it.kind == tx::ItemKind::Result && it.cls == tx::ResultClass::Outside) outsideId = it.id;
  expectSameMessagesExceptResult(r[r.size() - 2].view, r.back().view, outsideId);
}

// A restart reads every item back from its stored line: the same request, byte for byte.
void wireRestart(plain::Dialect d) {
  txb::RenderFixture fx(d);
  const txb::Snapshot s = txb::dayOfFourTurns(d)[5];
  txb::Convo back = s.c;
  for (tx::Item& it : back.items) {
    std::string line;
    TXB_OK(tx::encodeLine(it, line));
    tx::Item decoded;
    TXB_OK(tx::decodeLine(line.data(), line.size(), decoded));
    it = decoded;
  }
  txb::Rendered a, b;
  TXB_OK(txb::renderItems(fx, s.c, 0, a, s.openTurn));
  TXB_OK(txb::renderItems(fx, back, 0, b, s.openTurn));
  TEST_ASSERT_TRUE_MESSAGE(a.body == b.body, "a restart renders different bytes");
}

void test_wire_prefix_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wirePrefix(plain::Dialect::AnthropicMessages);
  wireRestart(plain::Dialect::AnthropicMessages);
}
void test_wire_prefix_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wirePrefix(plain::Dialect::OpenAIResponses);
  wireRestart(plain::Dialect::OpenAIResponses);
}
void test_wire_prefix_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wirePrefix(plain::Dialect::ChatCompletions);
  wireRestart(plain::Dialect::ChatCompletions);
}
void test_wire_prefix_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1", "RB-R4b");   // text-mode history (E2-R6)
  wirePrefix(plain::Dialect::Flat);
  wireRestart(plain::Dialect::Flat);
}

// ---- engine level -------------------------------------------------------------------
void expectPrefix(txb::Battery& b, size_t wantBreaksAtMost) {
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  txb::PrefixReport rep;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::prefixHolds(b.sentRequests(), b.events(items), rep, why), why.c_str());
  char msg[96];
  std::snprintf(msg, sizeof msg, "%zu prefix breaks over %zu transitions", rep.breaks, rep.transitions);
  TEST_ASSERT_TRUE_MESSAGE(rep.breaks <= wantBreaksAtMost, msg);
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

// Rounds inside one turn only ever append.
void test_rounds_inside_a_turn_extend() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.world.files["notes/b.md"] = "beta";
  txb::TurnScript t{30, "Compare a and b, and check today's ledger", {}};
  t.steps = {sp::call("read_file", R"({"path":"notes/a.md"})"), sp::call("read_file", R"({"path":"notes/b.md"})"),
             sp::call("ledger_day", R"({"day":"2026-11-17"})"), sp::say("a says alpha, b says beta; ledger empty.")};
  TXB_OK(txb::runTurns(b, {t}));
  TEST_ASSERT_EQUAL(4, (int)b.model.requests().size());
  expectPrefix(b, 0);
}

// Warm turns (60 s apart, under the 270 s cold gap) extend across the turn boundary.
void test_warm_turns_extend() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  b.world.files["notes/boat.md"] = "Nereid II";
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 6; ++i)
    turns.push_back(i % 2 ? txb::chat(60, "and " + std::to_string(i) + "?", "Answer " + std::to_string(i))
                          : txb::withTool(60, "boat " + std::to_string(i) + "?", "read_file",
                                          R"({"path":"notes/boat.md"})", "Nereid II, " + std::to_string(i)));
  TXB_OK(txb::runTurns(b, turns));
  expectPrefix(b, 0);
}

// A cold open (over 270 s) may break the prefix once, and only as a named change point.
void test_a_cold_open_breaks_only_at_a_named_point() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  // 20,000 B of bulk stays under the warm threshold (24,576 B): only the cold rule moves.
  TXB_OK(txb::runTurns(b, {txb::withTool(30, "big read", "big_read", R"({"tag":"a","bytes":20000})", "done"),
                           txb::chat(60, "next", "ok"), txb::chat(600, "after a pause", "still here"),
                           txb::chat(60, "and now", "yes")}));
  expectPrefix(b, 1);
}

// Outside content is whole inside its own turn and a stub from the next one; that break is
// named, and the outside bytes are gone from every later request.
void test_outside_content_leaves_at_a_named_point() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  TXB_OK(txb::runTurns(b, {txb::withTool(30, "train times?", "web_search", R"({"q":"trains"})", "17:05"),
                           txb::chat(60, "thanks", "welcome"), txb::chat(60, "one more", "sure")}));
  expectPrefix(b, 1);
  const auto& reqs = b.model.requests();
  for (size_t i = 0; i < reqs.size(); ++i)
    if (b.requestTurn[i] > 1) TEST_ASSERT_TRUE_MESSAGE(reqs[i].body.find("OUTSIDE-trains") == std::string::npos,
                                                       "outside bytes reached a later turn's request");
}

// A write to a path supersedes the earlier whole read of it: a named change point is logged, and
// (E2-VERB, rulings 81, 82 and 106) the read stays whole, as a simple loop replays it, so the prefix
// does not break at all (before, the read left the request there).
void test_a_superseded_read_leaves_at_a_named_point() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  // The tail sits past the stub's 200 B preview: only a whole read shows it.
  b.world.files["notes/plan.md"] = "PLAN-V1 " + std::string(1500, 'p') + " PLAN-V1-TAIL";
  TXB_OK(txb::runTurns(b, {txb::withTool(30, "read my plan", "read_file", R"({"path":"notes/plan.md"})", "read"),
                           txb::withTool(60, "replace it", "write_file", R"({"path":"notes/plan.md","text":"V2"})", "done"),
                           txb::chat(60, "what now?", "V2")}));
  expectPrefix(b, 0);
  TEST_ASSERT_TRUE_MESSAGE(b.model.last().body.find("PLAN-V1-TAIL") != std::string::npos,
                           "the superseded read became a stub by default");
}

// A whole scripted day with real gaps: every break is a named change point.
void test_a_mixed_day_breaks_only_at_named_points() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  b.world.files["notes/boat.md"] = "Nereid II";
  const uint32_t gaps[] = {30, 60, 45, 900, 30, 120, 3600, 60, 250, 280, 30, 60, 7200, 30, 60, 90, 20, 60, 400, 60};
  std::vector<txb::TurnScript> turns;
  for (size_t i = 0; i < 20; ++i) {
    const std::string n = std::to_string(i);
    switch (i % 4) {
      case 0: turns.push_back(txb::withTool(gaps[i], "boat " + n, "read_file", R"({"path":"notes/boat.md"})", "Nereid " + n)); break;
      case 1: turns.push_back(txb::withTool(gaps[i], "search " + n, "web_search", "{\"q\":\"s" + n + "\"}", "found " + n)); break;
      case 2: turns.push_back(txb::withTool(gaps[i], "bulk " + n, "big_read", "{\"tag\":\"b" + n + "\",\"bytes\":26000}", "read " + n)); break;
      default: turns.push_back(txb::chat(gaps[i], "chat " + n, "reply " + n)); break;
    }
  }
  TXB_OK(txb::runTurns(b, turns));
  expectPrefix(b, SIZE_MAX);   // every break spends one logged change point
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_accepts_extension_and_rejects_a_silent_rewrite);
  RUN_TEST(test_checker_counts_named_change_points);
  RUN_TEST(test_wire_prefix_anthropic);
  RUN_TEST(test_wire_prefix_responses);
  RUN_TEST(test_wire_prefix_chat);
  RUN_TEST(test_wire_prefix_flat);
  RUN_TEST(test_rounds_inside_a_turn_extend);
  RUN_TEST(test_warm_turns_extend);
  RUN_TEST(test_a_cold_open_breaks_only_at_a_named_point);
  RUN_TEST(test_outside_content_leaves_at_a_named_point);
  RUN_TEST(test_a_superseded_read_leaves_at_a_named_point);
  RUN_TEST(test_a_mixed_day_breaks_only_at_named_points);
  return UNITY_END();
}
