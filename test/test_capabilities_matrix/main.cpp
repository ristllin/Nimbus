#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/model_catalog.h"

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

static std::string buildMatrix() {
  struct Row {
    const char* provider;
    const char* fixture;
  };
  const Row rows[] = {
      {"openai", "openai.json"},
      {"anthropic", "anthropic.json"},
      {"mistral", "mistral.json"},
      {"zai", "zai.json"},
  };
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
  for (const Row& r : rows) {
    uint16_t roles, caps;
    bool apiCaps;
    aggregate(r.provider, r.fixture, roles, caps, apiCaps);
    md += std::string("| ") + r.provider + " | " + yn(roles & RoleOrchestrator) + " | " +
          yn(roles & RoleSubAgent) + " | " + yn(roles & RoleEmbedding) + " | " +
          yn(roles & RoleVision) + " | " + yn(roles & RoleStt) + " | " + yn(roles & RoleTts) +
          " | " + yn(roles & RoleImage) + " | " + yn(caps & CapTools) + " | " +
          yn(caps & CapStreaming) + " | " + (apiCaps ? "api" : "heuristic") + " |\n";
  }
  md += "| cumulo | per upstream | per upstream | per upstream | per upstream | per upstream | "
        "per upstream | per upstream | per upstream | per upstream | router |\n";
  md += "\nCumulo Nimbus is a router: each role inherits the capabilities of the upstream\n";
  md += "chosen for it (the model id is `<upstream>/<model>`), so its row is the union of\n";
  md += "whichever upstreams the admin enables.\n";
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
