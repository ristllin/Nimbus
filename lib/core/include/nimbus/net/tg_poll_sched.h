#pragma once
#include <cstdint>

// tg_poll_sched - when the tg_poll loop ends an idle wait early for a waiting
// local turn (CUM-462).
//
// On-device voice, web and serial turns reach the orchestrator through
// telegram::injectMessage(): a queue the tg_poll task drains once per loop cycle.
// On a device with a bot token that task spends almost all of its time idle,
// blocked in the getUpdates long-poll (30 s, 18 s while jobs run), then in the
// inter-cycle pause or a poll-error backoff, and none of those waits looked at the
// queue. So a hold-to-talk turn whose transcript was ready sat there for up to the
// whole long-poll before it started.
//
// This is the portable rule the device loop consults at each idle wait. It adds NO
// concurrency (AGENTS.md section 4): the same single task, the same single TLS
// session, the same serialized turn path. It only decides when an idle wait ends;
// the local turn then runs on tg_poll exactly as it always did. The rules, each
// pinned by test/test_tg_poll_sched:
//   1. Cycle top (TgPollScheduler::plan): with a local turn waiting, the cycle
//      skips its Telegram poll and runs the turn at once - but never two cycles
//      running. The cycle after a skip, if a local turn is waiting again, gives
//      Telegram a Check first: an immediate getUpdates (timeout 0) that is read
//      whole, so Telegram is served at least every other cycle however steady
//      the local stream and however slow the link.
//   2. Long-poll (tgWaitStep): a local turn that arrives while a long-poll waits
//      for its answer cuts the wait short. Only an idle wait is ever cut: never
//      once a decrypted response byte is available, and never a Check (it answers
//      within a round trip). The offset is not advanced, so anything Telegram held
//      is re-served by the next poll: at-least-once, nothing lost.
//   3. Pauses (tgIdleSleepMs): a waiting local turn ends the inter-cycle pause and
//      the poll-error backoff at once. They are slept in kTgIdleSliceMs slices,
//      checked between.
namespace nimbus::net {

// Slice length of the inter-cycle pause and the poll-error backoff: the longest a
// waiting local turn can sit in either before the loop notices it.
constexpr uint32_t kTgIdleSliceMs = 50;

// What one loop cycle does about Telegram.
enum class TgPoll : uint8_t {
  LongPoll,  // nothing local waiting: the normal long-poll
  Skip,      // a local turn waits: no poll this cycle, run the turn now
  Check,     // a local turn waits again right after a skip: Telegram's turn first,
             // as an immediate getUpdates (timeout 0), read whole
};

// Rule 1. The only state: whether the previous cycle skipped its poll.
class TgPollScheduler {
 public:
  // Called once per loop cycle, at its top. Never Skip twice running.
  TgPoll plan(bool localWaiting) {
    if (!localWaiting) {
      skippedLast_ = false;
      return TgPoll::LongPoll;
    }
    skippedLast_ = !skippedLast_;
    return skippedLast_ ? TgPoll::Skip : TgPoll::Check;
  }

 private:
  bool skippedLast_ = false;
};

// The getUpdates timeout (seconds) a planned poll asks for: a Check asks Telegram
// to answer at once; a long-poll keeps the loop's own timeout.
constexpr int tgPollTimeoutS(TgPoll plan, int longPollS) {
  return plan == TgPoll::Check ? 0 : longPollS;
}

// Rule 2: one pass of the wait for the first byte of a getUpdates response that
// asked for `timeoutS`.
enum class TgWait : uint8_t {
  Keep,    // nothing yet: sleep a tick and look again
  Read,    // the response has started: read it whole (never abandoned mid-response)
  GiveUp,  // the socket closed or the deadline passed: the caller's failure path
  Yield,   // give the idle long-poll up for the waiting local turn
};

// The checks run in this order so a response in progress always wins, and a dead
// socket or a passed deadline keeps the caller's existing failure path.
constexpr TgWait tgWaitStep(bool bytesReady, bool connected, bool pastDeadline,
                            bool localWaiting, int timeoutS) {
  if (bytesReady) return TgWait::Read;
  if (!connected || pastDeadline) return TgWait::GiveUp;
  if (localWaiting && timeoutS > 0) return TgWait::Yield;
  return TgWait::Keep;
}

// Rule 3: the next sleep of an idle pause of `pauseMs`, `elapsedMs` in. 0 ends the
// pause: its time is up, or a local turn is waiting.
constexpr uint32_t tgIdleSleepMs(bool localWaiting, uint32_t elapsedMs, uint32_t pauseMs) {
  if (localWaiting || elapsedMs >= pauseMs) return 0;
  return pauseMs - elapsedMs < kTgIdleSliceMs ? pauseMs - elapsedMs : kTgIdleSliceMs;
}

}  // namespace nimbus::net
