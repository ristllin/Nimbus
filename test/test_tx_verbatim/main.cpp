// test_tx_verbatim - the rule that makes engine 2 replay its history as a simple loop does (lane
// E2-VERB; owner rulings 81, 82 and 106). Layer L1, engine level through the battery driver.
//
//   1. Under the due mark a request carries every earlier call and result verbatim: the full
//      arguments the model sent and the full result it got, byte for byte, in every later turn,
//      after a cold gap and across a restart. No turn open moves the stub pointer.
//   2. Over the mark compaction runs first; a stub appears only after it (the overflow path), and
//      what is stubbed is the oldest: stubbed results are older than every whole one, and a stubbed
//      argument is older than every whole result (results first, then arguments).
//   3. The size the due mark measures (plain::residentBytes) is the size the request renders: it
//      counts the verbatim bytes, so the mark and compaction fire when the request is large.
//
// The first engine-2 comparison against the simple loop (lane E2-HOST, evidence/refloop-e2) lost
// long-tool-result and long-chat to the default stubbing this suite forbids.
#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/harness/plain/compact.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

// A 12,000 B write_file content (the write cap on a 120 s host) with a marker at its start.
std::string longText() {
  std::string s = "LONG-ARG journal entry.";
  for (int i = 0; s.size() < 12000; ++i) s += " Line " + std::to_string(i) + " of the long entry, written once.";
  s.resize(12000);
  return s;
}
std::string writeArgs(const std::string& path, const std::string& content) {
  JsonDocument d;
  d["path"] = path;
  d["content"] = content;
  std::string out;
  serializeJson(d, out);
  return out;
}
std::string argOf(const std::string& argsJson, const char* key) {
  JsonDocument d;
  if (deserializeJson(d, argsJson)) return std::string();
  return d[key].is<const char*>() ? std::string(d[key].as<const char*>()) : std::string();
}

const sp::Block* findBlock(const sp::RequestView& v, sp::BlockKind k, const std::string& name) {
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks)
      if (b.kind == k && (k != sp::BlockKind::ToolCall || b.name == name)) return &b;
  return nullptr;
}

// Every request from `from` on carries the read's 20,000 B result and the write's 12,000 B content.
void expectVerbatim(const txb::Battery& b, size_t from, const std::string& result, const std::string& content,
                    const char* where) {
  const auto& reqs = b.model.requests();
  TEST_ASSERT_TRUE_MESSAGE(reqs.size() > from, where);
  for (size_t i = from; i < reqs.size(); ++i) {
    const sp::RequestView& v = reqs[i].view;
    char msg[160];
    bool sawResult = false;
    for (const sp::Message& m : v.messages)
      for (const sp::Block& blk : m.blocks) {
        if (blk.kind == sp::BlockKind::ToolResult && blk.text.size() > 1000) sawResult |= blk.text == result;
        if (blk.kind == sp::BlockKind::ToolResult)
          TEST_ASSERT_TRUE_MESSAGE(blk.text.rfind("[stub ", 0) != 0, "a result rendered as its stub under the due mark");
      }
    std::snprintf(msg, sizeof msg, "%s: request %zu does not carry the 20,000 B result byte for byte", where, i);
    TEST_ASSERT_TRUE_MESSAGE(sawResult, msg);
    const sp::Block* call = findBlock(v, sp::BlockKind::ToolCall, "write_file");
    std::snprintf(msg, sizeof msg, "%s: request %zu does not carry the 12,000 B argument byte for byte", where, i);
    TEST_ASSERT_NOT_NULL_MESSAGE(call, msg);
    TEST_ASSERT_TRUE_MESSAGE(argOf(call->args, "content") == content, msg);
  }
}

// The conversation part of a recorded Anthropic request: its messages array as sent.
size_t messagesBytes(const sp::Recorded& r) {
  JsonDocument d;
  if (deserializeJson(d, r.body, DeserializationOption::NestingLimit(64))) return 0;
  std::string m;
  serializeJson(d["messages"], m);
  return m.size();
}

}  // namespace

// Rule 1: a 20 KB result and a 12 KB argument, byte for byte, in every later request: a warm turn,
// a turn after a cold gap (over 270 s, which moved the pointer before E2-VERB), and after a restart.
void test_under_the_due_mark_every_earlier_call_and_result_is_replayed_verbatim() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  const std::string result = txb::payload("BIG-RESULT", 20000), content = longText();
  b.world.files["docs/handbook.txt"] = result;
  txb::TurnScript t1{30, "Read the handbook and save my long entry", {}};
  t1.steps = {sp::call("read_file", R"({"path":"docs/handbook.txt"})"),
              sp::call("write_file", writeArgs("journal/day-1", content)), sp::say("Read it and saved your entry.")};
  TXB_OK(txb::runTurns(b, {t1}));
  const size_t after1 = b.model.requests().size();
  TXB_OK(txb::runTurns(b, {txb::chat(30, "What did section 3 say?", "It said what you read."),
                           txb::chat(900, "And after a coffee?", "Still the same.")}));
  expectVerbatim(b, 2, result, content, "warm and cold turns");
  TXB_OK(b.powerCycle());
  TXB_OK(txb::runTurns(b, {txb::chat(30, "After the restart?", "Still the same.")}));
  expectVerbatim(b, after1, result, content, "after a restart");
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  for (const tx::Item& it : items)
    TEST_ASSERT_FALSE_MESSAGE(it.kind == tx::ItemKind::PointerMove && (it.why == "cold" || it.why == "bulk"),
                              "a turn open moved the stub pointer by default");
}

