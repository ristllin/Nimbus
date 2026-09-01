#pragma once
#include <cstdint>

// loopback_target - the PURE, host-tested policy for choosing the relay's loopback
// connect target (CUM-173). No Arduino, no sockets.
//
// The device answers a tunneled request by replaying it into its OWN web server over
// a loopback socket. 127.0.0.1 is the reliable primary on the esp32s3 build
// (CONFIG_LWIP_NETIF_LOOPBACK=y - verified: it connects in ~2 ms and serves every
// route). The STA self-IP is only a FALLBACK, and only when it is a real address:
// WiFi.localIP() can transiently be 0.0.0.0 (right after a (re)join), and dialing
// 0.0.0.0 can only waste the connect timeout and fail. This helper decides whether
// the STA fallback is worth attempting, so the "never dial 0.0.0.0/invalid" rule is
// host-tested and cannot regress into another silent 502.

namespace nimbus {
namespace cloud {

// Max size of a tunneled RESPONSE body the device buffers + frames back (PSRAM on
// device). The biggest is the config page (GET /); it MUST fit with headroom or the
// parser overflows and every tunneled GET / becomes a 5xx (CUM-173). This is the
// single source both the device (relay_client.cpp kMaxRespBody) and the host
// regression guard (test_loopback_capacity, which asserts the assembled page +
// kLoopbackRespHeadroom fits here) read, so the NEXT page growth fails the battery
// instead of the field. It must stay under the relay's 512 KB res-frame protocol max
// even after base64 (the static_assert below is the counter-test for that bound).
// History: 256 KB -> 320 KB -> 336 KB, raised as the web UI gained features; the
// buffer is PSRAM-backed (not the scarce internal SRAM the 16 KB inbound cap guards).
// The CUM-279 honest-UI additions (hosted-flag branches + resolve links) crossed the
// old 320 KB line, so it moves to 336 KB (base64 ~458 KB, ~64 KB under the res frame).
constexpr unsigned kLoopbackMaxRespBody = 336u * 1024u;

// The relay frames each response body as base64 inside a res-frame bounded by this
// protocol max; the raw cap MUST leave room after the 4/3 base64 expansion or a full
// page would be truncated/rejected at the wire (CUM-173).
constexpr unsigned kRelayResFrameMax = 512u * 1024u;
static_assert(kLoopbackMaxRespBody + kLoopbackMaxRespBody / 3u < kRelayResFrameMax,
              "loopback body cap must stay under the relay res-frame max after base64");

// Headroom the guard reserves above the current assembled page, so growth is caught
// BEFORE it reaches the cap (not exactly at it).
constexpr unsigned kLoopbackRespHeadroom = 16u * 1024u;

// Is `ipABCD` a usable STA fallback target for the loopback? The address is packed
// a.b.c.d with `a` in the most-significant byte (a<<24 | b<<16 | c<<8 | d).
// Rejects 0.0.0.0 (the transient WiFi.localIP() value) and the 127.0.0.0/8 range
// (already covered by the 127.0.0.1 primary, so never a distinct fallback).
bool loopbackFallbackUsable(uint32_t ipABCD);

}  // namespace cloud
}  // namespace nimbus
