#include <unity.h>

#include <string>

#include "nimbus/cloud/device_key.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// --- capacity validation --------------------------------------------------------
static void test_valid_capacity() {
  TEST_ASSERT_FALSE(validMintCapacity(0));
  TEST_ASSERT_FALSE(validMintCapacity(-1));
  TEST_ASSERT_FALSE(validMintCapacity(-1000));
  TEST_ASSERT_TRUE(validMintCapacity(1));
  TEST_ASSERT_TRUE(validMintCapacity(1000));
  TEST_ASSERT_TRUE(validMintCapacity(1000000));
}

// --- the mint gate: ONLY an explicit user Save mints ----------------------------
// Property test over EVERY trigger (AGENTS.md: test the class, not the instance). A
// new trigger added to the enum with no guard makes this FAIL - which is the point.
static void test_only_user_save_is_allowed() {
  for (MintTrigger t : kAllMintTriggers) {
    const bool isUser = (t == MintTrigger::UserSave);
    // With a valid capacity, only UserSave may mint.
    TEST_ASSERT_EQUAL(isUser, mintAllowed(t, 1000));
    // With an invalid capacity, nothing mints - not even UserSave.
    TEST_ASSERT_FALSE(mintAllowed(t, 0));
  }
}

// The invariant the owner ruling turns on: the pairing-success path must NOT mint.
// (Boot and key-empty are the other two auto-paths that must stay dead.)
static void test_pairing_and_auto_paths_do_not_mint() {
  TEST_ASSERT_FALSE(mintAllowed(MintTrigger::Pairing, 1000));
  TEST_ASSERT_FALSE(mintAllowed(MintTrigger::Boot, 1000));
  TEST_ASSERT_FALSE(mintAllowed(MintTrigger::KeyEmpty, 1000));
}

// --- request body ---------------------------------------------------------------
static void test_build_request_valid() {
  std::string body = buildMintRequest("dev_123", "cred_abc", 500);
  TEST_ASSERT_TRUE(body.find("\"deviceId\":\"dev_123\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"credential\":\"cred_abc\"") != std::string::npos);
  TEST_ASSERT_TRUE(body.find("\"capacity\":500") != std::string::npos);
}

static void test_build_request_rejects_bad_capacity() {
  TEST_ASSERT_TRUE(buildMintRequest("dev", "cred", 0).empty());
  TEST_ASSERT_TRUE(buildMintRequest("dev", "cred", -5).empty());
}

// --- response parsing: success --------------------------------------------------
static void test_success_yields_key_to_persist() {
  MintResult r = parseMintResponse(
      200, "{\"key\":\"cumulo_sk_live_xyz\",\"capacity\":750,\"label\":\"Desk Nimbus\"}");
  TEST_ASSERT_TRUE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::Ok, r.status);
  TEST_ASSERT_EQUAL_STRING("cumulo_sk_live_xyz", r.key.c_str());
  TEST_ASSERT_EQUAL_INT(750, r.capacity);
  TEST_ASSERT_EQUAL_STRING("Desk Nimbus", r.label.c_str());
  TEST_ASSERT_TRUE(r.message.find("750") != std::string::npos);
}

static void test_success_without_key_is_not_a_mint() {
  MintResult r = parseMintResponse(200, "{\"capacity\":10}");
  TEST_ASSERT_FALSE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::Unknown, r.status);
}

// --- response parsing: errors map to honest messages ----------------------------
static void test_capacity_required() {
  MintResult r = parseMintResponse(400, "{\"error\":\"capacity_required\"}");
  TEST_ASSERT_FALSE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::CapacityRequired, r.status);
  TEST_ASSERT_EQUAL_STRING("Enter a capacity of at least 1 credit.", r.message.c_str());
}

static void test_capacity_exceeds_max_carries_the_max() {
  MintResult r =
      parseMintResponse(400, "{\"error\":\"capacity_exceeds_account_max\",\"max\":2500}");
  TEST_ASSERT_FALSE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::CapacityExceedsMax, r.status);
  TEST_ASSERT_EQUAL_INT(2500, r.max);
  TEST_ASSERT_EQUAL_STRING("Capacity is above your account maximum of 2500.",
                           r.message.c_str());
}

static void test_terms_required() {
  MintResult r = parseMintResponse(403, "{\"error\":\"terms_acceptance_required\"}");
  TEST_ASSERT_EQUAL(MintStatus::TermsRequired, r.status);
  TEST_ASSERT_EQUAL_STRING("Open the Nimbus app and accept the terms.", r.message.c_str());
}

static void test_invalid_credential() {
  MintResult r = parseMintResponse(401, "{\"error\":\"invalid_credential\"}");
  TEST_ASSERT_EQUAL(MintStatus::InvalidCredential, r.status);
  TEST_ASSERT_TRUE(r.message.length() > 0);
}

