#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/failure_chain.h"

// test_orch_failure_chain - the class rule: every failed turn keeps its FIRST cause
// and its last host, bounded and safe to store, whatever order the hosts failed in,
// whatever the errors said, and however many attempts ran.
//
// The 18 Oct dream failed on Anthropic (an invalid-unicode 400), then on OpenAI and
// Mistral; only Mistral's bare "chat HTTP 400" survived. The property below walks
// every host order x 9 error kinds x 1-6 attempts (and adversarial 2 KB invalid
// UTF-8 inputs), and the counter-test proves a keep-the-latest-text chain fails it.

using nimbus::orch::FailStep;
using nimbus::orch::FailureChain;
using nimbus::orch::sanitizeFailText;
using nimbus::orch::shortFailCause;

void setUp() {}
void tearDown() {}

// ---- an independent strict UTF-8 check (not the implementation's) -------------
static bool validUtf8NoControls(const std::string& s) {
  const unsigned char* u = (const unsigned char*)s.data();
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    unsigned char c = u[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((u[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (u[i + k] & 0x3F);
    }
    static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return false;
    i += len;
  }
  return true;
}

// ---- deterministic generator ----------------------------------------------------
static uint32_t g_seed = 0x5eed1234u;
static uint32_t rnd() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

static const char* kHosts[3] = {"anthropic", "openai", "mistral"};
// Nine error kinds, as the adapters and the transport word them.
static const char* kKinds[9] = {
    "messages: network error (connection refused, short body write 0/71508)",
    "messages HTTP 429: rate limited [rl:requests-minute]",
    "messages HTTP 400: The request body is not valid JSON: invalid unicode code point at "
    "line 1 column 55142",
    "resp HTTP 400: The model 'gpt-9' does not exist or you do not have access to it.",
    "chat HTTP 404: Not Found",
    "messages HTTP 529: Overloaded",
    "timeout: the provider did not answer within 120 s",
    "low memory: the device could not send the request",
    "schema parse",
};

// 2 KB of hostile bytes: invalid lead bytes, cut sequences, surrogates, overlongs,
// controls, quotes and real multi-byte characters.
static std::string hostileText() {
  static const char* kBits[] = {"\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\xED\xA0\x80",
                                "\xC0\xAF", "\xE0\x80\xAF", "\xF4\x90\x80\x80", "\x80",
                                "\xBF", "\xFE", "\xFF", "\xE2\x82", "\n", "\r\t", "\x7F",
                                "\xC2\x85", "\"", "\\", "ok ", "HTTP 400: "};
  std::string s;
  while (s.size() < 2048) s += kBits[rnd() % (sizeof(kBits) / sizeof(kBits[0]))];
  return s;
}

struct Attempt {
  std::string host, err;
};

// One generated failure sequence: hosts are visited in the order given (retries are
// consecutive, as in the fabric and the legacy ladder), 1-6 attempts in total.
static std::vector<Attempt> genSequence(const int order[3]) {
  std::vector<Attempt> v;
  const int total = 1 + (int)(rnd() % 6);
  int hostIdx = 0;
  for (int a = 0; a < total; ++a) {
    if (a > 0 && hostIdx < 2 && rnd() % 2) hostIdx++;
    const bool hostile = rnd() % 7 == 0;
    v.push_back({kHosts[order[hostIdx]], hostile ? hostileText() : kKinds[rnd() % 9]});
  }
  return v;
}

// The class property over one sequence, for any chain type with FailureChain's API.
template <typename Chain>
static bool propertyHolds(const std::vector<Attempt>& seq, std::string* why) {
  Chain ch;
  for (const Attempt& a : seq) ch.note(a.host, a.err);
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  if (ch.size() < 1 || ch.size() > FailureChain::kMaxSteps) return fail("size out of range");
  const std::string firstText = sanitizeFailText(seq[0].err.data(), seq[0].err.size(),
                                                 FailureChain::kTextMax);
  if (ch.first()->host != seq[0].host) return fail("first host wrong");
  if (ch.first()->text != firstText) return fail("first text is not the first error");
  if (ch.last()->host != seq.back().host) return fail("last host wrong");
  size_t runs = 1, attempts = 0;
  for (size_t i = 1; i < seq.size(); ++i)
    if (seq[i].host != seq[i - 1].host) runs++;
  for (size_t i = 0; i < ch.size(); ++i) {
    const FailStep& st = ch.at(i);
    if (st.text.size() > FailureChain::kTextMax) return fail("text over 160 B");
    if (!validUtf8NoControls(st.text)) return fail("text not valid UTF-8 / has controls");
    attempts += st.attempts;
  }
  if (runs <= FailureChain::kMaxSteps && attempts != seq.size()) return fail("attempts lost");
  for (size_t cap = 32; cap <= 512; cap += 16) {
    const std::string r = ch.render(cap);
    if (r.size() > cap) return fail("render over cap");
    if (!validUtf8NoControls(r)) return fail("render not valid UTF-8");
    if (r.compare(0, seq[0].host.size(), seq[0].host) != 0) return fail("render lost the first host");
  }
  return true;
}

static const int kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

static void test_property_every_order_kind_and_attempt_count() {
  g_seed = 0x5eed1234u;
  int cases = 0;
  for (int rep = 0; rep < 600; ++rep)
    for (const auto& order : kOrders) {
      const std::vector<Attempt> seq = genSequence(order);
      std::string why;
      if (!propertyHolds<FailureChain>(seq, &why)) {
        std::string msg = why + " (case " + std::to_string(cases) + ")";
        TEST_FAIL_MESSAGE(msg.c_str());
      }
      ++cases;
    }
  TEST_ASSERT_EQUAL_INT(3600, cases);
}

// Every single (order, kind, attempts) cell, exhaustively, one host per run.
static void test_exhaustive_kind_by_attempts_grid() {
  for (const auto& order : kOrders)
    for (int k = 0; k < 9; ++k)
      for (int attempts = 1; attempts <= 6; ++attempts) {
        std::vector<Attempt> seq;
        for (int a = 0; a < attempts; ++a)   // spread over the hosts in order
          seq.push_back({kHosts[order[(a * 3) / attempts]], kKinds[(k + a) % 9]});
        std::string why;
        TEST_ASSERT_TRUE_MESSAGE(propertyHolds<FailureChain>(seq, &why), why.c_str());
      }
}

// Counter-test: a chain that keeps the LATEST text of a run (what "remember the last
// error" would do) must fail the property - so the property really pins "first".
struct KeepLatestChain : FailureChain {
  void note(const std::string& host, const std::string& err) {
    FailureChain::note(host, err);
    latest_[size() - 1] = sanitizeFailText(err.data(), err.size(), kTextMax);
  }
  const FailStep* first() const {
    view_ = *FailureChain::first();
    view_.text = latest_[0];
    return &view_;
  }
  std::string latest_[kMaxSteps];
  mutable FailStep view_;
};

static void test_counter_keep_latest_fails() {
  g_seed = 0x5eed1234u;
  int failed = 0;
  for (int rep = 0; rep < 200; ++rep)
    for (const auto& order : kOrders)
      if (!propertyHolds<KeepLatestChain>(genSequence(order), nullptr)) failed++;
  TEST_ASSERT_TRUE_MESSAGE(failed > 0, "a keep-latest chain passed the property");
}

// The 18 Oct dream, verbatim shape: first cause kept, every host in order, the last
// host last, and the render reads the root cause first.
static void test_dream_shape_reads_root_cause_first() {
  FailureChain ch;
  const std::string ant =
      "messages HTTP 400: The request body is not valid JSON: invalid unicode code point at "
      "line 1 column 55142";
  ch.note("anthropic", ant);
  ch.note("anthropic", ant);
  ch.note("openai", "resp HTTP 400: Invalid body: encountered a unicode decode error");
  ch.note("openai", "resp HTTP 400: Invalid body: encountered a unicode decode error");
  ch.note("mistral", "chat HTTP 400");
  ch.note("mistral", "chat HTTP 400");
  TEST_ASSERT_EQUAL_UINT32(3, ch.size());
  TEST_ASSERT_EQUAL_STRING("anthropic -> openai -> mistral", ch.path().c_str());
  TEST_ASSERT_EQUAL_UINT8(2, ch.first()->attempts);
  const std::string r = ch.render(160);
  TEST_ASSERT_EQUAL_STRING(
      "anthropic (x2): messages HTTP 400: The request body is not valid JSON: invalid unicode "
      "code point at line 1 column 55142 > openai 400 (x2) > mistral 400 (x2)",
      r.c_str());
  // Tight: the first cause shrinks before any later host is dropped.
  const std::string tight = ch.render(96);
  TEST_ASSERT_TRUE(tight.size() <= 96);
  TEST_ASSERT_NOT_NULL(std::strstr(tight.c_str(), "anthropic (x2): messages HTTP 400"));
  TEST_ASSERT_NOT_NULL(std::strstr(tight.c_str(), "> mistral 400"));
}

static void test_full_chain_replaces_the_last_entry() {
  FailureChain ch;
  ch.note("anthropic", "a");
  ch.note("openai", "b");
  ch.note("mistral", "c");
  ch.noteDevice("parse");
  TEST_ASSERT_EQUAL_UINT32(3, ch.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", ch.first()->host.c_str());
  TEST_ASSERT_EQUAL_STRING("device", ch.last()->host.c_str());
  TEST_ASSERT_EQUAL_STRING("device parse", ch.brief(2).c_str());
}

static void test_retry_keeps_first_text_fills_empty() {
  FailureChain ch;
  ch.note("anthropic", "");
  ch.note("anthropic", "messages HTTP 500: boom");
  ch.note("anthropic", "messages HTTP 503: later");
  TEST_ASSERT_EQUAL_UINT32(1, ch.size());
  TEST_ASSERT_EQUAL_UINT8(3, ch.first()->attempts);
  TEST_ASSERT_EQUAL_STRING("messages HTTP 500: boom", ch.first()->text.c_str());
}

static void test_sanitize_and_short_cause() {
  static const char kMessy[] = "  a\n\n b \xFF c\t ";
  TEST_ASSERT_EQUAL_STRING("a b ? c", sanitizeFailText(kMessy, sizeof(kMessy) - 1, 160).c_str());
  // Cut on a character boundary, never inside "é".
  TEST_ASSERT_EQUAL_STRING("ab", sanitizeFailText("ab\xC3\xA9", 4, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("400", shortFailCause("chat HTTP 400").c_str());
  TEST_ASSERT_EQUAL_STRING("429", shortFailCause("messages HTTP 429: slow down").c_str());
  TEST_ASSERT_EQUAL_STRING("timeout", shortFailCause("timeout: no answer").c_str());
  TEST_ASSERT_EQUAL_STRING("low memory", shortFailCause("low memory: could not send").c_str());
  TEST_ASSERT_EQUAL_STRING("parse", shortFailCause("schema parse").c_str());
  TEST_ASSERT_EQUAL_STRING("loop failed", shortFailCause("loop failed").c_str());
  TEST_ASSERT_EQUAL_STRING("", shortFailCause("").c_str());
}

static void test_empty_chain_renders_nothing() {
  FailureChain ch;
  TEST_ASSERT_TRUE(ch.empty());
  TEST_ASSERT_EQUAL_STRING("", ch.render(160).c_str());
  TEST_ASSERT_NULL(ch.first());
  ch.note("anthropic", "x");
  ch.clear();
  TEST_ASSERT_TRUE(ch.empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_property_every_order_kind_and_attempt_count);
  RUN_TEST(test_exhaustive_kind_by_attempts_grid);
  RUN_TEST(test_counter_keep_latest_fails);
  RUN_TEST(test_dream_shape_reads_root_cause_first);
  RUN_TEST(test_full_chain_replaces_the_last_entry);
  RUN_TEST(test_retry_keeps_first_text_fills_empty);
  RUN_TEST(test_sanitize_and_short_cause);
  RUN_TEST(test_empty_chain_renders_nothing);
  return UNITY_END();
}
