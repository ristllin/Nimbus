// test_tx_pairing - tool calls kept with their results (PRD sections 2, 3 and 7; layer L1;
// lane RB-B0b writes it). In every request every call has exactly one result right after
// its round and no result lacks its call; in the stored transcript every call id is unique
// with exactly one result after it. This holds across stubbing at every pointer value
// (RB-W1a, RB-W2, RB-W3, RB-W4), parallel calls and cut answers (RB-E1a), suppressed
// repeats (RB-E1b), compaction (a call and its result move together, RB-C2) and a power
// cut at every append (RB-X1). The checkers are counter-tested first.
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

txb::Convo pairsDay() {
  txb::Convo c;
  for (int i = 0; i < 4; ++i) {
    c.openTurn();
    c.owner("q" + std::to_string(i));
    const std::string a = c.call("read_file", "{}"), b = c.call("ledger_day", "{}");
    c.receipt(a, "read_file", "A" + std::to_string(i));
    c.bulkResult(b, "ledger_day", txb::payload("L" + std::to_string(i), 6000));
    c.nextRound();
    c.say("done " + std::to_string(i));
    c.close();
  }
  return c;
}

// ---- counter-tests ---------------------------------------------------------------------
void test_checker_rejects_orphans_and_doubles_in_a_request() {
  const txb::Convo c = pairsDay();
  txb::CoverInput in;
  in.items = &c.items;
  in.bulk = &c.bulk;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(txb::toyRender(in), why), why.c_str());
  for (txb::Mutation m : {txb::Mutation::DropResult, txb::Mutation::DropCall, txb::Mutation::DuplicateResult}) {
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(txb::viewPaired(txb::toyRender(in, m), why), "a wrong pairing passed");
  }
}

void test_checker_rejects_orphans_and_doubles_in_the_transcript() {
  txb::Convo c = pairsDay();
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::itemsPaired(c.items, why), why.c_str());
  // A call with no result.
  txb::Convo open = c;
  open.call("write_file", "{}", true);
  why.clear();
  TEST_ASSERT_FALSE(txb::itemsPaired(open.items, why));
  // A result answering nothing.
  txb::Convo orphan = c;
  orphan.receipt("toolu_9999", "read_file", "ghost");
  why.clear();
  TEST_ASSERT_FALSE(txb::itemsPaired(orphan.items, why));
  // Two results for one call.
  txb::Convo twice = c;
  twice.receipt(c.items[3].id, "read_file", "again");
  why.clear();
  TEST_ASSERT_FALSE(txb::itemsPaired(twice.items, why));
  // A call id stored twice, and an intent naming no call.
  txb::Convo dup = c;
  dup.items.push_back(c.items[2]);
  dup.items.back().q = ++dup.q;
  why.clear();
  TEST_ASSERT_FALSE(txb::itemsPaired(dup.items, why));
  txb::Convo stray = c;
  stray.intent("toolu_7777", "ledger_write");
  why.clear();
  TEST_ASSERT_FALSE(txb::itemsPaired(stray.items, why));
}

// ---- wire level ------------------------------------------------------------------------
void wirePairing(plain::Dialect d) {
  txb::RenderFixture fx(d);
  for (const txb::Snapshot& s : txb::dayOfFourTurns(d))
    for (uint64_t p = 0; p <= s.c.q; ++p) {
      txb::Rendered r;
      TXB_OK(txb::renderItems(fx, s.c, p, r, s.openTurn));
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(r.view, why), (s.name + " pointer " + std::to_string(p) + ": " + why).c_str());
    }
}

void test_wire_pairing_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1a", "RB-T1");
  wirePairing(plain::Dialect::AnthropicMessages);
}
void test_wire_pairing_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-T1");
  wirePairing(plain::Dialect::OpenAIResponses);
}
void test_wire_pairing_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-T1");
  wirePairing(plain::Dialect::ChatCompletions);
}
void test_wire_pairing_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-T1");
  wirePairing(plain::Dialect::Flat);
}

// ---- engine level ----------------------------------------------------------------------
void expectPaired(txb::Battery& b) {
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.allPaired(why), why.c_str());
  for (const auto& r : b.model.requests()) {
    why.clear();
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(r.view, why), why.c_str());
  }
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::itemsPaired(items, why), why.c_str());
}

// Parallel calls in one round, then sequential rounds: one result each, stored and sent.
void test_parallel_and_sequential_calls_have_one_result_each() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  txb::TurnScript t{30, "check everything", {}};
  t.steps = {sp::calls({{"read_file", R"({"path":"notes/a.md"})", ""},
                        {"ledger_day", R"({"day":"2026-11-17"})", ""},
                        {"web_search", R"({"q":"x"})", ""}}),
             sp::call("read_file", R"({"path":"notes/missing.md"})"), sp::say("All checked.")};
  TXB_OK(txb::runTurns(b, {t, txb::withTool(60, "again", "ledger_day", R"({"day":"2026-11-16"})", "ok")}));
  TEST_ASSERT_EQUAL(5, (int)b.model.requests().size());
  expectPaired(b);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

