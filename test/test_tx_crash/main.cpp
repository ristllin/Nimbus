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

#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/intents.h"

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

bool hasKind(const std::vector<tx::Item>& items, tx::ItemKind k) {
  for (const tx::Item& it : items)
    if (it.kind == k) return true;
  return false;
}

bool hasEvent(const std::vector<tx::Item>& items, const std::string& ev) {
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd)
      for (const std::string& e : it.end.events)
        if (e == ev) return true;
  return false;
}

// Owner ruling 81: the first request after the resume carries every item stored before the
// kill (whole, or as its stored stub where the stub rule says so). Requests before `from` are
// the cut run's.
void expectEveryItemCarried(txb::Battery& b, const std::vector<tx::Item>& before, size_t from, const std::string& label) {
  if (b.model.requests().size() <= from) return;   // no model call after the restart
  std::vector<tx::Item> now;
  TXB_OK(b.stored(now));
  txb::CoverInput in = b.coverOf(before);
  for (const tx::Item& it : now)   // the pointer the resumed request was rendered with (its open's x)
    if (it.kind == tx::ItemKind::PointerMove) in.pointer = std::max(in.pointer, it.stub);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::covers(b.model.requests()[from].view, in, why), (label + " (ruling 81): " + why).c_str());
}

// A cut at every append of the turn. The boot cannot tell a cut just before the send from one
// just after it when the delivery record (d) did not make it to the card: both leave the stored
// answer and nothing after it. That one point is the cut inside the send (at most two replies,
// named by the e item's 'resume:sent-stored'); every other point gives exactly one reply.
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
      std::vector<tx::Item> before;
      TXB_OK(b.stored(before));
      const bool insideSend = replies(b, c.reply) > 0 && !hasKind(before, tx::ItemKind::Delivery);
      const size_t requests = b.model.requests().size();
      TXB_OK(b.restartAfterCut());
      expectSafe(b, c, insideSend, label);
      expectEveryItemCarried(b, before, requests, label);
      std::vector<tx::Item> after;
      TXB_OK(b.stored(after));
      TEST_ASSERT_TRUE_MESSAGE(hasKind(after, tx::ItemKind::TurnEnd), (label + ": the turn was never closed").c_str());
      if (insideSend)
        TEST_ASSERT_TRUE_MESSAGE(hasEvent(after, "resume:sent-stored"), (label + ": a second send is not named").c_str());
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(txb::itemsPaired(after, why), (label + ": " + why).c_str());
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


// ---- lane RB-X1: the kill points the definition of done names, one by one ------------------
namespace intents = nimbus::orch::intents;

const tx::Item* lastOf(const std::vector<tx::Item>& items, tx::ItemKind k) {
  const tx::Item* out = nullptr;
  for (const tx::Item& it : items)
    if (it.kind == k) out = &it;
  return out;
}

bool resumedRequestSays(const txb::Battery& b, size_t from, const std::string& text) {
  return b.model.requests().size() > from && b.model.requests()[from].body.find(text) != std::string::npos;
}

// The rule, written out a second time on its own: one resume and one boot send at most.
intents::BootAction expectedAction(const intents::OpenTurn& t, uint8_t attempts, bool whole) {
  using S = intents::TurnState;
  using A = intents::BootAction;
  const bool left = attempts < plain::kMaxAutoResumes || (t.resumedAnswer && attempts == plain::kMaxAutoResumes);
  if (t.state == S::Closed) return A::None;
  if (t.state == S::Delivered) return A::Close;
  if (t.state == S::AnswerStored) return whole && left ? A::SendStored : t.person ? A::Notice : A::Close;
  if (!t.person) return A::Close;
  return attempts >= plain::kMaxAutoResumes ? A::Notice : A::Resume;
}

