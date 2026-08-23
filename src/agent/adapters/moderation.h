#pragma once
#include <string>

#include "nimbus/orch/moderation.h"

// Device moderation classifier - the network seam behind the portable decision
// core (lib/core/orch_moderation). Runs ONE cheap HTTPS classify call and maps the
// result to a nimbus::orch::ClassifierVerdict; the caller (orchestrator gates)
// applies the fail-open/fail-closed policy. Provider selection follows
// nimbus::orch::pickProvider: the Cumulo moderation endpoint on a Cumulo key,
// else Mistral /v1/moderations on the user's key. With no provider key it returns
// Error, so the gate's fail mode decides. Blocking; call from the poll/turn task
// (it takes the TLS arbiter slot, like the fetch scan).

namespace agent {
namespace moderation {

// Classify one piece of text for the given gate. Never throws; returns
// ClassifierVerdict::Error on any transport / parse / no-key condition.
// acquireMs bounds how long it waits for the single TLS work slot: the inbound
// gate runs pre-turn with the slot free (default 15 s), but the outbound gate can
// fire while a turn still holds the slot, so it passes a SHORT timeout - if the
// slot is busy it returns Error fast (fail-open for outbound = deliver unscreened)
// instead of stalling the reply for the whole turn.
nimbus::orch::ClassifierVerdict classify(const std::string& text, nimbus::orch::ModGate gate,
                                         uint32_t acquireMs = 15000);

}  // namespace moderation
}  // namespace agent
