#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "nimbus/net/tg_poll_sched.h"

// tg_backoff - how long the tg_poll loop waits after a getUpdates that failed.
//
// Every getUpdates answer other than a 200 is a fetch error that backs off. Before
// this rule only 401, 403 and 409 did: a 429 (flood control), a 5xx, or a 404 from a
// malformed token fell through to the body parse, read as an empty page, and the
// loop polled again a second later, forever, ignoring Telegram's retry_after. A 200
// whose body does not parse as an ok:true page is a fetch error too.
//
// The backoff is a deadline, not a sleep: the loop does not poll Telegram before it,
// even when a waiting local turn (voice, web, serial) ends the pause early (CUM-462
// lets local turns run during a backoff; it never lets a poll in). The wait grows
// with consecutive errors (step x errors, capped at maxSteps steps) and is at least
// Telegram's retry_after when the error carried one, capped at kTgRetryAfterMaxS so
// a malformed value cannot silence the bot for days. A good poll clears it. Pure,
// host-tested in test/test_tg_backoff; adds no task and no TLS session.
namespace nimbus::net {

// The longest retry_after honored, in seconds.
constexpr uint32_t kTgRetryAfterMaxS = 3600;

// Digits after `p` (skipping spaces), as a non-negative int, or -1 when none. A value
// over kTgSecondsMax is nonsense and reads as absent. 32-bit unsigned arithmetic with
// the bound checked BEFORE each multiply, so host and device (where long is 32 bits)
// agree and nothing overflows.
constexpr uint32_t kTgSecondsMax = 1000000000u;
inline int tgParseSeconds(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  if (p >= end || *p < '0' || *p > '9') return -1;
  uint32_t v = 0;
  for (; p < end && *p >= '0' && *p <= '9'; ++p) {
    const uint32_t d = (uint32_t)(*p - '0');
    if (v > (kTgSecondsMax - d) / 10u) return -1;
    v = v * 10u + d;
  }
  return (int)v;
}

// retry_after from a Telegram error body
// ({"ok":false,"error_code":429,...,"parameters":{"retry_after":N}}), or -1.
inline int tgRetryAfterS(const char* body, size_t len) {
  static const char kKey[] = "\"retry_after\"";
  const size_t k = sizeof(kKey) - 1;
  if (!body || len < k) return -1;
  const char* end = body + len;
  for (const char* p = body; p + k <= end; ++p) {
    if (std::memcmp(p, kKey, k) != 0) continue;
    const char* q = p + k;
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    if (q >= end || *q != ':') return -1;
    return tgParseSeconds(q + 1, end);
  }
  return -1;
}

// The seconds form of a Retry-After header value, or -1 (the date form is ignored).
inline int tgRetryAfterHeaderS(const char* value) {
  return value ? tgParseSeconds(value, value + std::strlen(value)) : -1;
}

class TgPollBackoff {
 public:
  TgPollBackoff(uint32_t stepMs, uint32_t maxSteps) : stepMs_(stepMs), maxSteps_(maxSteps) {}

  // A fetch error at `nowMs`, with Telegram's retry_after (-1 when none). Returns how
  // long the loop must wait before its next poll.
  uint32_t onError(uint32_t nowMs, int retryAfterS) {
    ++fails_;
    uint32_t waitMs = stepMs_ * (fails_ < maxSteps_ ? fails_ : maxSteps_);
    if (retryAfterS > 0) {
      const uint32_t s = (uint32_t)retryAfterS < kTgRetryAfterMaxS ? (uint32_t)retryAfterS
                                                                   : kTgRetryAfterMaxS;
      if (s * 1000u > waitMs) waitMs = s * 1000u;
    }
    notBefore_ = nowMs + waitMs;
    active_ = true;
    return waitMs;
  }

  // A good poll (or a fresh token): no wait, the error count starts again.
  void onOk() {
    fails_ = 0;
    active_ = false;
  }

  bool mayPoll(uint32_t nowMs) const { return !active_ || (int32_t)(nowMs - notBefore_) >= 0; }

  uint32_t remainingMs(uint32_t nowMs) const {
    return mayPoll(nowMs) ? 0 : (uint32_t)(notBefore_ - nowMs);
  }

  uint32_t fails() const { return fails_; }

 private:
  uint32_t stepMs_;
  uint32_t maxSteps_;
  uint32_t fails_ = 0;
  uint32_t notBefore_ = 0;
  bool     active_ = false;
};

// The cycle's poll plan with the backoff applied: inside a backoff the cycle skips its
// Telegram poll (a waiting local turn still runs).
constexpr TgPoll tgPlanWithBackoff(TgPoll plan, bool mayPoll) {
  return mayPoll ? plan : TgPoll::Skip;
}

}  // namespace nimbus::net
