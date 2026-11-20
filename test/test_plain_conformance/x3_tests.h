#pragma once
// Lane RB-X3 (ruling 84 items 1, 2, 3 and 9, ruling 85 item 3, owner ruling 82 item 5): guards stop
// runaways, never honest work. On an attended turn the soft guards (16 rounds, 240 s, checked again
// every 16 rounds and every 240 s) let a progressing turn go on with a status line; a loop trips at
// once; the hard guards (64 rounds, 1,200 s, size, spend) end the turn honestly: what is done, what
// is left, one question, and the owner's yes continues from the log. Unattended turns keep today's
// limits. Run from main.cpp on the battery (txb::Battery).
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/props.h"
#include "../test_tx_budget/budget_table.h"
#include "engine_tests.h"
#include "nimbus/harness/plain/unattended.h"

namespace x3_tests {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace texts = agent::plain::texts;
using engine_tests::bodyHas;
using engine_tests::lastEnd;
using engine_tests::storedOf;

inline sp::Step readAt(size_t i, uint32_t takesMs = 0) {
  sp::Step s = sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}");
  s.takesMs = takesMs;
  return s;
}

// A progressing turn: `rounds` reads that each return something new, then "Done."; on a guard's
// last round (tool choice none) the model says what is done and what is left.
inline std::function<sp::Step(const sp::Recorded&, size_t)> progressing(size_t rounds, uint32_t takesMs = 0,
                                                                        sp::Usage u = sp::Usage{}) {
  return [rounds, takesMs, u](const sp::Recorded& r, size_t) {
    sp::Step s = r.view.toolChoice == "none" ? sp::say("Done: the reads so far. Left: the rest.")
                 : (size_t)r.step < rounds  ? readAt(r.step, takesMs)
                                            : sp::say("Done.");
    if (r.view.toolChoice == "none" || (size_t)r.step >= rounds) s.takesMs = takesMs;
    s.usage = u;
    return s;
  };
}

inline size_t countEvent(const tx::Item* e, const std::string& ev) {
  if (!e) return 0;
  return (size_t)std::count(e->end.events.begin(), e->end.events.end(), ev);
}

inline std::string limitNote(plain::GuardTrip g) {
  return std::string(texts::device_notes::kLimitReached) + plain::guardTripSlug(g) + texts::device_notes::kLimitReachedEnd;
}
inline std::string stoppedLine(plain::GuardTrip g) {
  return std::string(texts::device_notes::kLimitStopped) + plain::guardTripSlug(g) + texts::device_notes::kLimitStoppedEnd;
}

// The honest stop, as the owner and the log see it: one limit item that asks the last round for
// what is done and what is left, exactly one last round with tool choice none, its text delivered,
// and (attended) one owner line that asks the one question whether to go on.
inline void expectHonestStop(txb::Battery& b, const plain::TurnOutcome& out, plain::GuardTrip g, bool attended,
                             const char* who) {
  char msg[200];
  std::snprintf(msg, sizeof msg, "%s: expected the %s guard, tripped %s", who, plain::guardTripSlug(g),
                plain::guardTripSlug(out.trip));
  TEST_ASSERT_TRUE_MESSAGE(out.trip == g, msg);
  size_t none = 0;
  for (const auto& r : b.model.requests()) none += r.view.toolChoice == "none";
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)none, msg);
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), limitNote(g)), msg);
  TEST_ASSERT_TRUE_MESSAGE(limitNote(g).find("what is done") != std::string::npos, "the limit item asks for what is done");
  TEST_ASSERT_TRUE_MESSAGE(limitNote(g).find("what is left") != std::string::npos, "the limit item asks for what is left");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Done: the reads so far. Left: the rest.", out.text.c_str(), msg);
  const std::vector<tx::Item> items = storedOf(b);
  TEST_ASSERT_EQUAL_STRING_MESSAGE((std::string("limit:") + plain::guardTripSlug(g)).c_str(), lastEnd(items)->end.cause.c_str(), msg);
  bool line = false;
  for (const auto& s : b.sent) line |= s.text == stoppedLine(g);
  TEST_ASSERT_EQUAL_MESSAGE(attended, line, msg);
  if (!attended) return;
  const std::string q = stoppedLine(g);
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)std::count(q.begin(), q.end(), '?'), "the owner line asks one question");
}

inline constexpr int kLoopRequests = plain::kLoopRounds + 2;

