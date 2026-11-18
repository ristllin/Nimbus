#include "nimbus/orch/model_family.h"

#include <algorithm>
#include <cctype>

namespace nimbus {
namespace orch {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Lowercased, with a router selector's "mistral/" prefix dropped.
std::string bareMistralId(const std::string& model) {
  std::string m = model;
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  if (startsWith(m, "mistral/")) m = m.substr(8);
  return m;
}

// Large 4: the family prefix and the aliases Mistral lists for it (14 Nov 2026).
constexpr const char* kLarge4Prefix = "mistral-large-4";
constexpr const char* kLarge4Aliases[] = {"mistral-large-latest"};

}  // namespace

bool isMistralLarge4(const std::string& model) {
  const std::string m = bareMistralId(model);
  if (startsWith(m, kLarge4Prefix)) return true;
  for (const char* a : kLarge4Aliases)
    if (m == a) return true;
  return false;
}

bool mistralForcedReasoner(const std::string& model) {
  const std::string m = bareMistralId(model);
  return isMistralLarge4(m) || startsWith(m, "glm-") || startsWith(m, "zai-glm");
}

}  // namespace orch
}  // namespace nimbus
