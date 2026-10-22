// Host tests for nimbus::fault - the runtime capability-fault registry that the
// resilience suite uses to drive degraded paths on demand (see lib/core/src/fault.cpp).
// The DEGRADED LOGIC each fault gates (VDB cap/eviction, corrupt-blob tolerance) is
// covered in test_orch_vector; here we pin the registry mechanics themselves.
#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/fault.h"

using namespace nimbus::fault;

void setUp() { clearAll(); }
void tearDown() { clearAll(); }

static void test_default_all_clear() {
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  for (uint8_t i = 0; i < COUNT; i++) TEST_ASSERT_FALSE(active(Cap(i)));
}

static void test_set_and_clear_single() {
  set(MIC, true);
  TEST_ASSERT_TRUE(active(MIC));
  TEST_ASSERT_FALSE(active(SPEAKER));                 // isolation: only MIC set
  TEST_ASSERT_EQUAL_UINT16(uint16_t(1) << MIC, mask());
  set(MIC, false);
  TEST_ASSERT_FALSE(active(MIC));
  TEST_ASSERT_EQUAL_UINT16(0, mask());
}

static void test_independent_bits() {
  set(SD, true);
  set(SCREEN, true);
  TEST_ASSERT_TRUE(active(SD));
  TEST_ASSERT_TRUE(active(SCREEN));
  TEST_ASSERT_FALSE(active(LED));
  TEST_ASSERT_EQUAL_UINT16((uint16_t(1) << SD) | (uint16_t(1) << SCREEN), mask());
  set(SD, false);
  TEST_ASSERT_FALSE(active(SD));
  TEST_ASSERT_TRUE(active(SCREEN));                   // clearing one leaves the other
}

static void test_clear_all() {
  set(SD, true);
  set(MEMORY, true);
  set(LED, true);
  clearAll();
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  for (uint8_t i = 0; i < COUNT; i++) TEST_ASSERT_FALSE(active(Cap(i)));
  // Property: whatever subset is injected, clearAll leaves nothing behind.
  for (uint32_t subset = 1; subset < (1u << COUNT); subset += 37) {
    for (uint8_t i = 0; i < COUNT; i++) set(Cap(i), (subset >> i) & 1u);
    clearAll();
    TEST_ASSERT_EQUAL_UINT16(0, mask());
  }
}

// The unit L runner found FAULT clear leaving embed_fail on: that fault lived in a flag
// of the memory subsystem, outside this registry. It is a registry cap now.
static void test_embed_fail_is_a_registry_fault_that_clear_all_ends() {
  Cap c = SD;
  TEST_ASSERT_TRUE(parse("embed_fail", c));
  TEST_ASSERT_EQUAL(EMBED_FAIL, c);
  set(EMBED_FAIL, true);
  set(PROVIDER, true);
  TEST_ASSERT_TRUE(active(EMBED_FAIL));
  clearAll();
  TEST_ASSERT_FALSE(active(EMBED_FAIL));
  TEST_ASSERT_EQUAL_UINT16(0, mask());
}

