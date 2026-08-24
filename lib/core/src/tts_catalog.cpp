#include "nimbus/tts_catalog.h"

#include <ArduinoJson.h>

#include <cctype>

namespace core {

// "en_us" -> "US", "en_gb" -> "UK" (kept as the familiar label), "fr_fr" -> "FR",
// and any other "xx_yy" -> uppercased country part.
static std::string langShort(const std::string& l) {
  if (l.rfind("en_us", 0) == 0) return "US";
  if (l.rfind("en_gb", 0) == 0) return "UK";
  std::string s = l;
  auto us = l.find('_');
  if (us != std::string::npos) s = l.substr(us + 1);
  for (auto& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
  return s;
}

static std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

int mergeMistralVoicesPage(const char* pageJson, std::set<std::string>& seen,
                           std::string& outRows, int* added, int* total) {
  if (added) *added = 0;
  JsonDocument doc;
  if (deserializeJson(doc, pageJson)) return 0;
  if (total) {
    JsonVariantConst t = doc["total"];
    if (!t.isNull()) *total = t.as<int>();
  }
  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (items.isNull()) return 0;

  int processed = 0, newRows = 0;
  for (JsonObjectConst v : items) {
    processed++;
    const char* slug = v["slug"] | "";
    if (!slug[0]) continue;
    if (!seen.insert(slug).second) continue;  // duplicate slug -> skip (dedup)

    // Split Mistral's "Paul - Sad" into persona + emotion so the web cascade can
    // group by persona; fall back to the slug's last segment if there's no " - ".
    std::string mistralName = v["name"] | "";
    std::string persona = mistralName, emotion;
    auto dash = mistralName.find(" - ");
    if (dash != std::string::npos) {
      persona = mistralName.substr(0, dash);
      emotion = lower(mistralName.substr(dash + 3));
    } else {
      std::string s(slug);
      auto last = s.rfind('_'), first = s.find('_');
      if (last != std::string::npos && last != first) emotion = s.substr(last + 1);
    }

    std::string gender = v["gender"] | "";
    std::string lang;
    JsonArrayConst langs = v["languages"].as<JsonArrayConst>();
    if (!langs.isNull() && langs.size() > 0) {
      const char* l0 = langs[0] | "";
      lang = langShort(l0);
    }

    // "Paul (male, US) - sad" - matches the on-device fallback row shape.
    std::string label = persona;
    if (!gender.empty() || !lang.empty()) {
      label += " (";
      label += gender;
      if (!gender.empty() && !lang.empty()) label += ", ";
      label += lang;
      label += ")";
    }
    if (!emotion.empty()) {
      label += " - ";
      label += emotion;
    }

    JsonDocument row;
    row["value"] = slug;
    row["label"] = label;
    row["name"] = persona;
    row["gender"] = gender;
    row["lang"] = lang;
    if (!emotion.empty()) row["emotion"] = emotion;
    std::string rs;
    serializeJson(row, rs);
    if (!outRows.empty()) outRows += ",";
    outRows += rs;
    newRows++;
  }
  if (added) *added = newRows;
  return processed;
}

const char* speakerTtsFormat(const std::string& provider, bool* playAsMp3) {
  // OpenAI is the one provider whose /v1/audio/speech emits WAV the speaker plays
  // directly; every other provider (Mistral/Voxtral today) emits MP3, which the
  // vendored minimp3 decoder plays. Default unknown providers to the MP3 path.
  const bool wav = (provider == "openai");
  if (playAsMp3) *playAsMp3 = !wav;
  return wav ? "wav" : "mp3";
}

void downmixStereoToMono(const int16_t* interleaved, int frames, int16_t* out) {
  for (int i = 0; i < frames; i++)
    out[i] = static_cast<int16_t>(
        ((int)interleaved[2 * i] + (int)interleaved[2 * i + 1]) / 2);
}

}  // namespace core
