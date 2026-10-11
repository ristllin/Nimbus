#include "nimbus/orch/model_choices.h"

#include <cstring>

namespace nimbus {
namespace orch {

const char* staticModelChoices(const char* slug) {
  if (!slug) return "";
  if (std::strcmp(slug, "openai") == 0)    return NIMBUS_OPENAI_MODEL_CHOICES;
  if (std::strcmp(slug, "anthropic") == 0) return NIMBUS_ANT_MODEL_CHOICES;
  if (std::strcmp(slug, "mistral") == 0)   return NIMBUS_MISTRAL_MODEL_CHOICES;
  if (std::strcmp(slug, "cumulo") == 0)    return NIMBUS_CUMULO_MODEL_CHOICES;
  if (std::strcmp(slug, "zai") == 0)       return NIMBUS_ZAI_MODEL_CHOICES;
  return "";
}

std::string effectiveModelChoices(const char* slug, const std::string& harvestedCsv) {
  if (!harvestedCsv.empty()) return harvestedCsv;
  return staticModelChoices(slug);
}

bool modelInChoices(const std::string& csv, const std::string& model) {
  if (model.empty()) return false;
  static const char* kWs = " \t\r\n";
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    const size_t b = csv.find_first_not_of(kWs, start);
    if (b != std::string::npos && b < comma) {
      const size_t e = csv.find_last_not_of(kWs, comma - 1);
      if (csv.compare(b, e - b + 1, model) == 0) return true;   // equal length + bytes
    }
    start = comma + 1;
  }
  return false;
}

}  // namespace orch
}  // namespace nimbus
