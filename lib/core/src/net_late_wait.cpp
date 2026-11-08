#include "nimbus/net/late_wait.h"

namespace nimbus::net {

bool LateWait::callbackArrives() {
  if (st_ == State::Abandoned) return false;   // the waiter left: the state is ours to free
  st_ = State::Answered;
  return true;
}

bool LateWait::waiterLeaves() {
  if (st_ == State::Answered) return true;   // the answer came: the state is the waiter's
  st_ = State::Abandoned;                    // the callback frees it when it comes
  return false;
}

}  // namespace nimbus::net
