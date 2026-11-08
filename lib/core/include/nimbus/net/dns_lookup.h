#pragma once
#include <cstdint>

#include "nimbus/net/late_wait.h"

// dns_lookup - the device's name lookup (src/sys/net_util.h resolveHostLocked), as a
// template over the lwIP seam so the host tests run the code the device runs
// (test_late_wait drives it with a fake lwIP; the device instantiates it with
// nimbus_net_detail::LwipDnsOps).
//
// One lookup: the query goes out under the lwIP core lock and an answer already cached
// comes back at once. Otherwise the caller waits at most waitMs for lwIP's found
// callback, without the lock (the answer arrives on the TCPIP thread, which needs it).
// lwIP's own retries can outlast that wait (1 + 1 + 2 + 3 s per DNS server, up to three
// servers), so the state the two sides share lives on the heap and whichever side lets
// go last frees it (late_wait.h): an answer after the wait gave up frees it in the
// callback. The first version kept that state on the caller's stack and freed its
// semaphore on return, so a late answer wrote into a dead stack frame and signalled a
// freed semaphore (lane TGSTALL).
//
// Ops, all static:
//   using Addr, Sem                         ip_addr_t, sys_sem_t on the device
//   template <class T> T* make()            a new T, or nullptr when out of memory
//   template <class T> void destroy(T*)
//   bool semNew(Sem&)  void semFree(Sem&)  void semSignal(Sem&)  void semWait(Sem&, uint32_t ms)
//   bool onTcpipThread()                    the caller already holds the core lock
//   DnsQuery query(bool own, const char* host, Addr& cached, FoundFn fn, void* arg)
//                                           dns_gethostbyname under the core lock (taken
//                                           unless `own`): Cached fills `cached` and no
//                                           callback comes; Pending: `fn(arg)` comes
//                                           later, on the TCPIP thread, the lock held;
//                                           Failed: no answer and no callback
//   bool leave(bool own, LateWait& cell)    cell.waiterLeaves() under the core lock
namespace nimbus::net {

enum class DnsQuery : uint8_t { Cached, Pending, Failed };

template <class Ops>
class DnsLookup {
 public:
  using Addr = typename Ops::Addr;
  using FoundFn = void (*)(const char*, const Addr*, void*);

  // True with `out` set when an answer came within waitMs (or was cached).
  static bool resolve(const char* host, Addr& out, uint32_t waitMs) {
    State* s = Ops::template make<State>();
    if (!s) return false;
    if (!Ops::semNew(s->sem)) {
      Ops::destroy(s);
      return false;
    }
    const bool own = Ops::onTcpipThread();
    Addr cached{};
    const DnsQuery q = Ops::query(own, host, cached, &found, s);
    if (q != DnsQuery::Pending) {   // no callback will come: the state is ours
      release(s);
      if (q != DnsQuery::Cached) return false;
      out = cached;
      return true;
    }
    // The lock holder (the TCPIP thread itself) cannot block for the answer: it leaves
    // at once and the callback frees the state.
    if (!own) Ops::semWait(s->sem, waitMs);
    if (!Ops::leave(own, s->cell)) return false;   // gave up first: never touch s again
    const bool ok = s->ok;
    if (ok) out = s->addr;
    release(s);
    return ok;
  }

  // lwIP's found callback: on the TCPIP thread, the core lock held, so it never races
  // leave(). `addr` is null when the lookup failed.
  static void found(const char*, const Addr* addr, void* arg) {
    State* s = static_cast<State*>(arg);
    if (!s->cell.callbackArrives()) {   // the waiter left: the state is ours to free
      release(s);
      return;
    }
    if (addr) {
      s->addr = *addr;
      s->ok = true;
    }
    Ops::semSignal(s->sem);
  }

 private:
  struct State {
    typename Ops::Sem sem{};
    Addr addr{};
    bool ok = false;
    LateWait cell;
  };
  static void release(State* s) {
    Ops::semFree(s->sem);
    Ops::destroy(s);
  }
};

}  // namespace nimbus::net
