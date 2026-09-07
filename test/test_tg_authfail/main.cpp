// test_tg_authfail - CUM-308: honest "Telegram bot token rejected" telemetry.
//
// The token is verified ONCE (getMe) when saved; the verdict is cached. When the
// token is later revoked (a key rotation), Telegram answers every getUpdates with
// HTTP 401 while the status surface kept reporting tgVerify=1 / "configured" and
// the log filled with poll auth failures - the same lying-status class as
// CUM-303/306/307. These pin the portable debounce that turns a stream of poll
// outcomes into a "token rejected" verdict: only 401/403 counts, a 409 Conflict
// and a Transient (network/timeout) never trip it, and a single ok poll clears it.
#include <unity.h>

#include "nimbus/net/telegram_auth.h"

using nimbus::net::TgPollOutcome;
using nimbus::net::tgTokenRejected;
using nimbus::net::TelegramAuthFailDetector;

void setUp() {}
void tearDown() {}

// The pure predicate: rejected only once the streak reaches the threshold, and a
// zero threshold can never trip (guards a misconfigured detector).
static void test_predicate_threshold() {
  TEST_ASSERT_FALSE(tgTokenRejected(0, 3));
  TEST_ASSERT_FALSE(tgTokenRejected(2, 3));
  TEST_ASSERT_TRUE(tgTokenRejected(3, 3));
  TEST_ASSERT_TRUE(tgTokenRejected(9, 3));
  TEST_ASSERT_FALSE(tgTokenRejected(9, 0));  // threshold 0: never
}

// The task's headline stream: ok, 401, 401, 401 -> rejected; then ok -> cleared.
static void test_revoked_then_reissued() {
  TelegramAuthFailDetector det(3);
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Ok));        // token works
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::AuthFail));  // 1
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::AuthFail));  // 2 - still not tripped
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::AuthFail));   // 3 - rejected
  TEST_ASSERT_TRUE(det.rejected());
  // A re-issued token: the next successful poll clears the verdict immediately.
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Ok));
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_EQUAL_UINT16(0, det.authFailStreak());
}

// A lone 401 blip (a transient auth hiccup) must NOT trip a token that is fine.
static void test_single_401_does_not_trip() {
  TelegramAuthFailDetector det(3);
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::AuthFail));
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Ok));   // recovered
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_EQUAL_UINT16(0, det.authFailStreak());
}

// A 409 Conflict is a valid token being polled by another client - a DIFFERENT
// condition with its own handling. A stream of them must NEVER mark it rejected.
static void test_conflict_never_rejects() {
  TelegramAuthFailDetector det(3);
  for (int i = 0; i < 50; i++)
    TEST_ASSERT_FALSE(det.update(TgPollOutcome::Conflict));
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_EQUAL_UINT16(0, det.authFailStreak());
}

// Network errors / timeouts (Transient) prove nothing about the token and must
// NEVER trip it, however long the outage runs.
static void test_transient_never_rejects() {
  TelegramAuthFailDetector det(3);
  for (int i = 0; i < 50; i++)
    TEST_ASSERT_FALSE(det.update(TgPollOutcome::Transient));
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_EQUAL_UINT16(0, det.authFailStreak());
}

// Neutral outcomes (409 / Transient) interleaved with auth failures neither
// advance nor RESET the streak: a revoked token flapping against a busy network
// is still caught, and a Conflict mid-streak does not mask it.
static void test_neutral_outcomes_are_inert() {
  TelegramAuthFailDetector det(3);
  det.update(TgPollOutcome::AuthFail);                    // 1
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Transient)); // neutral, streak holds at 1
  TEST_ASSERT_EQUAL_UINT16(1, det.authFailStreak());
  det.update(TgPollOutcome::AuthFail);                    // 2
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Conflict));  // neutral, streak holds at 2
  TEST_ASSERT_EQUAL_UINT16(2, det.authFailStreak());
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::AuthFail));   // 3 - rejected
}

// A genuinely healthy bot - a long run of successful polls - never trips, no
// matter how many cycles it runs. This is the fresh/steady-state path.
static void test_steady_ok_never_trips() {
  TelegramAuthFailDetector det;   // default threshold
  for (int i = 0; i < 200; i++)
    TEST_ASSERT_FALSE(det.update(TgPollOutcome::Ok));
  TEST_ASSERT_FALSE(det.rejected());
}

// After it trips, the streak keeps climbing (still rejected) and only an ok poll
// clears it - the report never gets stuck stale, and never flickers on more 401s.
static void test_stays_rejected_until_ok() {
  TelegramAuthFailDetector det(3);
  for (int i = 0; i < 10; i++) det.update(TgPollOutcome::AuthFail);
  TEST_ASSERT_TRUE(det.rejected());
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::Conflict));  // neutral: still rejected
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::Transient)); // neutral: still rejected
  TEST_ASSERT_FALSE(det.update(TgPollOutcome::Ok));       // only an ok poll clears it
}

// The default threshold is 3 (documented constant), so a revoked token is caught
// on the third consecutive auth failure with no configuration.
static void test_default_threshold_is_three() {
  TelegramAuthFailDetector det;
  TEST_ASSERT_EQUAL_UINT16(3, det.threshold());
  det.update(TgPollOutcome::AuthFail);
  det.update(TgPollOutcome::AuthFail);
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::AuthFail));
}

// A threshold of 0 is clamped to 1 (never a can't-trip detector), so an explicit
// misconfiguration still catches a revoked token rather than silently ignoring it.
static void test_zero_threshold_clamped() {
  TelegramAuthFailDetector det(0);
  TEST_ASSERT_EQUAL_UINT16(1, det.threshold());
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::AuthFail));  // trips on the first
}

// reset() returns a clean slate for a live token swap: the old bot's auth-fail
// streak must not carry into the new, unproven token.
static void test_reset_clears_for_token_swap() {
  TelegramAuthFailDetector det(3);
  for (int i = 0; i < 5; i++) det.update(TgPollOutcome::AuthFail);
  TEST_ASSERT_TRUE(det.rejected());
  det.reset();
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_EQUAL_UINT16(0, det.authFailStreak());
  // The new token needs its own full streak before it could be called rejected.
  det.update(TgPollOutcome::AuthFail);
  det.update(TgPollOutcome::AuthFail);
  TEST_ASSERT_FALSE(det.rejected());
  TEST_ASSERT_TRUE(det.update(TgPollOutcome::AuthFail));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_predicate_threshold);
  RUN_TEST(test_revoked_then_reissued);
  RUN_TEST(test_single_401_does_not_trip);
  RUN_TEST(test_conflict_never_rejects);
  RUN_TEST(test_transient_never_rejects);
  RUN_TEST(test_neutral_outcomes_are_inert);
  RUN_TEST(test_steady_ok_never_trips);
  RUN_TEST(test_stays_rejected_until_ok);
  RUN_TEST(test_default_threshold_is_three);
  RUN_TEST(test_zero_threshold_clamped);
  RUN_TEST(test_reset_clears_for_token_swap);
  return UNITY_END();
}
