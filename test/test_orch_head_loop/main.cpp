#include <cstring>
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/transcript.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// A programmable fake head: a scripted list of steps consumed in order, plus a
// fake monotonic clock and a dispatch spy. Everything the controller needs is
// injected, so the loop logic is exercised with zero device state.
struct Fake {
  std::vector<HeadStep> script;    // one HeadStep per model call, consumed in order
  size_t stepIdx = 0;
  uint32_t clock = 0;              // fake millis
  uint32_t heap = 100000;         // fake free heap
  // record of what the controller passed us each step / dispatched:
  std::vector<bool> allowToolsSeen;
  std::vector<std::vector<HeadToolResult>> priorSeen;
  std::vector<uint32_t> budgetSeen;   // F25: remaining wall-clock budget per round
  std::vector<HeadToolCall> dispatched;
  // dispatch behavior:
  std::string dispatchOutput = "OK";
  bool dispatchIsError = false;
  size_t dispatchBytes = 0;       // if >0, emit an output of this many bytes
  std::vector<std::string> capReasonSeen;   // capReason handed to each step

  HeadLoopHooks hooks() {
    HeadLoopHooks h;
    h.step = [this](bool allowTools, const std::vector<HeadToolResult>& prior,
                    uint32_t budgetMs, const std::string& capReason) {
      allowToolsSeen.push_back(allowTools);
      priorSeen.push_back(prior);
      budgetSeen.push_back(budgetMs);
      capReasonSeen.push_back(capReason);
      if (stepIdx >= script.size()) {  // ran out of script -> pretend it answered
        HeadStep s; s.finished = true; s.finalTurn = "{\"reply\":\"fallback\"}";
        return s;
      }
      return script[stepIdx++];
    };
    h.dispatch = [this](const HeadToolCall& c) {
      dispatched.push_back(c);
      HeadToolResult r; r.id = c.id; r.name = c.name;
      r.isError = dispatchIsError;
      r.output = dispatchBytes ? std::string(dispatchBytes, 'x') : dispatchOutput;
      return r;
    };
    h.nowMs = [this]() { return clock; };
    h.freeHeap = [this]() { return heap; };
    return h;
  }
};

static HeadStep stepFinished(const std::string& turn) {
  HeadStep s; s.finished = true; s.finalTurn = turn; return s;
}
static HeadStep stepCall(const std::string& name, const std::string& args = "{}") {
  HeadStep s; s.finished = false;
  s.toolCalls.push_back(HeadToolCall{"call_" + name, name, args});
  return s;
}
static HeadStep stepFail(const std::string& err) {
  HeadStep s; s.ok = false; s.error = err; return s;
}

// ---- round prose ("thinking") observer (Glass Box A4) -----------------------
// onText fires once per step with non-empty HeadStep.text - including the
// TERMINAL round (prose can precede the orch_turn call) - and never for empty
// text. Order: text observed BEFORE the round's tools dispatch.
static void test_onText_fires_per_round_incl_terminal() {
  Fake f;
  HeadStep r0 = stepCall("memory.search");
  r0.text = "let me look that up";
  HeadStep r1 = stepFinished("{\"reply\":\"done\"}");
  r1.text = "found it";
  HeadStep silent = stepCall("web.search");   // no text -> no fire
  f.script = {r0, silent, r1};
  HeadLoopConfig cfg; cfg.maxRounds = 4;
  HeadLoopHooks h = f.hooks();
  std::vector<std::pair<std::string,int>> seen;
  h.onText = [&](const std::string& t, int round) { seen.emplace_back(t, round); };
  HeadOutcome out = runHeadLoop(cfg, h);
  TEST_ASSERT_TRUE(out.ok);
  TEST_ASSERT_EQUAL(2u, seen.size());                       // silent round skipped
  TEST_ASSERT_EQUAL_STRING("let me look that up", seen[0].first.c_str());
  TEST_ASSERT_EQUAL(0, seen[0].second);
  TEST_ASSERT_EQUAL_STRING("found it", seen[1].first.c_str());
  TEST_ASSERT_EQUAL(2, seen[1].second);                     // terminal round fires too
}

