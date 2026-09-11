#pragma once
#include <cstddef>

// relay_heap - the PURE, host-tested heap-floor policy that decides whether the
// relay may bring up its cloud link (CUM-167). No Arduino.
//
// The relay dials only when the head has enough INTERNAL SRAM to run a session.
// Its big buffers (TLS arena, tunneled response body, res frame) are PSRAM-backed
// and its task stack is pre-allocated at begin(), so the only real INTERNAL demand
// is the WS-upgrade handshake head (kMaxHandshakeHead = 4096) plus small transients.
// The floor therefore guards a SMALL contiguous block, not OTA's 16 KB.
//
// Why the largest-block floor moved from 8000 to 5000: solide-drivers v0.6.1 added a
// persistent 5 KB internal DMA bounce buffer (the CUM-167 white-screen fix - full
// frames from PSRAM are staged band-by-band through internal SRAM). That buffer is
// essential and cannot move to PSRAM (it exists BECAUSE a PSRAM DMA burst resets the
// panel); sitting mid-heap it splits the largest free internal block below the old
// 8000 headroom (field: ~26 KB free but the largest block dipped to ~5 KB), so the
// relay refused to dial - state=disabled, "Not enough memory right now" - even though
// it had ample room for its actual 4 KB handshake need. 5000 clears the real demand
// (4096 handshake head) with margin while coexisting with the bounce buffer.
//
// Why the total-free floor moved from 16000 to 8000 (CUM-387): the 16000 figure was
// sized for the pre-PSRAM era, when a cloud sync accumulated the whole tunneled
// response body (a full UI page, ~278 KB) plus the inbound WS frame and the outbound
// res frame on the scarce internal heap. All of that staging now rides PSRAM - the
// response body + parser staging, the inbound frame buffer + reassembly + decoded
// message payload, the request/upload-slice decode, and the res frame - each guarded
// by a compile-time static_assert (http_replay.h, relay_ws.h) that fails the firmware
// build if it regrows onto internal SRAM. mbedTLS's own RX/TX content buffers and the
// general String/JSON/HTTP churn around the call also ride PSRAM (main.cpp reroutes
// the mbedTLS allocator and lowers the internal spill threshold). So the relay's real
// INTERNAL demand at dial is no longer a large total - it is one modest contiguous
// block (the handshake head + lwIP/socket transients), which the largest-block floor
// already guards. Holding a 16 KB TOTAL reserve on top of that blocked a device whose
// internal SRAM was healthy but fragmented: the field low-water was ~9 KB while PSRAM
// sat ~7.7 MB empty, so the gate fired "Not enough memory right now" on a normal sync
// even though the largest block was ample. 8000 is the honest genuine-starvation
// backstop (below it, the lwIP/socket internal allocations for a new TLS connection
// genuinely cannot run); it matches the largest-block-centric model provider_verify.cpp
// already uses on this exact board for the same mbedTLS handshake. The on-device
// before/after low-water during a real sync is captured separately (see PR_BODY).

namespace nimbus {
namespace cloud {

constexpr size_t kRelayHeapFloorFree = 8000;      // total internal free (CUM-387: was 16000)
constexpr size_t kRelayHeapFloorLargest = 5000;   // largest internal contiguous block

// May the relay dial, given the current internal free total and largest free block?
bool relayCanDial(size_t freeInternal, size_t largestInternalBlock);

}  // namespace cloud
}  // namespace nimbus