// ---- a loop trips at once --------------------------------------------------------------------
// Three tool rounds in a row without a progressing call: a repeat answered from suppression, or
// the same error as the previous attempt. Four tool rounds (the first progresses), then the
// guard's last round: five requests, on every turn class.
inline void test_a_looping_turn_trips_at_once() {
  struct Case {
    const char* name;
    std::string args;
    plain::Trigger trigger;
  };
  const std::vector<Case> cases = {
      {"a repeat answered from suppression", R"({"path":"notes/a.md"})", plain::Trigger::Owner},
      {"the same error again", R"({"path":"notes/missing.md"})", plain::Trigger::Owner},
      {"a routine's repeat", R"({"path":"notes/a.md"})", plain::Trigger::Routine},
  };
  for (const Case& c : cases) {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    const std::string args = c.args;
    b.model.respondWith([args](const sp::Recorded& r, size_t) {
      return r.view.toolChoice == "none" ? sp::say("Done: the reads so far. Left: the rest.") : sp::call("read_file", args);
    });
    plain::TurnOutcome out;
    if (c.trigger == plain::Trigger::Owner) TXB_OK(b.owner("read a", out));
    else TXB_OK(b.unattended(c.trigger, "routine: read a", out));
    TEST_ASSERT_EQUAL_MESSAGE(kLoopRequests, (int)b.model.requests().size(), c.name);
    expectHonestStop(b, out, plain::GuardTrip::Loop, c.trigger == plain::Trigger::Owner, c.name);
  }
}
// Counter: a call that fails with a NEW error each round progresses (the model is trying
// something else); eight such rounds and the reply, no trip.
inline void test_a_new_error_each_round_is_not_a_loop() {
  txb::Battery b;
  b.model.respondWith([](const sp::Recorded& r, size_t) {
    return r.step < 8 ? sp::call("read_file", "{\"path\":\"notes/missing-" + std::to_string(r.step) + ".md\"}")
                      : sp::say("None of them exist.");
  });
  plain::TurnOutcome out;
  TXB_OK(b.owner("find the file", out));
  TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::None);
  TEST_ASSERT_EQUAL(9, (int)b.model.requests().size());
  TEST_ASSERT_EQUAL_STRING("None of them exist.", out.text.c_str());
}

// A seam that refuses the same call with a text that carries a changing number (a request id, a
// counter) fails the same way each time: the loop trips. Counter: a refusal whose words change is
// a new error each round, and the turn goes on to its reply.
inline void test_the_same_error_with_a_changing_number_is_still_a_loop() {
  for (bool words : {false, true}) {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    int n = 0;
    b.moreHooks = [&n, words](plain::PlainHooks& h) {
      (void)h.beforeDispatch(plain::HookSlot::Policy, [&n, words](const plain::DispatchCtx&) {
        static const char* const kWhy[] = {"busy", "locked", "offline", "moved", "renamed", "archived", "hidden", "late"};
        plain::DispatchDecision d;
        d.verdict = plain::DispatchVerdict::Refuse;
        d.text = words ? std::string("refused: ") + kWhy[n % 8] : "refused (request " + std::to_string(1000 + n) + ")";
        ++n;
        return d;
      });
    };
    b.model.respondWith([](const sp::Recorded& r, size_t) {
      if (r.view.toolChoice == "none") return sp::say("Done: the reads so far. Left: the rest.");
      return r.step < 8 ? sp::call("read_file", R"({"path":"notes/a.md"})") : sp::say("It stayed refused.");
    });
    plain::TurnOutcome out;
    TXB_OK(b.owner("read a", out));
    if (!words) {
      TEST_ASSERT_EQUAL(kLoopRequests, (int)b.model.requests().size());
      expectHonestStop(b, out, plain::GuardTrip::Loop, true, "a changing number");
    } else {
      TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::None, plain::guardTripSlug(out.trip));
      TEST_ASSERT_EQUAL(9, (int)b.model.requests().size());
    }
  }
}

