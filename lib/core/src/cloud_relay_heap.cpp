#include "nimbus/cloud/relay_heap.h"

namespace nimbus {
namespace cloud {

bool relayCanDial(size_t freeInternal, size_t largestInternalBlock) {
  return freeInternal >= kRelayHeapFloorFree && largestInternalBlock >= kRelayHeapFloorLargest;
}

}  // namespace cloud
}  // namespace nimbus
