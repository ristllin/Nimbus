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


// A long text with markers: "NEAR" just before byte `near`, "PAST" past the message cap.
inline std::string longReply(size_t bytes) {
  std::string s;
  for (int i = 0; s.size() < bytes; ++i) s += "line " + std::to_string(i) + " of the long answer.\n";
  s.resize(bytes);
  s.replace(12288 - 40, 4, "NEAR");
  if (bytes > 20000) s.replace(20000, 4, "PAST");
  return s;
}

// Ruling 91 blockers/F1, DoD item 4: a reply over the inline limit keeps at least the head's v5.1.2
// message cap inline (HeadBudget::messageCap: 12,288 B on Sonnet, never under it on a floor head)
// and then a view pointer to the whole, which the a item names by its bulk handle; the owner gets
// the whole reply. Tested at 12,289 B and 30,000 B on Sonnet direct and on a floor head (custom).
// The counter-case: a reply at the cap stays whole with no pointer.
inline void test_a_long_reply_keeps_the_message_cap_inline_then_a_view_pointer() {
  for (bool floor : {false, true})
    for (size_t bytes : {(size_t)12288, (size_t)12289, (size_t)30000}) {
      txb::Battery b;
      if (floor) b.hostOverride = "custom";
      tx::HeadBudget hb;
      TEST_ASSERT_TRUE(tx::budgetFor(b.headKey(), tx::Profile::Full, hb).ok());
      TEST_ASSERT_TRUE(hb.messageCap >= 12288);
      const std::string reply = longReply(bytes);
      b.model.push(sp::say(reply));
      b.model.push(sp::say("Short."));
      plain::TurnOutcome out;
      TXB_OK(b.owner("write it out", out));
      TEST_ASSERT_TRUE_MESSAGE(out.delivered, "the reply was not delivered");
      std::string sent;
      for (const txb::SentMessage& m : b.sent) sent += m.text;
      TEST_ASSERT_TRUE_MESSAGE(sent.find(reply.substr(bytes - 60)) != std::string::npos, "the owner did not get the whole reply");
      const std::vector<tx::Item> items = storedOf(b);
      const tx::Item* a = nullptr;
      for (const tx::Item& it : items)
        if (it.kind == tx::ItemKind::Assistant && it.fin) a = &it;
      TEST_ASSERT_NOT_NULL(a);
      if (bytes <= hb.messageCap) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(reply.c_str(), a->x.c_str(), "a reply at the cap must stay whole");
        TEST_ASSERT_TRUE(a->handle.empty());
        continue;
      }
      char why[160];
      std::snprintf(why, sizeof why, "%s %zu B: the stored reply keeps %zu B", floor ? "floor" : "sonnet", bytes, a->x.size());
      TEST_ASSERT_TRUE_MESSAGE(a->x.compare(0, hb.messageCap - 3, reply, 0, hb.messageCap - 3) == 0, why);
      TEST_ASSERT_TRUE_MESSAGE(a->x.find(tx::viewPointer(a->q)) != std::string::npos, "no view pointer to the rest");
      TEST_ASSERT_FALSE_MESSAGE(a->handle.empty(), "the a item does not name where the whole is");
      std::string whole;
      tx::TxLog reader(b.files, b.chatKey);
      TEST_ASSERT_TRUE(reader.readBulk(a->handle, 0, tx::kBulkMaxBytes, whole).ok());
      TEST_ASSERT_TRUE_MESSAGE(whole == reply, "the handle does not read back the whole reply");
      // The next turn's request carries the first messageCap bytes of it, not a 200 B preview.
      plain::TurnOutcome next;
      TXB_OK(b.owner("thanks", next));
      const sp::Recorded& req = b.model.requests().back();
      TEST_ASSERT_TRUE_MESSAGE(req.body.find("NEAR") != std::string::npos, "the next request lost the reply's first 12 KB");
      if (bytes > 20000) TEST_ASSERT_TRUE(req.body.find("PAST") == std::string::npos);
    }
}

inline void run() {
  RUN_TEST(test_the_turn_end_keeps_the_last_request_whole_prompt);
  RUN_TEST(test_a_long_reply_keeps_the_message_cap_inline_then_a_view_pointer);
}

}  // namespace h81_tests
