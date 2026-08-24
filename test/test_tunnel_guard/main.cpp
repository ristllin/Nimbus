// test_tunnel_guard - the cloud-relay secret-containment policy: path canonicalization
// (so the denylist sees what the router dispatches on), the denylist itself (secret
// endpoints hard-refused), and the JSON response-body secret scrubber (a backstop that
// strips the durable token / AP password from any tunneled body). These are the
// deploy-blocking cases for the two relay findings (signin gap + percent-decode bypass).
#include <unity.h>

#include <string>

#include "nimbus/cloud/tunnel_guard.h"

using namespace nimbus::cloud::tunnel;

void setUp() {}
void tearDown() {}

// ---- canonicalization ------------------------------------------------------

static void test_canon_strips_query_and_fragment() {
  TEST_ASSERT_EQUAL_STRING("/api/connect", canonicalizePath("/api/connect?x=1&y=2").c_str());
  TEST_ASSERT_EQUAL_STRING("/api/state", canonicalizePath("/api/state#frag").c_str());
}

static void test_canon_percent_decodes() {
  // The exact F3 payload: %63 -> 'c', so /api/%63onnect canonicalizes to /api/connect.
  TEST_ASSERT_EQUAL_STRING("/api/connect", canonicalizePath("/api/%63onnect").c_str());
  // Mixed case hex and the token/regen variant from the finding.
  TEST_ASSERT_EQUAL_STRING("/api/token/regen", canonicalizePath("/api/%74oken/regen").c_str());
  TEST_ASSERT_EQUAL_STRING("/api/connect", canonicalizePath("/api/%63%6Fnnect").c_str());
}

static void test_canon_malformed_percent_left_literal() {
  TEST_ASSERT_EQUAL_STRING("/api/%zz", canonicalizePath("/api/%zz").c_str());
  TEST_ASSERT_EQUAL_STRING("/api/%6", canonicalizePath("/api/%6").c_str());
}

static void test_canon_strips_trailing_slash() {
  TEST_ASSERT_EQUAL_STRING("/api/connect", canonicalizePath("/api/connect/").c_str());
  TEST_ASSERT_EQUAL_STRING("/", canonicalizePath("/").c_str());
}

// ---- denylist (F2 + F3) ----------------------------------------------------

static void test_denies_connect_and_regen() {
  TEST_ASSERT_TRUE(isTunnelDenied("/api/connect"));
  TEST_ASSERT_TRUE(isTunnelDenied("/api/token/regen"));
}

static void test_denies_signin_paths() {
  // F2: the sign-in endpoints must be denied - they exchange for the durable token.
  TEST_ASSERT_TRUE(isTunnelDenied("/api/signin/code"));
  TEST_ASSERT_TRUE(isTunnelDenied("/api/signin/exchange"));
  // With a query (the exchange carries ?code=..).
  TEST_ASSERT_TRUE(isTunnelDenied("/api/signin/exchange?code=ABCD"));
}

static void test_denies_percent_encoded_bypass() {
  // F3: percent-encoded forms that decode to a denied path must be denied.
  TEST_ASSERT_TRUE(isTunnelDenied("/api/%63onnect"));
  TEST_ASSERT_TRUE(isTunnelDenied("/api/%63onnect?x=1"));
  TEST_ASSERT_TRUE(isTunnelDenied("/api/%74oken/regen"));
  TEST_ASSERT_TRUE(isTunnelDenied("/api/signin/%65xchange"));  // %65 -> 'e'
}

static void test_normal_paths_pass() {
  // The tunnel serves the full remote web UI, so ordinary endpoints must NOT be denied.
  TEST_ASSERT_FALSE(isTunnelDenied("/api/state"));
  TEST_ASSERT_FALSE(isTunnelDenied("/api/chat"));
  TEST_ASSERT_FALSE(isTunnelDenied("/api/telegram"));
  TEST_ASSERT_FALSE(isTunnelDenied("/"));
  TEST_ASSERT_FALSE(isTunnelDenied("/api/state?poll=1"));
  // A path that merely contains a denied substring but is a different endpoint passes.
  TEST_ASSERT_FALSE(isTunnelDenied("/api/connectors"));
}

// ---- response-body scrubber (backstop) -------------------------------------

static void test_scrub_strips_token_and_appass() {
  std::string body =
      "{\"name\":\"nimbus\",\"apPass\":\"hunter2secret\",\"token\":\"AAAABBBBCCCCDDDD\"}";
  bool changed = scrubJsonSecrets("application/json", body);
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_TRUE(body.find("hunter2secret") == std::string::npos);
  TEST_ASSERT_TRUE(body.find("AAAABBBBCCCCDDDD") == std::string::npos);
  // Shape preserved: empty values remain, other fields untouched.
  TEST_ASSERT_TRUE(body.find("\"token\":\"\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"apPass\":\"\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"name\":\"nimbus\"") != std::string::npos);
}

static void test_scrub_only_json_content_type() {
  std::string html = "<p>token:\"AAAABBBB\" apPass:\"secret\"</p>";
  std::string keep = html;
  bool changed = scrubJsonSecrets("text/html", html);
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_EQUAL_STRING(keep.c_str(), html.c_str());
}

static void test_scrub_leaves_pairing_code_and_other_fields() {
  // The relay-status body legitimately carries a cloud-pairing `code`; it must survive
  // (its name collides with the sign-in code, which is contained by the denylist instead).
  std::string body = "{\"state\":\"paired\",\"code\":\"ABCD1234\",\"online\":true}";
  std::string keep = body;
  bool changed = scrubJsonSecrets("application/json", body);
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_EQUAL_STRING(keep.c_str(), body.c_str());
}

static void test_scrub_ignores_value_that_equals_key() {
  // A value that merely equals "token" must not be mistaken for a token key/value.
  std::string body = "{\"label\":\"token\"}";
  std::string keep = body;
  bool changed = scrubJsonSecrets("application/json", body);
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_EQUAL_STRING(keep.c_str(), body.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_canon_strips_query_and_fragment);
  RUN_TEST(test_canon_percent_decodes);
  RUN_TEST(test_canon_malformed_percent_left_literal);
  RUN_TEST(test_canon_strips_trailing_slash);
  RUN_TEST(test_denies_connect_and_regen);
  RUN_TEST(test_denies_signin_paths);
  RUN_TEST(test_denies_percent_encoded_bypass);
  RUN_TEST(test_normal_paths_pass);
  RUN_TEST(test_scrub_strips_token_and_appass);
  RUN_TEST(test_scrub_only_json_content_type);
  RUN_TEST(test_scrub_leaves_pairing_code_and_other_fields);
  RUN_TEST(test_scrub_ignores_value_that_equals_key);
  return UNITY_END();
}
