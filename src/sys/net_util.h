#pragma once
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>   // LOCK_TCPIP_CORE / sys_thread_tcpip (resolveHostLocked, connectHost)
#include <lwip/dns.h>      // dns_gethostbyname (raw API: asserts the lock held, never takes it)
#include <lwip/sys.h>      // sys_sem_t / sys_arch_sem_wait (the bounded wait for the answer)

#include <new>   // std::nothrow: the DNS wait's state lives on the heap (late_wait.h)

#include "nimbus/net/late_wait.h"   // who frees the DNS wait's state: the waiter or a late answer
#include "poll_watch.h"             // lane TGSTALL: every connect and close is a poll-task beat

// agent::store::tlsVerify() - the outbound-TLS security switch. Forward-declared
// (not #include "agent/store.h") so this sys-level header carries no dependency
// on the agent subsystem; the symbol links from src/agent/store.cpp.
namespace agent { namespace store { bool tlsVerify(); } }

// Outbound-TLS setup: the ONE seam every WiFiClientSecure passes through before
// connect(). When store::tlsVerify() is true (default), the server certificate is
// validated against the ~200-root CA bundle the IDF embeds in flash
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) - so an evil-twin AP / rogue router on a
// public network can NO LONGER MITM the provider connections and harvest the
// Authorization: Bearer <key> headers + the Telegram bot token. When false, it
// falls back to the historical setInsecure() (encrypted but unvalidated) - the
// escape hatch for a self-signed custom orchHost or a provider whose root isn't
// bundled (a connection ERROR, not a security hole). See docs/security.md.
//
// The bundle is linked in by the framework; these are its binary blob boundary
// symbols (nm libmbedtls.a: _binary_x509_crt_bundle_{start,end}). The asm() label
// pins the exact objcopy-generated symbol name regardless of C++ linkage - the
// idiom the arduino-esp32 CA-bundle examples use. One shared decl across every TU
// that includes this header; the linker resolves to the single embedded blob.
extern const uint8_t _nimbusCrtBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t _nimbusCrtBundleEnd[]   asm("_binary_x509_crt_bundle_end");

inline void tlsSetup(WiFiClientSecure& c) {
  if (agent::store::tlsVerify()) {
    c.setCACertBundle(_nimbusCrtBundleStart,
                      (size_t)(_nimbusCrtBundleEnd - _nimbusCrtBundleStart));
  } else {
    c.setInsecure();
  }
}

// Ported verbatim from Nuage-Solide src/net_util.h (Head Orchestrator v2).
//
// Abortive TLS close: set SO_LINGER with l_linger=0 so close() sends a RST instead
// of a FIN - which leaves NO TIME_WAIT TCP control block behind.
//
// Why this survives the S3/PSRAM relaxation (plan §3.7): the problem it solves is
// an lwIP PCB-POOL issue, not a heap issue. We open/close TLS FREQUENTLY - every
// orchestrator turn, every sub-session poll (round-robin), every Telegram
// long-poll. We active-close (stop() right after reading the HTTP/1.0 response
// body, before the server's FIN), so each normal close parks a ~1.5-2 KB
// TIME_WAIT PCB for ~1-2 minutes. Under load these accumulate faster than they
// drain and starve the fixed lwIP PCB pool (independent of PSRAM) until even DNS
// fails. RST-close skips TIME_WAIT entirely. Safe here: the response is fully read
// before we close, so the RST discards nothing we need.
//
// Depends on lwip/sockets.h + WiFiClientSecure::fd() - both in the arduino-esp32
// core (no new lib_deps).
inline void tlsClose(WiFiClientSecure& c) {
  nimbus::pollwatch::beat("close");
  int fd = c.fd();
  if (fd >= 0) {
    struct linger so;
    so.l_onoff  = 1;
    so.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so, sizeof(so));
  }
  c.stop();
}

