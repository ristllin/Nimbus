#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/net/tg_backoff.h"
#include "nimbus/net/tg_poll_sched.h"

// getUpdates backoff (nimbus/net/tg_backoff.h), driven the way the tg_poll loop in
// src/agent/telegram.cpp drives it. The class: every getUpdates answer that is not a
// 200 is a fetch error that backs off, and Telegram's retry_after is honored. Before,
// only 401/403/409 backed off: a 429, a 5xx or a 404 read as an empty page and the
// loop polled again a second later, forever. Properties over random status streams
// with local turns arriving at random (they end the pause early):
//   - after any non-200, no poll goes out inside the backoff;
//   - a retry_after is honored (capped at kTgRetryAfterMaxS);
//   - counter-test: a stream of 200s polls every cycle, exactly as before;
//   - counter-test: without the gate, a local turn lets a poll in early.

using nimbus::net::TgPoll;
using nimbus::net::TgPollBackoff;
using nimbus::net::TgPollScheduler;

void setUp() {}
void tearDown() {}

static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// ---- the parsers ---------------------------------------------------------------

static void test_retry_after_from_telegram_bodies() {
  const char* flood =
      "{\"ok\":false,\"error_code\":429,\"description\":\"Too Many Requests: retry after 17\","
      "\"parameters\":{\"retry_after\":17}}";
  TEST_ASSERT_EQUAL_INT(17, nimbus::net::tgRetryAfterS(flood, strlen(flood)));
  const char* spaced = "{\"parameters\": {\"retry_after\" :  5 }}";
  TEST_ASSERT_EQUAL_INT(5, nimbus::net::tgRetryAfterS(spaced, strlen(spaced)));
  const char* none = "{\"ok\":false,\"error_code\":502,\"description\":\"Bad Gateway\"}";
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterS(none, strlen(none)));
  const char* junk = "{\"retry_after\":\"soon\"}";
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterS(junk, strlen(junk)));
  const char* huge = "{\"retry_after\":99999999999999}";
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterS(huge, strlen(huge)));   // nonsense
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterS(nullptr, 0));
  // A truncated body never reads past its length.
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterS(flood, 20));
  TEST_ASSERT_EQUAL_INT(30, nimbus::net::tgRetryAfterHeaderS(" 30"));
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterHeaderS(" Wed, 21 Oct 2026 07:28:00 GMT"));
  TEST_ASSERT_EQUAL_INT(-1, nimbus::net::tgRetryAfterHeaderS(nullptr));
}

static void test_backoff_grows_caps_and_clears() {
  TgPollBackoff b(1500, 8);
  TEST_ASSERT_TRUE(b.mayPoll(0));
  TEST_ASSERT_EQUAL_UINT32(1500, b.onError(1000, -1));
  TEST_ASSERT_FALSE(b.mayPoll(2499));
  TEST_ASSERT_TRUE(b.mayPoll(2500));
  TEST_ASSERT_EQUAL_UINT32(3000, b.onError(3000, -1));
  for (int i = 0; i < 20; ++i) b.onError(10000, -1);
  TEST_ASSERT_EQUAL_UINT32(12000, b.onError(10000, -1));   // capped at 8 steps
  TEST_ASSERT_EQUAL_UINT32(60000, b.onError(10000, 60));   // retry_after wins when longer
  TEST_ASSERT_EQUAL_UINT32(nimbus::net::kTgRetryAfterMaxS * 1000u, b.onError(10000, 999999));
  b.onOk();
  TEST_ASSERT_TRUE(b.mayPoll(10001));
  TEST_ASSERT_EQUAL_UINT32(0, b.fails());
  TEST_ASSERT_EQUAL_UINT32(1500, b.onError(20000, 1));   // a short retry_after never shortens
  // millis() wrap: a deadline across the wrap still holds.
  TgPollBackoff w(1500, 8);
  w.onError(0xFFFFFF00u, -1);
  TEST_ASSERT_FALSE(w.mayPoll(0x00000100u));
  TEST_ASSERT_TRUE(w.mayPoll(0x000005E0u));
}

