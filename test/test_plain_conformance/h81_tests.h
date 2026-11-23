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
      // E2-FIX5: view is not bound in this rig: the line says how much more, and names no call.
      TEST_ASSERT_TRUE_MESSAGE(a->x.find("B more]") != std::string::npos, "no line for the rest");
      TEST_ASSERT_TRUE_MESSAGE(a->x.find("view id=") == std::string::npos, "names view, which is not bound");
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

// DoD item 3 (ruling 83 item 8): a call's long string argument is whole while its own turn is open,
// bounded like tier-2 bulk, and shows as its stub from the close of the turn. A two-round turn: the
// second request still carries the first round's 5,000 B argument whole, and so does the next turn's
// request (E2-VERB). The counter-case: an argument over the head's bulk-whole bound is a stub at once.
inline void test_a_long_call_argument_is_whole_in_its_open_turn_and_a_stub_after() {
  for (size_t bytes : {(size_t)5000, (size_t)40000}) {
    txb::Battery b;
    tx::HeadBudget hb;
    TEST_ASSERT_TRUE(tx::budgetFor(b.headKey(), tx::Profile::Full, hb).ok());
    const bool overBound = bytes > hb.bulkWholeCap;
    std::string text;
    for (int i = 0; text.size() < bytes; ++i) text += "para " + std::to_string(i) + ". ";
    text.resize(bytes);
    text.replace(bytes - 10, 4, "TAIL");
    b.model.push(sp::call("write_file", "{\"path\":\"notes/long.md\",\"text\":\"" + text + "\"}"));
    b.model.push(sp::say("Saved."));
    b.model.push(sp::say("Fine."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("save this", out));
    TEST_ASSERT_EQUAL(1, b.world.runsOf("write_file"));
    const size_t n = b.model.requests().size();
    const std::string& second = b.model.requests()[n - 1].body;   // the same turn's second round
    TEST_ASSERT_EQUAL_MESSAGE(!overBound, second.find("TAIL") != std::string::npos,
                              overBound ? "an argument over the bound must be a stub at once" : "the open turn lost its own argument");
    TEST_ASSERT_TRUE_MESSAGE(overBound || second.find("[stub ") == std::string::npos, "a stub inside its own open turn");
    b.advance(30);
    TXB_OK(b.owner("thanks", out));
    const std::string& next = b.model.requests().back().body;
    // E2-VERB (rulings 81, 82 and 106): the closed turn's argument stays whole too (before, its stub);
    // one over the bound was a stub from the start and stays one.
    TEST_ASSERT_EQUAL_MESSAGE(!overBound, next.find("TAIL") != std::string::npos,
                              overBound ? "an argument over the bound came back whole" : "a closed turn's long argument became its stub by default");
    TEST_ASSERT_EQUAL_MESSAGE(overBound, next.find("B sha:") != std::string::npos, "the stub shows where it must not, or not where it must");
  }
}

// Coordinator note 12 (RB-W1b's report): a tool round with text, then a last round that answers
// with nothing (an empty end_turn). The text the model already wrote is the reply: delivered, the
// turn's cause is the reply, and no provider line goes out. The counter-case: with no text in any
// round the owner still gets the plain line, never silence.
inline void test_text_written_beside_a_call_is_delivered_when_the_last_round_is_empty() {
  for (bool said : {true, false}) {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})", said ? "The note says alpha." : ""));
    b.model.push(sp::say(""));
    plain::TurnOutcome out;
    TXB_OK(b.owner("what does a say", out));
    std::string sent;
    for (const txb::SentMessage& m : b.sent) sent += m.text + "\n";
    if (!said) {
      TEST_ASSERT_TRUE_MESSAGE(!b.sent.empty(), "an empty turn went out silent");
      continue;
    }
    TEST_ASSERT_TRUE_MESSAGE(sent.find("The note says alpha.") != std::string::npos, "the text the model wrote was not delivered");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.sent.size(), sent.c_str());
    TEST_ASSERT_EQUAL_STRING("reply", lastEnd(storedOf(b))->end.cause.c_str());
  }
}

