#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/embed_pick.h"
#include "nimbus/orch/embedding.h"

// QW5: long-term memory embedded with OpenAI whatever key the owner had, so a device
// set up with only a Cumulo Nimbus key (the wizard's recommended path) or only a
// Mistral key failed every memory write, recall and search. pickEmbedProvider decides
// which keyed provider embeds, without ever breaking the set-once vector space.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static EmbedKeys keysFrom(unsigned mask) {
  EmbedKeys k;
  k.openai = (mask & 1u) != 0;
  k.cumulo = (mask & 2u) != 0;
  k.mistral = (mask & 4u) != 0;
  return k;
}

// Every configuration memory can be in: the stored default, each route's default, an
// owner's explicit large model, and providers with no device embeddings route (a
// stored value the device cannot call must never be kept as if it worked).
static std::vector<EmbedConfig> allConfigs() {
  return {
      {"openai", kEmbedOpenAiModel, kEmbedOpenAiDims},
      {"cumulo", kEmbedOpenAiModel, kEmbedOpenAiDims},
      {"openai", "text-embedding-3-large", 3072},
      {"cumulo", "text-embedding-3-large", 512},
      {"mistral", kEmbedMistralModel, kEmbedMistralDims},
      {"anthropic", "voyage-3", 1024},
      {"zai", "embedding-3", 1024},
      {"", "", 0},
  };
}

// Two configs describe the same vector space when the model and width match and both
// ride the OpenAI wire (openai direct or through the Cumulo router).
static bool sameSpace(const EmbedConfig& a, const EmbedConfig& b) {
  if (a.model != b.model || a.dims != b.dims) return false;
  if (a.provider == b.provider) return true;
  const bool aOai = a.provider == "openai" || a.provider == "cumulo";
  const bool bOai = b.provider == "openai" || b.provider == "cumulo";
  return aOai && bOai;
}

// The class, over every stored config x every key set x vectors or not:
//  - a configured provider with a usable key is always kept;
//  - a switch only ever lands on a provider with a usable key and a device route;
//  - with stored vectors, a switch keeps the vector space exactly;
//  - with no vectors, memory works whenever ANY embed-capable key exists;
//  - None only when nothing usable can embed into this memory.
static void test_every_state_obeys_the_rules() {
  int checked = 0;
  for (const EmbedConfig& cur : allConfigs()) {
    for (unsigned mask = 0; mask < 8; ++mask) {
      const EmbedKeys keys = keysFrom(mask);
      for (int v = 0; v < 2; ++v) {
        const bool hasVectors = v == 1;
        const EmbedPick p = pickEmbedProvider(cur, keys, hasVectors);
        char ctx[160];
        snprintf(ctx, sizeof ctx, "cur=%s/%s/%d keys=%u vectors=%d", cur.provider.c_str(),
                 cur.model.c_str(), cur.dims, mask, (int)hasVectors);
        const bool curUsable = embedKeyUsable(keys, cur.provider);
        if (curUsable) {
          TEST_ASSERT_TRUE_MESSAGE(p.act == EmbedPickAct::Keep, ctx);
        } else {
          TEST_ASSERT_FALSE_MESSAGE(p.act == EmbedPickAct::Keep, ctx);
        }
        if (p.act == EmbedPickAct::Switch) {
          TEST_ASSERT_TRUE_MESSAGE(embedKeyUsable(keys, p.pick.provider), ctx);
          TEST_ASSERT_TRUE_MESSAGE(embedRouteFor(p.pick.provider).known, ctx);
          TEST_ASSERT_TRUE_MESSAGE(p.pick.dims > 0, ctx);
          TEST_ASSERT_FALSE_MESSAGE(p.pick.model.empty(), ctx);
          if (hasVectors) TEST_ASSERT_TRUE_MESSAGE(sameSpace(cur, p.pick), ctx);
        }
        if (!hasVectors && mask != 0)
          TEST_ASSERT_FALSE_MESSAGE(p.act == EmbedPickAct::None, ctx);
        if (mask == 0) TEST_ASSERT_TRUE_MESSAGE(p.act == EmbedPickAct::None, ctx);
        ++checked;
      }
    }
  }
  TEST_ASSERT_EQUAL(8 * 8 * 2, checked);
}

// The wizard's recommended path: a fresh device (OpenAI default, nothing stored) whose
// only key is a Cumulo key embeds through the router in the SAME space, so the vectors
// stay comparable if an OpenAI key is added later.
static void test_cumulo_only_fresh_device_embeds_through_the_router() {
  const EmbedConfig def{"openai", kEmbedOpenAiModel, kEmbedOpenAiDims};
  const EmbedPick p = pickEmbedProvider(def, keysFrom(2), false);
  TEST_ASSERT_TRUE(p.act == EmbedPickAct::Switch);
  TEST_ASSERT_EQUAL_STRING("cumulo", p.pick.provider.c_str());
  TEST_ASSERT_EQUAL_STRING(kEmbedOpenAiModel, p.pick.model.c_str());
  TEST_ASSERT_EQUAL(kEmbedOpenAiDims, p.pick.dims);
  // Later the owner adds an OpenAI key: cumulo still works, so nothing moves.
  TEST_ASSERT_TRUE(pickEmbedProvider(p.pick, keysFrom(3), true).act == EmbedPickAct::Keep);
}

