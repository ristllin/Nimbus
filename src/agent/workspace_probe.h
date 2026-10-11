#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

// workspace_probe - the PURE policy behind the CUM-537 Mistral workspace probe (the
// ~300 KB GET /v1/connectors that tells a Studio connector's card and the head's
// catalog whether the owner connected it in Mistral). No Arduino, no globals: the
// device seam in provider_verify.cpp owns the one queue instance and wires the I/O
// hooks (WiFiClientSecure, the TLS work slot, the turn flag) on the same
// self-deleting verify task and the same single TLS session as before; the host
// suite (test/test_provider_verify_retry) drives both halves with a fake exec.
namespace agent {
namespace provider_verify {

// ---- the one coalesced request ------------------------------------------------
// Written from three tasks: the web task (a connector save), the main loop (the
// pump) and the verify task (an attempt's outcome). A plain "queued" flag let a
// stale actor clear a request it never served: the pump cleared it AFTER creating
// a worker that had already re-armed it, and a fetch that began before a save
// cleared the save's request when it landed. Here every request bumps a
// generation; an attempt reads the generation BEFORE it fetches and can only ever
// settle that generation, so a request made after that point stays queued with
// its own tries. The residual race (a save landing between rearm()'s check and
// its clock write) can at worst delay the save's first attempt by one retry
// spacing; it never drops the save or spends its tries.
class WorkspaceProbeQueue {
 public:
  static const uint8_t  kMaxTries = 3;
  static const uint32_t kRetryMs  = 120000u;   // 2 min between attempts

  // A save or a restart asks for a fresh answer, due `delayMs` after `now`. The
  // new generation starts with no tries spent.
  void request(uint32_t now, uint32_t delayMs) {
    nextAt_ = now + delayMs;
    req_.fetch_add(1);   // published last: whoever sees the new generation sees its clock
  }
  // A request is outstanding (neither answered nor dropped).
  bool queued() const { return req_.load() != done_.load(); }
  // The pump may start an attempt now.
  bool due(uint32_t now) const {
    return queued() && (int32_t)(now - nextAt_.load()) >= 0;
  }
  // The generation an attempt serves: read before it fetches.
  uint32_t generation() const { return req_.load(); }
  // Every request up to `gen` is settled by a fresh answer (or there is nothing
  // to probe). A request made after `gen` was read stays queued.
  void served(uint32_t gen) { settle(gen); }
  enum class Rearm : uint8_t { Retry, Newer, Dropped };
  // The attempt for `gen` landed no answer: retry on the slow clock (a running
  // turn defers without spending a try) or, past kMaxTries, drop `gen`. A request
  // made since `gen` owns the clock and its tries, and is left as it is (Newer).
  Rearm rearm(uint32_t gen, uint32_t now, bool countsTry) {
    if (req_.load() != gen) return Rearm::Newer;
    const uint8_t spent = triesGen_.load() == gen ? tries_.load() : (uint8_t)0;
    const uint8_t used = countsTry ? (uint8_t)(spent + 1) : spent;
    if (used >= kMaxTries) {
      settle(gen);
      return Rearm::Dropped;
    }
    tries_ = used;
    triesGen_ = gen;
    nextAt_ = now + kRetryMs;
    return Rearm::Retry;
  }
  // Tries spent on the current request.
  uint8_t tries() const { return triesGen_.load() == req_.load() ? tries_.load() : (uint8_t)0; }
  uint32_t nextAt() const { return nextAt_.load(); }

