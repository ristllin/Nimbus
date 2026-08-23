#include "nimbus/orch/mcp_resilience.h"

namespace nimbus {
namespace orch {
namespace mcp {

bool isRetryable(ErrorKind kind, int httpStatus) {
  switch (kind) {
    case ErrorKind::Timeout:
    case ErrorKind::Connect:
    case ErrorKind::Empty:
      return true;
    case ErrorKind::Http:
      return httpStatus >= 500 && httpStatus < 600;  // 5xx transient; 4xx is not
    case ErrorKind::Unauthorized:
    case ErrorKind::Malformed:
    case ErrorKind::Rpc:
    case ErrorKind::TooLarge:
    case ErrorKind::None:
    default:
      return false;
  }
}

uint32_t retryDelayMs(const RetryConfig& cfg, int attempt, uint32_t jitter) {
  if (attempt < 0) attempt = 0;
  uint32_t window = cfg.baseDelayMs;
  // window = min(cap, base << attempt), guarding the shift against overflow.
  for (int i = 0; i < attempt; i++) {
    if (window >= cfg.capDelayMs || window > (0xFFFFFFFFu >> 1)) { window = cfg.capDelayMs; break; }
    window <<= 1;
  }
  if (window > cfg.capDelayMs) window = cfg.capDelayMs;
  uint32_t half = window / 2;
  return half + (jitter % (half + 1));  // uniform in [half, window]
}

bool CircuitBreaker::allow(uint32_t nowMs) {
  if (state_ == BreakerState::Open) {
    if ((uint32_t)(nowMs - openedAt_) >= cfg_.openCooldownMs) {
      state_ = BreakerState::HalfOpen;  // let one probe through
      return true;
    }
    return false;
  }
  return true;  // Closed or HalfOpen
}

void CircuitBreaker::onSuccess() {
  consecutiveFailures_ = 0;
  state_ = BreakerState::Closed;
}

void CircuitBreaker::onFailure(uint32_t nowMs) {
  if (state_ == BreakerState::HalfOpen) {
    // The probe failed: back to Open, restart the cooldown.
    state_ = BreakerState::Open;
    openedAt_ = nowMs;
    return;
  }
  if (++consecutiveFailures_ >= cfg_.failureThreshold) {
    state_ = BreakerState::Open;
    openedAt_ = nowMs;
  }
}

uint32_t CircuitBreaker::cooldownRemaining(uint32_t nowMs) const {
  if (state_ != BreakerState::Open) return 0;
  uint32_t elapsed = (uint32_t)(nowMs - openedAt_);
  return elapsed >= cfg_.openCooldownMs ? 0 : (cfg_.openCooldownMs - elapsed);
}

}  // namespace mcp
}  // namespace orch
}  // namespace nimbus
