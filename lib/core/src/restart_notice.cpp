#include "nimbus/orch/restart_notice.h"

#include <utility>

#include "nimbus/sys/reset_reason.h"

namespace nimbus::orch {

bool isOwnerRequestKind(const std::string& kind) {
  return kind == "photo" || kind == "file" || kind == "voice" || kind == "message" || kind == "console";
}

RestartNoticePlan planRestartNotice(const std::string& deviceName, int resetReason, const TurnJournal* lost,
                                    const std::string& hhmm, const std::string& crashCause) {
  RestartNoticePlan p;
  if (!nimbus::sys::isCrashReset(resetReason)) return p;   // the non-crash line comes later
  p.text = crashNotice(deviceName, lost, hhmm, crashCause);
  // Only a journal that names the device's own work holds the notice: the owner
  // is not waiting on it. An owner request, or nothing in progress, goes at once.
  const bool backgroundTurn = lost && !isOwnerRequestKind(lost->kind);
  p.timing = backgroundTurn ? NoticeTiming::Held : NoticeTiming::Now;
  return p;
}

std::string RestartNoticeQueue::release() {
  std::string out;
  out.swap(held_);
  return out;
}

std::string RestartNoticeQueue::atBoot(RestartNoticePlan plan) {
  if (booted_) return {};   // one plan per boot
  booted_ = true;
  if (plan.timing == NoticeTiming::Now) return std::move(plan.text);
  if (plan.timing == NoticeTiming::Held) held_.swap(plan.text);
  return {};
}

std::string RestartNoticeQueue::atTurnStart(const std::string& turnKind) {
  return isOwnerRequestKind(turnKind) ? release() : std::string();
}

}  // namespace nimbus::orch