// A null onText hook with text-bearing steps must not crash (observer optional).
static void test_onText_null_hook_safe() {
  Fake f;
  HeadStep r0 = stepFinished("{\"reply\":\"ok\"}");
  r0.text = "thinking out loud";
  f.script = {r0};
  HeadLoopConfig cfg;
  HeadOutcome out = runHeadLoop(cfg, f.hooks());   // no onText installed
  TEST_ASSERT_TRUE(out.ok);
}

// (a) single-shot: the model returns orch_turn on the very first call. No tool
// round runs; the common non-agentic path is unchanged.
static void test_single_shot_terminates_immediately() {
  Fake f;
  f.script = {stepFinished("{\"reply\":\"hi\"}")};
  HeadLoopConfig cfg;
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(0, o.rounds);
  TEST_ASSERT_FALSE(o.hitCap);
  TEST_ASSERT_TRUE(has(o.finalTurn, "hi"));
  TEST_ASSERT_EQUAL_INT(1, (int)f.allowToolsSeen.size());
  TEST_ASSERT_TRUE(f.allowToolsSeen[0]);       // tools offered on the first call
  TEST_ASSERT_EQUAL_INT(0, (int)f.dispatched.size());
}

// (b) one tool round then finish: the call is dispatched and its result is fed
// back into the NEXT step (priorResults), then the model terminates.
static void test_one_tool_round_then_finish() {
  Fake f;
  f.dispatchOutput = "found: 42";
  f.script = {stepCall("memory.search", "{\"q\":\"x\"}"),
              stepFinished("{\"reply\":\"done\"}")};
  HeadOutcome o = runHeadLoop(HeadLoopConfig{}, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(1, o.rounds);
  TEST_ASSERT_EQUAL_INT(1, (int)f.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("memory.search", f.dispatched[0].name.c_str());
  TEST_ASSERT_TRUE(has(f.dispatched[0].argsJson, "\"q\":\"x\""));
  // second step saw the tool result fed back
  TEST_ASSERT_EQUAL_INT(2, (int)f.priorSeen.size());
  TEST_ASSERT_EQUAL_INT(0, (int)f.priorSeen[0].size());   // first call: no prior
  TEST_ASSERT_EQUAL_INT(1, (int)f.priorSeen[1].size());
  TEST_ASSERT_TRUE(has(f.priorSeen[1][0].output, "found: 42"));
}

// (c) a tool error is fed back (isError) and the loop keeps going - never fatal.
static void test_tool_error_is_not_fatal() {
  Fake f;
  f.dispatchIsError = true;
  f.dispatchOutput = "boom: no such key";
  f.script = {stepCall("memory.update"), stepFinished("{\"reply\":\"recovered\"}")};
  HeadOutcome o = runHeadLoop(HeadLoopConfig{}, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(1, o.rounds);
  TEST_ASSERT_TRUE(f.priorSeen[1][0].isError);
  TEST_ASSERT_TRUE(has(f.priorSeen[1][0].output, "boom"));
}

// (d1) rounds cap, well-behaved model: after maxRounds tool rounds the controller
// forces a tool-less "answer now" call; the model respects it and terminates.
static void test_rounds_cap_forces_final_answer() {
  Fake f;
  // 2 tool calls, then a finish for the forced final round.
  f.script = {stepCall("web.search"), stepCall("web.search"),
              stepFinished("{\"reply\":\"answer\"}")};
  HeadLoopConfig cfg; cfg.maxRounds = 2;
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(2, o.rounds);
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("rounds", o.capReason.c_str());
  // three model calls: two with tools, the third tool-less
  TEST_ASSERT_EQUAL_INT(3, (int)f.allowToolsSeen.size());
  TEST_ASSERT_TRUE(f.allowToolsSeen[0]);
  TEST_ASSERT_TRUE(f.allowToolsSeen[1]);
  TEST_ASSERT_FALSE(f.allowToolsSeen[2]);   // forced final round: no tools
}

// The forced final round must tell the model WHY its tools went away. Without
// this the loop removed them silently, and every provider confabulated a promise
// of future work instead of admitting it could not finish ("I'll report back when
// the scan finishes") - reproduced live on mistral, openai and anthropic. The
// device then went quiet, which is what the owner saw.
static void test_forced_final_round_carries_the_cap_reason() {
  Fake f;
  f.script = {stepCall("web.search"), stepCall("web.search"),
              stepFinished("{\"reply\":\"answer\"}")};
  HeadLoopConfig cfg; cfg.maxRounds = 2;
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(3, (int)f.capReasonSeen.size());
  // Normal rounds must carry NO reason - otherwise every round would nag the
  // model to wrap up and the loop would never do multi-step work.
  TEST_ASSERT_EQUAL_STRING("", f.capReasonSeen[0].c_str());
  TEST_ASSERT_EQUAL_STRING("", f.capReasonSeen[1].c_str());
  TEST_ASSERT_EQUAL_STRING("rounds", f.capReasonSeen[2].c_str());
}

static void test_each_cap_reason_reaches_the_step() {
  {   // heap
    Fake f;
    f.heap = 1000;   // under the re-gate
    f.script = {stepCall("x"), stepFinished("{\"reply\":\"a\"}")};
    HeadLoopConfig cfg; cfg.roundMinHeap = 28000;
    runHeadLoop(cfg, f.hooks());
    TEST_ASSERT_EQUAL_STRING("heap", f.capReasonSeen.back().c_str());
  }
  {   // stalled - the model asked for nothing without finishing
    Fake f;
    HeadStep idle; idle.ok = true; idle.finished = false;   // no tool calls
    f.script = {idle, stepFinished("{\"reply\":\"a\"}")};
    HeadLoopConfig cfg;
    runHeadLoop(cfg, f.hooks());
    TEST_ASSERT_EQUAL_STRING("stalled", f.capReasonSeen.back().c_str());
  }
}

// Every reason must render as a human sentence - a raw token in the prompt
// ("heap") tells the model nothing it can relay to the owner.
static void test_cap_reason_text_is_readable() {
  for (const char* r : {"heap", "rounds", "deadline", "bytes", "stalled", "wat"}) {
    const char* t = capReasonText(r);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(strlen(t) > 10);
    TEST_ASSERT_TRUE(strcmp(t, r) != 0);
  }
}

// The notice itself must forbid the exact failure mode it exists to prevent.
static void test_final_round_notice_bans_promising_future_work() {
  const std::string n = kFinalRoundNotice;
  TEST_ASSERT_TRUE(n.find("FINAL") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("%s") != std::string::npos);       // the reason slot
  TEST_ASSERT_TRUE(n.find("report back") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("background") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("next") != std::string::npos);
}

// (d2) rounds cap, misbehaving model: it keeps requesting tools even on the forced
// final round -> fail soft with an error, never an infinite loop.
static void test_rounds_cap_failsoft_when_model_wont_stop() {
  Fake f;
  // always asks for a tool, never finishes (script exhaustion returns finished,
  // so pad with more calls than the loop can make).
  f.script = {stepCall("web.search"), stepCall("web.search"),
              stepCall("web.search"), stepCall("web.search")};
  HeadLoopConfig cfg; cfg.maxRounds = 2;
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("rounds", o.capReason.c_str());
  TEST_ASSERT_TRUE(has(o.error, "budget"));
  TEST_ASSERT_EQUAL_INT(2, o.rounds);      // dispatched exactly maxRounds times
}

// (e) deadline cap: the clock jumps past the wall-clock budget between rounds ->
// the next call is the forced tool-less answer round, reason "deadline".
static void test_deadline_cap() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"late\"}")};
  HeadLoopConfig cfg; cfg.deadlineMs = 50;
  // dispatch is the expensive part (the TLS round-trip) - model wall-clock past the
  // deadline there, so the NEXT round's gate sees it and forces the answer round.
  auto hooks = f.hooks();
  auto innerDisp = hooks.dispatch;
  hooks.dispatch = [&](const HeadToolCall& c) { f.clock = 1000; return innerDisp(c); };
  HeadOutcome o = runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(o.ok);               // model answered on the forced round
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("deadline", o.capReason.c_str());
  TEST_ASSERT_FALSE(f.allowToolsSeen.back());  // last call was tool-less
}