// The boot's one action, over the whole class: every state, person or not, every attempt count
// up to past the limit, a whole or a stubbed stored answer.
void test_the_boot_action_over_every_turn_state() {
  using S = intents::TurnState;
  using A = intents::BootAction;
  for (S st : {S::Closed, S::Unanswered, S::AnswerStored, S::Delivered})
    for (bool person : {false, true})
      for (uint8_t attempts = 0; attempts <= plain::kMaxAutoResumes + 2; ++attempts)
        for (bool whole : {false, true})
          for (bool resumed : {false, true}) {
            intents::OpenTurn t;
            t.state = st;
            t.turn = st == S::Closed ? 0 : 7;
            t.person = person;
            t.resumedAnswer = resumed;
            const A got = intents::bootAction(t, attempts, plain::kMaxAutoResumes, whole);
            const A want = expectedAction(t, attempts, whole);
            const std::string label = std::string(intents::turnStateSlug(st)) + (person ? " person" : " unattended") +
                                      " attempts " + std::to_string(attempts) + (whole ? " whole" : " stub") +
                                      (resumed ? " resumed" : "");
            TEST_ASSERT_EQUAL_STRING_MESSAGE(intents::bootActionSlug(want), intents::bootActionSlug(got), label.c_str());
          }
}

// After an intent, before the effect ran (the cut falls inside the dispatch, before the tool
// changed anything): the boot cannot know, so the model reads 'outcome unknown' and the
// harness never runs the tool again.
void test_a_kill_between_the_intent_and_the_effect_is_outcome_unknown() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killAtDispatch = 0;
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "the dispatch was never cut");
  TEST_ASSERT_EQUAL(0, b.world.runsOf("ledger_write"));
  std::vector<tx::Item> before;
  TXB_OK(b.stored(before));
  TEST_ASSERT_NOT_NULL_MESSAGE(lastOf(before, tx::ItemKind::Intent), "the intent was not stored before the dispatch");
  const size_t from = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_TRUE_MESSAGE(resumedRequestSays(b, from, "outcome unknown"), "the resumed request never said 'outcome unknown'");
  TEST_ASSERT_FALSE(resumedRequestSays(b, from, plain::texts::tool_results::kRestartedBeforeRun));
  TEST_ASSERT_EQUAL_MESSAGE(0, b.world.runsOf("ledger_write"), "the harness ran the tool after the restart");
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  expectEveryItemCarried(b, before, from, "intent, no effect");
}

// After the call is stored and before its intent: no effect runs before its intent is
// stored, so the boot knows it changed nothing and says so (never 'outcome unknown'); a model
// that then asks again runs it exactly once.
void test_an_effect_that_never_started_is_answered_as_changed_nothing() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  const std::string unrun = plain::texts::tool_results::kRestartedBeforeRun;
  b.model.respondWith([unrun](const sp::Recorded& r, size_t) {
    if (r.body.find("row L00A3 added") != std::string::npos) return sp::say(kReply);
    if (r.body.find("outcome unknown") != std::string::npos) return sp::say("I am not sure it was logged.");
    return sp::call("ledger_write", kWrite);   // first ask, and again after 'changed nothing'
  });
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Intent, nullptr); };
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "no intent line was ever appended");
  TEST_ASSERT_EQUAL(0, b.world.runsOf("ledger_write"));
  std::vector<tx::Item> before;
  TXB_OK(b.stored(before));
  const size_t from = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_TRUE_MESSAGE(resumedRequestSays(b, from, unrun), "the resumed request did not say the call changed nothing");
  TEST_ASSERT_FALSE_MESSAGE(resumedRequestSays(b, from, "outcome unknown"), "a call that never started was called unknown");
  TEST_ASSERT_EQUAL_MESSAGE(1, b.world.runsOf("ledger_write"), "asked again, the effect must run exactly once");
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  expectEveryItemCarried(b, before, from, "call, no intent");
}

// After the send: the delivery record is stored, so the boot only closes the turn.
void test_a_cut_after_the_delivery_record_sends_nothing_again() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::TurnEnd, nullptr); };
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE(b.killed);
  const size_t requests = b.model.requests().size(), sent = b.sent.size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "closing a delivered turn called the model");
  TEST_ASSERT_EQUAL_MESSAGE(sent, b.sent.size(), "a delivered reply was sent again");
  std::vector<tx::Item> after;
  TXB_OK(b.stored(after));
  const tx::Item* e = lastOf(after, tx::ItemKind::TurnEnd);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_STRING("resume:closed", e->end.cause.c_str());
  TEST_ASSERT_EQUAL_STRING("close", intents::bootActionSlug(b.resumeReports.back().action));
}

