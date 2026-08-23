#include <unity.h>

#include <string>

#include "nimbus/cloud/relay_codec.h"        // b64Encode (to build test JWTs)
#include "nimbus/cloud/relay_credential.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// --- helpers: build a JWT whose payload carries the claims under test -----------
// Only segment[1] (the payload) is read by the parser, so the header/signature can be
// any placeholder. We base64url-encode a JSON payload (standard base64 then translate).
static std::string b64url(const std::string& s) {
  std::string std;
  b64Encode((const uint8_t*)s.data(), s.size(), std);
  std::string out;
  for (char c : std) {
    if (c == '+') out += '-';
    else if (c == '/') out += '_';
    else if (c == '=') { /* strip padding */ }
    else out += c;
  }
  return out;
}
static std::string jwt(const std::string& payloadJson) {
  return "hdr." + b64url(payloadJson) + ".sig";
}

// --- claim parsing --------------------------------------------------------------
static void test_parses_exp_and_iat() {
  std::string t = jwt("{\"sub\":\"dev1\",\"tv\":1,\"iat\":1000000,\"exp\":3592000}");
  TEST_ASSERT_EQUAL_UINT64(3592000ULL, credentialExp(t));
  TEST_ASSERT_EQUAL_UINT64(1000000ULL, credentialIat(t));
}

static void test_legacy_no_exp_is_zero() {
  std::string t = jwt("{\"sub\":\"dev1\",\"tv\":1,\"iat\":1000000}");
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp(t));   // legacy: non-expiring
  TEST_ASSERT_EQUAL_UINT64(1000000ULL, credentialIat(t));
}

static void test_malformed_tokens_are_zero() {
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp(""));
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp("not-a-jwt"));
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp("only.two"));       // no 3rd segment
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp("hdr..sig"));       // empty payload
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp("hdr.%%%%.sig"));   // bad base64
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp(jwt("{\"exp\":\"soon\"}")));  // non-numeric
  TEST_ASSERT_EQUAL_UINT64(0ULL, credentialExp(jwt("not json")));
}

// --- jittered proactive re-mint target ------------------------------------------
static void test_remint_target_within_jitter_window() {
  const uint64_t iat = 1000000, life = 2592000, exp = iat + life;  // 30d
  const uint64_t lo = iat + life * 50 / 100;   // 50%
  const uint64_t hi = iat + life * 80 / 100;   // 80%
  // Every seed must land the target inside [50%, 80%] of the lifetime.
  for (uint32_t seed = 0; seed < 5000; seed += 37) {
    uint64_t tgt = remintTarget(iat, exp, seed);
    TEST_ASSERT_TRUE(tgt >= lo);
    TEST_ASSERT_TRUE(tgt <= hi);
  }
}

static void test_remint_target_is_stable_and_varies() {
  const uint64_t iat = 1000000, exp = iat + 2592000;
  TEST_ASSERT_EQUAL_UINT64(remintTarget(iat, exp, 1234), remintTarget(iat, exp, 1234));  // stable
  // Two different seeds should (usually) differ - assert the spread is nonzero.
  TEST_ASSERT_TRUE(remintTarget(iat, exp, 0) != remintTarget(iat, exp, 3000));
}

static void test_legacy_has_no_proactive_schedule() {
  TEST_ASSERT_EQUAL_UINT64(0ULL, remintTarget(1000000, 0, 42));       // no exp
  TEST_ASSERT_EQUAL_UINT64(0ULL, remintTarget(0, 3592000, 42));       // no iat
  TEST_ASSERT_FALSE(remintDueProactive(1000000, 0, 9999999, 42));     // never due
}

static void test_remint_due_proactive_window() {
  const uint64_t iat = 1000000, exp = iat + 2592000;
  const uint32_t seed = 12345;
  const uint64_t tgt = remintTarget(iat, exp, seed);
  TEST_ASSERT_FALSE(remintDueProactive(iat, exp, tgt - 1, seed));   // before target
  TEST_ASSERT_TRUE (remintDueProactive(iat, exp, tgt,     seed));   // at target
  TEST_ASSERT_TRUE (remintDueProactive(iat, exp, exp - 1, seed));   // still before expiry
  TEST_ASSERT_FALSE(remintDueProactive(iat, exp, exp,     seed));   // expired -> not "proactive"
  TEST_ASSERT_FALSE(remintDueProactive(iat, exp, exp + 10, seed));
}

// --- grace window ---------------------------------------------------------------
static void test_expired_within_grace() {
  const uint64_t exp = 2000000, grace = kRemintGraceSec;
  TEST_ASSERT_FALSE(expiredWithinGrace(exp, exp - 1, grace));       // not expired yet
  TEST_ASSERT_TRUE (expiredWithinGrace(exp, exp, grace));           // just expired
  TEST_ASSERT_TRUE (expiredWithinGrace(exp, exp + grace - 1, grace));
  TEST_ASSERT_FALSE(expiredWithinGrace(exp, exp + grace, grace));   // grace edge -> past
  TEST_ASSERT_FALSE(expiredWithinGrace(0, 9999999, grace));         // legacy: never
}

static void test_expired_past_grace() {
  const uint64_t exp = 2000000, grace = kRemintGraceSec;
  TEST_ASSERT_FALSE(expiredPastGrace(exp, exp, grace));
  TEST_ASSERT_FALSE(expiredPastGrace(exp, exp + grace - 1, grace));
  TEST_ASSERT_TRUE (expiredPastGrace(exp, exp + grace, grace));     // must re-pair now
  TEST_ASSERT_FALSE(expiredPastGrace(0, 9999999, grace));           // legacy: never
}

// The three states are mutually exclusive and cover the timeline (no gaps/overlaps).
static void test_states_partition_the_timeline() {
  const uint64_t iat = 1000000, exp = iat + 2592000, grace = kRemintGraceSec;
  const uint32_t seed = 777;
  for (uint64_t now = iat; now < exp + grace + 100; now += 100000) {
    int flags = (int)remintDueProactive(iat, exp, now, seed)
              + (int)expiredWithinGrace(exp, now, grace)
              + (int)expiredPastGrace(exp, now, grace);
    TEST_ASSERT_TRUE(flags <= 1);   // never two states at once
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parses_exp_and_iat);
  RUN_TEST(test_legacy_no_exp_is_zero);
  RUN_TEST(test_malformed_tokens_are_zero);
  RUN_TEST(test_remint_target_within_jitter_window);
  RUN_TEST(test_remint_target_is_stable_and_varies);
  RUN_TEST(test_legacy_has_no_proactive_schedule);
  RUN_TEST(test_remint_due_proactive_window);
  RUN_TEST(test_expired_within_grace);
  RUN_TEST(test_expired_past_grace);
  RUN_TEST(test_states_partition_the_timeline);
  return UNITY_END();
}
