// test_tx_crash - crash safety: at-least-once reply, at-most-once effects (PRD section 7;
// layer L1; lane RB-B0b writes it, lane RB-X1 makes it pass). A power cut at every append
// of a turn (clean and torn), and inside the send, then the boot that follows: the owner
// gets one reply (two only when the cut fell inside the send, which is named), no effect
// runs twice, an intent with no result is shown as 'outcome unknown' and never re-run, an
// intent that cannot be stored keeps its tool from running, a second failure sends the
// plain crash notice and nothing more, a stored reply that was never sent is sent with no
// model call, and restart and install are never resumed. The checker is counter-tested.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

const char* kReply = "Logged skyr, 150 g.";
const char* kWrite = R"({"day":"2026-11-17","add":[{"food":"skyr","g":150}]})";

// ---- counter-tests ---------------------------------------------------------------------
void test_checker_rejects_double_effects_and_lost_or_doubled_replies() {
  txb::CrashOutcome o;
  o.replies = 1;
  o.effectRuns["ledger_write x"] = 1;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::crashSafe(o, why), why.c_str());
  txb::CrashOutcome twice = o;
  twice.effectRuns["ledger_write x"] = 2;
  TEST_ASSERT_FALSE(txb::crashSafe(twice, why));
  txb::CrashOutcome lost = o;
  lost.replies = 0;
  TEST_ASSERT_FALSE(txb::crashSafe(lost, why));
  lost.crashNotice = true;
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::crashSafe(lost, why), why.c_str());
  txb::CrashOutcome doubled = o;
  doubled.replies = 2;
  TEST_ASSERT_FALSE(txb::crashSafe(doubled, why));
  doubled.killInsideSend = true;
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(txb::crashSafe(doubled, why), why.c_str());
  doubled.replies = 3;
  TEST_ASSERT_FALSE(txb::crashSafe(doubled, why));
}

// ---- the scenario ------------------------------------------------------------------------
// Run turns that a cut is expected to end: the cut's Io is the point, a stub is not.
void runCut(txb::Battery& b, const std::vector<txb::TurnScript>& turns) {
  const tx::Status st = txb::runTurns(b, turns);
  if (st.notImplemented()) TXB_OK(st);
}

void sanity(txb::Battery& b, const std::string& tool, const std::string& args, const std::string& reply) {
  txb::callUntilAnswered(b, tool, args, reply);
}

size_t replies(const txb::Battery& b, const std::string& reply) {
  size_t n = 0;
  for (const auto& m : b.sent) n += m.text.find(reply) != std::string::npos;
  return n;
}

struct Case {
  const char* tool;
  const char* args;
  const char* reply;
};
const Case kCases[] = {{"ledger_write", kWrite, kReply},
                       {"device_control", R"({"action":"restart"})", "Restarting now."}};

size_t dryAppends(const Case& c) {
  txb::Battery dry;
  sanity(dry, c.tool, c.args, c.reply);
  TXB_OK(txb::runTurns(dry, {txb::TurnScript{30, "please do it", {}}}));
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)replies(dry, c.reply), "the uncut run did not reply once");
  return dry.files.appends;
}

void expectSafe(txb::Battery& b, const Case& c, bool insideSend, const std::string& label) {
  txb::CrashOutcome o;
  o.replies = replies(b, c.reply);
  o.effectRuns = b.world.effectRuns;
  o.killInsideSend = insideSend;
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::crashSafe(o, why), (label + ": " + why).c_str());
  why.clear();
  TEST_ASSERT_TRUE_MESSAGE(b.model.allPaired(why), (label + ": " + why).c_str());
}

void cutEverywhere(bool torn) {
  for (const Case& c : kCases) {
    const size_t appends = dryAppends(c);
    for (size_t k = 0; k < appends; ++k) {
      txb::Battery b;
      sanity(b, c.tool, c.args, c.reply);
      b.files.killAtAppend = (long)k;
      b.files.tornKill = torn;
      runCut(b, {txb::TurnScript{30, "please do it", {}}});
      const std::string label = std::string(c.tool) + " cut at append " + std::to_string(k) + (torn ? " (torn)" : "");
      TEST_ASSERT_TRUE_MESSAGE(b.killed, (label + ": the cut never fired").c_str());
      TXB_OK(b.restartAfterCut());
      expectSafe(b, c, false, label);
    }
  }
}

void test_a_cut_at_every_append_gives_one_reply_and_no_second_effect() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  cutEverywhere(false);
}

void test_a_torn_cut_at_every_append_gives_one_reply_and_no_second_effect() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  cutEverywhere(true);
}

// A cut inside the send (after the message went out) may deliver twice: named, never more,
// and the effect still ran once.
void test_a_cut_inside_the_send_delivers_at_most_twice() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  for (const Case& c : kCases) {
    txb::Battery b;
    sanity(b, c.tool, c.args, c.reply);
    b.killAtSend = 0;
    runCut(b, {txb::TurnScript{30, "please do it", {}}});
    TEST_ASSERT_TRUE(b.killed);
    TXB_OK(b.restartAfterCut());
    expectSafe(b, c, true, std::string(c.tool) + " cut inside the send");
  }
}