 private:
  // done_ only moves forward (wrap-safe), whichever actor settles first.
  void settle(uint32_t gen) {
    uint32_t cur = done_.load();
    while ((int32_t)(gen - cur) > 0 && !done_.compare_exchange_weak(cur, gen)) {
    }
  }
  std::atomic<uint32_t> req_{0};
  std::atomic<uint32_t> done_{0};
  std::atomic<uint32_t> nextAt_{0};
  std::atomic<uint32_t> triesGen_{0};   // the generation `tries_` counts for
  std::atomic<uint8_t>  tries_{0};
};

// ---- one attempt, yielding to the owner ------------------------------------------
// The probe is AUTOMATIC (a save or a restart queued it), so it yields to a turn:
// it never takes the single TLS work slot while a turn runs, and once it holds the
// slot it checks again after the connect and before every body read, so a turn
// that starts mid-fetch gets the slot back within one read instead of waiting out
// the whole body (the turn's own slot wait is 10 s; the body read is bounded by
// its byte cap and deadline either way). An aborted fetch publishes nothing and
// keeps the last good answer.

// The gate around one attempt: the memory floor, the turn flag and the slot.
struct ProbeGate {
  std::function<bool()> lowMemory;     // internal heap below the verify floor
  std::function<bool()> turnInFlight;
  std::function<bool()> acquireSlot;   // the TLS work slot (bounded wait)
  std::function<void()> releaseSlot;
};

// The I/O of one fetch.
struct FetchIo {
  std::function<bool()> turnInFlight;
  std::function<bool()> connect;                 // connect and send the GET
  std::function<int(char*, size_t)> read;        // >0 bytes; 0 nothing yet; <0 closed, drained
  std::function<void()> close;
  std::function<uint32_t()> nowMs;
  std::function<void()> idle;                    // a short pause between empty reads
};

enum class FetchResult : uint8_t { Ok, Truncated, Deadline, TurnStarted, ConnectFailed };

// Fetch the response into `buf` (`cap` bytes, NUL-terminated; `len` = bytes read)
// until the peer closes (Ok), `cap` fills (Truncated) or `bodyMs` pass after the
// connect (Deadline). With `yieldToTurn`, a turn seen after the connect or before
// any read aborts it (TurnStarted). The connection is closed on every path that
// opened it.
inline FetchResult fetchBody(const FetchIo& io, char* buf, size_t cap, uint32_t bodyMs,
                             bool yieldToTurn, size_t& len) {
  len = 0;
  if (cap == 0) return FetchResult::Truncated;
  buf[0] = 0;
  if (!io.connect()) return FetchResult::ConnectFailed;
  const uint32_t deadline = io.nowMs() + bodyMs;   // the body gets its whole budget
  FetchResult res = FetchResult::Truncated;
  while (len < cap - 1) {
    if ((int32_t)(io.nowMs() - deadline) >= 0) {
      res = FetchResult::Deadline;
      break;
    }
    if (yieldToTurn && io.turnInFlight()) {
      res = FetchResult::TurnStarted;
      break;
    }
    const size_t room = cap - 1 - len;
    const int r = io.read(buf + len, room < 4096 ? room : 4096);
    if (r > 0) {
      len += (size_t)r;
      continue;
    }
    if (r < 0) {
      res = FetchResult::Ok;   // drained and closed
      break;
    }
    io.idle();
  }
  io.close();
  buf[len] = 0;
  return res;
}

// What one sync (fetch + parse + publish) came to.
enum class SyncOutcome : uint8_t { Landed, NoAnswer, TurnStarted };

inline constexpr char kWhyTurn[] = "a turn is running";

// One attempt: nullptr when an answer landed, else why it did not run. The slot
// is released exactly once on every path that took it.
inline const char* attemptProbe(const ProbeGate& gate, const std::function<SyncOutcome()>& sync) {
  if (gate.lowMemory()) return "low memory";
  if (gate.turnInFlight()) return kWhyTurn;
  if (!gate.acquireSlot()) return "tls busy";
  if (gate.turnInFlight()) {
    gate.releaseSlot();
    return kWhyTurn;
  }
  const SyncOutcome o = sync();
  gate.releaseSlot();
  if (o == SyncOutcome::Landed) return nullptr;
  return o == SyncOutcome::TurnStarted ? kWhyTurn : "no answer";
}

// A running turn defers an attempt without spending one of its tries.
inline bool countsAsTry(const char* why) { return why != kWhyTurn; }

}  // namespace provider_verify
}  // namespace agent
