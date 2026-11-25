#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/router_refusals.h"

// wire_errors - the engine-2 error classifier of the provider router (lane E2-R5,
// P/analysis-2026-11-24/ROUTER-DESIGN.md section 9). Portable (NO Arduino), host-tested in
// test/test_wire_errors. Engine 1 does not read it (its billing_refusal.h is not edited).
//
// One order for every provider, after the transport state and the router's reason (both the
// exchange's): 401, 402 and 403 and the billing markers (billing before 429); then 413, and 400
// or 422 with the context markers; then 429, 408 and 5xx (a rate or server marker of that status
// decides between the two); then the body's type or code; then an error object inside a 2xx
// body. Markers come from the facts table (model_caps_data.h): a code or type marker matches
// the error object's field exactly, never a substring of the body; a text marker matches the
// message case-insensitively. A marker with a status matches only that status.
namespace nimbus {
namespace orch {
namespace wire {

// The error object of a provider's body: {"error":{...}} (OpenAI, Z.ai, Anthropic's inner
// object), Mistral's top-level {"object":"error",...}, or {"error":"<word>"}. A null "error"
// (an OpenAI Responses answer that succeeded) is no error object.
struct ErrorObject {
  bool present = false;
  std::string type;      // the type field ("" = none)
  std::string code;      // the code field, a number spelled as digits ("" = none)
  std::string param;     // OpenAI's param field ("" = none)
  std::string message;   // the provider's message (providerErrorText)
  std::string status;    // a 2xx answer's top-level status ("failed" on Responses)
  std::string finish;    // a 2xx chat answer's first finish_reason
};
ErrorObject readErrorObject(const std::string& body);

struct Classified {
  bool error = false;           // false: a 2xx answer with no error object (the codec reads it)
  caps::ErrClass cls = caps::ErrClass::BadRequest;
  bool marked = false;          // a marker row decided it
  // Lane E2-PARK1: a billing class that only the provider's message words decided (a text marker or the shared
  // billing words, at a status other than 402): the caller asks the account before it parks anything.
  bool byText = false;
  bool fileRef = false;         // a 400 or 404 naming a file id (the exchange's refusedRef path)
  uint16_t refused = 0;         // caps::refused:: bits a RefusedField names
};

// Classifies one answer against the marker rows of `markerSet` in `t` (kNoMarkers: none, the
// generic rules alone). status <= 0 is Transport.
inline constexpr uint8_t kNoMarkers = 0xff;
Classified classifyProvider(const caps::CapsTable& t, uint8_t markerSet, int status, const std::string& body);
Classified classifyProvider(uint8_t markerSet, int status, const std::string& body);

// The marker set of a provider slug ("anthropic", "openai", "mistral", "zai", "custom"),
// kNoMarkers for any other ("cumulo": pass the upstream).
uint8_t markerSetFor(const std::string& providerSlug);

// The optional fields a 400 or 422 refuses (caps::refused:: bits): the error's param, or a
// message that both names a field and says it is not supported. 0 = none named.
uint16_t refusedFieldsOf(const ErrorObject& e);

// The generic "resend once without a refused optional field": true when `fields` adds a bit
// that `mem` did not hold for (slot, model, routed); the bits are noted either way. A second
// refusal of the same field answers false, so a request is never resent twice for one field.
bool resendWithout(caps::RefusedFields& mem, caps::Slot slot, const char* model, bool routed, uint16_t fields);

// readRouterAnswer, with a reason engine 1's table lacks read from the engine-2 reasons
// (router_reasons_e2.h, the whole cloud contract).
RouterAnswer readRouterAnswerE2(const std::string& body);

}  // namespace wire
}  // namespace orch
}  // namespace nimbus
