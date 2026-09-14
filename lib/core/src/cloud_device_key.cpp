#include "nimbus/cloud/device_key.h"

#include <ArduinoJson.h>

namespace nimbus {
namespace cloud {

bool validMintCapacity(long capacity) { return capacity >= 1; }

bool mintAllowed(MintTrigger trigger, long capacity) {
  // The single allowed source. Every other trigger is denied unconditionally, so a
  // pairing-success / boot / key-empty caller can never mint (owner ruling: nothing
  // mints under the hood; only the user's Save does).
  return trigger == MintTrigger::UserSave && validMintCapacity(capacity);
}

std::string buildMintRequest(const std::string& deviceId,
                             const std::string& credential, long capacity) {
  if (!validMintCapacity(capacity)) return {};
  JsonDocument doc;
  doc["deviceId"]   = deviceId;
  doc["credential"] = credential;
  doc["capacity"]   = capacity;
  std::string out;
  serializeJson(doc, out);
  return out;
}

// Honest, user-facing copy (US English, no em dash, no " - ").
static const char* kMsgCapacity =
    "Enter a capacity of at least 1 credit.";
static const char* kMsgTerms =
    "Open the Nimbus app and accept the terms.";
static const char* kMsgCredential =
    "This device is not recognized by the cloud. Pair it again.";
static const char* kMsgRate =
    "Too many requests. Wait a moment and try again.";
static const char* kMsgNetwork =
    "Couldn't reach the cloud. Try again.";
static const char* kMsgUnknown =
    "Couldn't mint a key. Try again.";

MintResult parseMintResponse(int httpStatus, const std::string& body) {
  MintResult r;
  if (httpStatus <= 0) {
    r.status  = MintStatus::NetworkError;
    r.message = kMsgNetwork;
    return r;
  }
  JsonDocument doc;
  const bool parsed = (deserializeJson(doc, body) == DeserializationError::Ok);
  const char* err = parsed ? (doc["error"] | "") : "";

  if (httpStatus == 200) {
    const char* key = parsed ? (doc["key"] | "") : "";
    if (!key[0]) {  // 200 with no key is not a usable mint
      r.status  = MintStatus::Unknown;
      r.message = kMsgUnknown;
      return r;
    }
    r.status   = MintStatus::Ok;
    r.key      = key;
    r.capacity = parsed ? (doc["capacity"] | 0L) : 0L;
    r.label    = parsed ? (const char*)(doc["label"] | "") : "";
    std::string m = "Key minted with a cap of ";
    m += std::to_string(r.capacity);
    m += " credits.";
    r.message = m;
    return r;
  }
  if (httpStatus == 400) {
    if (std::string(err) == "capacity_exceeds_account_max") {
      r.status  = MintStatus::CapacityExceedsMax;
      r.max     = parsed ? (doc["max"] | 0L) : 0L;
      std::string m = "Capacity is above your account maximum of ";
      m += std::to_string(r.max);
      m += ".";
      r.message = m;
      return r;
    }
    // capacity_required, or any other 400: the capacity is the actionable field.
    r.status  = MintStatus::CapacityRequired;
    r.message = kMsgCapacity;
    return r;
  }
  if (httpStatus == 401) {
    r.status  = MintStatus::InvalidCredential;
    r.message = kMsgCredential;
    return r;
  }
  if (httpStatus == 403) {
    r.status  = MintStatus::TermsRequired;
    r.message = kMsgTerms;
    return r;
  }
  if (httpStatus == 429) {
    r.status  = MintStatus::RateLimited;
    r.message = kMsgRate;
    return r;
  }
  r.status  = MintStatus::Unknown;
  r.message = kMsgUnknown;
  return r;
}

}  // namespace cloud
}  // namespace nimbus
