#pragma once
#include <cstddef>
#include <cstdint>

// relay_slot - the cloud relay's use of the single TLS work slot (heap F2). No Arduino.
//
// The device runs one outbound TLS exchange at a time (src/sys/tls_arbiter, AGENTS.md
// "No on-device concurrency"), but the relay used to keep its own resident session
// outside that rule: it dialed, upgraded and streamed tunneled pages (a 375,598 B
// config page, about 500 KB on the wire) with no slot at all. On 14 Oct a 26 s page
// upload overlapped two head-turn body writes and both failed, and the turn was
// failed over to another provider for a device fault. The rule now:
//
//   - The dial (TLS connect, WebSocket upgrade, hello) takes the slot, like pairing.
//   - A response frame over kRelayBulkBytes takes the slot one 8 KB chunk at a time
//     and releases it between chunks, so a turn waiting for the slot gets it within
//     one chunk (a turn waits at most 10 s for the slot; a whole page can take far
//     longer than that on a busy link).
//   - Heartbeats, pongs, hello, acks and small frames (the dashboard's 5 s polls,
//     20 B to 6.4 KB) take no slot, so the page stays live during a turn.
//   - The slot is never held across the loopback replay: a tunneled handler can run
//     an embed call that takes the slot itself.
//   - Before a big frame starts, the wait is short takes (200 ms) with the inbound
//     socket pumped in between (pings answered, heartbeats sent), bounded so the
//     answer is a tunneled 503 with Retry-After about 20 s after the request arrived;
//     the cloud retries it, and the whole answer fits its 30 s request bound.
//   - The first dial after boot waits for Telegram's first poll (or 20 s), so the two
//     boot handshakes no longer overlap.
namespace nimbus {
namespace cloud {

inline constexpr size_t kRelayBulkBytes = 8192;         // a frame over this takes the slot
inline constexpr uint32_t kRelayDialSlotMs = 15000;     // the dial's acquire (pairing's value)
inline constexpr uint32_t kRelaySlotTakeMs = 200;       // one take between pumps
inline constexpr uint32_t kRelayBusyAfterMs = 20000;    // no slot this long after the request: 503
inline constexpr uint32_t kRelayResBudgetMs = 28000;    // the whole answer, inside the cloud's 30 s
inline constexpr uint32_t kRelayCloudTimeoutMs = 30000; // the cloud's request bound (lane X)
inline constexpr uint32_t kRelayFirstDialWaitMs = 20000;
inline constexpr uint32_t kTurnSlotAcquireMs = 10000;   // the turn transport's acquireWork bound

// The Retry-After hint (seconds) of a busy answer. A turn in flight usually lasts
// tens of seconds, a slot held by other work less; the cloud uses the hint as the
// floor of its next retry, capped at 30 s.
inline constexpr uint32_t kBusyRetryTurnS = 15;
inline constexpr uint32_t kBusyRetrySlotS = 5;
inline constexpr uint32_t kBusyRetryLowMemS = 10;
inline constexpr const char* kBusyBody = "Device busy, try again.";

// Does a response frame of this many bytes need the slot?
inline bool frameNeedsSlot(size_t frameBytes) { return frameBytes > kRelayBulkBytes; }

// May the first dial after boot go now? Once Telegram's first poll has connected,
// or kRelayFirstDialWaitMs after Wi-Fi came up, whichever is first.
inline bool firstDialReady(bool telegramPolledOk, uint32_t msSinceWifi) {
  return telegramPolledOk || msSinceWifi >= kRelayFirstDialWaitMs;
}

enum class SlotWait : uint8_t { Acquired, Busy, Ended };

// Wait for the slot before a big frame starts. `take(ms)` tries the slot for up to
// `ms`; `pump()` services the WebSocket between takes (false: the session ended).
// Tries at least once, then pumps at least every kRelaySlotTakeMs until `deadlineMs`.
template <class Now, class Take, class Pump>
SlotWait waitForSlot(uint32_t deadlineMs, Now nowMs, Take take, Pump pump) {
  for (;;) {
    const int32_t left = (int32_t)(deadlineMs - nowMs());
    const uint32_t ms = left <= 0 ? 0 : ((uint32_t)left < kRelaySlotTakeMs ? (uint32_t)left : kRelaySlotTakeMs);
    if (take(ms)) return SlotWait::Acquired;
    if (left <= 0) return SlotWait::Busy;
    if (!pump()) return SlotWait::Ended;
  }
}

enum class BulkResult : uint8_t { Sent, Stalled, Failed };

// Write a big frame `len` bytes long in kRelayBulkBytes chunks, each under the slot.
// The caller enters with the slot held for the first chunk. Between chunks the slot
// is released and `yieldTurn()` runs (a waiting turn takes it at once), then
// `reacquire(deadlineMs)` takes it back (false: not before the deadline). A frame
// cannot be interleaved with any other frame once started, so a stall past the
// deadline (Stalled) or a write failure (Failed) leaves the stream unusable: the
// caller must end the session.
//   writeChunk(off, n) -> bool : write the whole chunk (false on a dead socket)
template <class Reacquire, class Release, class WriteChunk, class Yield>
BulkResult writeBulk(size_t len, uint32_t deadlineMs, Reacquire reacquire, Release release,
                     WriteChunk writeChunk, Yield yieldTurn) {
  size_t off = 0;
  bool held = true;
  while (off < len) {
    if (!held && !reacquire(deadlineMs)) return BulkResult::Stalled;
    held = true;
    const size_t n = (len - off) < kRelayBulkBytes ? (len - off) : kRelayBulkBytes;
    const bool ok = writeChunk(off, n);
    release();
    held = false;
    if (!ok) return BulkResult::Failed;
    off += n;
    if (off < len) yieldTurn();
  }
  if (held) release();   // len == 0: the slot taken for nothing goes back
  return BulkResult::Sent;
}

}  // namespace cloud
}  // namespace nimbus