// (e2) F25 budget threading: step() receives the turn's REMAINING wall-clock budget.
// No deadline configured -> UINT32_MAX every round (clamp is a no-op downstream, so
// a healthy turn is byte-for-byte the old behavior). With a deadline, the budget
// SHRINKS as the fake clock advances between rounds - this is what lets an adapter
// clamp a late round's socket timeout so slow rounds can't stack past the deadline.
static void test_budget_infinite_without_deadline() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg; cfg.deadlineMs = 0;   // no wall-clock budget
  auto hooks = f.hooks();
  runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(f.budgetSeen.size() >= 1);
  for (uint32_t b : f.budgetSeen) TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, b);
}

static void test_budget_shrinks_with_elapsed() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg; cfg.deadlineMs = 600000;
  auto hooks = f.hooks();
  auto innerDisp = hooks.dispatch;
  // Round 0 starts at clock 0 (full budget); the dispatch burns 120 s, so round 1's
  // gate sees 480 s remaining and hands that to the forced/next step.
  hooks.dispatch = [&](const HeadToolCall& c) { f.clock = 120000; return innerDisp(c); };
  runHeadLoop(cfg, hooks);
  TEST_ASSERT_EQUAL_UINT32(2, f.budgetSeen.size());
  TEST_ASSERT_EQUAL_UINT32(600000, f.budgetSeen[0]);   // round 0: full budget
  TEST_ASSERT_EQUAL_UINT32(480000, f.budgetSeen[1]);   // round 1: budget minus elapsed
}

