#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/harness/turn_progress.h"
#include "nimbus/orch/head_loop.h"

using nimbus::harness::TurnProgressPlan;
using nimbus::harness::planTurnProgress;
using nimbus::harness::progressActivity;
using nimbus::harness::progressWait;
using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the core seam: the loop fires progress at EVERY round boundary ---------

// A fake head whose clock advances 35 s per round (the real case's pace:
// evidence/lumi-v490-watch, 9 provider calls over 311 s). Two tool rounds,
// then the cap forces the tool-less final round.
struct PacedFake {
  uint32_t clock = 1000;
  int roundsRun = 0;
  std::vector<HeadProgress> seen;
  std::vector<std::string> toolNames{"web_search", "memory_search"};

  HeadLoopHooks hooks(int maxRounds) {
    HeadLoopHooks h;
    h.nowMs = [this] { return clock; };
    h.log = [](const std::string&) {};
    h.progress = [this](const HeadProgress& hp) { seen.push_back(hp); };
    h.step = [this, maxRounds](bool allowTools, const std::vector<HeadToolResult>&, uint32_t,
                               const std::string&) {
      clock += 35000;
      HeadStep s;
      if (!allowTools || roundsRun >= 2) {
        s.finished = true;
        s.finalTurn = "{\"reply\":\"done\"}";
        return s;
      }
      HeadToolCall c;
      c.id = "c" + std::to_string(roundsRun);
      c.name = toolNames[roundsRun++];
      s.toolCalls.push_back(c);
      return s;
    };
    h.dispatch = [](const HeadToolCall& c) {
      HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      r.output = "ok";
      return r;
    };
    return h;
  }
};

// The loop fires the hook at every boundary - before the first request (round
// 0) included - with the calls the PREVIOUS round dispatched, and marks the
// forced final round. Failing-first: without the seam there is nothing to fire.
void test_progress_fires_at_every_boundary() {
  PacedFake f;
  HeadLoopConfig cfg;
  cfg.maxRounds = 8;
  cfg.deadlineMs = 0;
  const HeadOutcome o = runHeadLoop(cfg, f.hooks(8));
  TEST_ASSERT_TRUE(o.ok);
  // rounds 0, 1, 2 (the final): three boundaries, none missed.
  TEST_ASSERT_EQUAL_INT(3, (int)f.seen.size());
  TEST_ASSERT_EQUAL_INT(0, f.seen[0].round);
  TEST_ASSERT_EQUAL_INT(1, f.seen[1].round);
  TEST_ASSERT_EQUAL_INT(2, f.seen[2].round);
  // Round 0 has no previous tools; round 1 names round 0's call; round 2 names
  // round 1's.
  TEST_ASSERT_EQUAL_INT(0, (int)f.seen[0].tools.size());
  TEST_ASSERT_EQUAL_INT(1, (int)f.seen[1].tools.size());
  TEST_ASSERT_EQUAL_STRING("web_search", f.seen[1].tools[0].c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)f.seen[2].tools.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", f.seen[2].tools[0].c_str());
  // The elapsed clock runs, and the final boundary is not marked last unless
  // the cap forced it (here the model answered on its own).
  TEST_ASSERT_TRUE(f.seen[1].elapsedMs > f.seen[0].elapsedMs);
  TEST_ASSERT_FALSE(f.seen[2].lastRound);
}

// The cap-forced final round is marked: the answer is imminent, so a surface
// must not say "still working" over it.
void test_progress_marks_forced_final_round() {
  PacedFake f;
  HeadLoopConfig cfg;
  cfg.maxRounds = 2;   // rounds 0, 1, then the forced tool-less round 2
  cfg.deadlineMs = 0;
  const HeadOutcome o = runHeadLoop(cfg, f.hooks(2));
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(3, (int)f.seen.size());
  TEST_ASSERT_TRUE(f.seen[2].lastRound);
  TEST_ASSERT_FALSE(f.seen[1].lastRound);
}

// ---- the plan: what a surface should show at a boundary ---------------------

static HeadProgress hp(int round, uint32_t elapsedMs, bool lastRound = false) {
  HeadProgress h;
  h.round = round;
  h.elapsedMs = elapsedMs;
  h.lastRound = lastRound;
  if (round > 0) h.tools = {"web_search"};
  return h;
}

// A keep-alive between every tool round and provider request, and a queue
// check at every boundary: the two signals that make a running turn look alive.
void test_plan_keeps_alive_and_checks_queue() {
  const TurnProgressPlan p = planTurnProgress(hp(1, 35000), 8, false);
  TEST_ASSERT_TRUE(p.keepAlive);
  TEST_ASSERT_TRUE(p.checkQueue);
}

