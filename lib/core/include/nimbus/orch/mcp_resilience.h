#pragma once
#include <cstdint>

#include "nimbus/orch/mcp_client.h"  // ErrorKind

// mcp_resilience - the PORTABLE resilience policy for the outbound MCP client
// (host-tested via pio test -e native): a per-server circuit breaker plus the
// retry-with-jitter and retryability rules. NO Arduino / time / RNG here - the
// device seam passes in the current millis() and a random jitter value, so the
// whole policy is deterministic under test.
//
// Why a breaker at all: the turn path is single-TLS-slot and fully serialized
// (AGENTS.md frozen invariant). A dead or slow MCP server would otherwise burn
// the turn's whole budget on every call, one timeout after another. The breaker
// trips after a few consecutive failures and then fails FAST (no socket, no
// wait) until a cooldown elapses, so one bad server cannot wedge a turn.
namespace nimbus {
namespace orch {
namespace mcp {

// ---- retryability ------------------------------------------------------------

// Whether a failed exchange is worth retrying. Transient transport faults
// (timeout, connect, 5xx, empty 2xx) are; a definitive answer (auth rejected,
// malformed body, an RPC error, an over-cap body) is not - retrying just wastes
// the budget. `httpStatus` is only consulted for ErrorKind::Http.
bool isRetryable(ErrorKind kind, int httpStatus);

// ---- retry backoff -----------------------------------------------------------

struct RetryConfig {
  int      maxAttempts = 3;      // total tries incl. the first (>=1)
  uint32_t baseDelayMs = 250;    // first backoff window
  uint32_t capDelayMs  = 4000;   // per-attempt ceiling before jitter
};

// The delay before retry `attempt` (0-based: the wait AFTER attempt 0 fails).
// Equal-jitter backoff: window = min(cap, base << attempt); the delay is
// window/2 + (jitter mod (window/2 + 1)), i.e. uniformly in [window/2, window].
// `jitter` is any value the caller sourced (device: esp_random()).
uint32_t retryDelayMs(const RetryConfig& cfg, int attempt, uint32_t jitter);

// ---- circuit breaker ---------------------------------------------------------

enum class BreakerState : uint8_t { Closed = 0, Open = 1, HalfOpen = 2 };

struct BreakerConfig {
  int      failureThreshold = 3;      // consecutive failures in Closed -> Open
  uint32_t openCooldownMs   = 30000;  // Open dwell before a HalfOpen probe
};

// A per-server breaker. Not thread-safe by design (the turn path is serialized);
// one instance lives per configured MCP server in the device seam.
class CircuitBreaker {
 public:
  CircuitBreaker() = default;
  explicit CircuitBreaker(const BreakerConfig& cfg) : cfg_(cfg) {}

  // May a request proceed at `nowMs`? In Open, transitions to HalfOpen (allowing
  // ONE probe) once the cooldown has elapsed; otherwise returns false. In
  // HalfOpen a second concurrent call would also return true, but the path is
  // serialized so at most one probe is in flight.
  bool allow(uint32_t nowMs);

  // Report the outcome of the request that allow() cleared.
  void onSuccess();
  void onFailure(uint32_t nowMs);

  BreakerState state() const { return state_; }
  int consecutiveFailures() const { return consecutiveFailures_; }

  // ms until allow() would clear a probe (0 when it would clear now). Lets the
  // caller render "server <x> is cooling down, try again in Ns".
  uint32_t cooldownRemaining(uint32_t nowMs) const;

 private:
  BreakerConfig cfg_;
  BreakerState  state_ = BreakerState::Closed;
  int           consecutiveFailures_ = 0;
  uint32_t      openedAt_ = 0;
};

}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
