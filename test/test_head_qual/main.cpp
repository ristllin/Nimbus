#include <unity.h>

#include <type_traits>

#include <cstring>
#include <set>
#include <string>

#include "nimbus/orch/head_qual.h"
#include "nimbus/orch/model_defaults.h"

using namespace nimbus::orch::headqual;

void setUp() {}
void tearDown() {}

// The holdHeads each line renders. The SAME literals are pinned in
// tools/test_make_manifest.py (test_hold_heads_golden), which renders them from the
// HQ_ROW lines in Python: change a row, change both goldens.
static const char* kGoldenE1 =
    "[\"*/*\",\"!anthropic/claude-haiku-5-5-*\",\"!anthropic/claude-haiku-5-5\",\"!anthropic/claude-sonnet-5-5-*\",\"!anthropic/claude-sonnet-5-5\"]";
static const char* kGoldenE2 = "[\"*/*\"]";

static void test_render_golden() {
  TEST_ASSERT_EQUAL_STRING(kGoldenE1, renderHoldHeads(Line::E1).c_str());
  TEST_ASSERT_EQUAL_STRING(kGoldenE2, renderHoldHeads(Line::E2).c_str());
}

// Every host the device can answer with has rows, the host's "*" row is its last
// (so no row is shadowed), and one wire per host on the device.
static void test_rows_are_total_and_ordered() {
  const char* hosts[] = {"anthropic", "openai", "mistral", "zai", "cumulo", "custom"};
  size_t n = 0;
  const Row* r = rows(n);
  for (const char* h : hosts) {
    int count = 0, starAt = -1, last = -1;
    for (size_t i = 0; i < n; i++) {
      if (strcmp(r[i].host, h) != 0) continue;
      count++;
      last = (int)i;
      if (!strcmp(r[i].family, "*")) starAt = (int)i;
      TEST_ASSERT_EQUAL_MESSAGE((int)wireForHost(h), (int)r[i].wire, h);
    }
    TEST_ASSERT_TRUE_MESSAGE(count > 0, h);
    TEST_ASSERT_EQUAL_INT_MESSAGE(last, starAt, h);
  }
  std::set<std::string> known(std::begin(hosts), std::end(hosts));
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(known.count(r[i].host), r[i].host);
    TEST_ASSERT_TRUE(r[i].wire != Wire::Any);
    const char* star = strchr(r[i].family, '*');
    TEST_ASSERT_TRUE_MESSAGE(!star || star[1] == '\0', r[i].family);   // '*' only last
    // A row is never shadowed by an earlier row of its host.
    for (size_t j = 0; j < i; j++) {
      if (strcmp(r[j].host, r[i].host) != 0) continue;
      std::string probe = r[i].family;
      if (probe.back() == '*') probe.pop_back();
      TEST_ASSERT_FALSE_MESSAGE(patternMatch(r[j].family, probe.empty() ? "x" : probe.c_str()),
                                r[i].family);
    }
  }
}

// Every provider's default head model is named by a row of its own (never classed by
// the host's catch-all by accident), unless the whole host is one row.
static void test_default_heads_have_rows() {
  for (size_t i = 0; i < nimbus::orch::kRoleDefaultCount; i++) {
    const auto& d = nimbus::orch::kRoleDefaults[i];
    if (d.role != nimbus::orch::DefaultRole::Head) continue;
    size_t n = 0;
    const Row* r = rows(n);
    int hostRows = 0;
    for (size_t k = 0; k < n; k++) hostRows += !strcmp(r[k].host, d.slot);
    const Verdict v = lookup(d.slot, d.fallback, Line::E1);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(v.row->host, "*") != 0, d.fallback);
    TEST_ASSERT_TRUE_MESSAGE(hostRows == 1 || strcmp(v.row->family, "*") != 0, d.fallback);
  }
}

// PRD section 9: Sonnet and Haiku qualified on e1, Mistral and custom held for every
// model on every line, nothing qualified on e2 before its parity run.
static void test_initial_states() {
  TEST_ASSERT_EQUAL((int)State::Qualified, (int)lookup("anthropic", "claude-sonnet-5-5", Line::E1).state);
  TEST_ASSERT_EQUAL((int)State::Qualified, (int)lookup("anthropic", "claude-haiku-5-5", Line::E1).state);
  TEST_ASSERT_EQUAL((int)State::Assumed, (int)lookup("openai", "gpt-5.6", Line::E1).state);
  TEST_ASSERT_EQUAL((int)State::Assumed, (int)lookup("openai", "gpt-5.6-luna", Line::E1).state);
  TEST_ASSERT_EQUAL((int)State::Assumed, (int)lookup("zai", "glm-5.3", Line::E1).state);
  TEST_ASSERT_EQUAL((int)State::Assumed, (int)lookup("cumulo", "gpt-5.6", Line::E1).state);
  for (Line l : {Line::E1, Line::E2}) {
    for (const char* m : {"mistral-large-4-0", "mistral-medium-latest", "mistral-small-latest"})
      TEST_ASSERT_EQUAL((int)State::Held, (int)lookup("mistral", m, l).state);
    TEST_ASSERT_EQUAL((int)State::Held, (int)lookup("custom", "llama", l).state);
  }
  size_t n = 0;
  const Row* r = rows(n);
  for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE_MESSAGE(holds(r[i].e2), r[i].family);
  // Ruling 9: Anthropic direct heads start in enforce; luna, glm, routed, Mistral and
  // custom are receipt-only.
  TEST_ASSERT_EQUAL((int)Truth::Enforce, (int)lookup("anthropic", "claude-sonnet-5-5", Line::E1).truth);
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)lookup("openai", "gpt-5.6-luna", Line::E1).truth);
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)lookup("zai", "glm-5.3", Line::E1).truth);
  TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)lookup("mistral", "mistral-large-4-0", Line::E1).truth);
}

