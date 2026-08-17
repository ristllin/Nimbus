#include "hw/hal_status.h"

namespace nimbus::hw {

namespace {
HalHealth g_hal;   // set-once at boot from solide::begin()'s BeginResult
}  // namespace

void setHalHealth(const HalHealth& h) { g_hal = h; }
const HalHealth& halHealth() { return g_hal; }

}  // namespace nimbus::hw
