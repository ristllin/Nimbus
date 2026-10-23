#include "nimbus/orch/model_choices.h"

#include <cstdint>
#include <cstring>

#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/router_route.h"

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

namespace {
// Each trimmed, non-empty item of a comma-separated list, in order, as a slice
// `fn(p, n)` of the list (nothing copied); stops (false) when fn returns false.
template <typename Fn>
bool eachItem(const std::string& csv, Fn fn) {
  static const char* kWs = " \t\r\n";
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    const size_t b = csv.find_first_not_of(kWs, start);
    if (b != std::string::npos && b < comma &&
        !fn(csv.data() + b, csv.find_last_not_of(kWs, comma - 1) - b + 1))
      return false;
    start = comma + 1;
  }
  return true;
}
}  // namespace

std::string effectiveModelChoices(const char* slug, const std::string& harvestedCsv) {
  if (harvestedCsv.empty()) return staticModelChoices(slug);
  if (!slug || std::strcmp(slug, "cumulo") != 0) return harvestedCsv;
  // The common case (a list this build harvested) is returned as it is.
  if (eachItem(harvestedCsv, [](const char* p, size_t n) { return routableSelector(p, n); }))
    return harvestedCsv;
  std::string kept;
  eachItem(harvestedCsv, [&kept](const char* p, size_t n) {
    if (!routableSelector(p, n)) return true;
    if (!kept.empty()) kept += ',';
    kept.append(p, n);
    return true;
  });
  return kept.empty() ? std::string(staticModelChoices(slug)) : kept;
}

std::vector<std::string> choiceItems(const std::string& csv) {
  std::vector<std::string> out;
  eachItem(csv, [&out](const char* p, size_t n) {
    out.emplace_back(p, n);
    return true;
  });
  return out;
}

bool modelInChoices(const std::string& csv, const std::string& model) {
  if (model.empty()) return false;
  return !eachItem(csv, [&model](const char* p, size_t n) {   // stops at the match
    return !(n == model.size() && std::memcmp(p, model.data(), n) == 0);
  });
}

std::string cheapestChoice(const char* slug, const std::string& csv) {
  std::string best;
  uint64_t bestCost = UINT64_MAX;
  eachItem(csv, [&](const char* p, size_t n) {
    const std::string m(p, n);
    if (const BuiltinPrice* row = builtinModelPrice(slug, m.c_str())) {
      const uint64_t cost = (uint64_t)row->microIn + row->microOut;
      if (cost < bestCost) {
        best = m;
        bestCost = cost;
      }
    }
    return true;
  });
  return best;
}

}  // namespace orch
}  // namespace nimbus
