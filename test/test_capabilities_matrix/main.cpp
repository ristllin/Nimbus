#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/embedding.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/provider_slots.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Generates docs/reference/capabilities-matrix.md FROM the catalog code, so the doc
// cannot drift from reality (CUM-56). The truth values come from classifyModel /
// parseModelsList over the recorded /v1/models fixtures; this file only formats
// them. Re-generate with:  GOLDEN_UPDATE=1 pio test -e native -f test_capabilities_matrix
// In compare mode a drift FAILS the suite (so a catalog change that isn't
// re-blessed is caught).
static const char* kFixDir = "test/support/fixtures/models";
static const char* kDocPath = "docs/reference/capabilities-matrix.md";

static bool readFile(const char* path, std::string& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
static std::string readFixture(const char* name) {
  std::string out;
  readFile((std::string(kFixDir) + "/" + name).c_str(), out);
  return out;
}

// OR together every model's roles+caps for a provider fixture (the union that a
// keyed provider can offer).
static void aggregate(const std::string& provider, const char* fixture, uint16_t& roles,
                      uint16_t& caps, bool& apiCaps) {
  std::vector<ModelInfo> v;
  parseModelsList(provider, readFixture(fixture), v);
  roles = 0;
  caps = 0;
  apiCaps = false;
  for (const ModelInfo& m : v) {
    roles |= m.roles;
    caps |= m.caps;
    if (m.apiCaps) apiCaps = true;
  }
}

static const char* yn(bool b) { return b ? "yes" : "no"; }

static bool fileExists(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// CUM-469: which providers the DEVICE can embed memories with, and the exact request
// it sends each one. The catalog's Embedding column above only says a provider HAS an
// embedding model; Mistral was "yes" there while every device embed call to it 422'd,
// because one OpenAI-shaped body went to every provider. These rows come from the
// real route table + request builder (nimbus/orch/embedding.h) over the same
// registry, so a provider gets an honest row (or an explicit "no") the moment it is
// added, and the request-field list changes here whenever the builder's output does.
static std::string bodyFields(const std::string& body, bool& carriesWidth) {
  ArduinoJson::JsonDocument d;
  carriesWidth = false;
  if (deserializeJson(d, body)) return "";
  std::string out;
  for (ArduinoJson::JsonPairConst kv : d.as<ArduinoJson::JsonObjectConst>()) {
    if (!out.empty()) out += ", ";
    out += std::string("`") + kv.key().c_str() + "`";
    if (std::strcmp(kv.key().c_str(), "dimensions") == 0) carriesWidth = true;
  }
  return out;
}

static std::string buildEmbeddingsSection() {
  std::string md;
  md += "\n## Memory embeddings\n\n";
  md += "Which providers the device can embed long-term memories with (Memory >\n";
  md += "Embedding model). Generated from the embeddings route table and request\n";
  md += "builder (`lib/core/include/nimbus/orch/embedding.h`), so each row shows the\n";
  md += "request the device really sends. `Width` says what sets the vector width:\n";
  md += "`Dimensions` when the request carries the saved Dimensions, `model` when the\n";
  md += "provider has no width field and the model's own width comes back\n";
  md += "(mistral-embed: 1024), so Dimensions must equal it. A vector of any other\n";
  md += "width is refused, never truncated.\n\n";
  md += "| Provider | Memory embeddings | Endpoint | Request fields | Width |\n";
  md += "|---|---|---|---|---|\n";
  for (const ProviderSlot& slot : kProviderSlots) {
    const EmbedRoute r = embedRouteFor(slot.slug);
    if (!r.known) {
      md += std::string("| ") + slot.slug + " | no | - | - | - |\n";
      continue;
    }
    bool carriesWidth = false;
    const std::string fields =
        bodyFields(buildEmbeddingRequest(slot.slug, "m", "x", 256), carriesWidth);
    TEST_ASSERT_FALSE_MESSAGE(fields.empty(), slot.slug);   // a routed provider must build a body
    md += std::string("| ") + slot.slug + " | yes | `" + r.path + "`" +
          (r.viaCumuloRouter ? " (router key)" : "") + " | " + fields + " | " +
          (carriesWidth ? "Dimensions" : "model") + " |\n";
  }
  return md;
}

// CUM-246: the matrix is driven by the canonical provider registry, NOT a list
// hand-copied beside it. The old rows[] = {openai, anthropic, mistral, zai} was
// exactly the "provider-list hardcode" anti-pattern this issue guards: a provider
// added to kProviderSlots would be silently absent from the matrix (and its
// capability doc) with every test still green. Now each BYOK slot in the registry
// renders a fixture-driven row (in registry order), the recommended flagship
// (Cumulo) renders the router row, and a BYOK slot whose `<slug>.json` fixture is
// missing FAILS here - so adding a provider forces adding its capability coverage.
static std::string buildMatrix() {
  std::string md;
  md += "# Provider capability matrix\n\n";
  md += "Generated from the model catalog code (`lib/core/src/model_catalog.cpp`) over\n";
  md += "recorded `/v1/models` fixtures, so it cannot drift from what the device\n";
  md += "actually classifies. Do not edit by hand: re-run\n";
  md += "`GOLDEN_UPDATE=1 pio test -e native -f test_capabilities_matrix`.\n\n";
  md += "A cell is `yes` when at least one of that provider's live models carries the\n";
  md += "role or capability. `source` is `api` when the provider's models endpoint\n";
  md += "supplies capability fields, `heuristic` when the device infers them from the\n";
  md += "model id family.\n\n";
  md += "| Provider | Orchestrator | Sub-agent | Embedding | Vision | STT | TTS | Image | Tools | Streaming | Source |\n";
  md += "|---|---|---|---|---|---|---|---|---|---|---|\n";
  const ProviderSlot* router = nullptr;   // the recommended flagship (Cumulo) row
  for (const ProviderSlot& slot : kProviderSlots) {
    if (slot.recommended) {   // the router: its capabilities are per-upstream, no fixture
      router = &slot;
      continue;
    }
    // A first-class BYOK provider must ship a recorded /v1/models fixture, or its
    // capability row cannot be generated - fail loudly rather than emit a blank row.
    const std::string fixture = std::string(slot.slug) + ".json";
    TEST_ASSERT_TRUE_MESSAGE(fileExists((std::string(kFixDir) + "/" + fixture).c_str()),
                             slot.slug);   // missing test/support/fixtures/models/<slug>.json
    uint16_t roles, caps;
    bool apiCaps;
    aggregate(slot.slug, fixture.c_str(), roles, caps, apiCaps);
    md += std::string("| ") + slot.slug + " | " + yn(roles & RoleOrchestrator) + " | " +
          yn(roles & RoleSubAgent) + " | " + yn(roles & RoleEmbedding) + " | " +
          yn(roles & RoleVision) + " | " + yn(roles & RoleStt) + " | " + yn(roles & RoleTts) +
          " | " + yn(roles & RoleImage) + " | " + yn(caps & CapTools) + " | " +
          yn(caps & CapStreaming) + " | " + (apiCaps ? "api" : "heuristic") + " |\n";
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(router, "no recommended (router) provider in kProviderSlots");
  md += std::string("| ") + router->slug +
        " | per upstream | per upstream | per upstream | per upstream | per upstream | "
        "per upstream | per upstream | per upstream | per upstream | router |\n";
  md += "\nCumulo Nimbus is a router: each role inherits the capabilities of the upstream\n";
  md += "chosen for it (the model id is `<upstream>/<model>`), so its row is the union of\n";
  md += "whichever upstreams the admin enables.\n";
  md += buildEmbeddingsSection();
  return md;
}

static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}

static void test_capabilities_matrix_matches_catalog() {
  const std::string current = buildMatrix();
  if (blessMode()) {
    FILE* f = std::fopen(kDocPath, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, kDocPath);
    std::fwrite(current.data(), 1, current.size(), f);
    std::fclose(f);
    TEST_MESSAGE("blessed docs/reference/capabilities-matrix.md");
    return;
  }
  std::string onDisk;
  if (!readFile(kDocPath, onDisk)) {
    TEST_FAIL_MESSAGE("missing docs/reference/capabilities-matrix.md - run with GOLDEN_UPDATE=1");
    return;
  }
  TEST_ASSERT_EQUAL_STRING_MESSAGE(
      onDisk.c_str(), current.c_str(),
      "capabilities matrix drifted from the catalog code - re-bless with GOLDEN_UPDATE=1");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_capabilities_matrix_matches_catalog);
  UNITY_END();
  return 0;
}
