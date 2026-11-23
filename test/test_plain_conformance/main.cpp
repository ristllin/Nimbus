// test_plain_conformance - the plain loop, per dialect (PRD section 1; layer L1; lane
// RB-B0b writes it). Text ends the turn in one request; tool then text is two; text said
// with a call is kept and the loop continues; requests equal rounds with calls plus one;
// parallel calls are one round. Every request parses, offers at most 14 tools with tool
// choice auto, and on a guard trip the device writes one '[device] limit reached (rounds)'
// item, the last round goes out with tool choice none, and its text is delivered.
// Anthropic waits for the loop (RB-E1a; the guard for RB-E1b); Responses for RB-W2, chat
// completions for RB-W3, custom endpoints for RB-W4. The shape checker is counter-tested.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "engine_tests.h"
#include "h81_tests.h"
#include "photo_tests.h"
#include "x3_tests.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

// The allocator hook of engine_tests.h (rulings 56 and 59): while armed, it refuses the Nth
// allocation, or the Nth and every one after it, or every allocation of S bytes or more.
void* operator new(size_t n) {
  if (engine_tests::alloc_hook::refuse(n)) throw std::bad_alloc();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

namespace {

sp::RequestView viewWith(size_t calls, const std::string& assistantText = "", bool withResults = true) {
  sp::RequestView v;
  v.parsed = true;
  v.messages.push_back(sp::Message{"user", {sp::detail::textBlock("hi")}});
  sp::Message a{"assistant", {}};
  if (!assistantText.empty()) a.blocks.push_back(sp::detail::textBlock(assistantText));
  sp::Message u{"user", {}};
  for (size_t i = 0; i < calls; ++i) {
    sp::Block c;
    c.kind = sp::BlockKind::ToolCall;
    c.id = "c" + std::to_string(i);
    a.blocks.push_back(c);
    sp::Block r;
    r.kind = sp::BlockKind::ToolResult;
    r.id = c.id;
    if (withResults) u.blocks.push_back(r);
  }
  if (calls || !assistantText.empty()) v.messages.push_back(a);
  if (!u.blocks.empty()) v.messages.push_back(u);
  return v;
}

// ---- counter-test --------------------------------------------------------------------
void test_checker_rejects_a_wrong_loop() {
  const sp::RequestView r0 = viewWith(0), r1 = viewWith(1, "let me look"), r2 = viewWith(2, "let me look");
  std::string why;
  const std::vector<txb::StepShape> twoSteps = {{1, "let me look"}, {0, "found it"}};
  TEST_ASSERT_TRUE_MESSAGE(txb::loopShapeHolds(twoSteps, {&r0, &r1}, "found it", why), why.c_str());
  // One request too many (an extra round after the text), or one too few.
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0, &r1, &r2}, "found it", why));
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0}, "found it", why));
  // The results of the first round missing from the second request (its call and text kept).
  const sp::RequestView noResults = viewWith(1, "let me look", false);
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0, &noResults}, "found it", why));
  // The text said with the call dropped.
  const sp::RequestView noText = viewWith(1);
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0, &noText}, "found it", why));
  // The delivered text is not the model's last text (appended, rewritten, or the interim text).
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0, &r1}, "found it. Note: ...", why));
  TEST_ASSERT_FALSE(txb::loopShapeHolds(twoSteps, {&r0, &r1}, "let me look", why));
  // A text-only step that did not end the turn.
  TEST_ASSERT_FALSE(txb::loopShapeHolds({{0, "a"}, {0, "b"}}, {&r0, &r0}, "b", why));
}

// ---- the loop ------------------------------------------------------------------------
struct Shape {
  std::vector<sp::Step> steps;
  std::vector<txb::StepShape> shape;
};

std::vector<Shape> shapes() {
  const std::string rf = R"({"path":"notes/a.md"})";
  return {
      {{sp::say("Hello.")}, {{0, "Hello."}}},
      {{sp::call("read_file", rf), sp::say("It says alpha.")}, {{1, ""}, {0, "It says alpha."}}},
      {{sp::call("read_file", rf, "Let me look."), sp::say("Alpha.")}, {{1, "Let me look."}, {0, "Alpha."}}},
      {{sp::call("read_file", rf), sp::call("ledger_day", R"({"day":"2026-11-17"})"),
        sp::call("read_file", R"({"path":"notes/b.md"})"), sp::say("Three rounds.")},
       {{1, ""}, {1, ""}, {1, ""}, {0, "Three rounds."}}},
      {{sp::calls({{"read_file", rf, ""}, {"ledger_day", "{}", ""}}), sp::say("Both.")}, {{2, ""}, {0, "Both."}}},
  };
}

