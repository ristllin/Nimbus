#include <unity.h>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/key_shape.h"

// CUM-514: the device's POST /api/orch stored any provider key or bot token as
// posted, and every provider request is built by hand around it
// ("Authorization: Bearer " + key + "\r\n", Telegram's /bot<token>/ path), so a
// CR/LF in a posted key injected header lines into every request to that
// provider. The shape check refuses it, and every other control byte, a space
// inside it or an impossible length, before anything is stored.

using nimbus::orch::KeyShape;
using nimbus::orch::connectorTokenError;
using nimbus::orch::keyShapeError;
using nimbus::orch::keyShapeFor;

void setUp() {}
void tearDown() {}

// A realistic value for each field (shape only, never a real key).
static std::string sampleFor(const std::string& field) {
  if (field == "mistKey") return std::string(32, 'a');
  if (field == "tgToken") return "123456789:" + std::string(35, 'B');
  if (field == "custKey") return "local";
  if (field == "zaiKey") return std::string(32, 'c') + "." + std::string(16, 'd');
  if (field == "cumuloKey") return "cumulo_sk_" + std::string(40, 'e');
  if (field == "tavKey") return "tvly-" + std::string(32, 'f');
  if (field == "antKey") return "sk-ant-api03-" + std::string(95, 'g');
  return "sk-proj-" + std::string(48, 'h');
}

static const char* const kFields[] = {"oaiKey", "antKey", "mistKey", "zaiKey",
                                      "cumuloKey", "tavKey", "custKey", "tgToken"};

// The class: for EVERY credential field, a control byte anywhere (the header
// injection), a space inside, DEL or a non-ASCII byte is refused; a realistic
// value, and one with spaces around it, passes.
static void test_every_field_refuses_control_bytes_and_inner_spaces() {
  for (const char* f : kFields) {
    const KeyShape* ks = keyShapeFor(f);
    TEST_ASSERT_NOT_NULL_MESSAGE(ks, f);
    const std::string ok = sampleFor(f);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", keyShapeError(*ks, ok).c_str(), f);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", keyShapeError(*ks, "  " + ok + " ").c_str(), f);
    const std::string bad[] = {
        ok + "\r\nX-Injected: 1", ok + "\n", "\r" + ok, ok.substr(0, 3) + "\t" + ok.substr(3),
        ok + std::string(1, '\0') + "x", ok.substr(0, 4) + " " + ok.substr(4), ok + "\x7f",
        ok + "\xc3\xa9", ok + "\x1b[0m",
    };
    for (const std::string& b : bad) {
      const std::string err = keyShapeError(*ks, b);
      TEST_ASSERT_TRUE_MESSAGE(err.find("has a space or a hidden character in it") != std::string::npos, f);
      TEST_ASSERT_TRUE_MESSAGE(err.find(ks->noun) != std::string::npos, f);
    }
  }
  // An empty or all-space value is "leave the stored one", not an error.
  TEST_ASSERT_EQUAL_STRING("", keyShapeError(*keyShapeFor("mistKey"), "").c_str());
  TEST_ASSERT_EQUAL_STRING("", keyShapeError(*keyShapeFor("mistKey"), "   ").c_str());
}

static void test_length_outside_the_shape_is_refused() {
  const KeyShape& m = *keyShapeFor("mistKey");
  TEST_ASSERT_EQUAL_STRING("That Mistral key looks too short. Paste the whole key and save again.",
                           keyShapeError(m, "abc").c_str());
  TEST_ASSERT_EQUAL_STRING("That Mistral key looks too long. Paste just the key and save again.",
                           keyShapeError(m, std::string(m.maxLen + 1, 'a')).c_str());
  TEST_ASSERT_EQUAL_STRING("", keyShapeError(m, std::string(m.minLen, 'a')).c_str());
  TEST_ASSERT_EQUAL_STRING("", keyShapeError(m, std::string(m.maxLen, 'a')).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "That Telegram bot token has a space or a hidden character in it. Paste just the token "
      "and save again.",
      keyShapeError(*keyShapeFor("tgToken"), "123:abc\r\nHost: evil").c_str());
  // Every shape is sane: a real key fits between its bounds.
  for (const char* f : kFields) {
    const KeyShape* ks = keyShapeFor(f);
    TEST_ASSERT_TRUE_MESSAGE(ks->minLen >= 1 && ks->minLen <= sampleFor(f).size(), f);
    TEST_ASSERT_TRUE_MESSAGE(ks->maxLen >= sampleFor(f).size(), f);
  }
  TEST_ASSERT_NULL(keyShapeFor("sysPrompt"));   // not a credential
  TEST_ASSERT_NULL(keyShapeFor("clr_oaiKey"));  // a clear carries no value to check
}

// Test the class, not the instance: every credential field the /api/orch handler
// stores (an `if (n == "<x>Key")` / `"<x>Token"` branch in applyOrchField) has a
// shape, so a new key field cannot ship unchecked.
static void test_every_stored_credential_field_has_a_shape() {
  std::ifstream in("src/net/webui.cpp");
  TEST_ASSERT_TRUE_MESSAGE(in.good(), "run from the project root");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();
  const size_t from = src.find("static bool applyOrchField(");
  TEST_ASSERT_TRUE(from != std::string::npos);
  const size_t to = src.find("\n}\n", from);
  const std::string body = src.substr(from, to - from);
  const std::regex branch("n == \"([A-Za-z]+(Key|Token))\"");
  int seen = 0;
  for (std::sregex_iterator it(body.begin(), body.end(), branch), end; it != end; ++it) {
    const std::string field = (*it)[1];
    TEST_ASSERT_NOT_NULL_MESSAGE(keyShapeFor(field), field.c_str());
    seen++;
  }
  TEST_ASSERT_TRUE(seen >= 8);
}

// The connector token (POST /api/connectors "tok") rides the same header raw.
static void test_connector_token_refuses_control_bytes() {
  TEST_ASSERT_EQUAL_STRING("", connectorTokenError("github", "ghp_" + std::string(36, 'x')).c_str());
  TEST_ASSERT_EQUAL_STRING("", connectorTokenError("github", "").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "The token for github has a space or a hidden character in it. Paste just the token and "
      "save again.",
      connectorTokenError("github", "ghp_abc\r\nX-Injected: 1").c_str());
  TEST_ASSERT_FALSE(connectorTokenError("notion", "secret abc").empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_field_refuses_control_bytes_and_inner_spaces);
  RUN_TEST(test_length_outside_the_shape_is_refused);
  RUN_TEST(test_every_stored_credential_field_has_a_shape);
  RUN_TEST(test_connector_token_refuses_control_bytes);
  return UNITY_END();
}