// Rule 3: the size the due mark measures counts the verbatim bytes, and equals what the request
// renders of the conversation (within the framing allowance, as test_plain_compact holds it).
void test_the_measured_size_is_the_rendered_size() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  const std::string result = txb::payload("BIG-RESULT", 20000), content = longText();
  b.world.files["docs/handbook.txt"] = result;
  txb::TurnScript t1{30, "Read the handbook and save my long entry", {}};
  t1.steps = {sp::call("read_file", R"({"path":"docs/handbook.txt"})"),
              sp::call("write_file", writeArgs("journal/day-1", content)), sp::say("Done.")};
  TXB_OK(txb::runTurns(b, {t1, txb::chat(600, "Anything else?", "No.")}));
  const size_t measured = plain::residentBytes(*b.arena, b.engine->head()->thru);
  const size_t rendered = messagesBytes(b.model.requests().back());
  char msg[160];
  std::snprintf(msg, sizeof msg, "the due mark measures %zu B, the request renders %zu B of conversation", measured, rendered);
  TEST_ASSERT_TRUE_MESSAGE(measured >= result.size() + content.size(), msg);
  TEST_ASSERT_TRUE_MESSAGE(measured * 10 <= rendered * 11 && rendered * 10 <= measured * 11, msg);
}

namespace {

// The fourteen tool-heavy turns rule 2 runs: a 20,000 B read and a 3,000 B write each.
std::vector<txb::TurnScript> heavyTurns() {
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 14; ++i) {
    const std::string tag = "R" + std::to_string(i);
    std::string body = "entry " + tag + ":";
    while (body.size() < 3000) body += " the long text of entry " + tag + ".";
    txb::TurnScript t{30, "Read part " + tag + " and save entry " + tag, {}};
    t.steps = {sp::call("big_read", R"({"tag":")" + tag + R"(","bytes":20000})"),
               sp::call("write_file", writeArgs("journal/" + tag, body)), sp::say("Saved " + tag + ".")};
    turns.push_back(t);
  }
  return turns;
}

// The first turn that compacted: a digest, the tool fold's x item, or the in-turn event (0: none).
uint32_t firstCompaction(const txb::Battery& b, const std::vector<tx::Item>& items) {
  uint32_t at = 0;
  auto take = [&at](uint32_t turn) { at = !at || turn < at ? turn : at; };
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Digest || (it.kind == tx::ItemKind::PointerMove && it.why == "tools")) take(it.turn ? it.turn : 1);
  for (const auto& kv : b.events(items))
    for (const std::string& e : kv.second)
      if (e == "change:compaction") take(kv.first);
  return at;
}

// One request's bulk results and long write arguments, oldest first: ('r' | 'c', stubbed).
std::vector<std::pair<char, bool>> heavyItems(const sp::RequestView& v) {
  std::vector<std::pair<char, bool>> seq;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& blk : m.blocks) {
      const bool stub = blk.text.rfind("[stub ", 0) == 0;
      if (blk.kind == sp::BlockKind::ToolResult && (blk.text.size() > 2048 || stub)) seq.push_back({'r', stub});
      if (blk.kind == sp::BlockKind::ToolCall && blk.name == "write_file")
        seq.push_back({'c', argOf(blk.args, "content").rfind("[stub ", 0) == 0});
    }
  return seq;
}

// Stubs are the oldest: no stubbed result after a whole one, no stubbed argument after a whole
// argument or result. True when the request holds a stub at all.
bool expectOldestFirst(const std::vector<std::pair<char, bool>>& seq, size_t i) {
  bool wholeResult = false, wholeCall = false, any = false;
  char msg[96];
  for (const auto& s : seq) {
    any |= s.second;
    std::snprintf(msg, sizeof msg, "request %zu: a stub newer than a whole %s", i, s.first == 'r' ? "result" : "argument");
    if (s.first == 'r' && s.second) TEST_ASSERT_FALSE_MESSAGE(wholeResult, msg);
    if (s.first == 'c' && s.second) TEST_ASSERT_FALSE_MESSAGE(wholeCall || wholeResult, msg);
    if (!s.second) (s.first == 'r' ? wholeResult : wholeCall) = true;
  }
  return any;
}

}  // namespace

// Rule 2: over the mark compaction comes first, and stubs only after it, oldest first.
void test_over_the_mark_compaction_runs_first_and_stubs_are_the_oldest() {
  TXB_WAITS_FOR("PENDING RB-E1a");
  txb::Battery b;
  TXB_OK(txb::runTurns(b, heavyTurns()));
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  const uint32_t compactedAt = firstCompaction(b, items);
  TEST_ASSERT_TRUE_MESSAGE(compactedAt > 0, "fourteen 23 KB turns never reached the due mark");
  const auto& reqs = b.model.requests();
  for (size_t i = 0; i < reqs.size(); ++i) {
    const uint32_t turn = i < b.requestTurn.size() ? b.requestTurn[i] : 0;
    char msg[128];
    std::snprintf(msg, sizeof msg, "request %zu (turn %u) holds a stub before the first compaction (turn %u)", i, turn,
                  compactedAt);
    if (expectOldestFirst(heavyItems(reqs[i].view), i)) TEST_ASSERT_TRUE_MESSAGE(turn >= compactedAt, msg);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_under_the_due_mark_every_earlier_call_and_result_is_replayed_verbatim);
  RUN_TEST(test_the_measured_size_is_the_rendered_size);
  RUN_TEST(test_over_the_mark_compaction_runs_first_and_stubs_are_the_oldest);
  return UNITY_END();
}
