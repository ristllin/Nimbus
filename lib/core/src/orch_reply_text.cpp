#include "nimbus/orch/reply_text.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {
bool isTextChunk(ArduinoJson::JsonVariantConst c) {
  const char* type = c["type"] | "";
  return std::strcmp(type, "text") == 0 && c["text"].is<const char*>();
}

std::string joinTextChunks(ArduinoJson::JsonVariantConst list, const char* sep) {
  std::string out;
  for (ArduinoJson::JsonVariantConst c : list.as<ArduinoJson::JsonArrayConst>()) {
    if (!isTextChunk(c)) continue;
    if (!out.empty()) out += sep;
    out += c["text"].as<const char*>();
  }
  return out;
}
}  // namespace

std::string chatContentText(ArduinoJson::JsonVariantConst content) {
  if (content.is<const char*>()) return content.as<const char*>();
  if (content.is<ArduinoJson::JsonArrayConst>()) return joinTextChunks(content, "");
  return std::string();
}

std::string messagesContentText(ArduinoJson::JsonVariantConst content) {
  if (content.is<const char*>()) return content.as<const char*>();
  if (content.is<ArduinoJson::JsonArrayConst>()) return joinTextChunks(content, "\n");
  return std::string();
}

}  // namespace orch
}  // namespace nimbus
