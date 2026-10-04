#pragma once
// tg_poll_sim - a millisecond model of the device's tg_poll loop (pollTask in
// src/agent/telegram.cpp), driven by the REAL scheduling rule in
// nimbus/net/tg_poll_sched.h (CUM-462). Host tests use it to pin what the rule is
// for: a local turn (voice / web / serial inject) never waits on an idle wait of the
// loop, Telegram is never starved by local turns, and nothing new churns TLS.
//
// One cycle mirrors pollTask, in the same order:
//   top      TgPollScheduler::plan(local waiting). Skip closes the poll socket
//            (the local turn needs the single TLS session).
//   poll     LongPoll or Check (timeout tgPollTimeoutS): reconnect when the socket
//            is closed (handshakeMs), send getUpdates, then wait on tgWaitStep()
//            (consulted at every tick its answer could change): Read (Telegram
//            answered) or Yield (the socket is closed; the offset is untouched, so
//            whatever Telegram held is re-served next poll). An answer with updates
//            closes the socket and runs one turn; an empty answer keeps it open
//            (keep-alive). With telegramDown the connect fails after connectFailMs
//            and the poll-error backoff is slept like the pause.
//   drain    every waiting local message is batched into one turn (socket closed)
//   work     job tick + reply drain (workMs)
//   pause    slept in tgIdleSleepMs() slices
// "Idle" time is time spent in the long-poll wait, the pause or the backoff - the
// waits the rule governs. Handshakes, failed connects, turns and work are busy time
// no scheduling rule can skip.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

#include "nimbus/net/tg_poll_sched.h"

namespace tgsim {

struct Config {
  uint32_t handshakeMs = 1500;    // TLS connect when the poll socket is closed
  uint32_t longPollS = 30;        // TELEGRAM_LONG_POLL_TIMEOUT_S
  uint32_t rttMs = 150;           // Telegram answers this soon after it can
  uint32_t turnMs = 4000;         // one orchestrator turn (Telegram or local)
  uint32_t workMs = 20;           // job tick + reply drain
  uint32_t pauseMs = 1000;        // TELEGRAM_POLL_INTERVAL_MS
  bool telegramDown = false;      // every connect fails ...
  uint32_t connectFailMs = 20400; // ... after this long (2 x 10 s + 0.4 s retry)
  uint32_t backoffStepMs = 1500;  // TG_BACKOFF_STEP_MS
  uint32_t backoffMaxSteps = 8;   // TG_BACKOFF_MAX_STEPS
};

struct Stats {
  uint32_t cycles = 0, longPolls = 0, checks = 0, skips = 0, yields = 0, handshakes = 0;
  uint32_t maxSkipRun = 0;   // longest run of consecutive skipped polls
};

struct Dispatched {
  uint32_t injectedAt;
  uint32_t startedAt;   // the turn carrying it began
  uint32_t idleMs;      // of the wait, how much was spent in an idle wait
  uint32_t latency() const { return startedAt - injectedAt; }
};

class Sim {
 public:
  explicit Sim(Config c = Config()) : c_(c) {}

  void injectLocal(uint32_t at) { insertSorted(futureLocal_, at); }
  void telegramArrives(uint32_t at) { insertSorted(tg_, at); }

  void runUntil(uint32_t t) {
    while (now_ < t) cycle();
  }

  uint32_t now() const { return now_; }
  const Stats& stats() const { return stats_; }
  const std::vector<Dispatched>& local() const { return localDone_; }
  const std::vector<Dispatched>& telegram() const { return tgDone_; }
  size_t telegramPending() const { return tg_.size(); }

 private:
  struct Waiting {
    uint32_t at;
    uint32_t idleMs;
  };

  static void insertSorted(std::vector<uint32_t>& v, uint32_t at) {
    v.insert(std::upper_bound(v.begin(), v.end(), at), at);
  }

  bool localWaiting() const { return !waiting_.empty(); }

  // Injects due by `now_` join the inbound queue.
  void deliverDue() {
    while (!futureLocal_.empty() && futureLocal_.front() <= now_) {
      waiting_.push_back({futureLocal_.front(), 0});
      futureLocal_.erase(futureLocal_.begin());
    }
  }

  // Advance the clock. Idle time is charged to every message waiting through it,
  // including one injected part-way.
  void advance(uint32_t ms, bool idle) {
    const uint32_t t0 = now_, t1 = now_ + ms;
    if (idle)
      for (Waiting& w : waiting_) w.idleMs += ms;
    while (!futureLocal_.empty() && futureLocal_.front() <= t1) {
      const uint32_t at = futureLocal_.front() < t0 ? t0 : futureLocal_.front();
      waiting_.push_back({futureLocal_.front(), idle ? t1 - at : 0});
      futureLocal_.erase(futureLocal_.begin());
    }
    now_ = t1;
  }