// A stored reply sent at one boot is never sent again at the next one.
void test_a_stored_reply_is_sent_once_across_later_boots() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE(b.killed);
  TXB_OK(b.restartAfterCut());
  TXB_OK(b.restartAfterCut());   // a clean restart later
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  std::vector<tx::Item> after;
  TXB_OK(b.stored(after));
  TEST_ASSERT_NOT_NULL(lastOf(after, tx::ItemKind::Delivery));
  const tx::Item* e = lastOf(after, tx::ItemKind::TurnEnd);
  TEST_ASSERT_NOT_NULL_MESSAGE(e, "the boot never closed the turn");
  TEST_ASSERT_EQUAL_STRING("resume:sent-stored", e->end.cause.c_str());
}

// A turn the boot closed counts as closed for the next turn: its cold open moves the stub
// pointer to that turn's e item, as it would for a turn the loop closed.
void test_a_turn_the_boot_closed_counts_as_closed() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TXB_OK(b.restartAfterCut());
  std::vector<tx::Item> closed;
  TXB_OK(b.stored(closed));
  const tx::Item* end = lastOf(closed, tx::ItemKind::TurnEnd);
  TEST_ASSERT_NOT_NULL_MESSAGE(end, "the boot never closed the turn");
  const uint64_t eQ = end->q;
  b.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("Morning."); });
  TXB_OK(txb::runTurns(b, {txb::chat(600, "good morning", "Morning.")}));
  std::vector<tx::Item> after;
  TXB_OK(b.stored(after));
  const tx::Item* x = lastOf(after, tx::ItemKind::PointerMove);
  TEST_ASSERT_NOT_NULL_MESSAGE(x, "the first turn after the restart did not open cold");
  TEST_ASSERT_EQUAL_MESSAGE((int)eQ, (int)x->stub, "the pointer did not reach the turn the boot closed");
}

// A turn no person started (a routine fire) is closed at boot, never resumed: nobody is in it
// to answer, and its effects stay at most once.
void test_a_turn_no_person_started_is_closed_never_resumed() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Assistant, nullptr); };
  plain::TurnOutcome out;
  const tx::Status st = b.unattended(plain::Trigger::Routine, "routine: log it", out);
  if (st.notImplemented()) TXB_OK(st);
  TEST_ASSERT_TRUE(b.killed);
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "an unattended turn was resumed");
  TEST_ASSERT_EQUAL(0, (int)b.sent.size());
  TEST_ASSERT_EQUAL(1, b.world.runsOf("ledger_write"));
  std::vector<tx::Item> after;
  TXB_OK(b.stored(after));
  const tx::Item* e = lastOf(after, tx::ItemKind::TurnEnd);
  TEST_ASSERT_NOT_NULL_MESSAGE(e, "the boot never closed the turn");
  TEST_ASSERT_EQUAL_STRING("resume:closed", e->end.cause.c_str());
}

// When the attempt cannot be written ahead to head.json, the boot never risks a resume it
// cannot count: it sends the plain crash notice. Counter-test: with head.json writable the
// same cut resumes.
void test_no_resume_without_its_counter_written_first() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  for (bool writable : {true, false}) {
    txb::Battery b;
    sanity(b, "ledger_write", kWrite, kReply);
    b.files.killAtAppend = 1;   // after the owner item
    runCut(b, {txb::TurnScript{30, "log it", {}}});
    TEST_ASSERT_TRUE(b.killed);
    const size_t requests = b.model.requests().size();
    b.engine.reset();
    b.files.clearFaults();
    b.files.refuseWholeWrites = !writable;
    TXB_OK(b.boot(true));
    TEST_ASSERT_FALSE(b.resumeReports.empty());
    const char* want = writable ? "resume" : "notice";
    TEST_ASSERT_EQUAL_STRING(want, intents::bootActionSlug(b.resumeReports.back().action));
    TEST_ASSERT_EQUAL(writable ? 1 : 0, (int)replies(b, kReply));
    if (!writable) {
      TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "a resume ran without its counter");
      TEST_ASSERT_EQUAL(1, (int)b.sent.size());
    }
  }
}


