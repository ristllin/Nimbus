#pragma once
#include <cstdint>

// telegram_auth - honest "the Telegram bot token was rejected" telemetry.
//
// The token is verified ONCE (getMe) when it is saved, and that verdict is
// cached. But a token can be revoked LATER - during a key rotation, say - while
// the device keeps long-polling. Telegram then answers every getUpdates with
// HTTP 401 Unauthorized, yet the cached verdict still reads "verified" and the
// status surface reports the bot live. The owner's device did exactly this: the
// log filled with poll auth failures while /api/orch reported tgVerify=1 and the
// Health row said "configured" (the same lying-status class as CUM-303/306/307).
//
// This is the portable decision that turns a stream of poll outcomes into a
// debounced "token rejected" verdict. It is deliberately narrow so a blip never
// trips it and a live token never reads as rejected:
//   - only an AUTH failure (HTTP 401/403) counts toward rejection; a 409 Conflict
//     (another poller holds this bot token - a different, existing condition) and
//     a Transient outcome (network error, timeout, a 5xx, an unparseable body)
//     are NEUTRAL: they neither advance nor clear the streak, so bus/network
//     trouble is never evidence in either direction;
//   - a single successful poll (HTTP 200 with ok:true) is proof the token works
//     and clears the verdict IMMEDIATELY;
//   - the fault is claimed only after a debounce streak of consecutive auth
//     failures, so a one-off 401 does not trip it but a revoked token is caught
//     within a few poll cycles (seconds).
//
// Pure + host-tested (no Arduino). The device classifies each getUpdates response
// into one TgPollOutcome per poll cycle and feeds it here; the debounced verdict
// is surfaced over HTTP (/api/orch tgAuthFail, and tgVerify reported as rejected)
// and in the Health report, so the two never disagree. Three rules the tests pin:
//   - it NEVER trips on a stream of 409 Conflicts (a valid token, another poller),
//   - it NEVER trips on a stream of Transient outcomes (network trouble), and
//   - it clears IMMEDIATELY on the next successful poll (a re-issued token).

namespace nimbus::net {

// One Telegram getUpdates long-poll response, classified for the auth-failure
// debounce. Exactly one is produced per poll cycle.
enum class TgPollOutcome : uint8_t {
  Ok,        // HTTP 200 and body ok:true - the token works
  AuthFail,  // HTTP 401 or 403 - the token is rejected (revoked or invalid)
  Conflict,  // HTTP 409 - another client long-polls this bot token (its own handling)
  Transient, // network error, timeout, 5xx, or an unparseable body - proves nothing
};

// The debounced predicate as a pure value: the token is rejected only once the
// consecutive auth-failure streak has reached the debounce threshold.
constexpr bool tgTokenRejected(uint16_t authFailStreak, uint16_t threshold) {
  return threshold != 0 && authFailStreak >= threshold;
}

// Stateful debouncer, driven one poll outcome per poll cycle.
class TelegramAuthFailDetector {
 public:
  // Default 3 consecutive auth failures: enough to ride out a lone 401 blip, short
  // at the device's long-poll cadence (a revoked token is caught within seconds -
  // a few poll cycles - not left lying "verified" indefinitely).
  static constexpr uint16_t kDefaultThreshold = 3;

  TelegramAuthFailDetector() = default;
  explicit TelegramAuthFailDetector(uint16_t threshold)
      : threshold_(threshold ? threshold : 1) {}

  // Fold in one poll cycle's outcome and return the current verdict.
  bool update(TgPollOutcome outcome) {
    switch (outcome) {
      case TgPollOutcome::Ok:
        authFailStreak_ = 0;              // proof the token works: clear immediately
        break;
      case TgPollOutcome::AuthFail:
        if (authFailStreak_ != 0xFFFF) authFailStreak_++;
        break;
      case TgPollOutcome::Conflict:       // a valid token, another poller
      case TgPollOutcome::Transient:      // network/timeout/5xx/parse hiccup
        break;                            // neutral: neither advance nor clear
    }
    rejected_ = tgTokenRejected(authFailStreak_, threshold_);
    return rejected_;
  }

  // Reset to a clean slate. The device calls this on a live token swap: the new
  // bot starts unproven, and the old bot's auth-fail streak must not carry over.
  void reset() { authFailStreak_ = 0; rejected_ = false; }

  bool     rejected() const { return rejected_; }
  uint16_t authFailStreak() const { return authFailStreak_; }
  uint16_t threshold() const { return threshold_; }

 private:
  uint16_t threshold_ = kDefaultThreshold;
  uint16_t authFailStreak_ = 0;
  bool     rejected_ = false;
};

}  // namespace nimbus::net
