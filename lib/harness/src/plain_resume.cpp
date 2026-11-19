// Crash resume (plain/resume.h). Lane RB-X1. RED-FIRST PLACEHOLDER: registers a seam that does nothing.
#include "nimbus/harness/plain/resume.h"

namespace agent {
namespace plain {

Status installResume(PlainHooks& hooks, const ResumeDeps& deps) {
  if (!deps.log || !deps.arena || !deps.head || !deps.runTurn || !deps.deliver || !deps.context)
    return Status::fail(Err::InvalidArgument, "plain::installResume");
  return hooks.bootResume(HookSlot::Resume, [](const BootCtx&) { return Status::success(); });
}

}  // namespace plain
}  // namespace agent
