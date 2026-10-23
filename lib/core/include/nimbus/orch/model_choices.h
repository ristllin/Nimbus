#pragma once
// Model choice lists - the ONE derivation behind "which models can this
// provider run" (CUM-486).
//
// The recurring bug this closes: the prompt's [AVAILABLE MODELS] line and the
// spawn validator read DIFFERENT sources. The prompt used compile-time lists for
// three providers and "" for the rest (so the head saw "zai: ;" and refused Z.ai
// / Cumulo sub-agents), while the validator used the live /models harvest - which
// drops deep-research ids - so the head kept offering o4-mini-deep-research and
// every such spawn was silently coerced to the default model. Now the prompt reads
// effectiveModelChoices() (the model store's short list, else the cold-start list)
// and every check - orch_model, spawn, the web pick - asks modelSelectable()
// (model_selectable.h) over the same store, of which the short list is a subset: a
// model is advertised only if it validates.
//
// Portable (no Arduino): host-tested in test/test_model_choices over EVERY slot
// in kProviderSlots, so a new provider without a cold-start list fails there.

#include <string>
#include <vector>

// Cold-start choice lists (comma-separated), one per first-class provider slot.
// The model store (/data/models_<slug>.txt, filled by every verify's fetch) replaces
// them on the first verify; these are what a never-verified provider offers. They
// must agree with the store's filter: no deep-research or other non-chat ids, since
// those cannot run the turn contract and a verified device never offers them. Each
// holds current active ids only (tools/model_id_lint.py checks them against the
// recorded provider lists, and model_price_defaults.h prices every one). The Cumulo
// list always holds the router default (agent_config.h CUMULO_MODEL, gpt-5.6).
// src/agent/agent_config.h keeps the historical *_MODEL_CHOICES names as aliases.
#define NIMBUS_MISTRAL_MODEL_CHOICES "mistral-large-latest,mistral-medium-latest,mistral-small-latest"
// Lane DEF: each list carries its provider's role defaults (model_defaults.h), so a
// never-verified provider offers and validates the sub default it runs on.
#define NIMBUS_OPENAI_MODEL_CHOICES \
  "gpt-6.1-sol,gpt-6-sol,gpt-6-astra,gpt-5.6,gpt-5.6-sol,gpt-5.6-terra,gpt-5.6-luna,gpt-6-luna"
#define NIMBUS_ANT_MODEL_CHOICES \
  "claude-opus-5-5,claude-sonnet-5-5,claude-haiku-5-5,claude-fable-5-1,claude-opus-5,claude-sonnet-5"
// Router providers: Cumulo speaks the OpenAI-compatible router (default upstream
// openai), so its defaults are the proven router models; Z.ai is GLM.
#define NIMBUS_CUMULO_MODEL_CHOICES  "gpt-5.6,gpt-5.6-terra,gpt-5.6-luna"
#define NIMBUS_ZAI_MODEL_CHOICES     "glm-5.3,glm-5.2,glm-5.3-flash"

namespace nimbus {
namespace orch {

// The cold-start list for a provider slug; "" for a slug with none (e.g.
// "custom", whose single model comes from its endpoint config).
const char* staticModelChoices(const char* slug);

// THE derivation: the store's short list when non-empty, else the cold-start list.
// A Cumulo list never offers a selector no upstream runs (routableSelector): a
// list an older build harvested and stored can still hold one (CUM-530), and the
// prompt's model line, the spawn and pick validators and the picker all read
// this, so each drops it at once instead of waiting for the next verify. A list
// with nothing left is the cold-start list.
std::string effectiveModelChoices(const char* slug, const std::string& harvestedCsv);

// The trimmed, non-empty items of a comma-separated list, in order: the one CSV reader
// the store, the checks and the pick rule share.
std::vector<std::string> choiceItems(const std::string& csv);

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