// Ruling 91 blockers/F6: a binding that never installs compaction is refused at boot, loudly (a
// Status and one log line), unless it names why; the battery's engine, which installs it, boots.
inline void test_an_engine_with_no_compaction_installed_refuses_to_boot() {
  txb::Battery b;
  TXB_OK(b.boot());   // installs compaction
  TEST_ASSERT_TRUE(b.hooks->has(plain::Seam::IdleStep, plain::HookSlot::Compaction));
  plain::PlainHooks none;
  plain::EngineDeps d;   // the battery's own bindings, with hooks that hold no compaction
  d.platform = &b.platform;
  d.transport = &b.model;
  d.log = b.log.get();
  d.arena = b.arena.get();
  d.files = &b.files;
  d.chatKey = b.chatKey;
  d.dispatch = [](const plain::ToolCall&, const plain::TurnCtx&) { return plain::ToolOutput{}; };
  d.hooks = &none;
  plain::PlainEngine bare(d);
  const tx::Status st = bare.boot(plain::BootCtx{b.epoch, false});
  TEST_ASSERT_EQUAL((int)tx::Err::InvalidArgument, (int)st.err);
  TEST_ASSERT_EQUAL_STRING("plain::PlainEngine::boot.compaction", st.where);
  d.noCompaction = "h81_tests: the opt-out names itself";
  plain::PlainEngine named(d);
  TXB_OK(named.boot(plain::BootCtx{b.epoch, false}));
}

// DoD 5 (ruling 84 item 5): no note-writing tool comes back; a photo's note (a migrated v5.1.2 note
// or a blind host's transcription, on the owner message's part) survives in the later-turn photo
// line (lane E2-FIX4: shown earlier, not attached to this request, no view pointer while view is not
// bound). The photo arrives with its note; later requests with no pixels to show (no photo hook) show
// the line with the note. The counter-case: a photo with no note shows the plain line.
inline void test_a_photo_note_survives_in_the_later_turn_stub() {
  for (bool noted : {true, false}) {
    txb::Battery b;
    tx::Part p{tx::PartKind::Image, "pa1b2c3d4", "image/jpeg", 90000};
    if (noted) p.note = "A handwritten shopping list: eggs, flour, two lemons.";
    b.model.push(sp::say("I see the list."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("what is this", out, {p}));
    for (int k = 0; k < 3; ++k) {
      b.advance(3 * 3600);   // past every photo's time in view
      b.model.push(sp::say("ok"));
      TXB_OK(b.owner("and now " + std::to_string(k), out));
    }
    const std::string& body = b.model.requests().back().body;
    // No photo hook here: the photo renders as its line (lane E2-FIX4: true, and naming no unbound tool).
    TEST_ASSERT_TRUE_MESSAGE(body.find("[photo pa1b2c3d4") != std::string::npos &&
                                 body.find("not attached to this request") != std::string::npos,
                             "the photo's line is missing");
    TEST_ASSERT_TRUE_MESSAGE(body.find("view id=pa1b2c3d4") == std::string::npos, "the line names view, which is not bound");
    TEST_ASSERT_EQUAL_MESSAGE(noted, body.find("two lemons") != std::string::npos,
                              noted ? "the photo's note did not survive in its stub" : "a note appeared from nowhere");
  }
}

inline void run() {
  RUN_TEST(test_the_turn_end_keeps_the_last_request_whole_prompt);
  RUN_TEST(test_a_long_reply_keeps_the_message_cap_inline_then_a_view_pointer);
  RUN_TEST(test_a_long_call_argument_is_whole_in_its_open_turn_and_a_stub_after);
  RUN_TEST(test_text_written_beside_a_call_is_delivered_when_the_last_round_is_empty);
  RUN_TEST(test_an_engine_with_no_compaction_installed_refuses_to_boot);
  RUN_TEST(test_a_photo_note_survives_in_the_later_turn_stub);
}

}  // namespace h81_tests