// The resumed run reads the outside content its turn already read whole: the resumed turn is
// tainted like the first run was (the rails gate its effects the same way).
void test_a_resumed_turn_that_read_outside_content_stays_tainted() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  std::vector<std::pair<std::string, bool>> seen;   // tool, turn tainted
  b.moreHooks = [&seen](plain::PlainHooks& h) {
    (void)h.beforeDispatch(plain::HookSlot::Policy, [&seen](const plain::DispatchCtx& c) {
      seen.emplace_back(c.call->name, c.turn->tainted);
      return plain::DispatchDecision{};
    });
  };
  b.model.respondWith([](const sp::Recorded& r, size_t) {
    if (r.body.find("OUTSIDE-") == std::string::npos) return sp::call("web_search", R"({"q":"train times"})");
    if (r.body.find("wrote notes/t.md") == std::string::npos) return sp::call("write_file", R"({"path":"notes/t.md","text":"x"})");
    return sp::say("Saved the times.");
  });
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Call, "write_file"); };
  runCut(b, {txb::TurnScript{30, "find the train times and save them", {}}});
  TEST_ASSERT_TRUE(b.killed);
  seen.clear();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)seen.size(), "the resumed run did not call write_file once");
  TEST_ASSERT_EQUAL_STRING("write_file", seen[0].first.c_str());
  TEST_ASSERT_TRUE_MESSAGE(seen[0].second, "the resumed turn lost the taint of the outside content it carries");
  TEST_ASSERT_EQUAL(1, (int)replies(b, "Saved the times."));
}

// A small arena: the turn's own long owner message fills most of it. The resumed run makes room
// at its open, and never by dropping its own turn: the request still carries the message.
void test_a_resumed_turn_keeps_its_own_items_when_the_arena_is_short() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  b.arenaBytes = 64 * 1024;
  b.inboxRefuses = true;   // the long owner text stays in the arena (RB-TG1 would file it)
  sanity(b, "ledger_write", kWrite, kReply);
  const std::string ask = "log it. " + std::string(40000, 'n');
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Assistant, nullptr); };
  runCut(b, {txb::TurnScript{30, ask, {}}});
  TEST_ASSERT_TRUE(b.killed);
  std::vector<tx::Item> before;
  TXB_OK(b.stored(before));
  const size_t from = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_TRUE_MESSAGE(b.model.requests().size() > from, "no resume");
  TEST_ASSERT_TRUE_MESSAGE(resumedRequestSays(b, from, std::string(40000, 'n')), "the resumed run dropped its own owner message");
  expectEveryItemCarried(b, before, from, "short arena");
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("ledger_write"));
}

// The arena has no room for the boot's own items: the d and e items go to the card, the open
// calls are left to the engine's own turn open, and the stored reply still goes out once.
void test_a_full_arena_at_boot_still_sends_the_stored_reply_once() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  b.arenaBytes = 48 * 1024;
  b.inboxRefuses = true;   // the long owner text stays in the arena (RB-TG1 would file it)
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  runCut(b, {txb::TurnScript{30, "log it. " + std::string(44500, 'n'), {}}});
  TEST_ASSERT_TRUE(b.killed);
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_TRUE_MESSAGE(b.arena->freeBytes() < 4096, "the arena was not short at boot: the case tests nothing");
  TEST_ASSERT_EQUAL(requests, b.model.requests().size());
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  TXB_OK(b.restartAfterCut());   // the card holds the d and e items: nothing is sent again
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  std::vector<tx::Item> after;
  TXB_OK(b.stored(after));
  const tx::Item* e = lastOf(after, tx::ItemKind::TurnEnd);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_STRING("resume:sent-stored", e->end.cause.c_str());
}

