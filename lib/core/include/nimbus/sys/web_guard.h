#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "nimbus/cloud/relay_timing.h"   // kTaskWdtTimeoutMs: the one watchdog constant

// web_guard - the rules that keep the web server inside its task watchdog.
//
// Every web handler runs on the AsyncTCP service task: the LAN page, the cloud relay's
// loopback replay of a tunneled request, and the LAN /mcp endpoint. The AsyncTCP
// library subscribes that task to the same 8 s task watchdog as the main loop
// (CONFIG_ASYNC_TCP_USE_WDT, stated in platformio.ini), so a handler that waits 8 s
// resets the device. On the owner's device on 26 Oct 2026 a GET /api/state waited for
// the shared card lock (agent::memory::lock, a recursive mutex) while a turn's tool
// (image.look) held it across two 60 s photo requests, and the watchdog reset it.
//
// So a web request never waits unbounded:
//   - for the card lock: it tries for at most kWebLockWaitMs (a response filler, which
//     the server calls again, kWebFillerWaitMs) and, when the lock is busy, answers
//     degraded (a field marked busy) or 503 with Retry-After;
//   - for the network: the TLS work slot is waited for at most kWebSlotWaitMs, and a
//     whole exchange started from a web request gets kWebNetBudgetMs;
//   - and it never holds the card lock across a network request (no task does: a
//     tool that waits on the network runs outside the dispatcher's card lock and takes
//     the lock only around its own card reads and writes).
// The device counts a blocking card-lock take on the web task and a network request
// started while the card lock is held (both bugs); test_web_guard pins the rules and
// rejects, at source level, a web path that takes the card lock without a timeout.
namespace nimbus::sys {

// The longest a web handler tries for the card lock before it answers busy.
inline constexpr uint32_t kWebLockWaitMs = 150;
// A chunked response filler's try: the server calls a filler that answered "try
// again" on its next poll, so it never needs to wait long.
inline constexpr uint32_t kWebFillerWaitMs = 20;
// Card-lock tries one request may chain (an accessor, then its filler's first chunk).
inline constexpr uint32_t kWebLockTriesPerRequest = 2;
// The longest a web request waits for the TLS work slot (a turn usually holds it).
inline constexpr uint32_t kWebSlotWaitMs = 250;
// One network exchange started from a web request, whole: connect, handshake, request
// and reply. Its connect and handshake shares bound the two phases that have their own
// timers in the TLS client, so the sum can never pass the budget.
inline constexpr uint32_t kWebNetBudgetMs = 5000;
inline constexpr uint32_t kWebNetConnectMs = 2000;
inline constexpr uint32_t kWebNetHandshakeS = 3;
// Retry-After on a busy answer: a card operation other than a turn's lasts well under it.
inline constexpr uint32_t kCardBusyRetryS = 5;

static_assert(kWebNetConnectMs + kWebNetHandshakeS * 1000 <= kWebNetBudgetMs,
              "the connect and handshake shares must fit the web network budget");
// The worst web request: every card-lock try times out, the slot wait times out or the
// exchange runs its whole budget, with a quarter of the watchdog left for the handler.
static_assert(kWebLockWaitMs * kWebLockTriesPerRequest + kWebSlotWaitMs + kWebNetBudgetMs <=
                  cloud::kTaskWdtTimeoutMs * 3 / 4,
              "a web request's bounded waits must leave a quarter of the watchdog");
static_assert(kWebFillerWaitMs < kWebLockWaitMs, "a filler is called again: it waits less");

// How long a TLS user waits for the work slot: on the web task at most kWebSlotWaitMs,
// elsewhere what it asked for.
uint32_t slotWaitMs(bool webTask, uint32_t requestedMs);

// The deadline of one network exchange: on the web task at most kWebNetBudgetMs.
uint32_t netBudgetMs(bool webTask, uint32_t requestedMs);

// Tools whose call waits on the network (a provider request, an image download, a
// Telegram upload, a speaker playback, a connected tool server's "mcp.*" tools). The turn's tool dispatcher
// (agent::memory::handleMcp) runs them outside its whole-call card lock, and the web
// task (/mcp) refuses them: their wait is longer than the watchdog allows there.
bool toolWaitsOnNetwork(const std::string& name);

// The JSON-RPC error the LAN /mcp endpoint answers for such a tool. `idJson` is the
// request's id as JSON ("null" when it had none); the name is escaped.
std::string turnOnlyToolRpcError(const std::string& name, const std::string& idJson);

// The JSON body of a busy answer (503 + Retry-After kCardBusyRetryS).
const char* cardBusyJson();

// An upload session whose abort found the card lock busy (the abort runs on the web
// task, which never waits for it): owed, and dropped by the next holder of the lock.
// Session generations start at 1 and only grow, so the newest owed one wins (a stale
// disconnect of a finished upload never overwrites the live session's abort) and 0
// means none.
class OwedAbort {
 public:
  void owe(uint32_t gen) {
    uint32_t cur = v_.load();
    while (gen > cur && !v_.compare_exchange_weak(cur, gen)) {
    }
  }
  uint32_t take() { return v_.exchange(0); }

 private:
  std::atomic<uint32_t> v_{0};
};

}  // namespace nimbus::sys