// ---- a progressing turn goes on to the hard guards ------------------------------------------------
// The head at full (claude-sonnet-5-5 direct): each round's request is the due mark, 90% read from
// the cache, priced at the head's rates. 40 progressing rounds pass two soft checks and reach the
// reply: neither the round guard nor the size or spend guard stops honest work before 64.
inline plain::HeadCost sonnetAtFull() {
  const budgettable::Head& h = budgettable::heads()[0];
  const budgettable::Rates r = budgettable::ratesOf(h);
  return plain::guardCostFor(budgettable::budgetOf(h),
                             plain::HeadRates{(uint64_t)r.in, (uint64_t)r.out, (uint64_t)r.read, (uint64_t)r.write});
}
inline sp::Usage dueMarkWarm(const plain::HeadCost& c) {
  sp::Usage u;
  u.input = c.requestTokensAtTrigger / 10;
  u.cacheRead = c.requestTokensAtTrigger - u.input;
  u.output = plain::kGuardOutputTokens;
  return u;
}

inline void test_a_progressing_40_round_turn_does_not_trip_before_64() {
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.cost = sonnetAtFull();
    b.model.respondWith(progressing(40, 0, dueMarkWarm(b.cost)));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read it all", out));
    TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::None, plain::guardTripSlug(out.trip));
    TEST_ASSERT_EQUAL(41, (int)b.model.requests().size());
    TEST_ASSERT_EQUAL_STRING("Done.", out.text.c_str());
    TEST_ASSERT_EQUAL(1, (int)b.sent.size());
    const std::vector<tx::Item> items = storedOf(b);
    TEST_ASSERT_EQUAL(2, (int)countEvent(lastEnd(items), "soft:rounds"));   // at 16 and 32
    TEST_ASSERT_TRUE(out.end.microUsd > 0);
  }
  {   // 70 progressing rounds: the hard round guard at 64, then the honest stop
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.cost = sonnetAtFull();
    b.model.respondWith(progressing(70, 0, dueMarkWarm(b.cost)));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read it all", out));
    TEST_ASSERT_EQUAL(plain::kHardRounds + 1, (int)b.model.requests().size());
    expectHonestStop(b, out, plain::GuardTrip::Rounds, true, "70 rounds");
    TEST_ASSERT_EQUAL(3, (int)countEvent(lastEnd(storedOf(b)), "soft:rounds"));   // 16, 32, 48
  }
}

// ---- past 240 s: a status line, and the turn goes on ----------------------------------------------
// Each answer takes 70 s; six progressing rounds and the reply take 490 s. The soft time check at
// 280 s finds the turn progressing: the progress line (RB-E3's class, edited in place) says it is
// taking longer and still making progress, and the reply follows.
inline void test_a_progressing_turn_past_240_s_shows_a_status_line_and_goes_on() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  std::vector<std::string> shown;
  std::vector<plain::ProgressEvent> events;
  plain::ProgressDeps pd;
  pd.send = [&shown](const std::string&, const std::string& t, int64_t& id) {
    shown.push_back(t);
    id = 1;
    return true;
  };
  pd.edit = [&shown](const std::string&, int64_t, const std::string& t) {
    shown.push_back(t);
    return true;
  };
  plain::ProgressReporter rep(pd);
  b.moreHooks = [&rep, &b](plain::PlainHooks& h) { (void)plain::installProgress(h, rep, [&b] { return b.ms; }); };
  b.progressFn = [&rep, &events](const plain::ProgressEvent& e) {
    events.push_back(e);
    (void)rep.tick(e);
  };
  b.model.respondWith(progressing(6, 70000));
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("read it slowly", out));
  TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::None, plain::guardTripSlug(out.trip));
  TEST_ASSERT_EQUAL_STRING("Done.", out.text.c_str());
  TEST_ASSERT_TRUE(b.ms - t0 > plain::kSoftWallMs);
  TEST_ASSERT_EQUAL(1, (int)countEvent(lastEnd(storedOf(b)), "soft:time"));
  size_t soft = 0;
  for (const plain::ProgressEvent& e : events) soft += e.kind == plain::ProgressKind::SoftGuard && e.detail == "time";
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)soft, "one soft-guard progress event, naming the guard");
  bool longer = false;
  for (const std::string& s : shown) longer |= s.find(texts::device_notes::kProgressLong) != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(longer, "the owner saw no status line at the soft guard");
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());   // the status line is not a message after the turn
}

// A progressing attended turn reaches the 1,200 s bound: the time guard trips 180 s before it, so
// the honest stop's last round ends inside it; every soft time check on the way went on.
inline void test_a_progressing_turn_ends_honestly_inside_1200_s() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.respondWith(progressing(100, 70000));
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("read it slowly", out));
  expectHonestStop(b, out, plain::GuardTrip::Time, true, "1,200 s");
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kAttendedHardWallMs, "the turn ran past 1,200 s");
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 > plain::kAttendedHardWallMs - plain::kLastRoundReserveMs - 70000,
                           "the turn stopped long before its hard wall");
  TEST_ASSERT_EQUAL(4, (int)countEvent(lastEnd(storedOf(b)), "soft:time"));   // 240, 480, 720, 960
}

