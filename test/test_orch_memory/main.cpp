#include <unity.h>

#include <cstring>
#include <string>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/memory.h"

using namespace nimbus::orch;

// In-memory MemoryStore standing in for the device LittleFS impl. `persisted` is
// public so tests can seed an out-of-band (over-cap) blob and inspect what was
// saved after a setModel/clear.
struct FakeStore : MemoryStore {
  std::string persisted;
  bool        cleared = false;
  std::string loadModel() override { return persisted; }
  void        saveModel(const std::string& v) override { persisted = v; }
  void        clearModel() override { persisted.clear(); cleared = true; }
};

static FakeStore g_store;

void setUp() { g_store = FakeStore(); }
void tearDown() {}

// A repeated 3-byte UTF-8 char (U+20AC EURO SIGN, "\xE2\x82\xAC") built to `bytes`
// total. bytes must be a multiple of 3 for a clean start.
static std::string euros(int bytes) {
  std::string s;
  for (int i = 0; i < bytes; i += 3) s += "\xE2\x82\xAC";
  return s;
}

// ---- directive --------------------------------------------------------------

static void test_directive_capped_at_600() {
  OrchMemory m;
  std::string big(kMemDirectiveMax + 50, 'A');
  m.begin(&g_store, big);
  TEST_ASSERT_EQUAL_INT(kMemDirectiveMax, (int)m.directive().size());
}

static void test_directive_shorter_is_untouched() {
  OrchMemory m;
  m.begin(&g_store, "be terse");
  TEST_ASSERT_EQUAL_STRING("be terse", m.directive().c_str());
}

static void test_directive_utf8_safe_cap() {
  OrchMemory m;
  // 600 is a multiple of 3, so a EURO-sign directive of 601 bytes has its final
  // (601st byte = start of a new char, but only 1 of 3 bytes) sequence dropped
  // whole → capped to 600, never a split sequence.
  m.begin(&g_store, euros(kMemDirectiveMax + 3));  // 603 bytes → cap to 600
  const std::string d = m.directive();
  TEST_ASSERT_EQUAL_INT(kMemDirectiveMax, (int)d.size());
  TEST_ASSERT_EQUAL_INT(0, (int)d.size() % 3);  // whole 3-byte chars only
}

// ---- model memory + truncation flag -----------------------------------------

static void test_setmodel_under_cap_no_truncation() {
  OrchMemory m;
  m.begin(&g_store, "");
  TEST_ASSERT_FALSE(m.setModel("running memory"));
  TEST_ASSERT_EQUAL_STRING("running memory", m.model().c_str());
  // Persisted in the B3 map codec (chat "" + \x1F + text); the TEXT survives.
  TEST_ASSERT_TRUE(g_store.persisted.find("running memory") != std::string::npos);
}

static void test_setmodel_over_cap_returns_true_and_stores_truncated() {
  OrchMemory m;
  m.begin(&g_store, "");
  std::string big(kMemModelMax + 100, 'x');
  TEST_ASSERT_TRUE(m.setModel(big));  // truncation signal is load-bearing
  TEST_ASSERT_EQUAL_INT(kMemModelMax, (int)m.model().size());
  // The persisted value is the TRUNCATED one (+1 codec byte for the \x1F sep).
  TEST_ASSERT_EQUAL_INT(kMemModelMax + 1, (int)g_store.persisted.size());
}

static void test_setmodel_utf8_safe_cap() {
  OrchMemory m;
  m.begin(&g_store, "");
  TEST_ASSERT_TRUE(m.setModel(euros(kMemModelMax + 3)));  // 1203 → 1200
  const std::string mem = m.model();
  TEST_ASSERT_EQUAL_INT(kMemModelMax, (int)mem.size());
  TEST_ASSERT_EQUAL_INT(0, (int)mem.size() % 3);  // never a split sequence
}

// ---- begin() re-caps an out-of-band over-cap persisted blob ------------------

static void test_begin_recaps_overcap_persisted_blob() {
  // Out-of-band edit IN THE MAP CODEC (a raw legacy blob is discarded instead -
  // covered by the B3 legacy test): the loaded entry is re-capped.
  g_store.persisted = std::string("\x1F") + std::string(kMemModelMax + 500, 'z');
  OrchMemory m;
  m.begin(&g_store, "");
  TEST_ASSERT_EQUAL_INT(kMemModelMax, (int)m.model().size());
}

// ---- carry across turns (model memory is the failover/reboot seed) -----------

static void test_model_memory_carries_via_store() {
  {
    OrchMemory m;
    m.begin(&g_store, "dir");
    m.setModel("thread: build the journal; state: mid-port");
  }
  // A fresh instance (e.g. after reboot / failover) re-seeds from the store.
  OrchMemory m2;
  m2.begin(&g_store, "dir");
  TEST_ASSERT_EQUAL_STRING("thread: build the journal; state: mid-port",
                           m2.model().c_str());
}

// ---- appendPromptBlock ------------------------------------------------------