// --- lwIP TCPIP core lock (bench A03, fp=7f7617df, candidate 28813029) ---------
// NetworkManager::hostByName() calls the RAW lwIP dns_clear_cache() when an interface
// IP state just flipped (NetworkManager.cpp:84 - exactly the Wi-Fi drop/rejoin case)
// WITHOUT holding the TCPIP core lock. dns_clear_cache() synchronously fires every
// pending DNS callback (dns.c:414 -> dns_call_found), so with SNTP's lookup pending
// the chain sntp_dns_found -> sntp_try_next_server -> sntp_request -> dns_gethostbyname
// -> dns_alloc_random_port -> udp_new_ip_type hits LWIP_ASSERT_CORE_LOCKED() (udp.c:1278)
// and panics the calling task (decoded frame by frame in
// evidence/crash2/a03-decode-28813029.txt: task=relay via WiFiClientSecure::connect ->
// hostByName -> dns_clear_cache).
//
// The class fix removes hostByName from every task path instead of locking around it:
// Network.hostByName resolves through the netconn API, which TAKES the core lock
// itself (lwip_getaddrinfo -> netconn_gethostbyname_addrtype -> tcpip_send_msg_wait_sem
// -> LOCK_TCPIP_CORE, tcpip.c:446), and the core lock is a NON-RECURSIVE mutex taken
// with portMAX_DELAY (port/freertos/sys_arch.c:44,63) - holding it across
// Network.hostByName self-deadlocks the calling task on a mutex it already holds.
// So the seam resolves with the RAW dns_gethostbyname() under LOCK_TCPIP_CORE() (a raw
// API: it asserts the lock held but never takes it - the webDnsReady pattern in
// embeddings.cpp), then connects by IPAddress, whose connect() runs no DNS at all
// (NetworkClient.cpp:312 / NetworkClientSecure.cpp:140,192 are the host overloads).
// dns_clear_cache()'s only caller in the whole stack is NetworkManager.cpp:84, so with
// no hostByName left in any task it is never called from outside the TCPIP thread.
// tools/release_gate/check_lwip_core_lock.py fails the release on any raw lwIP core
// call outside a LOCK_TCPIP_CORE region, any hostByName (bare, or under a lock - that
// one self-deadlocks), any host-based connect that does not go through connectHost(),
// and an unclosed LOCK_TCPIP_CORE.
namespace nimbus_net_detail {
struct DnsWait {
  sys_sem_t sem;
  ip_addr_t addr;
  bool ok = false;
  nimbus::net::LateWait cell;
};
inline void dnsWaitFree(DnsWait* w) {
  sys_sem_free(&w->sem);
  delete w;
}
// Runs on the TCPIP thread, which holds the core lock: the cell's two transitions
// never race (late_wait.h).
inline void dnsWaitFound(const char*, const ip_addr_t* addr, void* arg) {
  auto* w = static_cast<DnsWait*>(arg);
  if (!w->cell.callbackArrives()) {   // the waiter gave up: the state is ours to free
    dnsWaitFree(w);
    return;
  }
  if (addr != nullptr) {
    w->addr = *addr;
    w->ok = true;
  } else {
    w->ok = false;
  }
  sys_sem_signal(&w->sem);
}
}  // namespace nimbus_net_detail

// Resolve a hostname with the RAW lwIP DNS API under the TCPIP core lock (see above).
// Cache hit: ERR_OK with the address immediately. Miss: the query is sent while the
// lock is held and ERR_INPROGRESS comes back; the answer arrives in the TCPIP thread,
// which needs the lock, so the wait happens WITHOUT holding it (the same shape the
// netconn layer uses, api_msg.c lwip_netconn_do_gethostbyname) and is bounded by
// waitMs. lwIP's own retries can outlast that wait (1 + 1 + 2 + 3 s per server, up to
// three servers), so the shared state lives on the heap and whichever side lets go
// last frees it (nimbus/net/late_wait.h): an answer after the wait gave up frees it
// in the callback instead of writing into this function's dead stack frame and
// signalling a freed semaphore (lane TGSTALL; the first version did exactly that).
inline bool resolveHostLocked(const char* host, IPAddress& out, uint32_t waitMs = 10000) {
  if (out.fromString(host)) return true;  // literal address: no DNS involved
  auto* w = new (std::nothrow) nimbus_net_detail::DnsWait();
  if (!w) return false;
  if (sys_sem_new(&w->sem, 0) != ERR_OK) {
    delete w;
    return false;
  }
  const bool own = sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
  if (!own) LOCK_TCPIP_CORE();
  ip_addr_t addr;
  err_t e = dns_gethostbyname(host, &addr, nimbus_net_detail::dnsWaitFound, w);
  bool callbackFrees = false;   // an answer still to come owns the state
  if (e == ERR_OK) {
    out.from_ip_addr_t(&addr);   // cached: no callback will come
  } else if (e == ERR_INPROGRESS) {
    // The lock holder (TCPIP thread) cannot block for the answer: it leaves at once.
    if (!own) {
      UNLOCK_TCPIP_CORE();
      sys_arch_sem_wait(&w->sem, waitMs);
      LOCK_TCPIP_CORE();
    }
    const bool answered = w->cell.waiterLeaves();   // under the lock: no race with the callback
    callbackFrees = !answered;                       // gave up first: never touch w again
    if (answered && w->ok) {
      out.from_ip_addr_t(&w->addr);
      e = ERR_OK;
    } else {
      e = ERR_VAL;
    }
  }
  if (!callbackFrees) nimbus_net_detail::dnsWaitFree(w);
  if (!own) UNLOCK_TCPIP_CORE();
  return e == ERR_OK;
}

// Host-based connect through the seam: resolve under the TCPIP core lock (see above),
// then connect by IPAddress so the core's own hostByName inside connect(host, ...)
// never runs. Use this for EVERY connect(host, ...); connecting by IPAddress stays
// direct (no DNS inside).
inline int connectHost(Client& c, const char* host, uint16_t port) {
  nimbus::pollwatch::beat("connect");
  IPAddress ip;
  if (!resolveHostLocked(host, ip)) return 0;
  return c.connect(ip, port);
}
inline int connectHost(WiFiClientSecure& c, const char* host, uint16_t port) {
  nimbus::pollwatch::beat("connect");
  IPAddress ip;
  if (!resolveHostLocked(host, ip)) return 0;
  return c.connect(ip, port);
}
inline int connectHost(WiFiClientSecure& c, const char* host, uint16_t port, int32_t timeoutMs) {
  nimbus::pollwatch::beat("connect");
  IPAddress ip;
  if (!resolveHostLocked(host, ip)) return 0;
  return c.connect(ip, port, timeoutMs);
}