// A turn that is not progressing at a soft check stops there, honestly. The first answer (an error,
// its first attempt: progress) and the second (the same error again) each take 250 s: the check at
// 250 s goes on, the one at 500 s finds nothing new since and stops.
inline void test_a_turn_not_progressing_at_a_soft_check_stops_honestly() {
  txb::Battery b;
  b.model.respondWith([](const sp::Recorded& r, size_t) {
    sp::Step s = r.view.toolChoice == "none" ? sp::say("Done: the reads so far. Left: the rest.")
                                             : sp::call("read_file", R"({"path":"notes/missing.md"})");
    s.takesMs = 250000;
    return s;
  });
  plain::TurnOutcome out;
  TXB_OK(b.owner("read the missing file", out));
  TEST_ASSERT_EQUAL(3, (int)b.model.requests().size());
  expectHonestStop(b, out, plain::GuardTrip::Time, true, "no progress at 500 s");
  TEST_ASSERT_EQUAL(1, (int)countEvent(lastEnd(storedOf(b)), "soft:time"));
}

// A stall that would outlast every read window: each attempt hangs until the transport's own timeout
// (the request's timeoutMs, as the device's transport ends a read), at most 300 s. The exchange ends
// inside kExchangeMaxMs on an attended turn, though its time limit is 1,020 s (ruling 85 item 3).
inline void test_a_stall_ends_inside_450_s_on_an_attended_turn() {
  txb::Battery b;
  b.model.respondWith([](const sp::Recorded& r, size_t) { return sp::transportCut(std::min<uint32_t>(r.req.timeoutMs, 300000)); });
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello?", out));
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::ProviderError);
  char msg[80];
  std::snprintf(msg, sizeof msg, "the stall ran %u s", (unsigned)((b.ms - t0) / 1000));
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kExchangeMaxMs, msg);
}

// ---- the honest stop and the owner's yes ---------------------------------------------------------
// The spend guard stops a turn of large reads; the owner says yes and the next turn continues from
// the log: its request carries the stopped turn's results whole. Counter: after a turn that ended
// with its reply, the same reads are stubs in the next turn (the pointer rule is unchanged there).
inline sp::Step bigReadAt(size_t i) {
  sp::Step s = sp::call("big_read", "{\"tag\":\"t" + std::to_string(i) + "\",\"bytes\":20000}");
  s.usage.input = 20000;
  return s;
}

inline void test_a_hard_guard_stops_honestly_and_the_owners_yes_continues_from_the_log() {
  txb::Battery b;
  b.cost.inMicroPerM = 10000000;   // 10 USD per 1M: 0.20 USD a request at the 0.50 USD floor cap
  b.model.respondWith([](const sp::Recorded& r, size_t) {
    if (r.view.toolChoice == "none") return sp::say("Done: the reads so far. Left: the rest.");
    if (r.step < 3) return bigReadAt(r.step);
    return sp::say("Continuing from the fourth part.");
  });
  plain::TurnOutcome out;
  TXB_OK(b.owner("read every part", out));
  TEST_ASSERT_EQUAL(4, (int)b.model.requests().size());
  expectHonestStop(b, out, plain::GuardTrip::Spend, true, "spend");
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());   // the model's text, then the one question
  TEST_ASSERT_EQUAL_STRING(stoppedLine(plain::GuardTrip::Spend).c_str(), b.sent[1].text.c_str());
  plain::TurnOutcome yes;
  TXB_OK(b.owner("yes", yes));
  TEST_ASSERT_TRUE(yes.trip == plain::GuardTrip::None);   // the continuation's guards start fresh
  TEST_ASSERT_EQUAL_STRING("Continuing from the fourth part.", yes.text.c_str());
  const sp::Recorded& first = b.model.requests()[4];
  for (int i = 0; i < 3; ++i) {
    const std::string tag = "BULK-t" + std::to_string(i);
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(first, tag), ("the continuation lost the stopped turn's " + tag).c_str());
  }
  TEST_ASSERT_FALSE_MESSAGE(bodyHas(first, "[stub big_read"), "the continuation shows the stopped turn as stubs");
  // Counter: the same reads in a turn that ended with its reply leave the next request as stubs.
  txb::Battery c;
  c.model.respondWith([](const sp::Recorded& r, size_t) { return r.step < 3 ? bigReadAt(r.step) : sp::say("All read."); });
  plain::TurnOutcome done, next;
  TXB_OK(c.owner("read every part", done));
  TEST_ASSERT_TRUE(done.trip == plain::GuardTrip::None);
  TXB_OK(c.owner("thanks", next));
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(c.model.requests()[4], "[stub big_read"), "the counter case kept the bulk whole");
}

