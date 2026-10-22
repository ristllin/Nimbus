#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "nimbus/orch/models_tool.h"
#include "nimbus/orch/orch_schema.h"
#include "nimbus/orch/provider_slots.h"

// Contract goldens for the agentic harness (Rule 5: additive APIs, never
// mutate). The turn schema (ORCH_SCHEMA_BODY) and the prompt field-doc block
// (ORCH_FIELD_DOCS) both generate from the ORCH_D_* macro single source; these
// snapshots make ANY drift - accidental or intentional - a visible red diff.
//
// Same workflow as test_golden: GOLDEN_UPDATE=1 blesses; in compare mode a
// MISSING golden is a FAILURE (an uncommitted golden must never pass green).
// On mismatch the current text is dumped to test/golden/out/<name> so a plain
// `diff` shows exactly what changed before re-blessing.

static const char* kGoldenDir = "test/golden";
static const char* kOutDir = "test/golden/out";

void setUp() {}
void tearDown() {}

static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}

static bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

static void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

static void checkTextGolden(const char* name, const std::string& current) {
  std::string goldenPath = std::string(kGoldenDir) + "/" + name;
  if (blessMode()) {
    writeFile(goldenPath, current);
    TEST_MESSAGE((std::string("blessed ") + goldenPath).c_str());
    return;
  }
  std::string blessed;
  if (!readFile(goldenPath, blessed)) {
    writeFile(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("missing golden ") + goldenPath +
                       " - bless with GOLDEN_UPDATE=1 (current dumped to " +
                       kOutDir + ")").c_str());
    return;
  }
  if (blessed != current) {
    writeFile(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("contract drift vs ") + goldenPath +
                       " - inspect `diff " + goldenPath + " " + kOutDir + "/" +
                       name + "`, re-bless only if intentional").c_str());
  }
}

static void test_schema_body_golden() {
  checkTextGolden("orch_schema.json", nimbus::orch::ORCH_SCHEMA_BODY);
}

static void test_field_docs_golden() {
  checkTextGolden("orch_field_docs.txt", ORCH_FIELD_DOCS);
}


// R5: on a turn where the head can call models.list, [AVAILABLE MODELS] is this
// one line (5d of the token table) instead of the per-provider lists.
static void test_available_models_pointer_line_golden() {
  std::vector<std::string> keyed;
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots) keyed.push_back(slot.slug);
  checkTextGolden("available_models_line.txt", nimbus::orch::modelsPointerLine(keyed));
}

// The orch_model provider enum and its prose change together with the provider
// registry: every slot is in the enum and in ORCH_D_DEVICE, and nothing else is.
static void test_orch_model_enum_and_prose_follow_the_registry() {
  const std::string schema = nimbus::orch::ORCH_SCHEMA_BODY;
  const std::string tag = "\"const\":\"orch_model\"},\"provider\":{\"type\":\"string\",\"enum\":[";
  const size_t b = schema.find(tag);
  TEST_ASSERT_TRUE(b != std::string::npos);
  const std::string en = schema.substr(b + tag.size(), schema.find(']', b) - b - tag.size());
  size_t n = 0;
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots) {
    TEST_ASSERT_TRUE_MESSAGE(en.find(std::string("\"") + slot.slug + "\"") != std::string::npos, slot.slug);
    n++;
  }
  TEST_ASSERT_EQUAL((int)n, (int)std::count(en.begin(), en.end(), ',') + 1);
  // The prose names the same set (its own order).
  const std::string dev = ORCH_D_DEVICE;
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots)
    TEST_ASSERT_TRUE_MESSAGE(dev.find(slot.slug) != std::string::npos, slot.slug);
  TEST_ASSERT_TRUE(dev.find("openai|anthropic|mistral|zai|cumulo") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_schema_body_golden);
  RUN_TEST(test_field_docs_golden);
  RUN_TEST(test_available_models_pointer_line_golden);
  RUN_TEST(test_orch_model_enum_and_prose_follow_the_registry);
  return UNITY_END();
}
