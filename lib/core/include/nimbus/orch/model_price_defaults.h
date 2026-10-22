#pragma once
#include <cstdint>
#include <cstring>

#include "nimbus/orch/model_key.h"

// Built-in price estimates (owner 2026-10-13: per-model prices). Portable, in flash
// (constexpr), and ALWAYS shown as an estimate ('~'): they are what an unpriced model
// costs until the owner enters a price or the Cumulo router reports one.
//
// Seeded from the Cumulo router's MODEL_PRICES seed (cumulo-nimbus
// packages/shared/src/router.ts) for every cold-start id in model_choices.h, plus
// gpt-4o-mini (vision, image scans) and text-embedding-3-small (memory embeddings).
// Units: micro-USD per 1M tokens (the router's own unit), micro-USD per 1000 calls.
// A per-slot '*' row is that provider's default for any other model: it carries the
// per-provider defaults the web page used to hold client-side (openai 2.5/10,
// anthropic 3/15, mistral 2/6, zai glm-5.3 1.4/4.4, tavily $8 per 1000 searches).
// Custom endpoints have none (a LAN model is often free; the owner prices it).

namespace nimbus {
namespace orch {

inline constexpr const char* kBuiltinPricesAsOf = "2026-10";

// A model row's request-size tier (claude-haiku-5-5 bills a request whose prompt is
// over 100K tokens at its own, higher rates): the spend of such a request files under
// "<model>:long", a row of its own (TurnSpend::add).
inline constexpr const char* kLongTierSuffix = ":long";

struct BuiltinPrice {
  const char* slot;        // provider slot ("openai", ..., "tavily")
  const char* family;      // model family, or "*" for the provider default
  uint32_t    microIn;     // micro-USD per 1M input tokens
  uint32_t    microOut;    // micro-USD per 1M output tokens
  uint32_t    microPerKCalls;   // micro-USD per 1000 calls (search providers)
  // Prompt-cache rates (owner ruling R4, 16 Oct 2026): micro-USD per 1M cached
  // (read) and cache-written input tokens. 0 = the default share of the input rate
  // (reads 0.1x, writes 1.25x, the 5-minute write).
  uint32_t    microCacheR = 0;
  uint32_t    microCacheW = 0;
  // A request whose prompt is OVER this many tokens bills at the "<family>:long" row
  // (0 = no size tier).
  uint32_t    longOver = 0;
  const char* note = nullptr;   // shown beside the estimate (a promotional price, ...)
};

// Prices are list prices as of kBuiltinPricesAsOf. Where a provider runs a time-
// limited promotion, the row keeps the regular price (fail-closed money: an estimate
// never reads cheaper than the bill once the promotion ends) and says so in `note`.
inline constexpr const char* kGpt56PromoNote = "promo $4 / $20 until at least 21 Nov 2026";

inline constexpr BuiltinPrice kBuiltinPrices[] = {
    {"openai", "gpt-6-astra", 10000000, 50000000, 0},
    {"openai", "gpt-6-sol", 2000000, 10000000, 0},
    {"openai", "gpt-6.1-sol", 2000000, 10000000, 0},
    {"openai", "gpt-6-luna", 100000, 500000, 0},
    // The gpt-5.6 alias routes to gpt-5.6-sol: one price, cache read $0.50, write $6.25.
    {"openai", "gpt-5.6", 5000000, 30000000, 0, 500000, 6250000, 0, kGpt56PromoNote},
    {"openai", "gpt-5.6-sol", 5000000, 30000000, 0, 500000, 6250000, 0, kGpt56PromoNote},
    {"openai", "gpt-5.6-terra", 2000000, 12000000, 0},
    {"openai", "gpt-5.6-luna", 200000, 1200000, 0},
    {"openai", "o4-mini-deep-research", 2000000, 8000000, 0},
    {"openai", "gpt-4o-mini", 150000, 600000, 0},
    {"openai", "text-embedding-3-small", 20000, 0, 0},
    // Lane DEF: the paid calls outside a turn (seam_meter.h) price their reported usage
    // here. gpt-image-1 input at its image-input rate (text input is half), output
    // tokens are the image; gpt-4o-mini-transcribe input at its audio rate.
    {"openai", "gpt-image-1", 10000000, 40000000, 0},
    {"openai", "gpt-4o-mini-transcribe", 3000000, 5000000, 0},
    {"openai", "*", 2500000, 10000000, 0},
    {"anthropic", "claude-fable-5-1", 10000000, 50000000, 0},
    {"anthropic", "claude-opus-5-5", 4000000, 20000000, 0},
    {"anthropic", "claude-opus-5", 5000000, 25000000, 0},
    {"anthropic", "claude-sonnet-5-5", 2000000, 10000000, 0},
    {"anthropic", "claude-sonnet-5", 2000000, 10000000, 0},
    // Cache read $0.01, 5-minute write $0.125; a prompt over 100K tokens: $0.50 / $2.50.
    {"anthropic", "claude-haiku-5-5", 100000, 500000, 0, 10000, 125000, 100000},
    {"anthropic", "claude-haiku-5-5:long", 500000, 2500000, 0},
    {"anthropic", "claude-haiku-4-5", 1000000, 5000000, 0},
    {"anthropic", "*", 3000000, 15000000, 0},
    {"mistral", "mistral-large", 2000000, 6000000, 0},
    {"mistral", "mistral-medium", 1500000, 7500000, 0},
    {"mistral", "mistral-small", 200000, 600000, 0},
    {"mistral", "mistral-embed", 100000, 0, 0},        // lane DEF: memory embeddings
    {"mistral", "mistral-moderation", 100000, 0, 0},   // lane DEF: the message classifier
    {"mistral", "*", 2000000, 6000000, 0},
    {"zai", "glm-5.3", 1400000, 4400000, 0},
    {"zai", "glm-5.2", 1400000, 4400000, 0},
    {"zai", "glm-5.3-flash", 150000, 500000, 0},
    {"zai", "*", 1400000, 4400000, 0},
    {"tavily", "*", 0, 0, 8000000},
};
inline constexpr size_t kBuiltinPriceCount = sizeof(kBuiltinPrices) / sizeof(kBuiltinPrices[0]);

// The provider-default ('*') row of a slot, or nullptr (custom, unknown).
inline const BuiltinPrice* builtinSlotDefault(const char* slot) {
  if (!slot) return nullptr;
  for (const BuiltinPrice& p : kBuiltinPrices)
    if (std::strcmp(p.slot, slot) == 0 && std::strcmp(p.family, "*") == 0) return &p;
  return nullptr;
}

// The model row of a slot: the LONGEST family the model is, or extends by exactly
// one version suffix (the money-safe rule: "gpt-5.6-luna" never inherits gpt-5.6).
inline const BuiltinPrice* builtinSlotModel(const char* slot, const char* model) {
  if (!slot || !model || !*model) return nullptr;
  const BuiltinPrice* best = nullptr;
  size_t bestLen = 0;
  for (const BuiltinPrice& p : kBuiltinPrices) {
    if (std::strcmp(p.slot, slot) != 0 || std::strcmp(p.family, "*") == 0) continue;
    const size_t fl = std::strlen(p.family);
    if (std::strncmp(model, p.family, fl) != 0) continue;
    if (model[fl] != 0 && !isVersionSuffix(model + fl)) continue;
    if (fl > bestLen) { best = &p; bestLen = fl; }
  }
  return best;
}

// The slot a (provider, model key) is priced from: a Cumulo key "<upstream>/<bare>"
// reads its upstream's rows. `bare` receives the model part to match.
inline const char* builtinSlotFor(const char* prov, const char* key, const char** bare,
                                  char* upBuf /* kModelKeyCap */) {
  *bare = key ? key : "";
  if (!prov || std::strcmp(prov, "cumulo") != 0) return prov;
  const char* sl = key ? std::strchr(key, '/') : nullptr;
  if (!sl || sl == key || (size_t)(sl - key) >= kModelKeyCap) return "openai";
  std::memcpy(upBuf, key, (size_t)(sl - key));
  upBuf[sl - key] = 0;
  *bare = sl + 1;
  return upBuf;
}

// Built-in model estimate for (provider, model key), or nullptr.
inline const BuiltinPrice* builtinModelPrice(const char* prov, const char* key) {
  char up[kModelKeyCap];
  const char* bare;
  const char* slot = builtinSlotFor(prov, key, &bare, up);
  return builtinSlotModel(slot, bare);
}

// Built-in provider default for (provider, model key): the slot's '*' row. A Cumulo
// key reads its upstream's default, and Cumulo itself (an unknown upstream, or the
// provider-level fallback) reads openai's: the router's default upstream.
inline const BuiltinPrice* builtinDefaultPrice(const char* prov, const char* key) {
  char up[kModelKeyCap];
  const char* bare;
  const char* slot = builtinSlotFor(prov, key, &bare, up);
  const BuiltinPrice* p = builtinSlotDefault(slot);
  if (!p && prov && std::strcmp(prov, "cumulo") == 0) p = builtinSlotDefault("openai");
  return p;
}

// The highest built-in token price, per field, of any row `prov` can be priced from:
// its own slot, or every slot for Cumulo (whose keys read their upstream's rows).
// 0 when there is none (custom).
inline void builtinHighestPrice(const char* prov, uint32_t& microIn, uint32_t& microOut) {
  microIn = microOut = 0;
  if (!prov) return;
  const bool anySlot = std::strcmp(prov, "cumulo") == 0;
  for (const BuiltinPrice& p : kBuiltinPrices) {
    if (!anySlot && std::strcmp(p.slot, prov) != 0) continue;
    if (p.microIn > microIn) microIn = p.microIn;
    if (p.microOut > microOut) microOut = p.microOut;
  }
}

// The price of a model with NO row of its own (owner ruling R4, fail-closed money):
// per field, the highest built-in price of the slot it is priced from - a Cumulo key
// "<upstream>/<bare>" reads its upstream's rows, any other Cumulo key every slot. 0 when
// the slot has no rows (custom).
inline void builtinUnknownPrice(const char* prov, const char* key, uint32_t& microIn,
                                uint32_t& microOut) {
  char up[kModelKeyCap];
  const char* bare;
  const char* slot = builtinSlotFor(prov, key, &bare, up);
  const bool upstreamKnown = slot && prov && std::strcmp(prov, "cumulo") == 0 &&
                             key && std::strchr(key, '/') && builtinSlotDefault(slot);
  builtinHighestPrice(upstreamKnown ? slot : prov, microIn, microOut);
}

// The row a request-size tier files under: "<key>:long" when `key`'s built-in row has
// a tier and the request's prompt was over it (out: kModelKeyCap bytes). False (out
// untouched) when there is no tier, the prompt was within it, or the key cannot grow.
inline bool builtinLongTierKey(const char* prov, const char* key, uint32_t promptTokens,
                               char* out) {
  const BuiltinPrice* row = builtinModelPrice(prov, key);
  if (!row || !row->longOver || promptTokens <= row->longOver) return false;
  const size_t kl = std::strlen(key), sl = std::strlen(kLongTierSuffix);
  if (kl + sl >= kModelKeyCap) return false;
  std::memcpy(out, key, kl);
  std::memcpy(out + kl, kLongTierSuffix, sl + 1);
  return true;
}

}  // namespace orch
}  // namespace nimbus