// The stopped turn stays whole until an attended turn finishes: an agent report in between, a
// restart in between, and a second stop before the yes each keep it (and the second stopped turn)
// whole in the request the yes opens.
inline sp::Step bigOrSay(const sp::Recorded& r, int* reads) {
  if (r.view.toolChoice == "none") return sp::say("Done: the reads so far. Left: the rest.");
  if (*reads > 0) {
    --*reads;
    return bigReadAt(100 + (size_t)r.step);   // every read its own tag: t100, t101, ...
  }
  return sp::say("Continuing.");
}
inline void test_the_stopped_turn_stays_whole_until_a_turn_finishes() {
  for (int shape = 0; shape < 3; ++shape) {
    txb::Battery b;
    b.cost.inMicroPerM = 10000000;   // 0.20 USD a request: the spend guard after three
    int reads = 3;
    b.model.respondWith([&reads](const sp::Recorded& r, size_t) { return bigOrSay(r, &reads); });
    plain::TurnOutcome out;
    TXB_OK(b.owner("read every part", out));
    TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Spend);
    const std::string firstTag = "BULK-t100";   // the first stopped turn's first read
    if (shape == 0) TXB_OK(b.unattended(plain::Trigger::AgentResult, "report: the helper is done", out));
    if (shape == 1) TXB_OK(b.boot());
    if (shape == 2) {   // the continuation stops too
      reads = 3;
      TXB_OK(b.owner("go on", out));
      TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Spend);
    }
    const size_t at = b.model.requests().size();
    plain::TurnOutcome yes;
    TXB_OK(b.owner("yes", yes));
    TEST_ASSERT_EQUAL_STRING("Continuing.", yes.text.c_str());
    const sp::Recorded& first = b.model.requests()[at];
    char msg[80];
    std::snprintf(msg, sizeof msg, "shape %d: the stopped turn left the request whole", shape);
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(first, firstTag), msg);
    TEST_ASSERT_FALSE_MESSAGE(bodyHas(first, "[stub big_read"), msg);
    // The yes finished: the next turn no longer keeps them whole.
    plain::TurnOutcome after;
    TXB_OK(b.owner("thanks", after));
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), "[stub big_read"), "a finished continuation still kept the bulk whole");
  }
}

// The owner's line after a guard stop, for every guard: one question whether to go on, never an
// order to ask again.
inline void test_the_limit_line_asks_one_question() {
  for (plain::GuardTrip g : {plain::GuardTrip::Rounds, plain::GuardTrip::Time, plain::GuardTrip::Spend,
                             plain::GuardTrip::Size, plain::GuardTrip::Loop}) {
    const std::string l = stoppedLine(g);
    TEST_ASSERT_EQUAL(1, (int)std::count(l.begin(), l.end(), '?'));
    TEST_ASSERT_TRUE(l.back() == '?');
    TEST_ASSERT_TRUE(l.find("Ask again") == std::string::npos);
    TEST_ASSERT_TRUE(l.find(plain::guardTripSlug(g)) != std::string::npos);
  }
}

// ---- unattended turns keep today's limits ------------------------------------------------------
// A routine that keeps making progress stops at 16 rounds, and one with slow answers at 240 s
// (inside 420 s); no soft check runs, no owner line goes out, its last round names what is left.
inline void test_an_unattended_turn_keeps_todays_limits() {
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.model.respondWith(progressing(40));
    plain::TurnOutcome out;
    TXB_OK(b.unattended(plain::Trigger::Routine, "routine: read it all", out));
    TEST_ASSERT_EQUAL(plain::kDefaultRounds + 1, (int)b.model.requests().size());
    expectHonestStop(b, out, plain::GuardTrip::Rounds, false, "routine rounds");
    TEST_ASSERT_EQUAL(0, (int)countEvent(lastEnd(storedOf(b)), "soft:rounds"));
  }
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.model.respondWith(progressing(40, 70000));
    const uint32_t t0 = b.ms;
    plain::TurnOutcome out;
    TXB_OK(b.unattended(plain::Trigger::AgentResult, "report: read it all", out));
    expectHonestStop(b, out, plain::GuardTrip::Time, false, "agent report time");
    TEST_ASSERT_TRUE(b.ms - t0 <= plain::kHardWallMs);
    TEST_ASSERT_EQUAL(0, (int)countEvent(lastEnd(storedOf(b)), "soft:time"));
  }
}

