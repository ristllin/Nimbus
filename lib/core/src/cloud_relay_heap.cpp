#include "nimbus/cloud/relay_heap.h"

namespace nimbus {
namespace cloud {

bool relayCanDial(size_t freeInternal, size_t largestInternalBlock) {
  return freeInternal >= kRelayHeapFloorFree && largestInternalBlock >= kRelayHeapFloorLargest;
}

ServeDecision tunnelServeDecision(size_t bodyHint, bool turnInFlight, size_t intFree) {
  if (bodyHint <= kServeSmallMax) return ServeDecision::Serve;
  if (turnInFlight || intFree < kServeFloorFree) return ServeDecision::Busy;
  return ServeDecision::Trickle;
}

ServeDecision lanServeDecision(size_t bodyHint, bool turnInFlight, size_t intFree) {
  const ServeDecision d = tunnelServeDecision(bodyHint, turnInFlight, intFree);
  // A turn alone never refuses a LAN client: its page trickles.
  if (d == ServeDecision::Busy && intFree >= kServeFloorFree) return ServeDecision::Trickle;
  return d;
}

const char* serveDecisionName(ServeDecision d) {
  switch (d) {
    case ServeDecision::Serve: return "serve";
    case ServeDecision::Trickle: return "trickle";
    case ServeDecision::Busy: return "busy";
  }
  return "?";
}

size_t trickleGrant(size_t maxLen, size_t fullOffer) {
  // The server offers min(its 2-segment buffer, the socket's free send space). A
  // short offer means the send buffer already holds more than half its size in
  // flight: wait for an acknowledgement. A full offer always gets one segment, so a
  // trickle can never stall with nothing in flight.
  if (maxLen < fullOffer) return 0;
  return maxLen < kTrickleChunk ? maxLen : kTrickleChunk;
}

}  // namespace cloud
}  // namespace nimbus
