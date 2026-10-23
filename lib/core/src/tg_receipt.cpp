#include "nimbus/net/tg_receipt.h"

#include <ArduinoJson.h>


namespace nimbus {
namespace net {

namespace {

constexpr size_t kErrorMax = 120;

// Printable ASCII only (the text reaches the log ring and the model), at most kErrorMax.
std::string printable(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (out.size() >= kErrorMax) break;
    const unsigned char u = (unsigned char)c;
    out += (u >= 0x20 && u < 0x7F) ? c : ' ';
  }
  return out;
}

}  // namespace

namespace {

// Why an answer is not a receipt: Telegram's own description first, else the transport.
std::string noReceiptReason(bool transportOk, const std::string& transportErr, bool parsed,
                            const std::string& desc) {
  if (!desc.empty()) return printable(desc);
  if (!transportOk) return printable(transportErr.empty() ? "no answer" : transportErr);
  return parsed ? "Telegram's answer had no message id" : "Telegram's answer could not be read";
}

}  // namespace

TgReceipt parseTgSendAnswer(bool transportOk, const std::string& transportErr,
                            const std::string& body) {
  TgReceipt r;
  JsonDocument filter;
  filter["ok"] = true;
  filter["error_code"] = true;
  filter["description"] = true;
  filter["result"]["message_id"] = true;
  JsonDocument d;
  const bool parsed = !body.empty() &&
                      deserializeJson(d, body, DeserializationOption::Filter(filter)) ==
                          DeserializationError::Ok &&
                      d.is<JsonObject>();
  JsonVariantConst mid = d["result"]["message_id"];
  const long long id = (parsed && mid.is<long long>()) ? mid.as<long long>() : 0;
  if (transportOk && parsed && (d["ok"] | false) && id > 0) {
    r.ok = true;
    r.messageId = (int64_t)id;
    return r;
  }
  if (parsed) r.errorCode = d["error_code"] | 0;
  r.error = noReceiptReason(transportOk, transportErr, parsed,
                            parsed ? std::string(d["description"] | "") : std::string());
  return r;
}

std::string tgSendLogLine(const char* method, const TgReceipt& r, size_t bytes) {
  std::string line = std::string("telegram: ") + (method ? method : "send");
  if (r.ok)
    return line + " ok (message_id " + std::to_string((long long)r.messageId) + ", " +
           std::to_string((unsigned long long)bytes) + " bytes)";
  return line + " FAIL (" + r.error + ")";
}

}  // namespace net
}  // namespace nimbus
