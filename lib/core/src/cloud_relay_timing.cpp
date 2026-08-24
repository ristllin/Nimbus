#include "nimbus/cloud/relay_timing.h"

namespace nimbus {
namespace cloud {

uint32_t relaySlotHoldBudgetMs(uint32_t wdtTimeoutMs) {
  // Not enough watchdog to carve a margin out of: fall back to the floor, but never
  // exceed the watchdog itself (a degenerate/tiny watchdog still bounds the hold).
  if (wdtTimeoutMs <= kRelaySlotHoldMarginMs)
    return wdtTimeoutMs < kRelaySlotHoldFloorMs ? wdtTimeoutMs : kRelaySlotHoldFloorMs;
  uint32_t budget = wdtTimeoutMs - kRelaySlotHoldMarginMs;
  if (budget < kRelaySlotHoldFloorMs) budget = kRelaySlotHoldFloorMs;
  return budget;
}

}  // namespace cloud
}  // namespace nimbus