  // An idle pause (inter-cycle pause or poll-error backoff), sliced exactly as
  // idlePause() does on the device.
  void idleSleep(uint32_t pauseMs) {
    const uint32_t start = now_;
    for (;;) {
      deliverDue();
      const uint32_t ms = nimbus::net::tgIdleSleepMs(localWaiting(), now_ - start, pauseMs);
      if (ms == 0) return;
      advance(ms, true);
    }
  }

  // When Telegram answers a getUpdates sent at `sentAt` asking for `timeoutMs`: a
  // round trip after it holds an update, else empty when the timeout runs out (a
  // Check, timeout 0, answers after one round trip either way).
  uint32_t answerAt(uint32_t sentAt, uint32_t timeoutMs) const {
    const uint32_t empty = sentAt + std::max(timeoutMs, c_.rttMs);
    if (tg_.empty()) return empty;
    return std::min(std::max(tg_.front(), sentAt) + c_.rttMs, empty);
  }

  void dispatchLocal() {
    socketOpen_ = false;   // the turn needs the single TLS session
    for (const Waiting& w : waiting_) localDone_.push_back({w.at, now_, w.idleMs});
    waiting_.clear();
    advance(c_.turnMs, false);
  }

  // Telegram answered: an update it could have answered with (reached it, a round
  // trip ago) is in the answer. Updates close the socket and run one turn; an empty
  // answer keeps the socket open.
  void readAnswer(uint32_t sentAt) {
    std::vector<uint32_t> got;
    while (!tg_.empty() && std::max(tg_.front(), sentAt) + c_.rttMs <= now_) {
      got.push_back(tg_.front());
      tg_.erase(tg_.begin());
    }
    if (got.empty()) return;
    socketOpen_ = false;
    for (uint32_t at : got) tgDone_.push_back({at, now_, 0});
    advance(c_.turnMs, false);
  }

  void pollError() {
    ++fails_;
    advance(c_.connectFailMs, false);
    idleSleep(c_.backoffStepMs * std::min(fails_, c_.backoffMaxSteps));
  }

  void poll(nimbus::net::TgPoll plan) {
    const int timeoutS = nimbus::net::tgPollTimeoutS(plan, int(c_.longPollS));
    if (c_.telegramDown) return pollError();
    if (!socketOpen_) {
      ++stats_.handshakes;
      advance(c_.handshakeMs, false);
      socketOpen_ = true;
    }
    fails_ = 0;
    const uint32_t sentAt = now_;
    const uint32_t answer = answerAt(sentAt, uint32_t(timeoutS) * 1000u);
    for (;;) {
      deliverDue();
      const nimbus::net::TgWait w =
          nimbus::net::tgWaitStep(now_ >= answer, true, false, localWaiting(), timeoutS);
      if (w == nimbus::net::TgWait::Read) return readAnswer(sentAt);
      if (w == nimbus::net::TgWait::Yield) {
        ++stats_.yields;
        socketOpen_ = false;
        return;
      }
      // Keep: the device sleeps a tick and looks again. Nothing tgWaitStep reads
      // changes before the next event (the answer or an inject), so jumping there
      // is exactly equivalent to ticking, and fast.
      uint32_t next = answer;
      if (!futureLocal_.empty()) next = std::min(next, futureLocal_.front());
      advance(next > now_ ? next - now_ : 1, true);
    }
  }

  void cycle() {
    ++stats_.cycles;
    deliverDue();
    const nimbus::net::TgPoll plan = sched_.plan(localWaiting());
    if (plan == nimbus::net::TgPoll::Skip) {
      ++stats_.skips;
      stats_.maxSkipRun = std::max(stats_.maxSkipRun, ++skipRun_);
      socketOpen_ = false;
    } else {
      skipRun_ = 0;
      ++(plan == nimbus::net::TgPoll::Check ? stats_.checks : stats_.longPolls);
      poll(plan);
    }
    deliverDue();
    if (localWaiting()) dispatchLocal();
    advance(c_.workMs, false);
    idleSleep(c_.pauseMs);
  }

  Config c_;
  nimbus::net::TgPollScheduler sched_;
  uint32_t now_ = 0;
  bool socketOpen_ = false;
  uint32_t skipRun_ = 0;
  uint32_t fails_ = 0;
  std::vector<uint32_t> futureLocal_, tg_;
  std::deque<Waiting> waiting_;
  std::vector<Dispatched> localDone_, tgDone_;
  Stats stats_;
};

}  // namespace tgsim