static void test_rate_limited() {
  MintResult r = parseMintResponse(429, "{\"error\":\"rate_limited\"}");
  TEST_ASSERT_EQUAL(MintStatus::RateLimited, r.status);
  TEST_ASSERT_TRUE(r.message.length() > 0);
}

static void test_network_error_when_no_response() {
  MintResult r = parseMintResponse(0, "");
  TEST_ASSERT_EQUAL(MintStatus::NetworkError, r.status);
  TEST_ASSERT_TRUE(r.message.length() > 0);
  MintResult r2 = parseMintResponse(-1, "");
  TEST_ASSERT_EQUAL(MintStatus::NetworkError, r2.status);
}

static void test_unknown_status() {
  MintResult r = parseMintResponse(500, "{\"error\":\"server_error\"}");
  TEST_ASSERT_EQUAL(MintStatus::Unknown, r.status);
  TEST_ASSERT_FALSE(r.ok());
}

// The cloud's other pre-mint refusals (contract addendum 2026-09-14) map to honest,
// specific messages, never to the capacity message or the generic failure.
static void test_key_limit_reached_carries_the_limit() {
  MintResult r = parseMintResponse(400, "{\"error\":\"key_limit_reached\",\"limit\":20}");
  TEST_ASSERT_FALSE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::KeyLimitReached, r.status);
  TEST_ASSERT_EQUAL_INT(20, r.limit);
  TEST_ASSERT_EQUAL_STRING(
      "Your account already has 20 keys. Revoke one in the portal first.",
      r.message.c_str());
}

static void test_unpaired_404() {
  MintResult r = parseMintResponse(404, "{\"error\":\"unpaired\"}");
  TEST_ASSERT_FALSE(r.ok());
  TEST_ASSERT_EQUAL(MintStatus::Unpaired, r.status);
  TEST_ASSERT_EQUAL_STRING("This device is not paired with the cloud. Pair it first.",
                           r.message.c_str());
}

static void test_bad_request_codes_never_blame_the_capacity() {
  MintResult a = parseMintResponse(400, "{\"error\":\"missing_fields\"}");
  TEST_ASSERT_EQUAL(MintStatus::BadRequest, a.status);
  TEST_ASSERT_TRUE(a.message.find("capacity") == std::string::npos);
  MintResult b = parseMintResponse(400, "{\"error\":\"invalid_json\"}");
  TEST_ASSERT_EQUAL(MintStatus::BadRequest, b.status);
  TEST_ASSERT_TRUE(b.message.find("capacity") == std::string::npos);
}

// No message is ever an em dash or a " - " (project copy rule), across every path.
static void test_messages_obey_copy_rules() {
  const std::string bodies[] = {
      "{\"key\":\"cumulo_sk_x\",\"capacity\":5,\"label\":\"L\"}",
      "{\"error\":\"capacity_required\"}",
      "{\"error\":\"capacity_exceeds_account_max\",\"max\":9}",
      "{\"error\":\"terms_acceptance_required\"}",
      "{\"error\":\"x\"}",
      "{\"error\":\"key_limit_reached\",\"limit\":3}",
      "{\"error\":\"unpaired\"}",
      "{\"error\":\"missing_fields\"}"};
  const int codes[] = {200, 400, 400, 403, 401, 400, 404, 400};
  for (int i = 0; i < 8; ++i) {
    std::string m = parseMintResponse(codes[i], bodies[i]).message;
    TEST_ASSERT_TRUE(m.find(" - ") == std::string::npos);
    TEST_ASSERT_TRUE(m.find("\xe2\x80\x94") == std::string::npos);  // U+2014 em dash
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_valid_capacity);
  RUN_TEST(test_only_user_save_is_allowed);
  RUN_TEST(test_pairing_and_auto_paths_do_not_mint);
  RUN_TEST(test_build_request_valid);
  RUN_TEST(test_build_request_rejects_bad_capacity);
  RUN_TEST(test_success_yields_key_to_persist);
  RUN_TEST(test_success_without_key_is_not_a_mint);
  RUN_TEST(test_capacity_required);
  RUN_TEST(test_capacity_exceeds_max_carries_the_max);
  RUN_TEST(test_terms_required);
  RUN_TEST(test_invalid_credential);
  RUN_TEST(test_rate_limited);
  RUN_TEST(test_network_error_when_no_response);
  RUN_TEST(test_unknown_status);
  RUN_TEST(test_key_limit_reached_carries_the_limit);
  RUN_TEST(test_unpaired_404);
  RUN_TEST(test_bad_request_codes_never_blame_the_capacity);
  RUN_TEST(test_messages_obey_copy_rules);
  return UNITY_END();
}