// The one resume stored its answer and was cut before the send: the next boot sends that
// answer (one boot send after the one resume), with no model call.
void test_the_resumed_runs_stored_answer_is_sent_at_the_next_boot() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Assistant, nullptr); };
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE(b.killed);
  b.engine.reset();
  b.files.clearFaults();
  b.killBeforeSend = (long)b.sends;   // the resume's own send is cut
  const tx::Status st = b.boot(true);
  if (st.notImplemented()) TXB_OK(st);
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "the resume's send was not cut");
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "the stored answer cost a model call");
  TEST_ASSERT_EQUAL_STRING("send-stored", intents::bootActionSlug(b.resumeReports.back().action));
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL(1, b.world.runsOf("ledger_write"));
}

// A routine's stored answer that was never sent goes out at boot (no model call): only the
// model call of a resume is a person's turn's alone.
void test_a_routines_stored_answer_is_sent_not_lost() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  plain::TurnOutcome out;
  const tx::Status st = b.unattended(plain::Trigger::Routine, "routine: log it", out);
  if (st.notImplemented()) TXB_OK(st);
  TEST_ASSERT_TRUE(b.killed);
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL(requests, b.model.requests().size());
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
}

// The channel refuses the boot's send (no network yet): nothing reached the owner, so the
// attempt is given back and the turn stays open; the next boot sends the reply once.
void test_a_refused_boot_send_is_tried_again_at_the_next_boot() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.killBeforeSend = 0;
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  TEST_ASSERT_TRUE(b.killed);
  b.refuseSends = true;
  (void)b.restartAfterCut();
  TEST_ASSERT_EQUAL(0, (int)b.sent.size());
  TEST_ASSERT_FALSE(b.resumeReports.back().status.ok());
  b.refuseSends = false;
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_STRING("send-stored", intents::bootActionSlug(b.resumeReports.back().action));
  TEST_ASSERT_EQUAL(1, (int)replies(b, kReply));
  // A refused crash notice leaves the turn open too: a later boot sends it.
  txb::Battery n;
  sanity(n, "ledger_day", R"({"day":"2026-11-17"})", "Nothing logged today.");
  n.files.killAtAppend = 1;
  runCut(n, {txb::TurnScript{30, "what did I eat?", {}}});
  n.files.refuseWholeWrites = true;   // no counter: the boot can only send the notice
  n.refuseSends = true;
  n.engine.reset();
  (void)n.boot(true);
  TEST_ASSERT_EQUAL(0, (int)n.sent.size());
  n.refuseSends = false;
  n.engine.reset();
  TXB_OK(n.boot(true));
  TEST_ASSERT_EQUAL(1, (int)n.sent.size());
  TEST_ASSERT_EQUAL_STRING("notice", intents::bootActionSlug(n.resumeReports.back().action));
}


// head.json lost after the one resume stored its t item: the t item's attempt still counts it,
// so the next boot sends the notice, never a second resume.
void test_the_resumed_t_item_counts_when_head_json_is_lost() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  txb::Battery b;
  sanity(b, "ledger_write", kWrite, kReply);
  b.files.killAtAppend = 1;   // after the owner item
  runCut(b, {txb::TurnScript{30, "log it", {}}});
  b.engine.reset();
  b.files.clearFaults();
  b.files.killWhen = [](const char* d, size_t n) { return lineIs(d, n, tx::ItemKind::Assistant, nullptr); };
  const tx::Status st = b.boot(true);   // the resume runs and is cut at its answer
  if (st.notImplemented()) TXB_OK(st);
  TEST_ASSERT_TRUE(b.killed);
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  bool resumedT = false;
  for (const tx::Item& it : items) resumedT |= it.kind == tx::ItemKind::TurnOpen && it.attempt == 1;
  TEST_ASSERT_TRUE_MESSAGE(resumedT, "the resume stored no t item of attempt 1");
  std::string head;
  TXB_OK(tx::headPath(b.chatKey, head));
  b.files.files.erase(head);
  const size_t requests = b.model.requests().size();
  TXB_OK(b.restartAfterCut());
  TEST_ASSERT_EQUAL_MESSAGE(requests, b.model.requests().size(), "a second resume after head.json was lost");
  TEST_ASSERT_EQUAL_STRING("notice", intents::bootActionSlug(b.resumeReports.back().action));
}

