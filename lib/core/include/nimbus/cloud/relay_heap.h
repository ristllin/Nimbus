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
// (4096 handshake head) with margin while coexisting with the bounce buffer; the free
// total floor is unchanged, so genuine starvation still refuses.

namespace nimbus {
namespace cloud {

constexpr size_t kRelayHeapFloorFree = 16000;     // total internal free
constexpr size_t kRelayHeapFloorLargest = 5000;   // largest internal contiguous block

// May the relay dial, given the current internal free total and largest free block?
bool relayCanDial(size_t freeInternal, size_t largestInternalBlock);

}  // namespace cloud
}  // namespace nimbus
