#pragma once
#include <atomic>
#include <cstdint>

// oauth_step_runner - the main loop STARTS an MCP OAuth sign-in network step; it
// never runs one. Each step (discovery, client registration, the token exchange) is
// an HTTPS request with up to 8-10 s timeouts behind a TLS work-slot wait, and the
// main loop is the one task the 8 s task watchdog watches: run inline, a slow
// server reset the device (lane WDT's finding on Lumi's resets, made reachable by
// the CUM-718 route fix). So a due step runs on a short-lived worker task (the same
// self-deleting pattern as the provider verify: one TLS session, through the work
// slot, never while a turn runs), and the loop only decides. Pure and host-tested
// (test/test_oauth_step_runner).
namespace nimbus {
namespace orch {

class OauthStepRunner {
 public:
  // A worker the device refused to start (a turn in flight, low memory) is asked
  // again after this long; the loop never retries every pass.
  static constexpr uint32_t kRetryMs = 5000;

  // One loop pass: when a step is `due`, none runs, and no retry is pending, start
  // the worker through `spawn` (true = started). Returns whether a worker started.
  template <class Spawn>
  bool pump(bool due, uint32_t now, Spawn&& spawn) {
    if (busy_.load() || !due) return false;
    if (backoff_ && (int32_t)(now - retryAt_) < 0) return false;   // wrap-safe
    backoff_ = false;
    busy_.store(true);   // set before the worker exists: it may finish at once
    if (spawn()) {
      started_++;
      return true;
    }
    busy_.store(false);
    backoff_ = true;
    retryAt_ = now + kRetryMs;
    refused_++;
    return false;
  }
  // The worker finished its one step (called by the worker itself).
  void done() { busy_.store(false); }
  // A worker owns the flow right now: the loop must not touch it.
  bool busy() const { return busy_.load(); }
  uint32_t started() const { return started_; }
  uint32_t refused() const { return refused_; }

 private:
  std::atomic<bool> busy_{false};
  uint32_t retryAt_ = 0;
  bool backoff_ = false;   // a refused start is waiting out kRetryMs
  uint32_t started_ = 0;
  uint32_t refused_ = 0;
};

}  // namespace orch
}  // namespace nimbus