// ---- the console's FAULT verbs (class guard over src/test_console.cpp) ------------
static std::string readSource(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::string sliceBetween(const std::string& s, const std::string& from, const std::string& to) {
  const size_t a = s.find(from);
  TEST_ASSERT_TRUE_MESSAGE(a != std::string::npos, from.c_str());
  const size_t b = s.find(to, a + from.size());
  TEST_ASSERT_TRUE_MESSAGE(b != std::string::npos, to.c_str());
  return s.substr(a, b - a);
}

// Every "FAULT <name> ..." form the console parses before the registry lookup either
// names a registry cap (so FAULT clear's clearAll ends it) or is a resource the clear
// branch itself releases. A new verb with a flag of its own fails here until it is one
// or the other - FAULT clear can never again leave a fault on.
static void test_every_console_fault_verb_is_ended_by_fault_clear() {
  const std::string src = readSource("src/test_console.cpp");
  const std::string block = sliceBetween(src, "if (line.startsWith(\"FAULT \")) {", "if (line == \"RENDER?\")");
  const std::string clear = sliceBetween(block, "if (rest == \"clear\"", "return;");
  TEST_ASSERT_TRUE(clear.find("nimbus::fault::clearAll()") != std::string::npos);
  struct Released { const char* verb; const char* release; };
  static const Released kSelfHeld[] = {{"sram_ballast", "faultSramBallast(0)"},
                                       {"scratch_fail", "s_h.scratchFail(false)"}};
  std::vector<std::string> verbs;
  const std::string open = "rest.startsWith(\"";
  for (size_t i = block.find(open); i != std::string::npos; i = block.find(open, i + 1)) {
    const size_t a = i + open.size();
    verbs.push_back(block.substr(a, block.find('"', a) - a));
  }
  TEST_ASSERT_TRUE(verbs.size() >= 2);
  for (const std::string& v : verbs) {
    Cap c = SD;
    bool ended = parse(v.c_str(), c);
    for (const Released& r : kSelfHeld)
      if (v == r.verb) ended = clear.find(r.release) != std::string::npos;
    TEST_ASSERT_TRUE_MESSAGE(ended, v.c_str());
  }
}

static void test_idempotent_set() {
  set(MEMORY, true);
  set(MEMORY, true);                                  // double-set is idempotent
  TEST_ASSERT_EQUAL_UINT16(uint16_t(1) << MEMORY, mask());
  set(MEMORY, false);
  set(MEMORY, false);                                 // double-clear is safe
  TEST_ASSERT_EQUAL_UINT16(0, mask());
}

static void test_parse_roundtrip() {
  Cap c;
  for (uint8_t i = 0; i < COUNT; i++) {
    TEST_ASSERT_TRUE(parse(name(Cap(i)), c));
    TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)c);
  }
}

static void test_parse_case_insensitive() {
  Cap c;
  TEST_ASSERT_TRUE(parse("SD", c));       TEST_ASSERT_EQUAL(SD, c);
  TEST_ASSERT_TRUE(parse("Memory", c));   TEST_ASSERT_EQUAL(MEMORY, c);
  TEST_ASSERT_TRUE(parse("SPEAKER", c));  TEST_ASSERT_EQUAL(SPEAKER, c);
}

// sd_io is a DISTINCT capability from sd: injecting one must not set the other
// (sd = absent -> tier caps; sd_io = present-but-writes-fail -> demote path).
static void test_sd_io_distinct_from_sd() {
  clearAll();
  Cap c;
  TEST_ASSERT_TRUE(parse("sd_io", c));   TEST_ASSERT_EQUAL(SD_IO, c);
  set(SD_IO, true);
  TEST_ASSERT_TRUE(active(SD_IO));
  TEST_ASSERT_FALSE(active(SD));         // isolated: sd stays clear
  set(SD, true);
  TEST_ASSERT_TRUE(active(SD) && active(SD_IO));
  set(SD_IO, false);
  TEST_ASSERT_TRUE(active(SD) && !active(SD_IO));
  clearAll();
}

static void test_parse_rejects_unknown() {
  Cap c = SD;
  TEST_ASSERT_FALSE(parse("wifi", c));
  TEST_ASSERT_FALSE(parse("", c));
  TEST_ASSERT_FALSE(parse(nullptr, c));
  TEST_ASSERT_FALSE(parse("sdd", c));     // a valid name plus extra chars must not match
  TEST_ASSERT_FALSE(parse("s", c));       // a strict prefix must not match
}

static void test_out_of_range_cap_is_safe() {
  set(COUNT, true);                        // out of range -> no-op, no OOB write
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  TEST_ASSERT_FALSE(active(COUNT));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_default_all_clear);
  RUN_TEST(test_set_and_clear_single);
  RUN_TEST(test_independent_bits);
  RUN_TEST(test_clear_all);
  RUN_TEST(test_embed_fail_is_a_registry_fault_that_clear_all_ends);
  RUN_TEST(test_every_console_fault_verb_is_ended_by_fault_clear);
  RUN_TEST(test_idempotent_set);
  RUN_TEST(test_parse_roundtrip);
  RUN_TEST(test_parse_case_insensitive);
  RUN_TEST(test_sd_io_distinct_from_sd);
  RUN_TEST(test_parse_rejects_unknown);
  RUN_TEST(test_out_of_range_cap_is_safe);
  return UNITY_END();
}
