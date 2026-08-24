#include "nimbus/cloud/loopback_target.h"

namespace nimbus {
namespace cloud {

bool loopbackFallbackUsable(uint32_t ipABCD) {
  if (ipABCD == 0) return false;             // 0.0.0.0 - the transient no-IP value
  if ((ipABCD >> 24) == 127) return false;   // 127.0.0.0/8 - already the primary target
  return true;
}

}  // namespace cloud
}  // namespace nimbus
