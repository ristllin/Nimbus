#include "nimbus/orch/provider_error.h"

#include <ArduinoJson.h>

#include "nimbus/mem_cap.h"   // utf8CapLen - cut on a character boundary

namespace nimbus {
namespace orch {

std::string oneLineText(std::string s, size_t cap) {
  for (char& c : s)
    if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
  if (s.size() > cap) s.resize((size_t)utf8CapLen(s.data(), (int)s.size(), (int)cap));
  return s;
}

namespace {
// A "detail" that is a string, or a list of {"msg": ...} entries joined with "; ".
std::string detailText(JsonVariantConst d) {
  if (d.is<const char*>()) return d.as<const char*>();
  std::string out;
  for (JsonVariantConst e : d.as<JsonArrayConst>()) {
    const char* m = e["msg"] | "";
    if (*m) out += (out.empty() ? "" : "; ") + std::string(m);
    if (out.size() > 200) break;
  }
  return out;
}
}  // namespace

std::string providerErrorText(const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc.is<JsonObjectConst>()) return std::string();
  JsonVariantConst err = doc["error"];
  if (err.is<const char*>()) return err.as<const char*>();
  if (err["message"].is<const char*>()) return err["message"].as<const char*>();
  JsonVariantConst msg = doc["message"];
  if (msg.is<const char*>()) return msg.as<const char*>();
  std::string t = detailText(msg["detail"]);
  if (t.empty()) t = detailText(doc["detail"]);
  return t;
}

std::string providerErrLine(const char* what, int status, const std::string& providerMsg,
                            const std::string& tag) {
  std::string e = std::string(what ? what : "provider") + " HTTP " + std::to_string(status);
  if (!tag.empty()) e += " " + tag;
  if (!providerMsg.empty()) e += ": " + oneLineText(providerMsg);
  return e;
}

}  // namespace orch
}  // namespace nimbus