// Mistral only, nothing stored: mistral-embed at its native width (a 256-wide store
// could never hold a 1024-wide vector, CUM-469).
static void test_mistral_only_fresh_device_takes_the_native_width() {
  const EmbedConfig def{"openai", kEmbedOpenAiModel, kEmbedOpenAiDims};
  const EmbedPick p = pickEmbedProvider(def, keysFrom(4), false);
  TEST_ASSERT_TRUE(p.act == EmbedPickAct::Switch);
  TEST_ASSERT_EQUAL_STRING("mistral", p.pick.provider.c_str());
  TEST_ASSERT_EQUAL_STRING(kEmbedMistralModel, p.pick.model.c_str());
  TEST_ASSERT_EQUAL(kEmbedMistralDims, p.pick.dims);
  // Once Mistral vectors exist, an OpenAI key added later must NOT take over.
  const EmbedPick later = pickEmbedProvider(p.pick, keysFrom(1 | 4), true);
  TEST_ASSERT_TRUE(later.act == EmbedPickAct::Keep);
  // ...and with the Mistral key removed, nothing else can embed into that space.
  TEST_ASSERT_TRUE(pickEmbedProvider(p.pick, keysFrom(1 | 2), true).act == EmbedPickAct::None);
}

// OpenAI vectors on a device that now has only a Cumulo key: same model, same width,
// through the router. The owner's explicit large model is carried over unchanged.
static void test_stored_openai_space_moves_to_the_router_unchanged() {
  const EmbedConfig big{"openai", "text-embedding-3-large", 3072};
  const EmbedPick p = pickEmbedProvider(big, keysFrom(2 | 4), true);
  TEST_ASSERT_TRUE(p.act == EmbedPickAct::Switch);
  TEST_ASSERT_EQUAL_STRING("cumulo", p.pick.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("text-embedding-3-large", p.pick.model.c_str());
  TEST_ASSERT_EQUAL(3072, p.pick.dims);
}

// Anthropic or Z.ai only: nothing can embed. The caller says so plainly.
static void test_no_embed_capable_key_is_none() {
  const EmbedConfig def{"openai", kEmbedOpenAiModel, kEmbedOpenAiDims};
  TEST_ASSERT_TRUE(pickEmbedProvider(def, keysFrom(0), false).act == EmbedPickAct::None);
  TEST_ASSERT_TRUE(embedDefaultsFor("anthropic").provider.empty());
  TEST_ASSERT_TRUE(embedDefaultsFor("zai").provider.empty());
}

// The guard against a mixed store (prism review): stored vectors whose width differs
// from the stored config (a card from another device, a cut write) are never re-widened
// or moved, whatever keys exist; only an agreeing width lets the same-space switch run.
static void test_a_width_conflict_with_stored_vectors_switches_nothing() {
  for (const EmbedConfig& cur : allConfigs()) {
    for (unsigned mask = 0; mask < 8; ++mask) {
      const int otherWidth = cur.dims == 1024 ? 256 : 1024;
      const EmbedPick p = pickEmbedProvider(cur, keysFrom(mask), true, otherWidth);
      TEST_ASSERT_TRUE(p.act == EmbedPickAct::Conflict);
      // No vectors: the width the empty store carries does not matter.
      TEST_ASSERT_FALSE(pickEmbedProvider(cur, keysFrom(mask), false, otherWidth).act ==
                        EmbedPickAct::Conflict);
    }
  }
  // The finding's case: a card with 1024-wide Mistral vectors in a device whose config
  // is the OpenAI default, with only a Cumulo key.
  const EmbedConfig def{"openai", kEmbedOpenAiModel, kEmbedOpenAiDims};
  TEST_ASSERT_TRUE(pickEmbedProvider(def, keysFrom(2), true, 1024).act == EmbedPickAct::Conflict);
  TEST_ASSERT_TRUE(pickEmbedProvider(def, keysFrom(2), true, 256).act == EmbedPickAct::Switch);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_state_obeys_the_rules);
  RUN_TEST(test_cumulo_only_fresh_device_embeds_through_the_router);
  RUN_TEST(test_mistral_only_fresh_device_takes_the_native_width);
  RUN_TEST(test_stored_openai_space_moves_to_the_router_unchanged);
  RUN_TEST(test_no_embed_capable_key_is_none);
  RUN_TEST(test_a_width_conflict_with_stored_vectors_switches_nothing);
  return UNITY_END();
}
