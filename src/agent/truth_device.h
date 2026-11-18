#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/effect_ring.h"   // fx::RingEntry (the header only: no link dependency)
#include "nimbus/orch/truth.h"

// truth_device - the truth check on the device (lane RB-G0, PRD section 8 rail 8): the
// binding the old engine's TurnCheck opens each turn (TurnEngine::Deps::truth). The probe
// goes out on the device's one TLS transport with the Anthropic key; the modes and counters
// live in /data/truth_state.json (LittleFS, written whole by temp and rename at each turn's
// end); the chat's recent effects are the effect ring (RB-FX), handed over with useRecent().
// Until a ring is handed over, or while a chat's ring lives in RAM only, every turn is
// receipt-only (the probe without the recent effects flags a third of true replies).
namespace agent {
namespace truthdev {

nimbus::orch::truth::Binding* binding();
// The chat's recent effects, oldest first, as the effect ring (RB-FX) holds them. Returns true
// only when that is the chat's whole record (the ring lives on storage). Null detaches.
using RecentFn = bool (*)(const std::string& chatId, std::vector<nimbus::orch::fx::RingEntry>& out);
void useRecent(RecentFn fn);

}  // namespace truthdev
}  // namespace agent