// ---- the loop property ---------------------------------------------------------

struct Answer {
  int status;        // HTTP status of one getUpdates
  int retryAfterS;   // -1 when none
};

struct SimResult {
  std::vector<uint32_t> polls;
  bool earlyPoll = false;       // a poll inside a backoff window
  bool retryIgnored = false;    // a poll before a retry_after elapsed
  int skips = 0;
};

// A millisecond model of the poll loop's backoff path, in pollTask's order: plan the
// cycle (local turn waiting?), gate it on the backoff, poll (or skip), run a waiting
// local turn, then pause - a 1 s inter-cycle pause or the backoff - in 50 ms slices
// that a newly arrived local turn ends at once (tgIdleSleepMs).
static SimResult simulate(const std::vector<Answer>& answers, const std::vector<uint32_t>& locals,
                          bool gate, uint32_t untilMs) {
  SimResult r;
  TgPollBackoff backoff(1500, 8);
  TgPollScheduler sched;
  uint32_t now = 0;
  size_t ai = 0, li = 0;
  uint32_t windowEnd = 0, retryEnd = 0;
  auto localWaiting = [&] { return li < locals.size() && locals[li] <= now; };
  while (now < untilMs) {
    TgPoll plan = sched.plan(localWaiting());
    if (gate) plan = nimbus::net::tgPlanWithBackoff(plan, backoff.mayPoll(now));
    uint32_t pause = 1000;
    if (plan == TgPoll::Skip) {
      ++r.skips;
    } else {
      if (now < windowEnd) r.earlyPoll = true;
      if (now < retryEnd) r.retryIgnored = true;
      r.polls.push_back(now);
      const Answer a = answers[ai++ % answers.size()];
      now += 150;   // round trip
      if (a.status != 200) {
        pause = backoff.onError(now, a.retryAfterS);
        windowEnd = now + pause;
        if (a.retryAfterS > 0) retryEnd = now + (uint32_t)a.retryAfterS * 1000u;
      } else {
        backoff.onOk();
      }
    }
    if (localWaiting()) { ++li; now += 3000; }   // the local turn runs
    for (uint32_t start = now;;) {
      const uint32_t ms = nimbus::net::tgIdleSleepMs(localWaiting(), now - start, pause);
      if (ms == 0) break;
      now += ms;
    }
  }
  return r;
}

struct Rng {
  uint32_t s;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  uint32_t below(uint32_t n) { return next() % n; }
};

static void test_no_poll_inside_any_backoff() {
  static const int kStatuses[] = {200, 200, 200, 409, 401, 403, 429, 500, 502, 503, 504, 404, 400, 420, 302};
  Rng rng{0xbac0ffu};
  int errors = 0, retries = 0;
  for (int trial = 0; trial < 400; ++trial) {
    std::vector<Answer> answers;
    for (int i = 0; i < 40; ++i) {
      Answer a{kStatuses[rng.below(sizeof(kStatuses) / sizeof(kStatuses[0]))], -1};
      if (a.status == 429 || rng.below(6) == 0) a.retryAfterS = 1 + (int)rng.below(40);
      if (a.status == 200) a.retryAfterS = -1;
      errors += a.status != 200;
      retries += a.retryAfterS > 0;
      answers.push_back(a);
    }
    std::vector<uint32_t> locals;
    for (uint32_t t = rng.below(4000); t < 600000; t += 500 + rng.below(9000)) locals.push_back(t);
    const SimResult r = simulate(answers, locals, /*gate=*/true, 600000);
    char ctx[32];
    std::snprintf(ctx, sizeof ctx, "trial %d", trial);
    TEST_ASSERT_FALSE_MESSAGE(r.earlyPoll, ctx);
    TEST_ASSERT_FALSE_MESSAGE(r.retryIgnored, ctx);
    TEST_ASSERT_TRUE_MESSAGE(r.polls.size() > 5, ctx);   // it does poll again after a backoff
  }
  TEST_ASSERT_TRUE(errors > 1000 && retries > 500);
}

