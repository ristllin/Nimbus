#include "nimbus/net/late_wait.h"

// FAILING-FIRST STUB (lane TGSTALL step 1): the pre-fix shape, where the waiter always
// frees the state on its way out and the callback always uses it.
namespace nimbus::net {
bool LateWait::callbackArrives() { st_ = State::Answered; return true; }
bool LateWait::waiterLeaves() { return true; }
}  // namespace nimbus::net
