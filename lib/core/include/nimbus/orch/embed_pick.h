#pragma once
#include <string>

// embed_pick - which provider long-term memory embeds with, given the keys the owner
// actually has. Portable and host-tested (test/test_embed_pick).
//
// The stored default is OpenAI (text-embedding-3-small at 256 wide), and nothing used
// to change it except the Memory page. So a device set up the way the wizard
// recommends (one Cumulo Nimbus key), or with only a Mistral key, failed every memory
// write, recall and search with "no embeddings key for openai". The device already
// has an embeddings route for each of openai, cumulo and mistral (embedding.h); this
// decides which one to use.
//
// The one hard rule is the set-once vector space: vectors from a different model or
// width cannot be compared. So:
//   - With stored vectors, the only allowed switch keeps the space: the same OpenAI
//     model and width through the other OpenAI route (openai <-> cumulo; the Cumulo
//     router proxies OpenAI's embeddings byte for byte).
//   - With no vectors yet, any keyed provider may take over, in this order: the
//     configured provider's OpenAI-route sibling (same model and width), openai,
//     cumulo, then mistral (mistral-embed at its native 1024).
// A configured provider that has a usable key always stays: an owner's explicit
// choice on the Memory page is never overridden while it works.
namespace nimbus {
namespace orch {

// The defaults a switch writes. EMBED_DEFAULT_MODEL / _DIMS (src/agent/agent_config.h)
// are asserted equal to the OpenAI-shaped pair at compile time.
constexpr const char* kEmbedOpenAiModel = "text-embedding-3-small";
constexpr int kEmbedOpenAiDims = 256;
constexpr const char* kEmbedMistralModel = "mistral-embed";
constexpr int kEmbedMistralDims = 1024;   // mistral-embed is fixed at 1024 (embedding.h)

// Compile-time string equality, for the drift assert in src/agent/agent_config.h.
constexpr bool embedTextEq(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || embedTextEq(a + 1, b + 1));
}

struct EmbedConfig {
  std::string provider;
  std::string model;
  int dims = 0;
};

// Which embed-capable providers have a usable key: stored, and not rejected by the
// provider's last verify. Anthropic and Z.ai have no device embeddings route.
struct EmbedKeys {
  bool openai = false;
  bool cumulo = false;
  bool mistral = false;
};

enum class EmbedPickAct : unsigned char {
  Keep,       // the configured provider has a usable key: change nothing
  Switch,     // store `pick` before the next embedding
  None,       // no usable key can embed into this memory (the owner needs to add one)
  Conflict,   // stored vectors and the stored config disagree on the width: switch nothing
};

struct EmbedPick {
  EmbedPickAct act = EmbedPickAct::None;
  EmbedConfig pick;   // set only for Switch
};

// Whether `provider` (an embedRouteFor slug) has a usable key in `keys`.
bool embedKeyUsable(const EmbedKeys& keys, const std::string& provider);

// The model and width a switch to `provider` writes when it does not keep the current
// space (openai/cumulo: the OpenAI defaults; mistral: mistral-embed at 1024). An
// unknown provider gets an empty config.
EmbedConfig embedDefaultsFor(const std::string& provider);

// The decision. `hasVectors`: any stored vector at all (the set-once lock, or a live
// or archived entry). `storedWidth`: the width the stored vectors actually have (the
// vector store's own, from its blob), 0 when unknown. Stored vectors ARE the space: a
// config whose width disagrees with them (a card from another device, a cut write) is
// a Conflict and nothing switches; the Memory page's reset is the way out.
EmbedPick pickEmbedProvider(const EmbedConfig& cur, const EmbedKeys& keys, bool hasVectors,
                            int storedWidth = 0);

}  // namespace orch
}  // namespace nimbus
