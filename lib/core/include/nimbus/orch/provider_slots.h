#pragma once
#include <cstddef>
#include <cstring>

// Canonical provider registry (CUM-213). ONE source of truth for the first-class
// AI provider slots the orchestrator UI renders, so the verify-to-unlock gating -
// and any future provider - is derived, never hand-listed per surface.
//
// The recurring bug this closes (CUM-242 class): the provider list was copied into
// half a dozen places (the /api/orch state emitter, the model-pick validator, the
// front-end label + key-field maps), each appending cumulo/zai by hand beside a
// three-element BYOK table. Adding a provider meant editing every copy, and a
// missed copy left that provider ungated ("only Cumulo shows verify-to-unlock").
// Every device-facing surface now loops this table instead, so a new row here
// appears - already gated - on all of them at once.
//
// Portable (no Arduino): host-unit-tested in test/test_provider_slots. It carries
// only the provider IDENTITY (machine slug, display label, the web UI key-field id,
// and whether it is the recommended flagship). The per-provider secret accessors
// and the static model-choice fallbacks stay in the firmware layer that owns them;
// this header never reaches up into src/.
//
// FROZEN: the `slug` values are machine keys stored on real devices and read by
// tools - never rename one (AGENTS.md frozen-strings rule). The `label` is the
// user-facing display text and MAY change per the copy guide.
namespace nimbus {
namespace orch {

struct ProviderSlot {
  const char* slug;      // machine key (FROZEN): "cumulo", "openai", ...
  const char* label;     // display label: "Cumulo Nimbus", "OpenAI", ...
  const char* keyField;  // web UI key-write field id: "cumuloKey", "oaiKey", ...
  bool        recommended;  // the flagship one-key/one-balance slot (Cumulo Nimbus)
};

// Canonical order: the recommended flagship first, then the direct BYOK providers,
// then the router-hosted heads. This is the order the Models UI renders the rows in.
inline constexpr ProviderSlot kProviderSlots[] = {
    {"cumulo",    "Cumulo Nimbus", "cumuloKey", true},
    {"openai",    "OpenAI",        "oaiKey",    false},
    {"anthropic", "Anthropic",     "antKey",    false},
    {"mistral",   "Mistral",       "mistKey",   false},
    {"zai",       "Z.ai",          "zaiKey",    false},
};
inline constexpr size_t kProviderSlotCount =
    sizeof(kProviderSlots) / sizeof(kProviderSlots[0]);

// The canonical registry entry for a slug, or nullptr if the slug is not a
// first-class provider (e.g. "custom", which has its own endpoint UI, or "").
inline const ProviderSlot* findProviderSlot(const char* slug) {
  if (!slug) return nullptr;
  for (size_t i = 0; i < kProviderSlotCount; i++)
    if (std::strcmp(slug, kProviderSlots[i].slug) == 0) return &kProviderSlots[i];
  return nullptr;
}

// True when `slug` names a first-class provider slot. Does NOT include "custom"
// (a free-form endpoint, gated + routed separately) - callers that also accept
// custom OR it in explicitly.
inline bool isProviderSlug(const char* slug) { return findProviderSlot(slug) != nullptr; }

// Provider fanout over the registry (CUM-246). Every device surface that asks "is
// any first-class provider keyed / verified?" walks the registry through this, never
// a hand-listed provider array - a copied list was the recurring CUM-242 drift
// (`anyKeyed` missed cumulo/zai; the onboarding-finish gate missed the recommended
// cumulo, so the flagship path could never complete setup). `pred(slug)` is applied
// to each slot in registry order and the walk short-circuits on the first true.
// Host-tested in test/test_provider_slots with a fake store, so a slot added to
// kProviderSlots is covered with no new code and a fanout that stops enumerating the
// registry FAILS. Callers that also accept "custom" OR it in explicitly (it is a
// free-form endpoint, not a registry slot).
template <typename SlotPred>
inline bool anySlotWhere(SlotPred pred) {
  for (size_t i = 0; i < kProviderSlotCount; i++)
    if (pred(kProviderSlots[i].slug)) return true;
  return false;
}

}  // namespace orch
}  // namespace nimbus