// The ONE progress line: past 30 s or at the third round, never twice, never on
// a quick turn.
void test_plan_one_line_only() {
  // A quick turn (two fast rounds): no line.
  TEST_ASSERT_FALSE(planTurnProgress(hp(1, 4000), 8, false).sayLine);
  // Past 30 s: the line fires...
  TEST_ASSERT_TRUE(planTurnProgress(hp(1, 35000), 8, false).sayLine);
  // ...exactly once.
  TEST_ASSERT_FALSE(planTurnProgress(hp(1, 35000), 8, true).sayLine);
  TEST_ASSERT_FALSE(planTurnProgress(hp(3, 120000), 8, true).sayLine);
  // The third round fires it even under 30 s.
  TEST_ASSERT_TRUE(planTurnProgress(hp(2, 12000), 8, false).sayLine);
}

// The forced final round: the answer is imminent, so every signal stays quiet -
// no keep-alive, no line, no queue ack over an answer that is about to land.
void test_plan_quiet_on_forced_final_round() {
  const TurnProgressPlan p = planTurnProgress(hp(2, 120000, true), 8, false);
  TEST_ASSERT_FALSE(p.keepAlive);
  TEST_ASSERT_FALSE(p.sayLine);
  TEST_ASSERT_FALSE(p.checkQueue);
}

// The line names what the turn is doing in plain words - a machine name never
// reaches the owner - and gives an honest rough wait from the observed pace.
void test_plan_line_words_and_wait() {
  // The real case's shape: round 2 of an 8-round cap at ~35 s a round.
  const TurnProgressPlan p = planTurnProgress(hp(2, 70000), 8, false);
  TEST_ASSERT_EQUAL_STRING("Searching the web for that, a few minutes more.", p.line.c_str());
  // A pace that fits in a minute says so.
  const TurnProgressPlan q = planTurnProgress(hp(2, 20000), 8, false);
  TEST_ASSERT_EQUAL_STRING("Searching the web for that, about a minute more.", q.line.c_str());
  // An unknown tool falls to the honest default, leaking no machine name.
  HeadProgress u = hp(2, 70000);
  u.tools = {"something_new"};
  const TurnProgressPlan r = planTurnProgress(u, 8, false);
  TEST_ASSERT_EQUAL_STRING("Still working on that, a few minutes more.", r.line.c_str());
  // No tools yet (round 0, before the first request): no line fires at all -
  // the boundary is instant and the turn has shown nothing slow yet.
  const TurnProgressPlan s = planTurnProgress(hp(0, 0), 8, false);
  TEST_ASSERT_FALSE(s.sayLine);
  TEST_ASSERT_EQUAL_STRING("", s.line.c_str());
}

// The wait buckets, pinned: the projection of the observed pace over the
// rounds the cap still allows.
void test_wait_buckets() {
  TEST_ASSERT_EQUAL_STRING("a moment more", progressWait(0, 0, 8).c_str());
  TEST_ASSERT_EQUAL_STRING("about half a minute more", progressWait(30000, 2, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("about a minute more", progressWait(60000, 2, 4).c_str());
  TEST_ASSERT_EQUAL_STRING("a few minutes more", progressWait(70000, 2, 8).c_str());
}

// The real case's timeline (evidence/lumi-v490-watch, 311 s with no signal):
// with the plan, the owner sees a keep-alive from the first boundary, ONE
// progress line at the third round, and nothing but the answer at the end.
void test_real_case_timeline() {
  bool lineSent = false;
  int lines = 0, keepAlives = 0, queueChecks = 0;
  for (int round = 0; round <= 8; ++round) {
    const bool last = round == 8;   // the cap forces the tool-less final round
    const uint32_t elapsed = 35000u * (uint32_t)round;
    const TurnProgressPlan p = planTurnProgress(hp(round, elapsed, last), 8, lineSent);
    if (p.sayLine) {
      lines++;
      lineSent = true;
    }
    keepAlives += p.keepAlive;
    queueChecks += p.checkQueue;
  }
  TEST_ASSERT_EQUAL_INT(1, lines);          // exactly one progress line
  TEST_ASSERT_EQUAL_INT(8, keepAlives);     // alive at every boundary but the last
  TEST_ASSERT_EQUAL_INT(8, queueChecks);    // watching for mid-turn messages
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_progress_fires_at_every_boundary);
  RUN_TEST(test_progress_marks_forced_final_round);
  RUN_TEST(test_plan_keeps_alive_and_checks_queue);
  RUN_TEST(test_plan_one_line_only);
  RUN_TEST(test_plan_quiet_on_forced_final_round);
  RUN_TEST(test_plan_line_words_and_wait);
  RUN_TEST(test_wait_buckets);
  RUN_TEST(test_real_case_timeline);
  return UNITY_END();
}
