#include "modes/notifier_mode.h"

namespace nimbus {

bool NotifierMode::tick(uint32_t nowMs, uint32_t ambientMs, uint32_t attnMs) {
  // Link gone quiet: clear a stale ring so it doesn't lie about live jobs.
  // BLE-only now, so this is the sole place frames can go stale (feedBle()
  // applies frames as they arrive; this just watches for silence). ambientMs is
  // the posture-scaled ambient hold; attnMs is the (tunable) call-to-action hold.
  notifier::FrameResult t = mapper_.timeout(router_, nowMs, ambientMs, attnMs);
  if (t.ringDirty) {
    last_ = t;
    return true;
  }
  return false;
}

}  // namespace nimbus
