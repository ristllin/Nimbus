#pragma once
#include <cstdint>

// hal_status - a tiny solide-free holder for the per-subsystem HAL health that
// solide::begin() reports (display/leds/storage/memory/input). main.cpp captures
// the BeginResult once at boot and publishes it here so BOTH the web layer
// (/api/health) and the agent layer (the system.health tool + the live
// capability manifest) can read which peripherals actually came up - without
// either reaching into main.cpp or depending on <solide/solide.h>.
//
// Set-once at boot then read-only, so no synchronization is needed.
namespace nimbus::hw {

struct HalHealth {
  bool display = true;
  bool leds    = true;
  bool storage = true;
  bool memory  = true;
  bool touch   = true;   // color touch panel (the only input device)
};

void            setHalHealth(const HalHealth& h);
const HalHealth& halHealth();

}  // namespace nimbus::hw
