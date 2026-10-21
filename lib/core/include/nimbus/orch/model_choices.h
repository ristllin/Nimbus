#pragma once
// Model choice lists - the ONE derivation behind "which models can this
// provider run" (CUM-486).
//
// The recurring bug this closes: the prompt's [AVAILABLE MODELS] line and the
// spawn validator read DIFFERENT sources. The prompt used compile-time lists for
// three providers and "" for the rest (so the head saw "zai: ;" and refused Z.ai
// / Cumulo sub-agents), while the validator used the live /models harvest - which
// drops deep-research ids - so the head kept offering o4-mini-deep-research and
// every such spawn was silently coerced to the default model. Now the prompt,
// the spawn validator and the orch_model validator all read
// effectiveModelChoices(): a model is advertised if and only if it validates.
//
// Portable (no Arduino): host-tested in test/test_model_choices over EVERY slot
// in kProviderSlots, so a new provider without a cold-start list fails there.

#include <string>

// Cold-start choice lists (comma-separated), one per first-class provider slot.
// The live /models harvest (NVS `mch_<slug>`) replaces them on the first verify;
// these are what a never-verified provider offers. They must agree with the
// harvest filter (provider_verify.cpp): no deep-research or other non-chat ids,
// since those cannot run the turn contract and a verified device never offers
// them. src/agent/agent_config.h keeps the historical *_MODEL_CHOICES names as
// aliases of these.
#define NIMBUS_MISTRAL_MODEL_CHOICES "mistral-large-latest,mistral-medium-latest,mistral-small-latest"
#define NIMBUS_OPENAI_MODEL_CHOICES  "gpt-6-astra,gpt-5.6,gpt-5.6-terra,gpt-5.6-luna"
#define NIMBUS_ANT_MODEL_CHOICES     "claude-opus-5,claude-sonnet-5,claude-haiku-4-5"
// Router providers: Cumulo speaks the OpenAI-compatible router (default upstream
// openai), so its defaults are the proven router models; Z.ai is GLM.
#define NIMBUS_CUMULO_MODEL_CHOICES  "gpt-5.6,gpt-5.6-terra,gpt-5.6-luna"
#define NIMBUS_ZAI_MODEL_CHOICES     "glm-5.3,glm-5.2,glm-5.3-flash"

namespace nimbus {
namespace orch {

// The cold-start list for a provider slug; "" for a slug with none (e.g.
// "custom", whose single model comes from its endpoint config).
const char* staticModelChoices(const char* slug);

// THE derivation: the harvested list when non-empty, else the cold-start list.
// A Cumulo list never offers a selector no upstream runs (routableSelector): a
// list an older build harvested and stored can still hold one (CUM-530), and the
// prompt's model line, the spawn and pick validators and the picker all read
// this, so each drops it at once instead of waiting for the next verify. A list
// with nothing left is the cold-start list.
std::string effectiveModelChoices(const char* slug, const std::string& harvestedCsv);

// Whether `model` is one of the comma-separated `csv` items (items trimmed; an
// empty model never matches).
bool modelInChoices(const std::string& csv, const std::string& model);

// The cheapest of the `csv` items for provider `slug` by the built-in price table
// (model_price_defaults.h: input plus output price per 1M tokens; the earlier item
// wins a tie), or "" when no item has a price row of its own. Fail-closed money: an
// unpriced model may cost more than any priced one, so it is never the answer. The
// spawn-model fallback reads this instead of the list's first item.
std::string cheapestChoice(const char* slug, const std::string& csv);

}  // namespace orch
}  // namespace nimbus
