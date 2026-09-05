#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

// model_catalog - the PORTABLE, capability-aware model catalog (host-tested via
// pio test -e native). It owns the pure decisions the old cap-8 harvest in
// src/agent/provider_verify.cpp did inline, plus everything that harvest could
// not express: per-model ROLE classification (orchestrator, sub-agent, embedding,
// vision, STT, TTS, image), CAPABILITY fields (from the provider API where it
// supplies them - Anthropic max_input_tokens/max_tokens, Mistral capabilities -
// and id-family heuristics otherwise), a size class S/M/L (shared with the
// fallback rule engine), and a stable JSON shape for GET /api/models.
//
// NO Arduino / store / TLS here. The device layer (src/agent/provider_verify.cpp)
// still owns the live GET /v1/models fetch + NVS cache; it hands the raw response
// body to parseModelsList() and serializes the result with modelsToJson(). Tests
// replay recorded fixtures per provider (test/test_model_catalog/).
//
// The 8-id cap is gone: every chat-capable model is listed (flagships first), and
// embedding/audio/image models now appear under their own roles instead of being
// dropped. A model the key cannot actually use is marked usable=false by the
// device usability probe and omitted from GET /api/models by default.
namespace nimbus {
namespace orch {

// Roles a model can fill. Bitmask - one model can carry several (a chat model is
// both orchestrator and sub-agent; a multimodal chat model adds vision).
enum ModelRole : uint16_t {
  RoleOrchestrator = 1u << 0,
  RoleSubAgent     = 1u << 1,
  RoleEmbedding    = 1u << 2,
  RoleVision       = 1u << 3,
  RoleStt          = 1u << 4,
  RoleTts          = 1u << 5,
  RoleImage        = 1u << 6,
};

// Wire capabilities. "streaming" is what the PROVIDER supports; the device itself
// still consumes responses non-streamed (see docs/provider-wire.md).
enum ModelCap : uint16_t {
  CapTools     = 1u << 0,  // function / tool calling (the turn contract needs this)
  CapVision    = 1u << 1,  // image input
  CapStreaming = 1u << 2,
  CapJson      = 1u << 3,  // structured / json output
  CapEmbedding = 1u << 4,
  CapAudioIn   = 1u << 5,  // speech-to-text
  CapAudioOut  = 1u << 6,  // text-to-speech
};

struct ModelInfo {
  std::string id;
  uint16_t    roles = 0;         // ModelRole bitmask
  uint16_t    caps = 0;          // ModelCap bitmask
  bool        usable = true;     // usability-probe verdict (true until a probe says otherwise)
  bool        probed = false;    // has the one-shot usability probe run for this id?
  uint32_t    ctxTokens = 0;     // input context window (API-reported or family heuristic)
  uint32_t    maxOutTokens = 0;  // 0 when unknown
  char        size = 0;          // 'S' | 'M' | 'L' | 0 (unclassified)
  std::string family;            // coarse id-family bucket ("gpt-5", "claude-opus", ...)
  bool        apiCaps = false;   // capability/window fields came from the API, not heuristics
  bool        deprecated = false;
  std::string upstream;          // cumulo only: upstream provider slug; "" otherwise

  bool hasRole(ModelRole r) const { return (roles & r) != 0; }
  bool hasCap(ModelCap c) const { return (caps & c) != 0; }
};

// ---- pure classification (also used by the fallback rule engine) -------------
// OpenAI generation number of a "gpt-<N>..." id (gpt-4o -> 4, gpt-5.5 -> 5,
// gpt-6-astra -> 6); 0 for anything else (o-series, gpt-realtime, other vendors).
// The ONE place the "gpt-5 and newer" rule lives: the size class, the vision
// heuristic, the family bucket, the context table, the Responses reasoning gate
// and the device harvest ordering all key on it, so a new generation (gpt-6-astra,
// 2026-09) is classified the day it appears instead of drifting per call site.
int gptGeneration(const std::string& id);
// True when `id` is in the provider's current flagship family - the ids the
// device lists first in the model dropdown (openai: gpt-5 and newer generations;
// zai: glm-5 and newer). Non-flagship families (gpt-4o, o-series, glm-4.x) are
// still usable, just not preferred.
bool isFlagshipFamily(const std::string& provider, const std::string& id);
// Coarse family bucket for an id (lowercased matching). "" if unrecognizable.
std::string modelFamily(const std::string& provider, const std::string& id);
// Size class 'S' | 'M' | 'L', or 0 when the id gives no signal.
char modelSizeClass(const std::string& provider, const std::string& id);
// Full classification of one id (roles + caps + size + family + heuristic window).
// Provider is the slug: openai | anthropic | mistral | cumulo | zai | custom.
ModelInfo classifyModel(const std::string& provider, const std::string& id);

// Like classifyModel but honours the Cumulo "<upstream>/<id>" convention (splits
// and classifies against the upstream, tagging ModelInfo.upstream). For any other
// provider it is classifyModel. Lets a caller rebuild a catalog from a bare id
// list (e.g. the harvested CSV) when a full /models body is unavailable.
ModelInfo classifyCatalogEntry(const std::string& provider, const std::string& id);

// ---- parse a provider /v1/models response into a catalog --------------------
// `body` may carry HTTP response headers ahead of the JSON (tolerated - the JSON
// body is located first). `alloc` backs the parse doc (device: PSRAM; host: null
// => default). Providers:
//   openai/anthropic - id list; Anthropic also reads max_input_tokens / max_tokens
//                      / capabilities when the API returns them (apiCaps=true).
//   mistral          - capabilities + deprecation + aliases metadata.
//   cumulo/zai       - OpenAI-compatible id list (cumulo tags each model's upstream
//                      when the id is prefixed "<upstream>/<id>" or upstreamHint set).
// Non-chat families are still returned, classified under their own role; nothing
// is capped. Flagships sort first. Returns the number of models parsed.
size_t parseModelsList(const std::string& provider, const std::string& body,
                       std::vector<ModelInfo>& out,
                       ArduinoJson::Allocator* alloc = nullptr);

// ---- usability probe verdict -------------------------------------------------
// A model your key cannot actually run must never appear (the phantom-model
// complaint). The device does one cheap probe call per candidate; this maps the
// probe's HTTP status + error body to a verdict. Unusable is reserved for the
// model itself being rejected (unknown model / no access), NOT a transient fault -
// a network/rate-limit/server error is Unknown and must not demote a model.
enum class ProbeVerdict { Usable, Unusable, Unknown };
ProbeVerdict probeVerdict(int httpStatus, const std::string& errBody);

// ---- serialization for GET /api/models --------------------------------------
// Append each model as an object to `arr`. When includeUnusable is false, models
// with usable=false are skipped (the default "a model your key cannot use never
// appears"). Roles/caps render as string arrays / boolean maps per the contract.
void modelsToJson(const std::vector<ModelInfo>& models, JsonArray arr,
                  bool includeUnusable);

// Inverse of modelsToJson: rebuild ModelInfo entries from a persisted /api/models
// model array (device NVS cache round-trip). Returns the number appended.
size_t modelsFromJson(JsonArrayConst arr, std::vector<ModelInfo>& out);

// Canonical role token list (index-stable), for the contract's top-level "roles".
const char* const* roleTokens(int& countOut);
// Token &lt;-&gt; bit helpers (used by tests + the fallback engine's capability predicate).
const char* roleToken(ModelRole r);
const char* capToken(ModelCap c);

}  // namespace orch
}  // namespace nimbus
