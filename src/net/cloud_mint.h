#pragma once
#include <Arduino.h>

#include "nimbus/cloud/device_key.h"  // MintTrigger: the caller names its trigger

// cloud_mint - the device seam for minting a Cumulo Nimbus SPEND key for THIS
// device (CUM-397). It performs POST /router/device-key with the stored device
// credential and the user's capacity, then persists the returned cumulo_sk_ key.
//
// It NEVER mints on its own: request() is called ONLY from the token-gated
// POST /api/cloud/mintkey handler, which fires ONLY from the user's Save click on
// the Cloud access card. There is no boot-time, pairing-success, or key-empty
// caller (owner ruling 2026-09-14). The policy gate lives in the portable
// nimbus::cloud::mintAllowed(); the request-body build and response-message
// mapping live in nimbus/cloud/device_key.h (host-tested).
//
// Threading follows provider_verify: request() spawns ONE watchdog-free task that
// takes the single TLS work slot (arbiter) for the blocking mbedTLS call, so the
// AsyncTCP service task never runs TLS and the F12 loop watchdog is never at risk.
// The task self-deletes when done.

namespace agent {
namespace cloud_mint {

// A mint's lifecycle, polled by the web UI (GET /api/cloud/mintkey).
enum class State : uint8_t { Idle, Pending, Done, Error };

// Start a mint of `capacity` credits. The caller must name its trigger; ONLY
// MintTrigger::UserSave is ever allowed (nimbus::cloud::mintAllowed), so a new
// caller cannot mint without declaring itself and failing the property test.
// Returns false (and does nothing) when the trigger is not UserSave, the capacity
// is invalid, the device is not cloud-paired, a mint is already pending, or the
// worker task could not be created. On true, the caller reports "pending" and
// polls status().
bool request(nimbus::cloud::MintTrigger trigger, long capacity);

// True while a mint is queued or running.
bool pending();

// Snapshot of the last/current mint for the web readback. Copied under a brief
// critical section so the AsyncTCP reader never sees a torn multi-field state.
struct Status {
  State   state = State::Idle;
  bool    ok = false;
  long    capacity = 0;   // echoed capacity on success
  long    max = 0;        // account maximum when the capacity was too high
  String  label;          // key label on success
  String  message;        // honest, user-facing (copy rules apply)
};
Status status();

}  // namespace cloud_mint
}  // namespace agent
