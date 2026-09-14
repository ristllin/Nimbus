#pragma once
#include <cstdint>
#include <string>

// device_key - the PURE, host-tested policy for minting a Cumulo Nimbus SPEND key
// for THIS device (CUM-397). No Arduino, no TLS. The device glue in
// src/net/cloud_mint.cpp performs the actual POST to /router/device-key and the
// NVS write; this module owns the two decisions the firmware must get right and a
// host test can pin:
//
//   1. mintAllowed(): a spend key is minted ONLY on an explicit user Save with a
//      valid capacity. NO pairing-success, boot, or key-empty path may mint. The
//      web handler calls this before enqueuing; the enum makes "some new trigger
//      quietly mints" a compile-visible, test-caught change (AGENTS.md: test the
//      class, not the instance).
//   2. parseMintResponse(): map the router's HTTP status + body to an honest,
//      user-facing message (copy rules: US English, no em dash, no " - ").
//
// This is the SPEND key (cumulo_sk_...), not the device auth credential; the
// credential's own re-mint policy lives in relay_credential.h and is unrelated.

namespace nimbus {
namespace cloud {

// Every place the firmware could conceivably reach the mint path. Only UserSave is
// ever allowed. Adding a trigger here forces a decision in mintAllowed() and its
// property test, so no new caller can mint silently.
enum class MintTrigger : uint8_t {
  UserSave,   // the Save button on the Cloud access card - the ONLY allowed source
  Pairing,    // pairing succeeded - must NOT mint (owner ruling 2026-09-14)
  Boot,       // device boot - must NOT mint
  KeyEmpty,   // cumulo key is empty - must NOT mint
};
constexpr MintTrigger kAllMintTriggers[] = {
    MintTrigger::UserSave, MintTrigger::Pairing, MintTrigger::Boot,
    MintTrigger::KeyEmpty};

// True iff a user-entered capacity is a whole number of credits >= 1.
bool validMintCapacity(long capacity);

// The gate. True ONLY for MintTrigger::UserSave with a valid capacity. Any other
// trigger is denied regardless of capacity.
bool mintAllowed(MintTrigger trigger, long capacity);

// Build the JSON request body for POST /router/device-key. Returns "" when the
// capacity is invalid (the caller must reject before spending a TLS session).
std::string buildMintRequest(const std::string& deviceId,
                             const std::string& credential, long capacity);

enum class MintStatus : uint8_t {
  Ok,                  // 200: key minted
  CapacityRequired,    // 400 capacity_required (also our local invalid-capacity case)
  CapacityExceedsMax,  // 400 capacity_exceeds_account_max (max carried)
  TermsRequired,       // 403 terms_acceptance_required
  InvalidCredential,   // 401
  RateLimited,         // 429
  KeyLimitReached,     // 400 key_limit_reached (limit carried)
  Unpaired,            // 404 unpaired (no live pairing for this device)
  BadRequest,          // 400 missing_fields / invalid_json (a device-side request bug, not a user field)
  NetworkError,        // no HTTP response (transport failure)
  Unknown,             // any other status / unparseable body
};

struct MintResult {
  MintStatus  status  = MintStatus::Unknown;
  std::string key;      // the cumulo_sk_ spend key, on Ok only
  long        capacity = 0;  // echoed capacity, on Ok
  std::string label;    // key label, on Ok
  long        max      = 0;  // account maximum, on CapacityExceedsMax
  long        limit    = 0;  // live-key limit, on KeyLimitReached
  std::string message;  // honest, user-facing (copy rules apply)
  bool ok() const { return status == MintStatus::Ok; }
};

// Parse an HTTP response from POST /router/device-key. httpStatus <= 0 means no
// response reached us (transport failure). The message is always set.
MintResult parseMintResponse(int httpStatus, const std::string& body);

}  // namespace cloud
}  // namespace nimbus
