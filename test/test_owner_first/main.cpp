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
// (staged system turn, then the fold pump), then the pause. Part 2 drives the REAL
// engine: a bounded turn runs at most two tool rounds with a small tool budget, and
// single-shot while the head is on a fallback provider.

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

// ---- part 2: the bounded turn in the engine ----------------------------------------
namespace {
const char* kQuietTurn = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";

struct MiniRig {
  FakePlatform plat;
  FakeConfig cfg;
  std::set<std::string> parked;
  struct Call {
    std::string host;
    bool tools = false;
    int maxRounds = -1;
    size_t totalBytes = 0, resultBytes = 0;
    std::string instructions;
    int requests = 0;          // runLoop: model requests the real loop controller made
    size_t replayBytes = 0;    // ...and the most replayed tool bytes any of them carried
  };
  bool runLoop = false;   // the host drives the real loop controller with the engine's caps

  // A model that always asks for one more tool with a 3 KB result until its tools
  // are taken away, run through runHeadLoop with the config the engine handed over.
  static void driveLoop(const agent::HeadTools& t, Call& c) {
    nimbus::orch::Transcript tr;
    tr.addUser("[FIRMWARE UPDATED]");
    nimbus::orch::HeadLoopHooks h;
    h.nowMs = [] { return 1000u; };
    h.transcript = &tr;
    h.dispatch = [](const nimbus::orch::HeadToolCall& call) {
      nimbus::orch::HeadToolResult r;
      r.id = call.id;
      r.name = call.name;
      r.output = std::string(3000, 'n');
      return r;
    };
    h.step = [&c, &tr](bool allowTools, const std::vector<nimbus::orch::HeadToolResult>&, uint32_t,
                       const std::string&) {
      c.requests++;
      c.replayBytes = std::max(c.replayBytes, tr.toolBytes());
      nimbus::orch::HeadStep st;
      if (!allowTools) {
        st.finished = true;
        st.finalTurn = "{\"reply\":\"\"}";
        return st;
      }
      st.toolCalls.push_back({"c" + std::to_string(c.requests), "memory_search", "{}"});
      return st;
    };
    nimbus::orch::runHeadLoop(t.cfg, h);
  }
  std::vector<Call> calls;
  std::vector<std::string> delivered;
  std::unique_ptr<JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;

  MiniRig() {
    JobEngine::Deps jd;
    jd.platform = plat.contract();
    jd.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    jobs.reset(new JobEngine(std::move(jd)));
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    d.composeInputs = [](const std::string&) { return agent::ComposeInputs{}; };
    d.firstAllowedChat = [] { return std::string("1001"); };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) {
      nimbus::orch::Principal p;
      p.ns = nimbus::orch::nsForChat(chat, false);
      return p;
    };
    d.hosts.parked = [this](const std::string& h, agent::providers::HostPark*) { return parked.count(h) > 0; };
    for (const char* h : {"openai", "anthropic", "mistral"}) {
      const std::string host = h;
      d.hosts.add(host, [this, host](std::string&, const std::string& ins, const std::string&, std::string& out,
                                     std::string&, const agent::HeadTools* t, nimbus::orch::TokenUsage*) {
        Call c;
        c.host = host;
        c.tools = t != nullptr;
        if (t) {
          c.maxRounds = t->cfg.maxRounds;
          c.totalBytes = t->cfg.maxTotalToolBytes;
          c.resultBytes = t->cfg.maxToolResultBytes;
          if (runLoop) driveLoop(*t, c);
        }
        c.instructions = ins;
        calls.push_back(c);
        out = kQuietTurn;
        return true;
      });
    }
    eng.reset(new TurnEngine(std::move(d)));
  }
  nimbus::orch::FireOutcome bounded() {
    return eng->injectBoundedTurn("1001", "[FIRMWARE UPDATED] v4.6.1", "firmware-update", TurnEngine::TurnBounds{});
  }
};
}  // namespace

