#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/owner_first.h"
#include "nimbus/orch/transcript.h"

// The owner is never queued behind background work, and the post-update awareness
// turn is bounded (Lumi, v4.6.0, 26 Oct 2026: the awareness turn ran the 8-round loop
// for four minutes, a fold followed in the same pump pass, and the owner's message,
// sent during the turn, was answered 5.5 minutes later).
//
// Part 1 drives the REAL gate (owner_first.h) with a model of the tg_poll cycle in
// pollTask order: Telegram poll (an owner turn when a message waits), then the pump
// (staged system turn, then the fold pump), then the pause. Since v5.2.0 (lane RB-Q2a)
// the staged work is the fixed update notice, with no model call; the gate is unchanged.

using agent::JobEngine;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using nimbus::orch::OwnerFirstGate;

void setUp() {}
void tearDown() {}

// ---- part 1: the gate in the tg_poll cycle -----------------------------------------
namespace {
struct Times {
  uint32_t ownerTurnMs = 60000;    // one owner turn (two rounds on Lumi)
  uint32_t systemTurnMs = 90000;   // the bounded awareness turn (<= 3 requests)
  uint32_t foldMs = 74000;         // Lumi's fold
  uint32_t longPollMs = 25000;     // a long-poll that nothing answers
  uint32_t pauseMs = 1000;
};

struct Cycle {
  Times tm;
  bool gated = true;               // false: v4.6.0 (no gate)
  OwnerFirstGate gate;
  uint32_t now = 100000;
  uint32_t stagedAt = 0;
  bool staged = false, foldDue = false, pageHeld = false;
  std::vector<uint32_t> arrivals;  // owner messages (ms), ascending
  size_t next = 0;
  std::vector<uint32_t> latency;   // per answered message: reply time - arrival
  uint32_t systemStart = 0, systemEnd = 0, foldStart = 0;
  int systemTurns = 0, folds = 0;
  int turns = 0, turnsAtFold = -1;   // owner and system turns ended, and the count when the fold began
  bool turnThisCycle = false;        // a turn ended in this cycle (its reply not drained yet)
  int foldsAfterTurnInCycle = 0;     // folds that started before the cycle's reply drain

  void ownerTurn(uint32_t arrival) {
    now += tm.ownerTurnMs;
    gate.noteTurnEnd(now);
    latency.push_back(now - arrival);
    turns++;
    turnThisCycle = true;
  }
  // The Telegram poll: a waiting message is answered at once; otherwise the long
  // poll waits until a message arrives or it times out.
  void poll() {
    if (next < arrivals.size() && arrivals[next] <= now) {
      ownerTurn(arrivals[next++]);
      return;
    }
    const uint32_t until = now + tm.longPollMs;
    if (next < arrivals.size() && arrivals[next] < until) {
      now = arrivals[next];
      ownerTurn(arrivals[next++]);
      return;
    }
    now = until;
  }
  void pump() {
    const bool sysOk = gated ? gate.systemTurnMayStart(pageHeld, now, stagedAt) : true;
    if (staged && sysOk) {
      staged = false;
      systemStart = now;
      now += tm.systemTurnMs;
      systemEnd = now;
      gate.noteTurnEnd(now);
      systemTurns++;
      turns++;
      turnThisCycle = true;
      foldDue = true;   // the turn grew the history past the fold trigger (Lumi)
    }
    if (!foldDue) gate.noteFoldNotDue();
    if (foldDue && (!gated || gate.foldMayStart(pageHeld, now))) {
      foldDue = false;
      foldStart = now;
      turnsAtFold = turns;
      foldsAfterTurnInCycle += turnThisCycle;
      now += tm.foldMs;
      folds++;
    }
    gate.notePumpEnd();
  }
  void stage() {
    staged = true;
    stagedAt = now;
  }
  void runUntil(uint32_t end) {
    while (now < end) {
      turnThisCycle = false;
      poll();
      pump();
      now += tm.pauseMs;
    }
  }
};
}  // namespace

// The counterfactual, pinned so the model is honest: v4.6.0's order (no gate, the
// unbounded 257 s turn) reproduces Lumi's 5.5-minute wait.
static void test_model_reproduces_lumi_without_the_gate() {
  Cycle c;
  c.gated = false;
  c.tm.systemTurnMs = 257000;   // 120-377 s on Lumi
  c.tm.ownerTurnMs = 68000;     // 460-528 s on Lumi
  c.stage();
  c.arrivals = {c.now + 78000};   // 14:58:51, uptime ~192 s, staged at ~114 s
  c.runUntil(c.now + 900000);
  TEST_ASSERT_EQUAL_INT(1, (int)c.latency.size());
  TEST_ASSERT_TRUE(c.latency[0] > 300000);
  TEST_ASSERT_TRUE(c.foldStart < c.arrivals[0] + c.latency[0]);   // the fold ran before the reply
}