// RB-U1a's routine ceiling and these guards together: a routine fire keeps its own ceiling (6
// rounds, 90 s soft, inside 180 s; applied over today's unattended limits), and an owner turn on the
// same chat right after it keeps the attended soft and hard limits (40 progressing rounds, no trip).
inline void test_a_routine_keeps_its_ceiling_and_an_owner_turn_its_soft_guards() {
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.routineCeiling = plain::routineCeiling();
    b.model.respondWith(progressing(40));
    plain::TurnOutcome out;
    TXB_OK(b.unattended(plain::Trigger::Routine, "routine: read it all", out));
    TEST_ASSERT_EQUAL(plain::kRoutineRounds + 1, (int)b.model.requests().size());
    expectHonestStop(b, out, plain::GuardTrip::Rounds, false, "routine ceiling rounds");
    const size_t before = b.model.requests().size();
    b.model.respondWith(progressing(before + 40));
    plain::TurnOutcome own;
    TXB_OK(b.owner("now you read it all", own));
    TEST_ASSERT_TRUE_MESSAGE(own.trip == plain::GuardTrip::None, plain::guardTripSlug(own.trip));
    TEST_ASSERT_EQUAL(41, (int)(b.model.requests().size() - before));
    TEST_ASSERT_EQUAL(2, (int)countEvent(lastEnd(storedOf(b)), "soft:rounds"));
  }
  {   // time: 40 s answers; the routine's time guard at 90 s, its last round inside 180 s
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.routineCeiling = plain::routineCeiling();
    b.model.respondWith(progressing(40, 40000));
    const uint32_t t0 = b.ms;
    plain::TurnOutcome out;
    TXB_OK(b.unattended(plain::Trigger::Routine, "routine: read it slowly", out));
    expectHonestStop(b, out, plain::GuardTrip::Time, false, "routine ceiling time");
    TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kRoutineHardMs, "the routine ran past its 180 s");
  }
  // The limits themselves: the ceiling lowers the unattended round and time guards; an empty one
  // leaves an attended turn's soft and hard limits as they are.
  plain::GuardLimits u, a;
  TXB_OK(plain::guardLimitsFor(0, plain::HeadCost{}, false, u));
  TXB_OK(plain::guardLimitsFor(0, plain::HeadCost{}, true, a));
  const plain::GuardLimits a0 = a;
  plain::applyCeiling(plain::routineCeiling(), u, u.lastRoundWallMs);
  TEST_ASSERT_EQUAL(plain::kRoutineRounds, u.hardRounds);
  TEST_ASSERT_EQUAL(plain::kRoutineSoftMs, u.timeLimitMs);
  TEST_ASSERT_EQUAL(plain::kRoutineHardMs, u.lastRoundWallMs);
  TEST_ASSERT_EQUAL(plain::kRoutineMaxMicroUsd, u.maxSpendMicroUsd);
  plain::applyCeiling(plain::TurnCeiling{}, a, a.lastRoundWallMs);
  TEST_ASSERT_EQUAL(a0.hardRounds, a.hardRounds);
  TEST_ASSERT_EQUAL(a0.timeLimitMs, a.timeLimitMs);
  TEST_ASSERT_EQUAL(a0.lastRoundWallMs, a.lastRoundWallMs);
  TEST_ASSERT_EQUAL(a0.rounds, a.rounds);
}