// A round that has ALREADY overrun the deadline hands the forced-final step a 0
// budget - the adapter (not the controller) floors that to a viable minimum, so the
// controller's job is just to report the true remaining (0), never a wrapped huge value.
static void test_budget_zero_when_overrun() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"late\"}")};
  HeadLoopConfig cfg; cfg.deadlineMs = 50;
  auto hooks = f.hooks();
  auto innerDisp = hooks.dispatch;
  hooks.dispatch = [&](const HeadToolCall& c) { f.clock = 1000; return innerDisp(c); };
  runHeadLoop(cfg, hooks);
  TEST_ASSERT_EQUAL_UINT32(2, f.budgetSeen.size());
  TEST_ASSERT_EQUAL_UINT32(0, f.budgetSeen[1]);   // overrun -> 0, not a wrapped value
}

// (f) heap cap: free heap drops below roundMinHeap between rounds -> forced final.
static void test_heap_cap() {
  Fake f;
  f.script = {stepCall("memory.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg; cfg.roundMinHeap = 34000;
  auto hooks = f.hooks();
  auto innerDisp = hooks.dispatch;
  hooks.dispatch = [&](const HeadToolCall& c) { f.heap = 20000; return innerDisp(c); };
  HeadOutcome o = runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("heap", o.capReason.c_str());
}

// (f2) the heap gate does NOT block round 0: entry gating is the caller's job (the
// turn-level floor admitted the turn), so a below-floor heap at the first call must
// still get its tool round - only SUBSEQUENT rounds re-gate. (Found live: prompt
// composition dipped heap below the floor and the loop degraded to tool-less on a
// turn the single-shot path handles fine.)
static void test_heap_gate_exempts_round_zero() {
  Fake f;
  f.heap = 20000;   // below the floor from the very start
  f.script = {stepCall("memory.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg; cfg.roundMinHeap = 34000;
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_INT(1, o.rounds);                  // the tool round RAN
  TEST_ASSERT_EQUAL_INT(1, (int)f.dispatched.size());
  TEST_ASSERT_TRUE(f.allowToolsSeen[0]);               // round 0 offered tools
  TEST_ASSERT_FALSE(f.allowToolsSeen[1]);              // round 1 re-gated (heap still low)
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("heap", o.capReason.c_str());
}

// (g) accumulator bound: oversized tool outputs are clamped, and once the
// cumulative budget is exceeded the loop forces the final answer round.
static void test_accumulator_clamp_and_byte_budget() {
  Fake f;
  f.dispatchBytes = 5000;   // each result larger than maxToolResultBytes
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"big\"}")};
  HeadLoopConfig cfg;
  cfg.maxToolResultBytes = 100;
  cfg.maxTotalToolBytes = 100;   // one big result blows the cumulative budget
  HeadOutcome o = runHeadLoop(cfg, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  // the fed-back result was clamped to ~maxToolResultBytes + marker
  TEST_ASSERT_TRUE(f.priorSeen[1][0].output.size() < 200);
  TEST_ASSERT_TRUE(has(f.priorSeen[1][0].output, "truncated"));
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("bytes", o.capReason.c_str());
  TEST_ASSERT_FALSE(f.allowToolsSeen.back());  // final round was tool-less
}

// (g2) the clamp cuts at a UTF-8 boundary: a multi-byte code point straddling the
// byte limit is dropped whole, never split (a split would feed invalid UTF-8 back
// into the next provider request).
static void test_accumulator_clamp_respects_utf8_boundary() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg;
  cfg.maxToolResultBytes = 10;   // boundary lands mid-codepoint below
  cfg.maxTotalToolBytes = 0;     // isolate the per-result clamp
  auto hooks = f.hooks();
  hooks.dispatch = [&](const HeadToolCall& c) {
    HeadToolResult r; r.id = c.id; r.name = c.name;
    // 9 ASCII bytes then a 3-byte code point (E2 82 AC = '€') spanning bytes 9-11:
    // a naive resize(10) would cut after E2, mid-sequence.
    r.output = std::string("123456789") + "\xE2\x82\xAC" + "tail";
    return r;
  };
  HeadOutcome o = runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(o.ok);
  const std::string& fed = f.priorSeen[1][0].output;
  // clamp backed up to byte 9 (dropping the whole '€'), then appended the marker
  TEST_ASSERT_TRUE(fed.rfind("123456789\xE2\x80\xA6", 0) == 0);   // "123456789…"
  TEST_ASSERT_TRUE(fed.find('\xE2' + std::string("\x82")) == std::string::npos);  // no split '€' prefix
  // every byte sequence is valid UTF-8: no lone continuation/lead bytes at the seam
  TEST_ASSERT_TRUE(has(fed, "truncated"));
}

// (g3) clip+spill (Context Fabric): the spill hook receives the FULL pre-clamp
// result, and the marker embeds its results.get handle. Null hook (g/g2 above)
// keeps the legacy "…[truncated]" bytes - those tests pin it unmodified.
static void test_clamp_spills_full_and_marker_carries_handle() {
  Fake f;
  f.script = {stepCall("web.search"), stepFinished("{\"reply\":\"ok\"}")};
  HeadLoopConfig cfg;
  cfg.maxToolResultBytes = 100;
  cfg.maxTotalToolBytes = 0;
  auto hooks = f.hooks();
  std::string big(5000, 'x');
  hooks.dispatch = [&](const HeadToolCall& c) {
    HeadToolResult r; r.id = c.id; r.name = c.name; r.output = big;
    return r;
  };
  size_t spilledBytes = 0;
  std::string spilledName;
  hooks.spill = [&](const HeadToolResult& full) {
    spilledBytes = full.output.size();
    spilledName = full.name;
    return std::string("r7");
  };
  HeadOutcome o = runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL_UINT32(5000, (uint32_t)spilledBytes);   // FULL, pre-clip
  TEST_ASSERT_EQUAL_STRING("web.search", spilledName.c_str());
  const std::string& fed = f.priorSeen[1][0].output;
  TEST_ASSERT_TRUE(has(fed, "truncated 100 of 5000 B"));
  TEST_ASSERT_TRUE(has(fed, "results.get(\"r7\")"));
}

// (g4) canonical transcript (Context Fabric): when a Transcript is hooked up the
// controller records the seed, each round's prose, every call and every result -
// one provider-neutral record a mid-turn provider switch can carry over. Purely
// additive: with no transcript hooked, every other test here is unchanged.
static void test_transcript_records_calls_and_results() {
  Fake f;
  f.script = {stepCall("web.search"), stepCall("memory_search"),
              stepFinished("{\"reply\":\"ok\"}")};
  f.script[0].text = "let me look";
  HeadLoopConfig cfg;
  cfg.maxRounds = 6;
  cfg.maxToolResultBytes = 0;
  cfg.maxTotalToolBytes = 0;
  Transcript t;
  t.addUser("the question");
  auto hooks = f.hooks();
  hooks.transcript = &t;
  HeadOutcome o = runHeadLoop(cfg, hooks);
  TEST_ASSERT_TRUE(o.ok);
  // seed + (prose + call + result) + (call + result)
  int calls = 0, results = 0, prose = 0;
  for (const auto& it : t.entries()) {
    if (it.kind == TranscriptItem::Kind::ToolUse) calls++;
    if (it.kind == TranscriptItem::Kind::ToolResult) results++;
    if (it.kind == TranscriptItem::Kind::AssistantText) prose++;
  }
  TEST_ASSERT_EQUAL_INT(2, calls);
  TEST_ASSERT_EQUAL_INT(2, results);
  TEST_ASSERT_EQUAL_INT(1, prose);                  // only round 0 carried text
  TEST_ASSERT_TRUE(t.entries()[0].pinned);          // the seed
  TEST_ASSERT_TRUE(t.toolBytes() > 0);
  // Every recorded call is answered - the pairing invariant a provider switch relies on.
  for (const auto& a : t.entries()) {
    if (a.kind != TranscriptItem::Kind::ToolUse) continue;
    bool answered = false;
    for (const auto& b : t.entries())
      if (b.kind == TranscriptItem::Kind::ToolResult && b.id == a.id) answered = true;
    TEST_ASSERT_TRUE(answered);
  }
}

// (h) stalled turn: not finished but no tool calls -> force one final answer round.
static void test_stalled_turn_forces_final() {
  Fake f;
  HeadStep empty; empty.finished = false;  // no tools, no finish
  f.script = {empty, stepFinished("{\"reply\":\"unstuck\"}")};
  HeadOutcome o = runHeadLoop(HeadLoopConfig{}, f.hooks());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(o.hitCap);
  TEST_ASSERT_EQUAL_STRING("stalled", o.capReason.c_str());
  TEST_ASSERT_EQUAL_INT(0, (int)f.dispatched.size());  // nothing dispatched
}

// (i) provider transport failure aborts the loop fail-soft (ok=false, error set).
static void test_provider_failure_aborts_failsoft() {
  Fake f;
  f.script = {stepCall("memory.search"), stepFail("HTTP 503")};
  HeadOutcome o = runHeadLoop(HeadLoopConfig{}, f.hooks());
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_TRUE(has(o.error, "503"));
  TEST_ASSERT_EQUAL_INT(1, o.rounds);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_onText_fires_per_round_incl_terminal);
  RUN_TEST(test_onText_null_hook_safe);
  RUN_TEST(test_single_shot_terminates_immediately);
  RUN_TEST(test_one_tool_round_then_finish);
  RUN_TEST(test_tool_error_is_not_fatal);
  RUN_TEST(test_rounds_cap_forces_final_answer);
  RUN_TEST(test_forced_final_round_carries_the_cap_reason);
  RUN_TEST(test_each_cap_reason_reaches_the_step);
  RUN_TEST(test_cap_reason_text_is_readable);
  RUN_TEST(test_final_round_notice_bans_promising_future_work);
  RUN_TEST(test_rounds_cap_failsoft_when_model_wont_stop);
  RUN_TEST(test_deadline_cap);
  RUN_TEST(test_budget_infinite_without_deadline);
  RUN_TEST(test_budget_shrinks_with_elapsed);
  RUN_TEST(test_budget_zero_when_overrun);
  RUN_TEST(test_heap_cap);
  RUN_TEST(test_heap_gate_exempts_round_zero);
  RUN_TEST(test_accumulator_clamp_and_byte_budget);
  RUN_TEST(test_accumulator_clamp_respects_utf8_boundary);
  RUN_TEST(test_clamp_spills_full_and_marker_carries_handle);
  RUN_TEST(test_transcript_records_calls_and_results);
  RUN_TEST(test_stalled_turn_forces_final);
  RUN_TEST(test_provider_failure_aborts_failsoft);
  UNITY_END();
  return 0;
}