static void test_prompt_block_has_both_labels_and_content() {
  OrchMemory m;
  m.begin(&g_store, "always answer in French");
  m.setModel("user prefers metric units");
  std::string sys = "SYSTEM PREAMBLE";
  m.appendPromptBlock(sys);
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "SYSTEM PREAMBLE"));
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "[USER DIRECTIVE"));
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "always answer in French"));
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "[YOUR MEMORY"));
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "user prefers metric units"));
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "1200 bytes"));  // the advertised cap
}

static void test_prompt_block_empty_memory_says_empty() {
  OrchMemory m;
  m.begin(&g_store, "dir");
  std::string sys;
  m.appendPromptBlock(sys);
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "(empty)"));
}

static void test_prompt_block_empty_directive_omitted() {
  OrchMemory m;
  m.begin(&g_store, "");  // no directive
  std::string sys;
  m.appendPromptBlock(sys);
  TEST_ASSERT_NULL(strstr(sys.c_str(), "[USER DIRECTIVE"));  // block omitted
  TEST_ASSERT_NOT_NULL(strstr(sys.c_str(), "[YOUR MEMORY"));  // memory always shown
}

// ---- clear ------------------------------------------------------------------

static void test_clear_wipes_model_not_directive() {
  OrchMemory m;
  m.begin(&g_store, "keep me");
  m.setModel("forget me");
  m.clear();
  TEST_ASSERT_EQUAL_STRING("", m.model().c_str());
  TEST_ASSERT_TRUE(g_store.cleared);
  TEST_ASSERT_TRUE(g_store.persisted.empty());
  TEST_ASSERT_EQUAL_STRING("keep me", m.directive().c_str());
}

// ---- no-store (pure RAM) mode -----------------------------------------------

static void test_null_store_ram_only() {
  OrchMemory m;
  m.begin(nullptr, "dir");           // no persistence
  TEST_ASSERT_FALSE(m.setModel("ram value"));
  TEST_ASSERT_EQUAL_STRING("ram value", m.model().c_str());
  m.clear();                          // must not crash on null store
  TEST_ASSERT_EQUAL_STRING("", m.model().c_str());
}


// ---- per-chat running memory (Release B3) -----------------------------------
static void test_per_chat_isolation_and_lru() {
  OrchMemory m;
  m.begin(nullptr, "d");
  m.setModel("web", "web notes");
  m.setModel("1001", "telegram notes");
  TEST_ASSERT_EQUAL_STRING("web notes", m.model("web").c_str());
  TEST_ASSERT_EQUAL_STRING("telegram notes", m.model("1001").c_str());
  TEST_ASSERT_EQUAL_STRING("", m.model("voice").c_str());          // isolated
  TEST_ASSERT_EQUAL_STRING("telegram notes", m.modelAny().c_str()); // newest
  for (int i = 0; i < 9; i++)
    m.setModel("chat" + std::to_string(i), "n" + std::to_string(i));
  TEST_ASSERT_EQUAL_STRING("", m.model("chat0").c_str());          // LRU-8 evict
  TEST_ASSERT_EQUAL_STRING("n8", m.model("chat8").c_str());
  m.setModel("chat8", "");                                          // empty removes
  TEST_ASSERT_EQUAL_STRING("", m.model("chat8").c_str());
}

static void test_per_chat_persistence_roundtrip_and_legacy_discard() {
  FakeStore st;
  {
    OrchMemory m;
    m.begin(&st, "d");
    m.setModel("web", "alpha");
    m.setModel("1001", "beta");
  }
  {
    OrchMemory m2;
    m2.begin(&st, "d");
    TEST_ASSERT_EQUAL_STRING("alpha", m2.model("web").c_str());
    TEST_ASSERT_EQUAL_STRING("beta", m2.model("1001").c_str());
  }
  st.persisted = "old global running memory";   // legacy single-blob
  {
    OrchMemory m3;
    m3.begin(&st, "d");
    TEST_ASSERT_EQUAL_STRING("", m3.model("web").c_str());
    TEST_ASSERT_EQUAL_STRING("", m3.modelAny().c_str());   // discarded at load
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_per_chat_isolation_and_lru);
  RUN_TEST(test_per_chat_persistence_roundtrip_and_legacy_discard);
  RUN_TEST(test_directive_capped_at_600);
  RUN_TEST(test_directive_shorter_is_untouched);
  RUN_TEST(test_directive_utf8_safe_cap);
  RUN_TEST(test_setmodel_under_cap_no_truncation);
  RUN_TEST(test_setmodel_over_cap_returns_true_and_stores_truncated);
  RUN_TEST(test_setmodel_utf8_safe_cap);
  RUN_TEST(test_begin_recaps_overcap_persisted_blob);
  RUN_TEST(test_model_memory_carries_via_store);
  RUN_TEST(test_prompt_block_has_both_labels_and_content);
  RUN_TEST(test_prompt_block_empty_memory_says_empty);
  RUN_TEST(test_prompt_block_empty_directive_omitted);
  RUN_TEST(test_clear_wipes_model_not_directive);
  RUN_TEST(test_null_store_ram_only);
  return UNITY_END();
}
