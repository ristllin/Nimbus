#include "nimbus/orch/wire_errors.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "nimbus/orch/billing_refusal.h"   // the generic billing words (read, not edited)
#include "nimbus/orch/compact.h"           // isContextOverflowError: each provider's size words
#include "nimbus/orch/provider_error.h"    // providerErrorText
#include "nimbus/orch/router_reasons_e2.h"

namespace nimbus {
namespace orch {
namespace wire {

namespace {

using caps::ErrClass;
using caps::MarkerKind;
using caps::MarkerRow;

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

std::string scalar(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<long long>()) return std::to_string(v.as<long long>());
  return std::string();
}

void fieldsOf(JsonVariantConst o, ErrorObject& e) {
  e.type = scalar(o["type"]);
  e.code = scalar(o["code"]);
  e.param = scalar(o["param"]);
  if (o["message"].is<const char*>()) e.message = o["message"].as<const char*>();
}

// ---- the refused optional fields ----------------------------------------------------------
struct FieldWord {
  const char* word;
  uint16_t bit;
};
constexpr FieldWord kFieldWords[] = {
    {"effort", caps::refused::kEffort},
    {"thinking", caps::refused::kThinking},
    {"response_format", caps::refused::kStructured},
    {"text.format", caps::refused::kStructured},
    {"output_config", caps::refused::kStructured},
    {"json_schema", caps::refused::kStructured},
    {"tool_choice", caps::refused::kChoiceNone},
    {"prompt_cache", caps::refused::kCacheBreakpoint},
    {"cache_control", caps::refused::kCacheBreakpoint},
    {"max_tokens", caps::refused::kCapField},
    {"max_completion_tokens", caps::refused::kCapField},
    {"max_output_tokens", caps::refused::kCapField},
    {"image", caps::refused::kImage},
    {"reasoning_content", caps::refused::kReplay},
    {"encrypted_content", caps::refused::kReplay},
};
// Words that say a field itself is refused (not that its value is too large).
constexpr const char* kRefusalWords[] = {"not supported", "unsupported", "does not support", "not enabled",
                                         "unknown parameter", "unrecognized", "not permitted", "extra inputs",
                                         "extra_forbidden"};

uint16_t fieldsNamed(const std::string& lowered) {
  uint16_t m = 0;
  for (const FieldWord& w : kFieldWords)
    if (has(lowered, w.word)) m |= w.bit;
  return m;
}

bool saysRefused(const ErrorObject& e, const std::string& lowered) {
  if (e.code == "unsupported_parameter" || e.code == "unsupported_value" || e.code == "unknown_parameter")
    return true;
  for (const char* w : kRefusalWords)
    if (has(lowered, w)) return true;
  return false;
}

// A 400 or 404 naming a provider file id (Anthropic file_..., OpenAI file-...).
bool namesFileId(const std::string& lowered) {
  return has(lowered, "file_") || has(lowered, "file-") || has(lowered, "file id") || has(lowered, "files/");
}

// ---- the marker rows ----------------------------------------------------------------------
struct Ctx {
  const caps::CapsTable& t;
  uint8_t set;
  int status;
  const ErrorObject& e;
  std::string msg;   // the message, lowercased
};

bool matches(const Ctx& c, const MarkerRow& m, bool anyStatus) {
  if (m.markerSet != c.set) return false;
  if (!anyStatus && m.status != 0 && m.status != c.status) return false;
  const char* v = caps::poolStr(c.t, m.text);
  if (!*v) return false;
  switch (m.kind) {
    case MarkerKind::Code: return c.e.code == v;
    case MarkerKind::Type: return c.e.type == v;
    case MarkerKind::Text: return has(c.msg, lower(v).c_str());
  }
  return false;
}

// The first marker of `set` that matches and whose class `want` accepts.
template <typename Want>
bool marked(const Ctx& c, bool anyStatus, Want want, ErrClass* out) {
  if (c.set == kNoMarkers) return false;
  for (size_t i = 0; i < c.t.markerCount; i++) {
    const MarkerRow& m = c.t.markers[i];
    if (want(m.cls) && matches(c, m, anyStatus)) {
      *out = m.cls;
      return true;
    }
  }
  return false;
}

bool put(Classified* out, Classified v) {
  *out = v;
  return true;
}

Classified made(ErrClass cls, bool byMarker) {
  Classified r;
  r.error = true;
  r.cls = cls;
  r.marked = byMarker;
  return r;
}

// A RefusedField marker names its field in the message; with none named it is a plain 400.
Classified refusedOrBad(const ErrorObject& e, const std::string& lowered, bool byMarker) {
  Classified r = made(ErrClass::RefusedField, byMarker);
  r.refused = fieldsNamed(lower(e.param) + " " + lowered);
  if (!r.refused) r.cls = ErrClass::BadRequest;
  return r;
}

Classified fromMarker(const Ctx& c, ErrClass cls) {
  return cls == ErrClass::RefusedField ? refusedOrBad(c.e, c.msg, true) : made(cls, true);
}

bool any(ErrClass) { return true; }
bool isBilling(ErrClass k) { return k == ErrClass::Billing; }
bool isKey(ErrClass k) { return k == ErrClass::KeyAuth || k == ErrClass::KeyForbidden; }
bool isContext(ErrClass k) { return k == ErrClass::Context; }
bool isRateOrServer(ErrClass k) { return k == ErrClass::RateLimited || k == ErrClass::Server; }

// Step 3: 401, 402, 403 and billing (billing first, and before 429).
bool keyOrBilling(const Ctx& c, const std::string& body, Classified* out) {
  ErrClass k;
  if (marked(c, false, isBilling, &k)) return put(out, made(k, true));
  if (isBillingRefusal(c.status, body)) return put(out, made(ErrClass::Billing, false));
  if (c.status < 401 || c.status > 403) return false;
  if (marked(c, false, isKey, &k)) return put(out, made(k, true));
  return put(out, made(c.status == 402   ? ErrClass::Billing
                       : c.status == 401 ? ErrClass::KeyAuth
                                         : ErrClass::KeyForbidden,
                       false));
}

// Step 4: 413, and 400 or 422 with a context marker or the shared size words.
bool context(const Ctx& c, const std::string& body, Classified* out) {
  ErrClass k;
  if (c.status == 413) return put(out, made(ErrClass::Context, marked(c, false, isContext, &k)));
  if (c.status != 400 && c.status != 422) return false;
  if (marked(c, false, isContext, &k)) return put(out, made(k, true));
  if (!isContextOverflowError(body.substr(0, 4096))) return false;
  return put(out, made(ErrClass::Context, false));
}

// Step 5: 429, 408 and 5xx; a rate or server marker of that status decides between them.
bool rateOrServer(const Ctx& c, Classified* out) {
  if (c.status != 429 && c.status != 408 && c.status < 500) return false;
  ErrClass k;
  if (marked(c, false, isRateOrServer, &k)) return put(out, made(k, true));
  return put(out, made(c.status == 429   ? ErrClass::RateLimited
                       : c.status == 408 ? ErrClass::Transport
                                         : ErrClass::Server,
                       false));
}

// Step 6: the body's type or code, then a file id, then a named refused field.
Classified byBody(const Ctx& c) {
  if ((c.status == 400 || c.status == 404) && namesFileId(c.msg)) {
    Classified r = made(ErrClass::BadRequest, false);
    r.fileRef = true;
    return r;
  }
  ErrClass k;
  if (marked(c, false, any, &k)) return fromMarker(c, k);
  if ((c.status == 400 || c.status == 422) && saysRefused(c.e, c.msg)) return refusedOrBad(c.e, c.msg, false);
  return made(ErrClass::BadRequest, false);
}

// Step 7: a 2xx body that carries an error object (or a failed or cut-off answer).
Classified in2xx(const caps::CapsTable& t, uint8_t set, int status, const std::string& body) {
  if (!has(body, "\"error\"") && !has(body, "\"failed\"") && !has(body, "network_error")) return Classified{};
  const ErrorObject e = readErrorObject(body);
  if (e.status == "failed" || e.finish == "network_error") return made(ErrClass::Transport, false);
  if (!e.present) return Classified{};
  const Ctx c{t, set, status, e, lower(e.message)};
  ErrClass k;
  if (marked(c, true, any, &k)) return fromMarker(c, k);
  return made(ErrClass::BadRequest, false);
}

}  // namespace

ErrorObject readErrorObject(const std::string& body) {
  ErrorObject e;
  JsonDocument filter;
  filter["error"] = true;
  filter["object"] = true;
  filter["type"] = true;
  filter["code"] = true;
  filter["param"] = true;
  filter["message"] = true;
  filter["status"] = true;
  filter["choices"][0]["finish_reason"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) || !doc.is<JsonObjectConst>()) return e;
  JsonVariantConst err = doc["error"];
  if (err.is<JsonObjectConst>()) {
    e.present = true;
    fieldsOf(err, e);
  } else if (err.is<const char*>()) {
    e.present = true;
    e.code = e.message = err.as<const char*>();
  } else if (scalar(doc["object"]) == "error") {   // Mistral: the error is the top level
    e.present = true;
    fieldsOf(doc.as<JsonVariantConst>(), e);
  }
  e.status = scalar(doc["status"]);
  e.finish = scalar(doc["choices"][0]["finish_reason"]);
  if (e.message.empty()) e.message = providerErrorText(body);
  return e;
}

Classified classifyProvider(const caps::CapsTable& t, uint8_t markerSet, int status, const std::string& body) {
  if (status <= 0) return made(ErrClass::Transport, false);
  if (status >= 200 && status < 300) return in2xx(t, markerSet, status, body);
  const ErrorObject e = readErrorObject(body);
  const Ctx c{t, markerSet, status, e, lower(e.message)};
  Classified r;
  if (keyOrBilling(c, body, &r) || context(c, body, &r) || rateOrServer(c, &r)) return r;
  return byBody(c);
}

Classified classifyProvider(uint8_t markerSet, int status, const std::string& body) {
  return classifyProvider(caps::builtinCaps(), markerSet, status, body);
}

uint8_t markerSetFor(const std::string& providerSlug) {
  caps::Slot slot;
  if (!caps::slotFromHost(providerSlug.c_str(), &slot)) return kNoMarkers;
  const caps::CapsTable& t = caps::builtinCaps();
  for (size_t i = 0; i < t.providerCount; i++)
    if (t.providers[i].slot == slot) return t.providers[i].markerSet;
  return kNoMarkers;
}

uint16_t refusedFieldsOf(const ErrorObject& e) {
  const std::string lowered = lower(e.message);
  if (!saysRefused(e, lowered)) return 0;
  return fieldsNamed(lower(e.param) + " " + lowered);
}

bool resendWithout(caps::RefusedFields& mem, caps::Slot slot, const char* model, bool routed, uint16_t fields) {
  if (!fields) return false;
  const uint16_t had = mem.fieldsFor(slot, model, routed);
  mem.note(slot, model, routed, fields);
  return (fields & (uint16_t)~had) != 0;
}

RouterAnswer readRouterAnswerE2(const std::string& body) {
  RouterAnswer a = readRouterAnswer(body);
  if (a.row || a.reason.empty()) return a;
  for (const RouterReasonRow& r : kRouterReasonsE2)
    if (a.reason == r.reason) {
      a.row = &r;
      break;
    }
  return a;
}

}  // namespace wire
}  // namespace orch
}  // namespace nimbus