// A reply over the head's message cap keeps its head inline and names its whole text in the bulk
// file (the a item's handle; lane RB-H81, as RB-X1's known limit asked): a 30,000 B reply stored
// and not sent is sent whole, once, at the next boot, with no model call. The counter-case: when
// the bulk file cannot give the whole back, the head is never sent as the reply; the owner gets
// the plain crash notice.
void test_a_long_stored_answer_is_sent_whole_from_its_bulk_handle() {
  TXB_WAITS_FOR("PENDING RB-X1", "RB-E1a");
  for (bool bulkLost : {false, true}) {
    txb::Battery b;
    std::string longReply = "Here is the plan. ";
    while (longReply.size() < 30000) longReply += "Step and detail, line by line.\n";
    longReply += "THE END";
    sanity(b, "ledger_write", kWrite, longReply);
    b.killBeforeSend = 0;
    runCut(b, {txb::TurnScript{30, "plan it", {}}});
    TEST_ASSERT_TRUE(b.killed);
    if (bulkLost)
      for (auto& kv : b.files.files)
        if (kv.first.find(".txb") != std::string::npos) kv.second.clear();
    const size_t requests = b.model.requests().size();
    TXB_OK(b.restartAfterCut());
    TEST_ASSERT_EQUAL(requests, b.model.requests().size());   // no model call
    std::string sent;
    for (const txb::SentMessage& m : b.sent) sent += m.text;
    if (bulkLost) {
      TEST_ASSERT_EQUAL_STRING("notice", intents::bootActionSlug(b.resumeReports.back().action));
      TEST_ASSERT_TRUE_MESSAGE(sent.find("[text continues:") == std::string::npos, "the head went out as the reply");
      continue;
    }
    TEST_ASSERT_EQUAL_STRING("send-stored", intents::bootActionSlug(b.resumeReports.back().action));
    TEST_ASSERT_TRUE_MESSAGE(sent.find("THE END") != std::string::npos, "the owner did not get the whole reply");
    TEST_ASSERT_EQUAL_MESSAGE(sent.find("THE END"), sent.rfind("THE END"), "the reply went out twice");
    TEST_ASSERT_TRUE(sent.find("[text continues:") == std::string::npos);
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
  RUN_TEST(test_the_boot_action_over_every_turn_state);
  RUN_TEST(test_a_kill_between_the_intent_and_the_effect_is_outcome_unknown);
  RUN_TEST(test_an_effect_that_never_started_is_answered_as_changed_nothing);
  RUN_TEST(test_a_cut_after_the_delivery_record_sends_nothing_again);
  RUN_TEST(test_a_stored_reply_is_sent_once_across_later_boots);
  RUN_TEST(test_a_turn_the_boot_closed_counts_as_closed);
  RUN_TEST(test_a_turn_no_person_started_is_closed_never_resumed);
  RUN_TEST(test_no_resume_without_its_counter_written_first);
  RUN_TEST(test_a_resumed_turn_that_read_outside_content_stays_tainted);
  RUN_TEST(test_a_resumed_turn_keeps_its_own_items_when_the_arena_is_short);
  RUN_TEST(test_a_full_arena_at_boot_still_sends_the_stored_reply_once);
  RUN_TEST(test_the_resumed_runs_stored_answer_is_sent_at_the_next_boot);
  RUN_TEST(test_a_routines_stored_answer_is_sent_not_lost);
  RUN_TEST(test_a_refused_boot_send_is_tried_again_at_the_next_boot);
  RUN_TEST(test_the_resumed_t_item_counts_when_head_json_is_lost);
  RUN_TEST(test_a_long_stored_answer_is_sent_whole_from_its_bulk_handle);
  return UNITY_END();
}