// Every status but 200 backs off: a stream of any one of them never polls faster than
// the first backoff step.
static void test_every_non_200_status_backs_off() {
  for (int status = 100; status < 600; ++status) {
    if (status == 200) continue;
    const SimResult r = simulate({{status, -1}}, {}, true, 60000);
    for (size_t i = 1; i < r.polls.size(); ++i)
      TEST_ASSERT_TRUE_MESSAGE(r.polls[i] - r.polls[i - 1] >= 1500 + 150, std::to_string(status).c_str());
    TEST_ASSERT_TRUE(r.polls.size() < 60000 / 1150);   // fewer polls than an all-200 stream
  }
}

// Counter-test: a 200 stream polls every cycle, exactly as before (no skips, one poll
// per round trip plus the 1 s pause).
static void test_200_stream_polls_as_before() {
  const SimResult r = simulate({{200, -1}}, {}, true, 60000);
  TEST_ASSERT_EQUAL_INT(0, r.skips);
  for (size_t i = 1; i < r.polls.size(); ++i)
    TEST_ASSERT_EQUAL_UINT32(1150, r.polls[i] - r.polls[i - 1]);
  TEST_ASSERT_EQUAL_UINT(53, r.polls.size());   // 60 s / 1.15 s, rounded up
  // With local turns, a 200 stream still polls at least every other cycle.
  std::vector<uint32_t> locals;
  for (uint32_t t = 0; t < 60000; t += 700) locals.push_back(t);
  const SimResult busy = simulate({{200, -1}}, locals, true, 60000);
  TEST_ASSERT_TRUE(busy.polls.size() > 5);
  TEST_ASSERT_FALSE(busy.earlyPoll);
}

// Counter-test: without the gate (the pause was the only backoff), a local turn ends
// the pause and a poll goes out inside the backoff - the property above catches it.
static void test_without_the_gate_a_local_turn_polls_early() {
  std::vector<uint32_t> locals;
  for (uint32_t t = 500; t < 120000; t += 2000) locals.push_back(t);
  const SimResult r = simulate({{429, 30}}, locals, /*gate=*/false, 120000);
  TEST_ASSERT_TRUE(r.earlyPoll);
  TEST_ASSERT_TRUE(r.retryIgnored);
}

// The device wiring the simulation stands for (src/agent/telegram.cpp): any status but
// 200 goes to failPollStatus (classify, read retry_after, close, fetch error); a 200
// that does not parse is a fetch error; the cycle plan is gated on the backoff; and the
// error path hands retry_after to the backoff.
static void test_device_wiring_uses_the_rule() {
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_TRUE(tg.find("if (status != 200) { failPollStatus(status, deadline, rp, outcome); return; }") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(tg.find("rp.fetchError = !parsedOk;") != std::string::npos);
  TEST_ASSERT_TRUE(tg.find("plan = nimbus::net::tgPlanWithBackoff(plan, g_backoff.mayPoll(millis()));") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(tg.find("g_backoff.onError(millis(), g_retryAfterS)") != std::string::npos);
  TEST_ASSERT_TRUE(tg.find("retryAfter = nimbus::net::tgRetryAfterS(body, (size_t)n);") != std::string::npos);
  // The old shape (only 401/403/409 backed off) is gone.
  TEST_ASSERT_TRUE(tg.find("pollStatusIsError") == std::string::npos);
  TEST_ASSERT_TRUE(tg.find("g_pollFails") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_retry_after_from_telegram_bodies);
  RUN_TEST(test_backoff_grows_caps_and_clears);
  RUN_TEST(test_no_poll_inside_any_backoff);
  RUN_TEST(test_every_non_200_status_backs_off);
  RUN_TEST(test_200_stream_polls_as_before);
  RUN_TEST(test_without_the_gate_a_local_turn_polls_early);
  RUN_TEST(test_device_wiring_uses_the_rule);
  return UNITY_END();
}