bool lineIs(const char* data, size_t len, tx::ItemKind kind, const char* tool) {
  tx::Item it;
  if (!tx::decodeLine(data, len, it).ok()) return false;
  return it.kind == kind && (!tool || it.n == tool);
}

// The cut falls after the effect ran and before its result was stored: at boot the model
// is told 'outcome unknown' and the effect is not run again.
void test_an_intent_without_result_is_shown_as_outcome_unknown_and_never_rerun() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Result, "ledger_write"); };
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "no result line of ledger_write was ever appended");
  TEST_ASSERT_EQUAL(1, b.world.runsOf("ledger_write"));
  const size_t before = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  bool told = false;
  for (size_t i = before; i < b.model.requests().size(); ++i)
    told |= b.model.requests()[i].body.find("outcome unknown") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(told, "the resumed request never said 'outcome unknown'");
  TEST_ASSERT_EQUAL_MESSAGE(1, b.world.runsOf("ledger_write"), "the effect was re-run after the restart");
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
}

// The intent cannot be stored: the tool does not run and the model reads why.
void test_an_intent_that_cannot_be_stored_keeps_the_tool_from_running() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, "I could not record that, so nothing was logged.");
  b.files.refuseWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Intent, nullptr); };
  TXB_OK(txb::runTurns(b, {txb::TurnScript{30, "log it", {}}}));
  TEST_ASSERT_TRUE_MESSAGE(b.world.effectRuns.empty(), "an effect ran without its intent stored");
  bool told = false;
  for (const auto& r : b.model.requests()) told |= r.body.find("not run: could not record the action first") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(told, "the model never read 'not run: could not record the action first'");
}

// One automatic resume; when it fails too, the plain crash notice and no further model call.
void test_a_second_failure_sends_the_crash_notice_and_nothing_more() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_day", R"({"day":"2026-11-17"})", "Nothing logged today.");
  b.files.killAtAppend = 3;
  runCut(b, {txb::TurnScript{30, "what did I eat?", {}}});
  TEST_ASSERT_TRUE(b.killed);
  b.engine.reset();
  b.files.killAtAppend = (long)b.files.appends;   // the resume is cut too, at its first append
  const tx::Status st = b.boot(true);
  if (st.notImplemented()) TXB_OK(st);
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "the resume made no append to cut");
  const size_t sentBefore = b.sent.size(), requestsBefore = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requestsBefore, b.model.requests().size(), "a second automatic resume called the model");
  TEST_ASSERT_EQUAL_MESSAGE(sentBefore + 1, b.sent.size(), "the crash notice is one message");
  TEST_ASSERT_TRUE_MESSAGE(b.sent.back().text.find("Nothing logged today.") == std::string::npos,
                           "the crash notice is not the model's text");
}

// The reply was stored, the cut came before the send: it is sent at boot with no model call.
void test_a_stored_reply_never_sent_is_sent_without_a_model_call() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE(b.killed);
  TEST_ASSERT_EQUAL(0, (int)b.sent.size());
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "a stored reply cost a model call");
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("ledger_write"));
}

// Restart and install run after the reply is sent and are excluded from resume: a cut
// anywhere after the reply leaves the restart run at most once and never re-asked.
void test_restart_and_install_are_never_resumed() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  for (const char* action : {"restart", "install"}) {
    const std::string args = std::string("{\"action\":\"") + action + "\"}";
    const Case c{"device_control", args.c_str(), "Done, see you in a minute."};
    txb::Battery b;
    sanity(b, c.tool, c.args, c.reply);
    b.killAtSend = 0;
    runCut(b, {txb::TurnScript{30, action, {}}});
    const size_t requests = b.model.requests().size();
    TXB_OK(b.restartAfterCut());
    TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "a deferred restart or install was resumed");
    expectSafe(b, c, true, action);
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_double_effects_and_lost_or_doubled_replies);
  RUN_TEST(test_a_cut_at_every_append_gives_one_reply_and_no_second_effect);
  RUN_TEST(test_a_torn_cut_at_every_append_gives_one_reply_and_no_second_effect);
  RUN_TEST(test_a_cut_inside_the_send_delivers_at_most_twice);
  RUN_TEST(test_an_intent_without_result_is_shown_as_outcome_unknown_and_never_rerun);
  RUN_TEST(test_an_intent_that_cannot_be_stored_keeps_the_tool_from_running);
  RUN_TEST(test_a_second_failure_sends_the_crash_notice_and_nothing_more);
  RUN_TEST(test_a_stored_reply_never_sent_is_sent_without_a_model_call);
  RUN_TEST(test_restart_and_install_are_never_resumed);
  return UNITY_END();
}
