#pragma once
#include <cstdint>

// late_wait - one bounded waiter and one late callback that share a piece of state
// (lane TGSTALL).
//
// The device resolves every host name with lwIP's raw DNS API (src/sys/net_util.h
// resolveHostLocked): the query goes out, the caller waits at most waitMs for the answer
// callback, and lwIP calls that callback when the answer arrives or when its own retries
// run out. Those retries can outlast the wait (1 + 1 + 2 + 3 s per DNS server, up to
// three servers). The first version kept the shared state on the waiter's stack and
// freed its semaphore on the way out, so an answer later than the wait wrote into a
// dead stack frame and signalled a freed semaphore, on the poll task's own connect path.
//
// The rule: the state lives on the heap and is freed exactly once, by whichever side
// lets go last. Both sides run under the lwIP core lock (the callback runs on the TCPIP
// thread, which holds it), so the two transitions below never race:
//   callbackArrives()  the answer side: true while the waiter is still there (store the
//                      answer, wake it); false when the waiter gave up (the state is the
//                      callback's to free, the answer is dropped).
//   waiterLeaves()     the waiter side, after its bounded wait: true when the answer
//                      already came (read it, the state is the waiter's to free); false
//                      when it has not (the state is the callback's now: never touch it
//                      again).
// A query that will never call back (an immediate answer, an error) is freed by the
// waiter directly. Portable and host-tested (test_late_wait).
namespace nimbus::net {

class LateWait {
 public:
  bool callbackArrives();
  bool waiterLeaves();

 private:
  enum class State : uint8_t { Waiting, Answered, Abandoned };
  State st_ = State::Waiting;
};

}  // namespace nimbus::net
