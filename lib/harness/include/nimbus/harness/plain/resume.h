#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

namespace nimbus {
namespace orch {
namespace tx {
class TxLog;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/resume.h - crash safety (PRD section 7): at-least-once reply, at-most-once
// effects. At boot an intent with no result is shown to the model as 'outcome unknown:
// the device restarted while this ran; check before repeating' and is never re-run; an
// unanswered message gets one automatic resume, a second failure the plain crash
// notice; a reply stored but not sent is sent with no model call. Restart, install and
// head switch are excluded. Registered through hooks.h only (bootResume, HookSlot::Resume).
//
// Seam declared by lane RB-B0a; the body (plain_resume.cpp) is lane RB-X1's.
namespace agent {
namespace plain {

inline constexpr uint8_t kMaxAutoResumes = 1;

struct ResumeDeps {
  nimbus::orch::tx::TxLog* log = nullptr;
};

Status installResume(PlainHooks& hooks, const ResumeDeps& deps);

}  // namespace plain
}  // namespace agent
