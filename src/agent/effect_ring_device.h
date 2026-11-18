#pragma once
#include <string>

#include "nimbus/orch/effect_ring.h"

// effect_ring_device - the device binding of each chat's recent effects (lane RB-FX, PRD
// section 8 rail 8): the portable ring (nimbus/orch/effect_ring.h) over the SD card at
// /mem/fx/<key>.ring, outside the file index. Every card step takes the card lock on its own
// (one line per effect, a rewrite of at most 12 KB in one piece, reads in 4 KB slices); every
// ring call runs under the PSRAM-first scope. With no card the ring runs from RAM and says so.
namespace agent::fxdev {

// Record one effect of `chatId` (the engine's Hooks::onEffect): the tool and its result or
// receipt (cleaned and clipped to one line of 160 B by the ring). A receipt noted for this
// tool by notePendingReceipt replaces `receipt`.
void record(const std::string& chatId, const std::string& tool, const std::string& receipt);

// A tool whose own one-line receipt is better than its result's first bytes (RB-L1b's ledger
// tools, through their ReceiptSink) notes it here while the tool runs; the record() that
// follows the call on the same task takes it. One slot; a take empties it.
void notePendingReceipt(const std::string& tool, const std::string& line);

// The ring the truth check (RB-G0) and the old engine's recent-actions view (RB-Q3a) read.
nimbus::orch::fx::EffectRing& ring();

}  // namespace agent::fxdev
