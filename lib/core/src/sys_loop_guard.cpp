#include "nimbus/sys/loop_guard.h"

#include <algorithm>

#include "nimbus/sys/boot_record.h"   // fieldValue: a tool name is model text, capped and printable

namespace nimbus::sys {

uint32_t lockWaitMs(bool watchedTask, bool ownsLock, uint32_t requestedMs) {
  if (!watchedTask || ownsLock) return UINT32_MAX;   // an owner's re-take never waits
  return std::min(requestedMs, kLoopLockWaitMs);
}

bool LoopPassMonitor::note(uint32_t passMs, uint32_t nowMs) {
  if (passMs > maxMs_) {
    maxMs_ = passMs;
    maxAt_ = nowMs;
  }
  if (passMs < kSlowPassMs) return false;
  ++slow_;
  const bool first = !logged_;
  const bool higher = logged_ && passMs >= loggedMs_ + kSlowPassRelogStepMs;
  const bool quietOver = logged_ && (uint32_t)(nowMs - loggedAt_) >= kSlowPassRelogQuietMs;
  if (!first && !higher && !quietOver) return false;
  logged_ = true;
  loggedMs_ = passMs;
  loggedAt_ = nowMs;
  return true;
}

std::string slowPassLine(uint32_t passMs, const LoopPassMonitor& m) {
  return "loop: one pass took " + std::to_string(passMs) + " ms (longest " +
         std::to_string(m.maxPassMs()) + " ms, " + std::to_string(m.slowPasses()) +
         (m.slowPasses() == 1 ? " slow pass" : " slow passes") + " this boot)";
}

bool LockHoldMonitor::note(const std::string& what, uint32_t holdMs, uint32_t nowMs) {
  if (holdMs > longestMs_) {
    longestMs_ = holdMs;
    longestWhat_ = what;
  }
  if (holdMs < kLongHoldMs) return false;
  ++long_;
  const bool higher = holdMs >= loggedMs_ + kSlowPassRelogStepMs;
  const bool quietOver = (uint32_t)(nowMs - loggedAt_) >= kLongHoldRelogQuietMs;
  if (logged_ && !higher && !quietOver) return false;
  logged_ = true;
  loggedMs_ = holdMs;
  loggedAt_ = nowMs;
  return true;
}

std::string longHoldLine(const std::string& what, uint32_t holdMs) {
  return "memory: " + (what.empty() ? std::string("a tool") : "tool " + fieldValue(what, 40)) +
         " held the card lock for " + std::to_string(holdMs) + " ms";
}

}  // namespace nimbus::sys