// ---- the guard table at full, per head class ------------------------------------------------------
// Every head class of RB-T4's table at Profile::Full: the attended and unattended limits follow the
// formulas, and a progressing attended turn of 40 rounds at the due mark (warm) fits both caps.
// The rows print for PR_BODY.md.
inline void test_the_guard_table_at_full_per_head_class() {
  for (const budgettable::Head& h : budgettable::heads()) {
    const tx::HeadBudget bud = budgettable::budgetOf(h);
    const budgettable::Rates r = budgettable::ratesOf(h);
    const plain::HeadCost c =
        plain::guardCostFor(bud, plain::HeadRates{(uint64_t)r.in, (uint64_t)r.out, (uint64_t)r.read, (uint64_t)r.write});
    TEST_ASSERT_EQUAL_MESSAGE(bud.tokens.due, c.requestTokensAtTrigger, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(bud.tokens.aim, c.targetTokens, h.name);
    plain::GuardLimits att, un;
    TXB_OK(plain::guardLimitsFor(0, c, true, att));
    TXB_OK(plain::guardLimitsFor(0, c, false, un));
    const uint64_t w = c.warmRequestMicroUsd, k = c.coldRequestMicroUsd;
    TEST_ASSERT_EQUAL_MESSAGE(16, att.rounds, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kHardRounds, att.hardRounds, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kSoftWallMs, att.softWallMs, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kAttendedHardWallMs - plain::kLastRoundReserveMs, att.timeLimitMs, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kAttendedHardWallMs, att.lastRoundWallMs, h.name);
    TEST_ASSERT_TRUE_MESSAGE(att.attended && !un.attended, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(std::max<uint64_t>(4ull * bud.tokens.aim, 64ull * bud.tokens.due), att.maxPromptTokens, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(std::max<uint64_t>(plain::kMinSpendCapMicroUsd, 64 * w + k), att.maxSpendMicroUsd, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(16, un.hardRounds, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kSoftWallMs, un.timeLimitMs, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(plain::kHardWallMs, un.lastRoundWallMs, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(std::max<uint64_t>(4ull * bud.tokens.aim, 16ull * bud.tokens.due), un.maxPromptTokens, h.name);
    TEST_ASSERT_EQUAL_MESSAGE(std::max<uint64_t>(plain::kMinSpendCapMicroUsd, 16 * w + k), un.maxSpendMicroUsd, h.name);
    // A progressing 40-round attended turn at the due mark: under both caps.
    TEST_ASSERT_TRUE_MESSAGE(40ull * bud.tokens.due < att.maxPromptTokens, h.name);
    TEST_ASSERT_TRUE_MESSAGE(40ull * w + k < att.maxSpendMicroUsd, h.name);
    char row[320];
    std::snprintf(row, sizeof row, "| %s | %u | %u | %u / %u | %u / %u | %.2f / %.2f |", h.name, (unsigned)bud.tokens.aim,
                  (unsigned)bud.tokens.due, (unsigned)att.maxPromptTokens, (unsigned)un.maxPromptTokens,
                  (unsigned)w, (unsigned)k, att.maxSpendMicroUsd / 1e6, un.maxSpendMicroUsd / 1e6);
    TEST_MESSAGE(row);
  }
}

// ---- the limits themselves -----------------------------------------------------------------------
// The soft check: due by rounds or by time; progressing goes on and moves the next check on by one
// interval; not progressing stops with the guard reached. A loop trips first, on every turn.
inline void test_the_soft_check_and_the_loop_rule() {
  plain::GuardLimits l;
  TXB_OK(plain::guardLimitsFor(0, plain::HeadCost{}, true, l));
  plain::GuardProgress p;
  plain::GuardState s;
  plain::GuardTrip which = plain::GuardTrip::None;
  s.rounds = 15;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, p, which) == plain::SoftCheck::None);
  s.rounds = 16;
  p.noteCall("read_file", "{}", false, false, "alpha");
  p.endRound();
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, p, which) == plain::SoftCheck::GoOn);
  TEST_ASSERT_TRUE(which == plain::GuardTrip::Rounds);
  TEST_ASSERT_EQUAL(32, p.nextRounds);
  s.rounds = 31;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, p, which) == plain::SoftCheck::None);
  s.rounds = 32;   // nothing new since the check at 16
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, p, which) == plain::SoftCheck::Stop);
  TEST_ASSERT_TRUE(which == plain::GuardTrip::Rounds);
  // New text counts as progress; the same text again does not.
  plain::GuardProgress t;
  t.noteText("first finding");
  s.rounds = 16;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, t, which) == plain::SoftCheck::GoOn);
  t.noteText("first finding");
  s.rounds = 32;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, t, which) == plain::SoftCheck::Stop);
  // Time: 240 s, then every 240 s.
  plain::GuardProgress m;
  s = plain::GuardState{};
  s.elapsedMs = plain::kSoftWallMs - plain::kMinRequestMs;
  m.noteCall("read_file", "{}", false, false, "alpha");
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, m, which) == plain::SoftCheck::GoOn);
  TEST_ASSERT_TRUE(which == plain::GuardTrip::Time);
  TEST_ASSERT_EQUAL(2 * plain::kSoftWallMs, m.nextMs);
  // One long round past two intervals: the next check is the next interval after now, not now.
  m.noteCall("read_file", "{\"o\":1}", false, false, "beta");
  s.elapsedMs = 3 * plain::kSoftWallMs + 30000;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(l, s, m, which) == plain::SoftCheck::GoOn);
  TEST_ASSERT_EQUAL(4 * plain::kSoftWallMs, m.nextMs);
  // Unattended: never a soft check.
  plain::GuardLimits u;
  TXB_OK(plain::guardLimitsFor(0, plain::HeadCost{}, false, u));
  s.rounds = 16;
  TEST_ASSERT_TRUE(plain::checkSoftGuards(u, s, m, which) == plain::SoftCheck::None);
  // The loop: three tool rounds in a row without a progressing call; one progressing call resets it.
  plain::GuardProgress lp;
  lp.noteCall("read_file", "{\"path\":\"x\"}", false, true, "no such file: x");   // first attempt: progress
  lp.endRound();
  TEST_ASSERT_EQUAL(0, lp.idle);
  lp.noteCall("read_file", "{\"path\":\"x\"}", false, true, "no such file: x");   // the same error again
  lp.endRound();
  lp.noteCall("read_file", "{\"path\":\"y\"}", true, false, "same as earlier");    // suppressed
  lp.endRound();
  TEST_ASSERT_EQUAL(2, lp.idle);
  lp.noteCall("read_file", "{\"path\":\"x\"}", false, false, "now it exists");    // a new outcome
  lp.endRound();
  TEST_ASSERT_EQUAL(0, lp.idle);
  for (int i = 0; i < 3; ++i) {
    lp.noteCall("read_file", "{\"path\":\"y\"}", true, false, "same as earlier");
    lp.endRound();
  }
  TEST_ASSERT_EQUAL(3, lp.idle);
  lp.noteText("a new finding");   // new text is progress at a soft check, never for the loop rule
  TEST_ASSERT_EQUAL(3, lp.idle);
  TEST_ASSERT_TRUE(lp.progressed);
  plain::GuardState ls;
  ls.idleRounds = lp.idle;
  plain::GuardTrip trip = plain::GuardTrip::None;
  TXB_OK(plain::checkGuards(u, ls, trip));
  TEST_ASSERT_TRUE(trip == plain::GuardTrip::Loop);
  TXB_OK(plain::checkGuards(l, ls, trip));
  TEST_ASSERT_TRUE(trip == plain::GuardTrip::Loop);
  ls.idleRounds = plain::kLoopRounds - 1;
  TXB_OK(plain::checkGuards(l, ls, trip));
  TEST_ASSERT_TRUE(trip == plain::GuardTrip::None);
}