// The DoD leg: an owner message within 10 s of the update is answered first, at
// normal latency; the awareness turn and the fold come after.
static void test_owner_within_10s_of_the_update_is_answered_first() {
  for (uint32_t delay : {0u, 3000u, 10000u}) {
    Cycle c;
    c.stage();
    c.arrivals = {c.now + delay};
    c.runUntil(c.now + 900000);
    TEST_ASSERT_EQUAL_INT(1, (int)c.latency.size());
    TEST_ASSERT_TRUE(c.latency[0] <= c.tm.ownerTurnMs + c.tm.pauseMs);
    const uint32_t replied = c.arrivals[0] + c.latency[0];
    TEST_ASSERT_EQUAL_INT(1, c.systemTurns);
    TEST_ASSERT_TRUE(c.systemStart >= replied);
    TEST_ASSERT_TRUE(c.foldStart >= c.systemEnd);
  }
}

// Lumi's own timeline with the gate: the message 78 s after staging is answered at
// normal latency, before the awareness turn and before any fold.
static void test_lumi_timeline_with_the_gate() {
  Cycle c;
  c.stage();
  c.arrivals = {c.now + 78000};
  c.runUntil(c.now + 900000);
  TEST_ASSERT_TRUE(c.latency[0] <= c.tm.ownerTurnMs + c.tm.pauseMs);
  TEST_ASSERT_TRUE(c.systemStart >= c.arrivals[0] + c.latency[0]);
  TEST_ASSERT_EQUAL_INT(1, c.folds);
}

// A message that lands while the bounded turn runs waits for that turn only, never
// for a fold behind it as well; the fold runs after the owner's reply.
static void test_message_during_the_system_turn_skips_the_fold_queue() {
  Cycle c;
  c.stage();
  c.runUntil(c.now + OwnerFirstGate::kQuietMs + c.tm.longPollMs + 2000);
  TEST_ASSERT_EQUAL_INT(1, c.systemTurns);
  c.arrivals = {c.systemStart + 5000};
  c.runUntil(c.now + 900000);
  TEST_ASSERT_EQUAL_INT(1, (int)c.latency.size());
  TEST_ASSERT_TRUE(c.latency[0] <= c.tm.systemTurnMs + c.tm.ownerTurnMs + c.tm.pauseMs);
  TEST_ASSERT_TRUE(c.foldStart >= c.arrivals[0] + c.latency[0]);
}

// Not starved: an owner chatting every minute holds the turn back, and it runs once
// the owner has been quiet long enough.
static void test_system_turn_runs_once_the_owner_is_quiet() {
  Cycle c;
  c.stage();
  for (int i = 0; i < 10; i++) c.arrivals.push_back(c.now + 5000 + (uint32_t)i * 90000);
  c.runUntil(c.now + 1800000);
  TEST_ASSERT_EQUAL_INT(1, c.systemTurns);
  const uint32_t lastReply = c.arrivals.back() + c.latency.back();
  TEST_ASSERT_TRUE(c.systemStart >= lastReply + OwnerFirstGate::kQuietMs);
  TEST_ASSERT_TRUE(c.systemStart <= lastReply + OwnerFirstGate::kQuietMs + c.tm.longPollMs + 2000);
  for (uint32_t l : c.latency) TEST_ASSERT_TRUE(l <= c.tm.ownerTurnMs + c.tm.pauseMs);
}

// An owner in a short conversation is never queued behind a fold that came due: each
// follow-up within two minutes of the last reply goes first, on a Telegram long-poll
// loop and on a device without Telegram (web and voice only: no poll wait at all).
// The fold runs once the owner has been quiet for two minutes.
static void test_owner_conversation_never_waits_behind_a_fold() {
  for (uint32_t longPoll : {25000u, 1000u}) {
    Cycle c;
    c.tm.longPollMs = longPoll;
    c.foldDue = true;   // the last turn pushed the chat past the fold trigger
    c.gate.noteTurnEnd(c.now);
    for (int i = 0; i < 5; i++) c.arrivals.push_back(c.now + 20000 + (uint32_t)i * 100000);
    c.runUntil(c.now + 1800000);
    TEST_ASSERT_EQUAL_INT(5, (int)c.latency.size());
    for (uint32_t l : c.latency) TEST_ASSERT_TRUE(l <= c.tm.ownerTurnMs + c.tm.pauseMs);
    const uint32_t lastReply = c.arrivals.back() + c.latency.back();
    TEST_ASSERT_EQUAL_INT(1, c.folds);
    TEST_ASSERT_TRUE(c.foldStart >= lastReply + OwnerFirstGate::kQuietMs);
    TEST_ASSERT_TRUE(c.foldStart <= lastReply + OwnerFirstGate::kQuietMs + c.tm.longPollMs + 2000);
  }
}

