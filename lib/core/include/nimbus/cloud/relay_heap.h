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
// even though the largest block was ample.
//
// What 8000 is, stated honestly: a CONSERVATIVE genuine-starvation backstop, NOT a
// measured cliff. Nothing has established the total-free level below which the
// lwIP/socket internal allocations for a new TLS connection actually fail, and
// docs/memory.md:66-68 puts the real internal red line for a network call much higher
// (the ~24 KB lwIP-pbuf + TLS-record "danger zone"), with :133-134 siting the other
// floors just above it (about 28 KB). Note also that the ~9 KB field low-water above
// was observed on v4.4.8, BEFORE this PSRAM staging landed, so the refusal that
// motivated the change may not reproduce on v4.5.0 at all. Read 8000 as "low enough
// to stop blocking a healthy but fragmented device, high enough to still refuse an
// obviously starved one" - not as a proven limit.
//
// It is also NOT the same gate provider_verify.cpp applies on this board: that one
// gates a single largest-block threshold (largest >= 8000), while this one gates
// largest >= 5000 AND free >= 8000. Same largest-block-centric spirit, different
// numbers; they are not interchangeable and neither validates the other.
//
// TODO(CUM-387): the on-device before/after internal low-water during a real cloud
// sync on v4.5.0 is still OUTSTANDING (the target unit has been off Wi-Fi since the
// release), so this floor is unvalidated on hardware. That measurement is what turns
// it from conservative to justified. While measuring, also check that a dial at
// 8-16 KB does not overlap a concurrent turn or Telegram TLS session: runSession
// takes no TLS arbiter, so the two can coexist. And note the user-visible
// consequence of moving the refusal off the memory path: a memory-caused dial
// failure now surfaces as "Couldn't reach the cloud. Retrying." rather than the
// memory copy, which hides the real cause from the owner.

namespace nimbus {
namespace cloud {

constexpr size_t kRelayHeapFloorFree = 8000;      // total internal free (CUM-387: was 16000)
constexpr size_t kRelayHeapFloorLargest = 5000;   // largest internal contiguous block

// May the relay dial, given the current internal free total and largest free block?
bool relayCanDial(size_t freeInternal, size_t largestInternalBlock);

}  // namespace cloud
}  // namespace nimbus