inline void run() {
  RUN_TEST(test_the_soft_check_and_the_loop_rule);
  RUN_TEST(test_a_looping_turn_trips_at_once);
  RUN_TEST(test_a_new_error_each_round_is_not_a_loop);
  RUN_TEST(test_the_same_error_with_a_changing_number_is_still_a_loop);
  RUN_TEST(test_a_progressing_40_round_turn_does_not_trip_before_64);
  RUN_TEST(test_a_progressing_turn_past_240_s_shows_a_status_line_and_goes_on);
  RUN_TEST(test_a_progressing_turn_ends_honestly_inside_1200_s);
  RUN_TEST(test_a_turn_not_progressing_at_a_soft_check_stops_honestly);
  RUN_TEST(test_a_stall_ends_inside_450_s_on_an_attended_turn);
  RUN_TEST(test_a_hard_guard_stops_honestly_and_the_owners_yes_continues_from_the_log);
  RUN_TEST(test_the_stopped_turn_stays_whole_until_a_turn_finishes);
  RUN_TEST(test_the_limit_line_asks_one_question);
  RUN_TEST(test_an_unattended_turn_keeps_todays_limits);
  RUN_TEST(test_a_routine_keeps_its_ceiling_and_an_owner_turn_its_soft_guards);
  RUN_TEST(test_the_guard_table_at_full_per_head_class);
}

}  // namespace x3_tests
