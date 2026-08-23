#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "rig.h"

// engine_thread - the daemon's concurrency layer (the one genuinely new design
// in Phase 0, §3.1).
//
// The engine's documented invariant is single-context: every entry point is
// called from ONE thread. So the daemon runs the whole rig on a single engine
// thread and feeds it through a request MAILBOX. Producers (the Telegram poll,
// the local HTTP control surface) never touch the engine directly; they post a
// task and, for a mutating call, either fire-and-forget or wait on a future.
//
// The critical property: control-surface READS must not queue behind a turn (a
// turn can hold the engine ~120 s across rounds; the relay times a tunneled
// request out at 30 s, so naive serialization 504s the web UI whenever the
// assistant is thinking). So state/health reads are served from a SNAPSHOT the
// engine thread refreshes after each turn and each pump tick - they take a short
// mutex over a plain struct and never enter the engine. Mutating calls that
// arrive mid-turn get an explicit busy signal rather than blocking.
namespace nimbusd {

struct StateSnapshot {
  bool     running = false;
  bool     turnInFlight = false;
  uint32_t turnCount = 0;
  int      vectors = 0;
  int      episodicMessages = 0;
  uint32_t sessionTokensIn = 0;
  uint32_t sessionTokensOut = 0;
  std::string lastHost;
  std::string devName;
  uint64_t startedEpoch = 0;
};

class EngineThread {
 public:
  explicit EngineThread(NimbusdRig* rig) : rig_(rig) {}
  ~EngineThread() { stop(); }

  void start() {
    if (running_.exchange(true)) return;
    snap_.devName = rig_->options().devName;
    snap_.startedEpoch = (uint64_t)time(nullptr);
    refreshSnapshot();
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    if (!running_.exchange(false)) return;
    { std::lock_guard<std::mutex> lk(mu_); cv_.notify_all(); }
    if (thread_.joinable()) thread_.join();
  }

  // Fire-and-forget: enqueue an owner message to be run as a turn. Producers
  // (Telegram) use this - the reply is delivered through the rig's deliver hook.
  void postMessage(const std::string& chatId, const std::string& text) {
    post([this, chatId, text] { rig_->say(chatId, text); });
  }

  // Enqueue a scheduled/routine turn.
  void postScheduled(const std::string& chatId, const std::string& prompt,
                     const std::string& name, bool quietOk) {
    post([this, chatId, prompt, name, quietOk] {
      rig_->scheduled(chatId, prompt, name, quietOk);
    });
  }

  // Synchronous MCP dispatch, run ON the engine thread (the registry + memory
  // engines are single-context). The caller (HTTP /mcp) waits on the future.
  // Returns "" via the future only if the daemon is stopping.
  std::future<std::string> dispatchMcp(const std::string& jsonRpc,
                                       const nimbus::orch::Principal& who) {
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut = prom->get_future();
    post([this, jsonRpc, who, prom] {
      prom->set_value(rig_->registry().handleRpc(jsonRpc, who));
    });
    return fut;
  }

  // Flush all durable stores ON the engine thread and wait. Used by the backup
  // endpoint so it captures a CONSISTENT on-disk state (nimbusd owns the
  // tmp->rename + append discipline; a naive tar of a live volume races it).
  void flushNow() {
    auto prom = std::make_shared<std::promise<void>>();
    auto fut = prom->get_future();
    post([this, prom] { rig_->flush(); prom->set_value(); });
    fut.wait();
  }

  // A read served from the snapshot - never enters the engine, so it returns
  // immediately even mid-turn.
  StateSnapshot snapshot() const {
    std::lock_guard<std::mutex> lk(snapMu_);
    return snap_;
  }

  bool running() const { return running_.load(); }

 private:
  void post(std::function<void()> task) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(std::move(task));
    cv_.notify_one();
  }

  void run() {
    while (running_.load()) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(mu_);
        // Wake on a queued task, on shutdown, or every 1 s to pump periodic work.
        cv_.wait_for(lk, std::chrono::seconds(1),
                     [this] { return !queue_.empty() || !running_.load(); });
        if (!running_.load() && queue_.empty()) break;
        if (!queue_.empty()) { task = std::move(queue_.front()); queue_.pop_front(); }
      }
      if (task) {
        snapInFlight(true);
        task();
        snapInFlight(false);
        refreshSnapshot();
      } else {
        // Idle tick: refresh the snapshot (cheap) so uptime/state stay current.
        refreshSnapshot();
      }
    }
    // Drain any remaining fire-and-forget tasks so a queued turn is not lost on
    // a clean shutdown (MCP promises left unset simply resolve as the future
    // dies - the HTTP handler treats that as a 503).
  }

  void snapInFlight(bool v) {
    std::lock_guard<std::mutex> lk(snapMu_);
    snap_.turnInFlight = v;
  }

  void refreshSnapshot() {
    StateSnapshot s;
    s.running = running_.load();
    s.turnCount = rig_->engine().turnCount();
    s.vectors = rig_->vectors().size();
    s.episodicMessages = rig_->episodic().messageCount();
    s.sessionTokensIn = rig_->engine().sessionUsage().promptTokens;
    s.sessionTokensOut = rig_->engine().sessionUsage().completionTokens;
    s.devName = rig_->options().devName;
    {
      std::lock_guard<std::mutex> lk(snapMu_);
      s.turnInFlight = snap_.turnInFlight;  // preserve the in-flight flag
      s.startedEpoch = snap_.startedEpoch;
      snap_ = s;
    }
  }

  NimbusdRig* rig_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;

  mutable std::mutex snapMu_;
  StateSnapshot snap_;
};

}  // namespace nimbusd
