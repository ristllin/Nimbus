#include <unity.h>

#include "nimbus/orch/mcp_resilience.h"

using namespace nimbus::orch::mcp;

void setUp() {}
void tearDown() {}

// ---- retryability -----------------------------------------------------------

static void test_retryable_transient() {
  TEST_ASSERT_TRUE(isRetryable(ErrorKind::Timeout, 0));
  TEST_ASSERT_TRUE(isRetryable(ErrorKind::Connect, 0));
  TEST_ASSERT_TRUE(isRetryable(ErrorKind::Empty, 0));
  TEST_ASSERT_TRUE(isRetryable(ErrorKind::Http, 503));
  TEST_ASSERT_TRUE(isRetryable(ErrorKind::Http, 500));
}

static void test_not_retryable_definitive() {
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::Unauthorized, 401));
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::Malformed, 200));
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::Rpc, 200));
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::TooLarge, 200));
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::Http, 400));  // 4xx is the client's fault
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::Http, 404));
}

// ---- backoff ----------------------------------------------------------------

static void test_backoff_grows_and_caps() {
  RetryConfig c;  // base 250, cap 4000
  // jitter 0 -> the low end (window/2)
  TEST_ASSERT_EQUAL_UINT32(125, retryDelayMs(c, 0, 0));   // window 250
  TEST_ASSERT_EQUAL_UINT32(250, retryDelayMs(c, 1, 0));   // window 500
  TEST_ASSERT_EQUAL_UINT32(500, retryDelayMs(c, 2, 0));   // window 1000
  // deep attempts saturate at the cap (window 4000 -> half 2000)
  TEST_ASSERT_EQUAL_UINT32(2000, retryDelayMs(c, 20, 0));
  TEST_ASSERT_EQUAL_UINT32(2000, retryDelayMs(c, 100, 0));  // no overflow blowup
}

static void test_backoff_jitter_within_window() {
  RetryConfig c;
  // attempt 2 -> window 1000, delay in [500, 1000]
  for (uint32_t j = 0; j < 5000; j += 137) {
    uint32_t d = retryDelayMs(c, 2, j);
    TEST_ASSERT_TRUE(d >= 500 && d <= 1000);
  }
}

// ---- circuit breaker --------------------------------------------------------

static void test_breaker_opens_after_threshold() {
  CircuitBreaker b(BreakerConfig{3, 30000});
  TEST_ASSERT_TRUE(b.allow(0));
  b.onFailure(0);
  TEST_ASSERT_EQUAL(1, b.consecutiveFailures());
  TEST_ASSERT_TRUE(b.allow(1));
  b.onFailure(1);
  TEST_ASSERT_TRUE(b.allow(2));
  b.onFailure(2);  // third failure -> Open
  TEST_ASSERT_EQUAL(BreakerState::Open, b.state());
  TEST_ASSERT_FALSE(b.allow(3));  // now fails fast
}

static void test_breaker_success_resets() {
  CircuitBreaker b(BreakerConfig{3, 30000});
  b.onFailure(0);
  b.onFailure(1);
  TEST_ASSERT_EQUAL(2, b.consecutiveFailures());
  b.onSuccess();
  TEST_ASSERT_EQUAL(0, b.consecutiveFailures());
  TEST_ASSERT_EQUAL(BreakerState::Closed, b.state());
}

static void test_breaker_cooldown_then_halfopen_probe_success() {
  CircuitBreaker b(BreakerConfig{2, 1000});
  b.onFailure(100);
  b.onFailure(200);  // -> Open at 200
  TEST_ASSERT_FALSE(b.allow(500));       // still cooling
  TEST_ASSERT_EQUAL_UINT32(700, b.cooldownRemaining(500));  // 1000 - (500-200)
  TEST_ASSERT_TRUE(b.allow(1300));       // cooldown elapsed -> HalfOpen probe
  TEST_ASSERT_EQUAL(BreakerState::HalfOpen, b.state());
  b.onSuccess();                          // probe ok -> Closed
  TEST_ASSERT_EQUAL(BreakerState::Closed, b.state());
  TEST_ASSERT_TRUE(b.allow(1400));
}

static void test_breaker_halfopen_probe_failure_reopens() {
  CircuitBreaker b(BreakerConfig{2, 1000});
  b.onFailure(0);
  b.onFailure(10);  // Open at 10
  TEST_ASSERT_TRUE(b.allow(1100));  // HalfOpen probe
  b.onFailure(1100);                // probe fails -> Open again, cooldown restarts
  TEST_ASSERT_EQUAL(BreakerState::Open, b.state());
  TEST_ASSERT_FALSE(b.allow(1200));
  TEST_ASSERT_TRUE(b.allow(2200));  // cooldown from 1100 elapsed
}

static void test_breaker_default_closed_allows() {
  CircuitBreaker b;
  TEST_ASSERT_TRUE(b.allow(0));
  TEST_ASSERT_EQUAL(BreakerState::Closed, b.state());
  TEST_ASSERT_EQUAL_UINT32(0, b.cooldownRemaining(0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_retryable_transient);
  RUN_TEST(test_not_retryable_definitive);
  RUN_TEST(test_backoff_grows_and_caps);
  RUN_TEST(test_backoff_jitter_within_window);
  RUN_TEST(test_breaker_opens_after_threshold);
  RUN_TEST(test_breaker_success_resets);
  RUN_TEST(test_breaker_cooldown_then_halfopen_probe_success);
  RUN_TEST(test_breaker_halfopen_probe_failure_reopens);
  RUN_TEST(test_breaker_default_closed_allows);
  return UNITY_END();
}