void loopShape(plain::Dialect d) {
  txb::Battery b(d);
  b.world.files["notes/a.md"] = "alpha";
  b.world.files["notes/b.md"] = "beta";
  for (const Shape& s : shapes()) {
    const size_t first = b.model.requests().size();
    for (const sp::Step& st : s.steps) b.model.push(st);
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("go", out));
    std::vector<const sp::RequestView*> reqs;
    for (size_t i = first; i < b.model.requests().size(); ++i) {
      const sp::RequestView& v = b.model.requests()[i].view;
      TEST_ASSERT_TRUE_MESSAGE(v.parsed, v.parseError.c_str());
      TEST_ASSERT_TRUE_MESSAGE(v.tools.size() <= plain::kMaxAlwaysOnTools, "more than 14 tools offered");
      TEST_ASSERT_TRUE_MESSAGE(v.toolChoice == "auto" || v.toolChoice.empty(), v.toolChoice.c_str());
      reqs.push_back(&v);
    }
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::loopShapeHolds(s.shape, reqs, out.text, why), why.c_str());
    TEST_ASSERT_TRUE(out.delivered);
  }
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

void test_loop_shape_anthropic() {
  TXB_WAITS_FOR("PENDING RB-E1a", "RB-W1b");
  loopShape(plain::Dialect::AnthropicMessages);
}
void test_loop_shape_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-E1a");
  loopShape(plain::Dialect::OpenAIResponses);
}
void test_loop_shape_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-E1a");
  loopShape(plain::Dialect::ChatCompletions);
}
void test_loop_shape_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-E1a");
  loopShape(plain::Dialect::Flat);
}

// ---- the guard's last round ------------------------------------------------------------
// A model that never stops calling (each call new, so the turn progresses): the hard round guard
// at 64 (ruling 84 item 2; 16 soft rounds let a progressing owner turn go on), then one device
// item and one last round with tool choice none, whose text is delivered.
void guardLastRound(plain::Dialect d) {
  txb::Battery b(d);
  b.world.files["notes/a.md"] = "alpha";
  for (int i = 0; i < plain::kHardRounds; ++i)
    b.model.push(sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}"));
  b.model.push(sp::say("I stopped: the round limit."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading", out));
  const auto& reqs = b.model.requests();
  TEST_ASSERT_EQUAL(plain::kHardRounds + 1, (int)reqs.size());
  TEST_ASSERT_EQUAL_STRING("none", reqs.back().view.toolChoice.c_str());
  for (size_t i = 0; i + 1 < reqs.size(); ++i) TEST_ASSERT_TRUE(reqs[i].view.toolChoice != "none");
  TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Rounds);
  TEST_ASSERT_EQUAL_STRING("I stopped: the round limit.", out.text.c_str());
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  size_t notes = 0;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::DeviceNote) {
      ++notes;
      TEST_ASSERT_TRUE_MESSAGE(it.x.rfind("[device] limit reached (rounds)", 0) == 0, it.x.c_str());
    }
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)notes, "a guard trip writes exactly one device item");
  // The device note reaches the model as user-role text starting '[device]'.
  bool seen = false;
  for (const sp::Message& m : reqs.back().view.messages)
    for (const sp::Block& blk : m.blocks)
      seen |= m.role == "user" && blk.kind == sp::BlockKind::Text && blk.text.find("[device] limit reached") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(seen, "the last request does not show the device note as user text");
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

void test_guard_last_round_anthropic() {
  TXB_WAITS_FOR("PENDING RB-W1b", "RB-E1a", "RB-E1b");
  guardLastRound(plain::Dialect::AnthropicMessages);
}
void test_guard_last_round_responses() {
  TXB_WAITS_FOR("PENDING RB-W2", "RB-E1a", "RB-E1b");
  guardLastRound(plain::Dialect::OpenAIResponses);
}
void test_guard_last_round_chat() {
  TXB_WAITS_FOR("PENDING RB-W3", "RB-E1a", "RB-E1b");
  guardLastRound(plain::Dialect::ChatCompletions);
}
void test_guard_last_round_flat() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-E1a", "RB-E1b");
  guardLastRound(plain::Dialect::Flat);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_a_wrong_loop);
  RUN_TEST(test_loop_shape_anthropic);
  RUN_TEST(test_loop_shape_responses);
  RUN_TEST(test_loop_shape_chat);
  RUN_TEST(test_loop_shape_flat);
  RUN_TEST(test_guard_last_round_anthropic);
  RUN_TEST(test_guard_last_round_responses);
  RUN_TEST(test_guard_last_round_chat);
  RUN_TEST(test_guard_last_round_flat);
  engine_tests::run();
  h81_tests::run();
  photo_tests::run();
  x3_tests::run();
  return UNITY_END();
}