// Fail closed: an unknown host, an empty model, a wire the row does not speak all get
// the unqualified row on both lines.
static void test_unknown_heads_fail_closed() {
  struct Q { const char* host; const char* model; Wire wire; } qs[] = {
      {"google", "gemini-9", Wire::Any}, {"", "claude-sonnet-5-5", Wire::Any},
      {nullptr, nullptr, Wire::Any},     {"anthropic", "", Wire::Any},
      {"anthropic", "claude-sonnet-5-5", Wire::Router},
  };
  for (const Q& q : qs)
    for (Line l : {Line::E1, Line::E2}) {
      const Verdict v = lookup(q.host, q.model, l, q.wire);
      TEST_ASSERT_EQUAL((int)State::Held, (int)v.state);
      TEST_ASSERT_EQUAL((int)Truth::ReceiptOnly, (int)v.truth);
      TEST_ASSERT_EQUAL_STRING("*", v.row->host);
    }
}

// Ruling 83 items 5 and 6: qualification decides which engine answers and the truth mode, never the
// size of the window. Neither a row nor a verdict carries a body or a profile: an unqualified head
// keeps its released budget on engine 1 and gets its full budget on engine 2 (tx::budgetFor).
template <typename T, typename = void>
struct HasBodyCap : std::false_type {};
template <typename T>
struct HasBodyCap<T, std::void_t<decltype(&T::bodyCap)>> : std::true_type {};
template <typename T, typename = void>
struct HasProfileCap : std::false_type {};
template <typename T>
struct HasProfileCap<T, std::void_t<decltype(&T::profileCap)>> : std::true_type {};
struct Sized {
  uint32_t bodyCap;
  int profileCap;
};

static void test_qualification_never_sizes_the_window() {
  TEST_ASSERT_TRUE(HasBodyCap<Sized>::value && HasProfileCap<Sized>::value);   // the detectors see a cap
  TEST_ASSERT_FALSE(HasBodyCap<Row>::value);
  TEST_ASSERT_FALSE(HasProfileCap<Row>::value);
  TEST_ASSERT_FALSE(HasBodyCap<Verdict>::value);
  TEST_ASSERT_FALSE(HasProfileCap<Verdict>::value);
  size_t n = 0;
  const Row* r = rows(n);
  for (size_t i = 0; i < n; i++) {
    std::string model = r[i].family;
    if (model.back() == '*') model.back() = 'x';
    for (Line l : {Line::E1, Line::E2}) TEST_ASSERT_TRUE_MESSAGE(lookup(r[i].host, model.c_str(), l).row == &r[i], model.c_str());
  }
}

static void test_slugs() {
  Line l;
  TEST_ASSERT_TRUE(lineFromSlug("e1", l));
  TEST_ASSERT_EQUAL((int)Line::E1, (int)l);
  TEST_ASSERT_TRUE(lineFromSlug("e2", l));
  TEST_ASSERT_EQUAL((int)Line::E2, (int)l);
  for (const char* bad : {"", "E1", "e3", "v5.2"}) TEST_ASSERT_FALSE(lineFromSlug(bad, l));
  TEST_ASSERT_FALSE(lineFromSlug(nullptr, l));
  TEST_ASSERT_EQUAL_STRING("e1", lineSlug(Line::E1));
  TEST_ASSERT_EQUAL_STRING("held", stateSlug(State::Held));
  TEST_ASSERT_EQUAL_STRING("assumed", stateSlug(State::Assumed));
  TEST_ASSERT_EQUAL_STRING("qualified", stateSlug(State::Qualified));
  TEST_ASSERT_EQUAL_STRING("router", wireSlug(wireForHost("cumulo")));
  TEST_ASSERT_EQUAL_STRING("any", wireSlug(wireForHost("google")));
  TEST_ASSERT_EQUAL_STRING("enforce", truthSlug(Truth::Enforce));
}

static void test_pattern_match() {
  TEST_ASSERT_TRUE(patternMatch("*", "anything"));
  TEST_ASSERT_TRUE(patternMatch("gpt-5.6*", "gpt-5.6"));
  TEST_ASSERT_TRUE(patternMatch("gpt-5.6*", "gpt-5.6-luna"));
  TEST_ASSERT_FALSE(patternMatch("gpt-5.6*", "gpt-5.5"));
  TEST_ASSERT_TRUE(patternMatch("claude-sonnet-5", "claude-sonnet-5"));
  TEST_ASSERT_FALSE(patternMatch("claude-sonnet-5", "claude-sonnet-5-5"));
  TEST_ASSERT_FALSE(patternMatch("claude-sonnet-5-5", "claude-sonnet-5"));
  TEST_ASSERT_FALSE(patternMatch("*", ""));
  TEST_ASSERT_FALSE(patternMatch("", "x"));
  TEST_ASSERT_FALSE(patternMatch(nullptr, "x"));
  TEST_ASSERT_FALSE(patternMatch("x", nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_golden);
  RUN_TEST(test_rows_are_total_and_ordered);
  RUN_TEST(test_default_heads_have_rows);
  RUN_TEST(test_initial_states);
  RUN_TEST(test_unknown_heads_fail_closed);
  RUN_TEST(test_qualification_never_sizes_the_window);
  RUN_TEST(test_slugs);
  RUN_TEST(test_pattern_match);
  return UNITY_END();
}