// A brisk conversation still gets its fold (merge review): the rows a history shift
// hid are in neither the summary nor the window until a fold takes them, so the wait
// is bounded - at most kFoldMaxDeferTurns turns or kFoldMaxDeferMs - and the fold then
// starts only in a pass after a poll with no turn ending (the last reply went out,
// nothing runs). The owner waits behind it once, for at most one fold.
static void test_brisk_conversation_gets_its_fold_within_the_bound() {
  for (uint32_t gap : {40000u, 60000u, 100000u}) {   // a message every 40, 60 or 100 s
    Cycle c;
    c.tm.ownerTurnMs = 20000;
    c.foldDue = true;
    c.gate.noteTurnEnd(c.now);
    const uint32_t dueAt = c.now;
    for (int i = 0; i < 30; i++) c.arrivals.push_back(c.now + 5000 + (uint32_t)i * gap);
    c.runUntil(c.arrivals.back() + 60000);
    char msg[96];
    std::snprintf(msg, sizeof msg, "gap %u s: folds=%d turnsAtFold=%d start=+%u s", (unsigned)(gap / 1000),
                  c.folds, c.turnsAtFold, (unsigned)((c.foldStart - dueAt) / 1000));
    TEST_MESSAGE(msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.folds, msg);
    // With a gap in the chat (a poll that finds nothing) the fold takes it within the
    // bound, after the last reply went out; with no gap at all, at twice the bound,
    // right after a turn (that one reply arrives a fold later).
    const bool gapless = gap < c.tm.ownerTurnMs + c.tm.longPollMs;
    const uint32_t tier = gapless ? 2 : 1;
    TEST_ASSERT_TRUE_MESSAGE(c.turnsAtFold <= (int)(tier * OwnerFirstGate::kFoldMaxDeferTurns) + 1 ||
                                 c.foldStart - dueAt <= tier * OwnerFirstGate::kFoldMaxDeferMs + gap + c.tm.longPollMs,
                             msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(gapless ? 1 : 0, c.foldsAfterTurnInCycle, msg);
    for (uint32_t l : c.latency) TEST_ASSERT_TRUE_MESSAGE(l <= c.tm.ownerTurnMs + c.tm.foldMs + c.tm.pauseMs, msg);
  }
}

// The gate's own rules, edge by edge.
static void test_gate_rules() {
  const uint32_t q = OwnerFirstGate::kQuietMs;
  OwnerFirstGate g;
  TEST_ASSERT_TRUE(g.foldMayStart(false, 5000));    // no turn yet this boot
  TEST_ASSERT_FALSE(g.foldMayStart(true, 5000));    // a held inbound page waits too
  g.noteTurnEnd(10000);
  g.notePumpEnd();
  TEST_ASSERT_FALSE(g.foldMayStart(false, 10000 + q - 1));   // the wait starts here
  TEST_ASSERT_TRUE(g.foldMayStart(false, 10000 + q));
  // The bound by turns: deferred, then six turns end; it starts only in a pass that
  // follows a poll with no turn ending.
  OwnerFirstGate t;
  uint32_t now = 1000;
  t.noteTurnEnd(now);
  t.notePumpEnd();
  TEST_ASSERT_FALSE(t.foldMayStart(false, now));
  for (uint32_t i = 0; i < OwnerFirstGate::kFoldMaxDeferTurns; i++) {
    now += 30000;
    t.noteTurnEnd(now);
    TEST_ASSERT_FALSE(t.foldMayStart(false, now));   // a turn ended in this pass
    t.notePumpEnd();
  }
  TEST_ASSERT_TRUE(t.foldMayStart(false, now + 1000));   // waited out, after a quiet pass
  // The bound by time: one turn a minute for ten minutes.
  OwnerFirstGate m;
  now = 1000;
  m.noteTurnEnd(now);
  m.notePumpEnd();
  TEST_ASSERT_FALSE(m.foldMayStart(false, now));
  for (int i = 0; i < 3; i++) {
    now += 59000;
    m.noteTurnEnd(now);
    m.notePumpEnd();
    TEST_ASSERT_FALSE(m.foldMayStart(false, now + 1000));
  }
  TEST_ASSERT_TRUE(m.foldMayStart(false, 1000 + OwnerFirstGate::kFoldMaxDeferMs));
  // A fold that stopped being due starts its own wait next time.
  m.noteTurnEnd(now = 2000000);
  m.notePumpEnd();
  TEST_ASSERT_FALSE(m.foldMayStart(false, now + 1000));
  m.noteFoldNotDue();
  const uint32_t later = now + 1000 + OwnerFirstGate::kFoldMaxDeferMs;   // the old wait would be out
  m.noteTurnEnd(later - 1000);
  m.notePumpEnd();
  TEST_ASSERT_FALSE(m.foldMayStart(false, later));   // a fresh wait starts instead
  // No gap at all (a turn ends in every pass): at twice the bound it starts right
  // after a turn, never inside one.
  OwnerFirstGate b;
  now = 1000;
  b.noteTurnEnd(now);
  TEST_ASSERT_FALSE(b.foldMayStart(false, now));
  for (uint32_t i = 0; i + 1 < 2 * OwnerFirstGate::kFoldMaxDeferTurns; i++) {
    b.notePumpEnd();
    b.noteTurnEnd(now += 40000);
    TEST_ASSERT_FALSE(b.foldMayStart(false, now));
  }
  b.notePumpEnd();
  b.noteTurnEnd(now += 40000);
  TEST_ASSERT_TRUE(b.foldMayStart(false, now));
  // The staged turn: quiet since it was staged and since the last turn.
  TEST_ASSERT_FALSE(g.systemTurnMayStart(false, 10000 + q, 10000 + q - 5));
  TEST_ASSERT_TRUE(g.systemTurnMayStart(false, 10000 + q, 5000));
  TEST_ASSERT_FALSE(g.systemTurnMayStart(true, 10000 + q, 5000));
  // millis() wraps: the quiet window still reads as elapsed time.
  OwnerFirstGate w;
  w.noteTurnEnd(0xFFFFFF00u);
  TEST_ASSERT_FALSE(w.systemTurnMayStart(false, 0xFFFFFF00u + q - 1, 0xFFFFFF00u - q));
  TEST_ASSERT_TRUE(w.systemTurnMayStart(false, 0xFFFFFF00u + q, 0xFFFFFF00u));
}

// ---- part 2: the post-update turn is gone ------------------------------------------
// The bounded awareness turn (v4.6.1) was deleted in v5.2.0 (lane RB-Q2a): the update
// notice is a fixed string with no model call, delivered once this gate lets it start
// (test_unattended_policy proves no provider request). The gate rules above still decide
// when it goes out.

// The idle re-check of a parked provider is one more blocking request on the task that reads
// the owner's messages: it waits for a quiet moment and for a held album, and no bound forces it.
static void test_the_idle_recheck_waits_for_the_owner() {
  using nimbus::orch::OwnerFirstGate;
  OwnerFirstGate g;
  TEST_ASSERT_TRUE(g.parkRecheckMayStart(false, 1000));    // a fresh device: nothing to wait for
  TEST_ASSERT_FALSE(g.parkRecheckMayStart(true, 1000));    // an album is arriving
  uint32_t now = 500000;
  g.noteTurnEnd(now);
  TEST_ASSERT_FALSE(g.parkRecheckMayStart(false, now));
  TEST_ASSERT_FALSE(g.parkRecheckMayStart(false, now + OwnerFirstGate::kQuietMs - 1));
  TEST_ASSERT_TRUE(g.parkRecheckMayStart(false, now + OwnerFirstGate::kQuietMs));
  // A brisk chat past the fold's bounds: the fold is forced, the re-check is not.
  for (int i = 0; i < 40; i++) {
    now += 60000;
    g.noteTurnEnd(now);
    g.notePumpEnd();
    TEST_ASSERT_FALSE(g.parkRecheckMayStart(false, now + 1000));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_model_reproduces_lumi_without_the_gate);
  RUN_TEST(test_owner_within_10s_of_the_update_is_answered_first);
  RUN_TEST(test_lumi_timeline_with_the_gate);
  RUN_TEST(test_message_during_the_system_turn_skips_the_fold_queue);
  RUN_TEST(test_system_turn_runs_once_the_owner_is_quiet);
  RUN_TEST(test_owner_conversation_never_waits_behind_a_fold);
  RUN_TEST(test_brisk_conversation_gets_its_fold_within_the_bound);
  RUN_TEST(test_gate_rules);
  RUN_TEST(test_the_idle_recheck_waits_for_the_owner);
  return UNITY_END();
}
