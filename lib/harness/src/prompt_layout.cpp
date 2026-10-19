#include "nimbus/harness/prompt_layout.h"

#include <cstring>

namespace agent {
namespace layout {

namespace {

std::string stripMarker(const std::string& s) {
  if (s.find(kPartSplit) == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    if (c != kPartSplit) out += c;
  return out;
}

void addText(ArduinoJson::JsonArray arr, const std::string& text, bool cachePoint) {
  ArduinoJson::JsonObject b = arr.add<ArduinoJson::JsonObject>();
  b["type"] = "text";
  b["text"] = text;
  if (cachePoint) b["cache_control"]["type"] = "ephemeral";
}

int countIn(ArduinoJson::JsonVariantConst v) {
  int n = 0;
  if (v.is<ArduinoJson::JsonObjectConst>()) {
    ArduinoJson::JsonObjectConst o = v.as<ArduinoJson::JsonObjectConst>();
    if (!o["cache_control"].isNull()) n++;
    for (ArduinoJson::JsonPairConst kv : o)
      if (strcmp(kv.key().c_str(), "cache_control") != 0) n += countIn(kv.value());
  } else if (v.is<ArduinoJson::JsonArrayConst>()) {
    for (ArduinoJson::JsonVariantConst e : v.as<ArduinoJson::JsonArrayConst>()) n += countIn(e);
  }
  return n;
}

}  // namespace

std::string join(const PromptParts& p) {
  std::string out = stripMarker(p.stat);
  out += kPartSplit;
  out += stripMarker(p.summary);
  out += kPartSplit;
  out += stripMarker(p.dyn);
  return out;
}

PromptParts split(const std::string& instructions) {
  PromptParts p;
  const size_t a = instructions.find(kPartSplit);
  if (a == std::string::npos) {
    p.stat = instructions;
    return p;
  }
  p.split = true;
  p.stat = instructions.substr(0, a);
  const size_t b = instructions.find(kPartSplit, a + 1);
  if (b == std::string::npos) {
    p.summary = instructions.substr(a + 1);
    return p;
  }
  p.summary = instructions.substr(a + 1, b - a - 1);
  // Anything after a third marker cannot come from join(); keep it as text.
  p.dyn = stripMarker(instructions.substr(b + 1));
  return p;
}

std::string flat(const std::string& instructions) {
  const PromptParts p = split(instructions);
  if (!p.split) return instructions;
  return p.stat + p.summary + p.dyn;
}

std::string display(const std::string& instructions) {
  const PromptParts p = split(instructions);
  if (!p.split) return instructions;
  std::string out = p.stat;
  if (!p.summary.empty()) out += "\n[----- static block ends; conversation summary block -----]\n" + p.summary;
  out += "\n[----- cached blocks end; per-turn block -----]\n" + p.dyn;
  return out;
}

std::string userWithDynamic(const PromptParts& p, const std::string& inputs) {
  if (!p.split || p.dyn.empty()) return inputs;
  if (inputs.empty()) return p.dyn;
  return p.dyn + "\n\n" + inputs;
}

int putAnthropicSystem(ArduinoJson::JsonArray system, const PromptParts& p) {
  if (!p.split) {
    addText(system, p.stat, true);
    return 1;
  }
  int points = 0;
  const bool onlyDyn = p.stat.empty() && p.summary.empty();
  if (!p.stat.empty()) { addText(system, p.stat, true); points++; }
  if (!p.summary.empty()) { addText(system, p.summary, true); points++; }
  if (!p.dyn.empty()) {
    addText(system, p.dyn, onlyDyn);
    if (onlyDyn) points++;
  }
  return points;
}

void textAsBlocks(ArduinoJson::JsonArray messages) {
  for (ArduinoJson::JsonObject m : messages) {
    if (!m["content"].is<const char*>()) continue;
    const std::string text = m["content"].as<const char*>();
    if (text.empty()) continue;   // an empty array (or empty block) is refused by the API
    addText(m["content"].to<ArduinoJson::JsonArray>(), text, false);
  }
}

int cacheLastMessage(ArduinoJson::JsonArray messages) {
  if (messages.size() == 0) return 0;
  ArduinoJson::JsonVariant content = messages[messages.size() - 1]["content"];
  if (!content.is<ArduinoJson::JsonArray>()) return 0;
  ArduinoJson::JsonArray blocks = content.as<ArduinoJson::JsonArray>();
  if (blocks.size() == 0) return 0;
  blocks[blocks.size() - 1]["cache_control"]["type"] = "ephemeral";
  return 1;
}

int countAnthropicBreakpoints(ArduinoJson::JsonVariantConst request) { return countIn(request); }

void putOpenAIDeveloper(ArduinoJson::JsonArray input, const PromptParts& p) {
  auto dev = [&](const std::string& text) {
    if (text.empty()) return;
    ArduinoJson::JsonObject m = input.add<ArduinoJson::JsonObject>();
    m["type"] = "message";
    m["role"] = "developer";
    m["content"] = text;
  };
  dev(p.stat);
  if (p.split) dev(p.summary);
}

}  // namespace layout
}  // namespace agent
