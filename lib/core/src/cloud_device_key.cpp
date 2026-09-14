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
static const char* kMsgUnpaired =
    "This device is not paired with the cloud. Pair it first.";
static const char* kMsgBadRequest =
    "The device sent an invalid request. Try again.";

// 200: a usable mint needs a non-empty key; echo capacity/label and say what landed.
static MintResult mintOk(JsonDocument& doc, bool parsed) {
  MintResult r;
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

// 400: the router refused BEFORE minting. Name the real reason; only the two
// capacity codes may talk about the capacity field.
static MintResult mint400(const std::string& err, JsonDocument& doc, bool parsed) {
  MintResult r;
  if (err == "capacity_exceeds_account_max") {
    r.status  = MintStatus::CapacityExceedsMax;
    r.max     = parsed ? (doc["max"] | 0L) : 0L;
    std::string m = "Capacity is above your account maximum of ";
    m += std::to_string(r.max);
    m += ".";
    r.message = m;
    return r;
  }
  if (err == "key_limit_reached") {
    r.status = MintStatus::KeyLimitReached;
    r.limit  = parsed ? (doc["limit"] | 0L) : 0L;
    std::string m = "Your account already has ";
    m += std::to_string(r.limit);
    m += " keys. Revoke one in the portal first.";
    r.message = m;
    return r;
  }
  if (err == "capacity_required") {
    r.status  = MintStatus::CapacityRequired;
    r.message = kMsgCapacity;
    return r;
  }
  // missing_fields / invalid_json / any other 400: a request the device built
  // wrongly, never the user's capacity, so do not blame the capacity field.
  r.status  = MintStatus::BadRequest;
  r.message = kMsgBadRequest;
  return r;
}

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

  if (httpStatus == 200) return mintOk(doc, parsed);
  if (httpStatus == 400) return mint400(std::string(err), doc, parsed);
  if (httpStatus == 404) {
    r.status  = MintStatus::Unpaired;
    r.message = kMsgUnpaired;
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
