#include "nimbus/cloud/relay_presence.h"

#include <cstring>

namespace nimbus {
namespace cloud {

AckResult evaluateHelloAck(const char* welcomeId, const char* ourId) {
  // Legacy relay: no id echoed -> accept (backward compatible). The Welcome
  // itself is still proof the relay registered us; the id echo only ADDS an
  // identity binding, it does not gate the legacy path.
  if (welcomeId == nullptr || welcomeId[0] == '\0') return AckResult::Accept;
  // The relay echoed an id: it must match ours, or we are talking to the wrong
  // endpoint (or being spoofed) - refuse to report online.
  if (ourId != nullptr && std::strcmp(welcomeId, ourId) == 0) return AckResult::Accept;
  return AckResult::RejectMismatch;
}

}  // namespace cloud
}  // namespace nimbus