// A cut answer's tool call is never dispatched and never left dangling in a request.
void test_a_cut_call_is_never_dispatched_nor_left_dangling() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  sp::Step cut = sp::call("write_file", R"({"path":"notes/x.md","text":"half of it)");
  cut.finish = sp::Finish::MaxTokens;
  txb::TurnScript t{30, "write my note", {cut, sp::call("write_file", R"({"path":"notes/x.md","text":"whole"})"),
                                          sp::say("Saved.")}};
  TXB_OK(txb::runTurns(b, {t}));
  for (const auto& kv : b.world.effectRuns)
    TEST_ASSERT_TRUE_MESSAGE(kv.first.find("half of it") == std::string::npos, "a cut call was dispatched");
  TEST_ASSERT_EQUAL(1, (int)b.world.effectRuns.size());
  expectPaired(b);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

// An identical pure call answered by suppression is still a call with one result.
void test_a_suppressed_repeat_is_still_paired() {
  TXB_WAITS_FOR("PENDING RB-E1b", "RB-E1a");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  txb::TurnScript t{30, "read it twice", {}};
  t.steps = {sp::call("read_file", R"({"path":"notes/a.md"})"), sp::call("read_file", R"({"path":"notes/a.md"})"),
             sp::say("alpha")};
  TXB_OK(txb::runTurns(b, {t}));
  int reads = 0;
  for (const std::string& n : b.world.dispatched) reads += n == "read_file";
  TEST_ASSERT_EQUAL_MESSAGE(1, reads, "an identical pure call inside one turn ran twice");
  expectPaired(b);
}

// A compaction folds a call and its result together: never one side of a pair.
void test_compaction_moves_a_call_with_its_result() {
  TXB_WAITS_FOR("PENDING RB-C2", "RB-E1a");
  txb::Battery b;
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 24; ++i)
    turns.push_back(txb::withTool(i % 8 == 0 ? 86400 : 60, "turn " + std::to_string(i) + std::string(3000, 'p'),
                                  "ledger_day", R"({"day":"2026-11-17"})", "ok " + std::to_string(i)));
  TXB_OK(txb::runTurns(b, turns));
  b.model.respondWith([](const sp::Recorded&, size_t) {
    return sp::say("Digest: ledger checks every turn, nothing pending, the owner keeps a food log daily.");
  });
  plain::IdleOutcome out;
  TXB_OK(b.idle(out));
  b.model.respondWith(nullptr);
  TXB_OK(txb::runTurns(b, {txb::chat(60, "and now?", "fine")}));
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  uint64_t thru = 0;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Digest) thru = std::max(thru, it.thru);
  TEST_ASSERT_TRUE_MESSAGE(thru > 0, "no compaction ran");
  std::map<std::string, uint64_t> callQ;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Call) callQ[it.id] = it.q;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Result)
      TEST_ASSERT_TRUE_MESSAGE((callQ[it.id] <= thru) == (it.q <= thru), "a compaction split a call from its result");
  expectPaired(b);
}

// A power cut at every append of an effect turn: after the restart every request is
// paired and the transcript holds exactly one result per call (outcome unknown included).
void test_pairs_survive_a_cut_at_every_append() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  auto script = [](txb::Battery& b) {
    b.model.respondWith([](const sp::Recorded& r, size_t) {
      return r.body.find("tool_result") != std::string::npos || r.body.find("function_call_output") != std::string::npos ||
                     r.body.find("\"role\":\"tool\"") != std::string::npos
                 ? sp::say("Logged.")
                 : sp::call("ledger_write", R"({"day":"2026-11-17","add":[{"food":"skyr","g":150}]})");
    });
  };
  txb::Battery dry;
  script(dry);
  TXB_OK(txb::runTurns(dry, {txb::TurnScript{30, "log 150 g skyr", {}}}));
  const size_t appends = dry.files.appends;
  TEST_ASSERT_TRUE(appends >= 5);
  for (size_t k = 0; k < appends; ++k) {
    txb::Battery b;
    script(b);
    b.files.killAtAppend = (long)k;
    const tx::Status st = txb::runTurns(b, {txb::TurnScript{30, "log 150 g skyr", {}}});
    if (st.notImplemented()) TXB_OK(st);
    TEST_ASSERT_TRUE_MESSAGE(b.killed, "the cut never fired");
    TXB_OK(b.restartAfterCut());
    expectPaired(b);
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_orphans_and_doubles_in_a_request);
  RUN_TEST(test_checker_rejects_orphans_and_doubles_in_the_transcript);
  RUN_TEST(test_wire_pairing_anthropic);
  RUN_TEST(test_wire_pairing_responses);
  RUN_TEST(test_wire_pairing_chat);
  RUN_TEST(test_wire_pairing_flat);
  RUN_TEST(test_parallel_and_sequential_calls_have_one_result_each);
  RUN_TEST(test_a_cut_call_is_never_dispatched_nor_left_dangling);
  RUN_TEST(test_a_suppressed_repeat_is_still_paired);
  RUN_TEST(test_compaction_moves_a_call_with_its_result);
  RUN_TEST(test_pairs_survive_a_cut_at_every_append);
  return UNITY_END();
}