// A healthy head: the loop runs, at most two rounds, a 4 KB tool budget, 2 KB per
// result - never the owner's configured loop (12 rounds, 24 KB here; 8 on Lumi).
static void test_bounded_turn_never_runs_the_owner_loop() {
  MiniRig r;
  const nimbus::orch::FireOutcome o = r.bounded();
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(1, (int)r.calls.size());
  TEST_ASSERT_TRUE(r.calls[0].tools);
  TEST_ASSERT_EQUAL_INT(2, r.calls[0].maxRounds);
  TEST_ASSERT_EQUAL_UINT32(4096, (uint32_t)r.calls[0].totalBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)r.calls[0].resultBytes);
  // The prompt states the same cap the loop runs (CUM-514's rule).
  TEST_ASSERT_TRUE(r.calls[0].instructions.find("several rounds (at most 2)") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(0, (int)r.delivered.size());   // quiet: silence is a valid outcome
  // The bounds end with the turn: the next scheduled turn has the configured loop.
  r.eng->injectScheduledTurn("1001", "check the weather", "morning");
  TEST_ASSERT_EQUAL_INT(12, r.calls.back().maxRounds);
  TEST_ASSERT_EQUAL_UINT32(24576, (uint32_t)r.calls.back().totalBytes);
  // An owner who tuned the loop below the bound keeps the tighter value.
  MiniRig small;
  small.cfg.rounds = 1;
  small.cfg.totalCap = 2048;
  small.bounded();
  TEST_ASSERT_EQUAL_INT(1, small.calls[0].maxRounds);
  TEST_ASSERT_EQUAL_UINT32(2048, (uint32_t)small.calls[0].totalBytes);
}

// The token bound, end to end: the engine's bounded caps through the REAL loop
// controller, against a model that would loop forever. At most three requests (two
// tool rounds, then the forced answer), and no request replays more than two
// clamped results - so every request is the base prompt plus a few KB, where the
// v4.6.0 turn sent eight ever-growing requests (166,975 input tokens on Lumi). An
// owner turn under the same model runs the configured loop, the counter-case.
static void test_bounded_turn_token_bound_through_the_loop() {
  MiniRig r;
  r.runLoop = true;
  r.bounded();
  TEST_ASSERT_EQUAL_INT(3, r.calls[0].requests);
  TEST_ASSERT_TRUE(r.calls[0].replayBytes <= 2 * (2048 + 64));
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_TRUE(r.calls.back().requests > 3);   // the owner keeps the configured loop
}

// On a fallback head (the configured head over its token budget, or parked or
// paused) the bounded turn is single-shot: no tool loop, and the prompt says so.
// An owner turn in the same state keeps its loop.
static void test_bounded_turn_on_a_fallback_is_single_shot() {
  for (int how = 0; how < 2; how++) {
    MiniRig r;
    if (how == 0) r.cfg.overBudgetHosts = {"anthropic"};   // Lumi: "anthropic over token budget"
    else r.parked = {"anthropic"};                          // Lumi: "anthropic paused"
    TEST_ASSERT_TRUE(r.eng->headOnFallback());
    r.bounded();
    TEST_ASSERT_FALSE_MESSAGE(r.calls.empty(), how ? "parked" : "over budget");
    TEST_ASSERT_FALSE_MESSAGE(r.calls.back().tools, how ? "parked" : "over budget");
    TEST_ASSERT_TRUE(r.calls.back().host != "anthropic");
    TEST_ASSERT_TRUE(r.calls.back().instructions.find("tool loop is OFF") != std::string::npos);
    r.eng->handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_TRUE(r.calls.back().tools);
  }
  MiniRig healthy;
  TEST_ASSERT_FALSE(healthy.eng->headOnFallback());
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
  RUN_TEST(test_bounded_turn_never_runs_the_owner_loop);
  RUN_TEST(test_bounded_turn_on_a_fallback_is_single_shot);
  RUN_TEST(test_bounded_turn_token_bound_through_the_loop);
  return UNITY_END();
}
