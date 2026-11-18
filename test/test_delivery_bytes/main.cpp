// test_delivery_bytes - one plain message per turn, written once (PRD goal 2, sections 1
// step 6 and 8.9; layer L1; lane RB-B0b writes it, lane RB-E3 makes it pass). The channel
// receives exactly the portable deliver function's chunks of the model's text: no sentence
// deleted, no correction appended, no claim vetoed, whatever the reply says. A device-
// written message is one of the closed list (deliver.h kAllStatusLines), sent as its own
// message, at most one after a turn. The checker is counter-tested against the old
// engine's habits (an appended 'Note:', a deleted sentence, two status lines).
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <cctype>
#include <string>
#include <vector>

#include "../support/tx_battery/deliver_oracle.h"
#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

std::vector<txb::SentMessage> sent(std::initializer_list<const char*> texts) {
  std::vector<txb::SentMessage> v;
  for (const char* t : texts) v.push_back(txb::SentMessage{"1001", t});
  return v;
}

// ---- counter-tests ---------------------------------------------------------------------
void test_checker_rejects_the_old_engines_rewrites() {
  const std::vector<std::string> chunks = {"I logged 150 g of skyr. Anything else?"};
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, false, "", sent({"I logged 150 g of skyr. Anything else?"}), why),
                           why.c_str());
  // The claim scanner's appended correction, inside the reply or as a second message.
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, false, "", sent({"I logged 150 g of skyr. Anything else? (Note: nothing was saved.)"}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, false, "", sent({"I logged 150 g of skyr. Anything else?", "Note: nothing was saved."}), why));
  // Sentence deletion at the send seam.
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, false, "", sent({"Anything else?"}), why));
  // A reply never sent.
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, false, "", {}, why));
  // A status line is allowed once, as its own message, only when the turn ended with one.
  TEST_ASSERT_TRUE(txb::deliveredExactly(chunks, true, "Stopped.",
                                         sent({"I logged 150 g of skyr. Anything else?", "Stopped."}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, true, "Stopped.",
                                          sent({"I logged 150 g of skyr. Anything else?", "Stopped. Sorry!"}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, true, "", sent({"I logged 150 g of skyr. Anything else?", "Stopped.", "Provider error."}), why));
}

// ---- through the engine -----------------------------------------------------------------
void expectExact(txb::Battery& b, const plain::TurnOutcome& out, const std::string& modelText, size_t from) {
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks(modelText, chunks));
  const std::vector<txb::SentMessage> got(b.sent.begin() + (long)from, b.sent.end());
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, out.status != plain::StatusLine::None, "", got, why),
                           why.c_str());
}

// Claim-shaped replies with no tool behind them, markup, entities, long text: the model's
// text arrives as the deliver function makes it, and nothing else arrives.
void test_the_reply_is_the_model_text_whatever_it_claims() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  const std::string replies[] = {
      "I've logged 150 g of skyr for you.",
      "Done! Saved it to notes/plan.md and set a reminder for 7.",
      "**Bold**, _italic_, `code`, <tags> & ampersands > here.",
      "Unicode: caf\xC3\xA9, \xF0\x9F\x8D\xB2, \xE2\x82\xAC 3.50",
      "I couldn't find it. I checked notes/ and the ledger; neither has it.",
  };
  txb::Battery b;
  for (const std::string& r : replies) {
    b.model.push(sp::say(r));
    const size_t from = b.sent.size();
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("tell me", out));
    TEST_ASSERT_TRUE(out.status == plain::StatusLine::None);
    TEST_ASSERT_EQUAL_STRING(r.c_str(), out.text.c_str());
    expectExact(b, out, r, from);
    // Independent of the deliver function: text with no markup or entity characters
    // arrives as one message, byte for byte (a deliver function that edits claims fails).
    if (r.find_first_of("*_`<>&") == std::string::npos) {
      TEST_ASSERT_EQUAL(from + 1, b.sent.size());
      TEST_ASSERT_EQUAL_STRING(r.c_str(), b.sent.back().text.c_str());
    }
  }
}

// A long reply is split by the deliver function, without a byte lost or added.
void test_a_long_reply_is_split_without_loss() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  std::string longText;
  for (int i = 0; i < 400; ++i) longText += "Line " + std::to_string(i) + ": the boat is the Nereid II.\n";
  txb::Battery b;
  b.model.push(sp::say(longText));
  plain::TurnOutcome out;
  TXB_OK(b.owner("tell me everything", out));
  TEST_ASSERT_TRUE_MESSAGE(b.sent.size() > 1, "12 KB went out as one Telegram message");
  expectExact(b, out, longText, 0);
  // Independent of the deliver function: the chunks hold every visible byte, in order.
  std::string joined;
  for (const auto& m : b.sent) joined += m.text;
  auto visible = [](const std::string& t) {
    std::string v;
    for (char c : t)
      if (!std::isspace((unsigned char)c)) v += c;
    return v;
  };
  TEST_ASSERT_TRUE_MESSAGE(visible(joined) == visible(longText), "a split lost or added bytes");
}

// A guard trip: the last round's text, then at most one status line, as its own message.
void test_a_status_line_is_separate_and_alone() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  for (int i = 0; i < plain::kDefaultRounds; ++i)
    b.model.push(sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}"));
  b.model.push(sp::say("I read it sixteen times; it says alpha."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading", out));
  TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::LimitReached, plain::statusLineSlug(out.status));
  expectExact(b, out, "I read it sixteen times; it says alpha.", 0);
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks("I read it sixteen times; it says alpha.", chunks));
  TEST_ASSERT_EQUAL_MESSAGE(chunks.size() + 1, b.sent.size(), "the limit-reached line is one message of its own");
  TEST_ASSERT_TRUE_MESSAGE(b.sent.back().text.find("sixteen times") == std::string::npos,
                           "the status line carries the model's text");
}

// A provider that keeps failing (one try and the two same-host retries): no model text, so
// exactly the one provider-error line and nothing else.
void test_a_failed_turn_sends_exactly_one_line() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  txb::Battery b;
  for (int i = 0; i < 3; ++i) b.model.push(sp::httpError(500));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello?", out));
  TEST_ASSERT_EQUAL_MESSAGE(3, (int)b.model.requests().size(), "a 5xx gets one try and two same-host retries");
  TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::ProviderError, plain::statusLineSlug(out.status));
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.sent.size(), "a failed turn sends exactly one line");
  TEST_ASSERT_FALSE(out.delivered && !out.text.empty());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_the_old_engines_rewrites);
  RUN_TEST(test_the_reply_is_the_model_text_whatever_it_claims);
  RUN_TEST(test_a_long_reply_is_split_without_loss);
  RUN_TEST(test_a_status_line_is_separate_and_alone);
  RUN_TEST(test_a_failed_turn_sends_exactly_one_line);
  return UNITY_END();
}
