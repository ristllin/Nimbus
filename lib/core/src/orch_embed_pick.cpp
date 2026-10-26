// embed_pick - see nimbus/orch/embed_pick.h. Pure, host-tested.
#include "nimbus/orch/embed_pick.h"

namespace nimbus {
namespace orch {

namespace {

// The other OpenAI-shaped route for the same model and width, or "" when there is none.
const char* openAiSibling(const std::string& provider) {
  if (provider == "openai") return "cumulo";
  if (provider == "cumulo") return "openai";
  return "";
}

EmbedPick switchTo(const EmbedConfig& c) {
  EmbedPick p;
  p.act = EmbedPickAct::Switch;
  p.pick = c;
  return p;
}

}  // namespace

bool embedKeyUsable(const EmbedKeys& keys, const std::string& provider) {
  if (provider == "openai") return keys.openai;
  if (provider == "cumulo") return keys.cumulo;
  if (provider == "mistral") return keys.mistral;
  return false;
}

EmbedConfig embedDefaultsFor(const std::string& provider) {
  if (provider == "openai" || provider == "cumulo")
    return EmbedConfig{provider, kEmbedOpenAiModel, kEmbedOpenAiDims};
  if (provider == "mistral") return EmbedConfig{provider, kEmbedMistralModel, kEmbedMistralDims};
  return EmbedConfig{};
}

EmbedPick pickEmbedProvider(const EmbedConfig& cur, const EmbedKeys& keys, bool hasVectors,
                            int storedWidth) {
  if (hasVectors && storedWidth > 0 && cur.dims != storedWidth)
    return EmbedPick{EmbedPickAct::Conflict, EmbedConfig{}};
  if (embedKeyUsable(keys, cur.provider)) return EmbedPick{EmbedPickAct::Keep, EmbedConfig{}};
  // The same model and width through the other OpenAI route keeps every stored vector
  // comparable, so it is the first choice with or without vectors.
  const std::string sibling = openAiSibling(cur.provider);
  if (!sibling.empty() && embedKeyUsable(keys, sibling))
    return switchTo(EmbedConfig{sibling, cur.model, cur.dims});
  if (hasVectors) return EmbedPick{};   // nothing else can embed into the stored space
  static const char* const kOrder[] = {"openai", "cumulo", "mistral"};
  for (const char* p : kOrder)
    if (embedKeyUsable(keys, p)) return switchTo(embedDefaultsFor(p));
  return EmbedPick{};
}

}  // namespace orch
}  // namespace nimbus
