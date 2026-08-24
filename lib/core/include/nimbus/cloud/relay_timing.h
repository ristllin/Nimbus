#pragma once
#include <cstdint>

// relay_timing - the PURE, host-tested timing contract that keeps the cloud relay
// from resetting the device (CUM-160). No Arduino, no TLS.
//
// The device serializes every outbound TLS through a SINGLE work slot
// (src/sys/tls_arbiter.cpp - a frozen invariant: one slot, no on-device
// concurrency). The task watchdog panics and RESETS the device if a watchdog-fed
// task is blocked past its timeout - and the relay's pairing / credential re-mint
// is the one path that BOTH takes the slot AND does a blocking TLS connect + read.
// If it holds the slot for a ~12 s connect + ~12 s read while a fed task waits on
// the slot, the 8 s watchdog fires: the field-reported "tunnel gateway-timeout ->
// device RESET -> lag". (An ordinary tunneled request replays over plain-HTTP
// loopback and never takes the slot, so only pairing / re-mint is at risk.)
//
// The fix is a budget: the relay must bound its TOTAL slot-hold (connect + read)
// below the watchdog, leaving a fed waiter time to acquire the slot and feed the
// dog. A pairing / re-mint that cannot finish inside the budget fails gracefully
// and retries on the next relay tick - never a reset.

namespace nimbus {
namespace cloud {

// The system task-watchdog timeout. SINGLE SOURCE for both the watchdog config
// (src/main.cpp esp_task_wdt_config) and the relay slot-hold budget below, so the
// two can never silently drift. Changing the on-device watchdog means changing
// this one constant.
constexpr uint32_t kTaskWdtTimeoutMs = 8000;

// Headroom left below the watchdog for a fed waiter to take the freed slot and
// reset the dog before it would fire.
constexpr uint32_t kRelaySlotHoldMarginMs = 2500;

// A TLS handshake needs room; never bound the hold below this even on a short
// watchdog (better to keep the invariant simple than to make pairing impossible).
constexpr uint32_t kRelaySlotHoldFloorMs = 2500;

// The maximum time the relay may hold the single TLS work slot for one pairing /
// re-mint (connect + read combined), given the watchdog timeout. Always strictly
// less than a sane watchdog, and never below the floor.
uint32_t relaySlotHoldBudgetMs(uint32_t wdtTimeoutMs);

}  // namespace cloud
}  // namespace nimbus
