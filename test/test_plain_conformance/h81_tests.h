#pragma once
// Lane RB-H81 (rulings 81 to 93): engine 2 carries no less of the conversation than v5.1.2, and
// nothing leaves a request without a named line or a summary. Run from main.cpp beside the loop
// core's own cases, on the battery (txb::Battery).
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/props.h"
#include "engine_tests.h"

namespace h81_tests {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
using engine_tests::lastEnd;
using engine_tests::storedOf;

// Ruling 91 T4-ADV-01: the e item keeps the whole prompt of the turn's LAST answered request
// (input plus cache reads and writes, in each provider's meaning), which compaction's hot trigger
// reads; promptTokens stays the turn's sum. Two requests on the Responses wire report 73,000 tokens
// each (70,000 of them cached); one Anthropic request reports 2,000 uncached and 94,000 read.
inline void test_the_turn_end_keeps_the_last_request_whole_prompt() {
  {
    txb::Battery b(plain::Dialect::OpenAIResponses);
    b.world.files["notes/a.md"] = "alpha";
    sp::Step first = sp::call("read_file", R"({"path":"notes/a.md"})");
    first.usage.input = 3000;
    first.usage.cacheRead = 70000;
    sp::Step last = sp::say("It says alpha.");
    last.usage.input = 3100;
    last.usage.cacheRead = 70000;
    b.model.push(first);
    b.model.push(last);
    plain::TurnOutcome out;
    TXB_OK(b.owner("what does a say", out));
    const std::vector<tx::Item> items = storedOf(b);
    const tx::Item* e = lastEnd(items);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(73100, e->end.lastPromptTokens, "not the last request's whole prompt");
  }
  {
    txb::Battery b(plain::Dialect::AnthropicMessages);
    sp::Step s = sp::say("Hello.");
    s.usage.input = 2000;
    s.usage.cacheRead = 94000;
    s.usage.cacheWrite = 500;
    b.model.push(s);
    plain::TurnOutcome out;
    TXB_OK(b.owner("hi", out));
    const std::vector<tx::Item> items = storedOf(b);
    const tx::Item* e = lastEnd(items);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(96500, e->end.lastPromptTokens, "the cached part of an Anthropic prompt is left out");
    TEST_ASSERT_EQUAL_UINT32(2000, e->end.promptTokens);
  }
}

inline void run() { RUN_TEST(test_the_turn_end_keeps_the_last_request_whole_prompt); }

}  // namespace h81_tests
