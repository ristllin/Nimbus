#pragma once
#include <cstddef>
#include <string>

// served_model - which concrete model each provider really answered with.
//
// Every provider names the model behind a response in its "model" field (captured per
// response into TokenUsage::servedModel). Anthropic, OpenAI and Z.ai name the model
// itself; Mistral echoes the alias it was asked for ("mistral-large-latest", measured
// 26 Oct 2026), so the device resolves such an alias through the provider's own model
// list (model_catalog.h concreteModelName: "mistral-large-2512"). TurnSpend::add files
// the result on the turn (SpendSlot::served: the turn spend line, /api/lastturn) and
// notes it here per (provider, model asked for), in RAM, so the next prompt can name the
// model a pick really runs (ComposeInputs::servedModels) and /model (lane BUD) can
// answer from the last answer (lastServedModel). Before this, Lumi only knew "mistral-large-latest" and the head
// guessed "Mistral Large 2" from its own training.
//
// Portable (no Arduino), host-tested (test_orch_turn_spend). Thread-safe.
namespace nimbus {
namespace orch {

// The device's alias resolver: the concrete id for `echo` (a response's model field) on
// `host` into `out` (`cap` bytes, always NUL-terminated). Unset (host tests, the hosted
// daemon): the echo is kept as returned.
using ServedModelResolver = void (*)(const char* host, const char* echo, char* out, size_t cap);
void installServedModelResolver(ServedModelResolver r);
// The installed resolver's answer for `echo`, or the echo itself; "" for an empty echo
// or one that is not a clean model id (cleanModelId: provider text is untrusted).
void resolveServedModel(const char* host, const char* echo, char* out, size_t cap);

// Remember that `host`, asked for `asked`, answered with `model` (all non-empty; an
// empty one notes nothing). The newest kServedNotes pairs are kept.
inline constexpr size_t kServedNotes = 8;
void noteServedModel(const char* host, const char* asked, const char* model);
// The model `host` last answered with when asked for `asked`; "" when none is known
// this boot.
std::string servedModelFor(const std::string& host, const std::string& asked);

struct ServedModelNote {
  std::string host;    // "" before the first answer this boot
  std::string asked;
  std::string model;
};
// The most recent answer of any provider.
ServedModelNote lastServedModel();

void clearServedModels();   // forget every note (tests)

}  // namespace orch
}  // namespace nimbus
