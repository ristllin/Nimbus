#include <ArduinoJson.h>
#include <unity.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/router_reasons_e2.h"
#include "nimbus/orch/wire_errors.h"

// test_wire_errors - the engine-2 error classifier (wire_errors.h, lane E2-R5; ROUTER-DESIGN.md
// section 9). Every row of the section 9 table has a fixture: Anthropic's unknown model from the
// recorded bodies, the rest from test/support/fixtures/*_wire_synthetic/errors/cases.json, each
// case naming its source. Every marker row of the facts table has a fixture that exercises it.

using namespace nimbus::orch;
using namespace nimbus::orch::caps;

void setUp() {}
void tearDown() {}

namespace {

constexpr const char* kFx = "test/support/fixtures/";

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), path.c_str());
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string lowered(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

const char* classSlug(const wire::Classified& c) {
  if (!c.error) return "none";
  switch (c.cls) {
    case ErrClass::Transport: return "transport";
    case ErrClass::RateLimited: return "rate_limited";
    case ErrClass::Server: return "server";
    case ErrClass::KeyAuth: return "key_auth";
    case ErrClass::KeyForbidden: return "key_forbidden";
    case ErrClass::Billing: return "billing";
    case ErrClass::Context: return "context";
    case ErrClass::RefusedField: return "refused_field";
    case ErrClass::UnknownModel: return "unknown_model";
    case ErrClass::BadRequest: return "bad_request";
    case ErrClass::Refusal: return "refusal";
  }
  return "?";
}

uint16_t refusedBit(const std::string& name) {
  if (name == "image") return refused::kImage;
  if (name == "effort") return refused::kEffort;
  if (name == "thinking") return refused::kThinking;
  if (name == "structured") return refused::kStructured;
  if (name == "choice_none") return refused::kChoiceNone;
  if (name == "cache_breakpoint") return refused::kCacheBreakpoint;
  if (name == "replay") return refused::kReplay;
  if (name == "file_ref") return refused::kFileRef;
  if (name == "cap_field") return refused::kCapField;
  TEST_FAIL_MESSAGE(("unknown refused field " + name).c_str());
  return 0;
}

struct Case {
  std::string folder, provider, row, expect, body, source;
  int status = 0;
  bool routed = false, fileRef = false;
  uint16_t refused = 0;
};

const char* const kFolders[] = {"anthropic_wire_synthetic", "openai_wire_synthetic", "mistral_wire_synthetic",
                                "zai_wire_synthetic", "router_wire_synthetic"};

void loadFolder(const char* folder, std::vector<Case>& out) {
  const std::string path = std::string(kFx) + folder + "/errors/cases.json";
  JsonDocument doc;
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, readFile(path)), path.c_str());
  const std::string provider = doc["provider"] | "";
  for (JsonObjectConst c : doc["cases"].as<JsonArrayConst>()) {
    Case k;
    k.folder = folder;
    k.provider = provider;
    k.row = c["row"] | "";
    k.expect = c["expect"] | "";
    k.status = c["status"] | 0;
    k.source = c["source"] | "";
    k.routed = c["routed"] | false;
    k.fileRef = c["file_ref"] | false;
    for (JsonVariantConst f : c["refused"].as<JsonArrayConst>()) k.refused |= refusedBit(f.as<std::string>());
    k.body = c["body"] | "";
    if (!k.body.empty() && k.body[0] == '@') k.body = readFile(std::string(kFx) + k.body.substr(1));
    TEST_ASSERT_FALSE_MESSAGE(k.source.empty(), ("a case with no source in " + path).c_str());
    out.push_back(k);
  }
}

// The recorded unknown-model bodies (all four providers, 18 Oct 2026) and Anthropic's.
void loadRecorded(std::vector<Case>& out) {
  JsonDocument doc;
  const std::string path = std::string(kFx) + "usage_2026-10/errors.json";
  TEST_ASSERT_FALSE(deserializeJson(doc, readFile(path)));
  for (JsonObjectConst e : doc["errors"].as<JsonArrayConst>()) {
    Case k;
    k.folder = "usage_2026-10";
    k.provider = e["provider"] | "";
    k.row = "unknown_model";
    k.expect = "unknown_model";
    k.status = e["status"] | 0;
    k.body = e["body"] | "";
    k.source = path;
    out.push_back(k);
  }
  Case a;
  a.folder = "anthropic_wire_2026-11";
  a.provider = "anthropic";
  a.row = a.expect = "unknown_model";
  a.status = 404;
  a.body = readFile(std::string(kFx) + "anthropic_wire_2026-11/error_not_found.json");
  a.source = "anthropic_wire_2026-11/error_not_found.json";
  out.push_back(a);
}

std::vector<Case> allCases() {
  std::vector<Case> v;
  for (const char* f : kFolders) loadFolder(f, v);
  loadRecorded(v);
  return v;
}

std::string label(const Case& c) { return c.folder + " " + c.row + " " + std::to_string(c.status) + ": " + c.body.substr(0, 90); }

}  // namespace

// Every provider case classifies to its expected class, with the refused fields and file-id
// mark it names.
static void test_every_fixture_classifies_as_recorded() {
  int n = 0;
  for (const Case& c : allCases()) {
    if (c.routed) continue;
    const uint8_t set = wire::markerSetFor(c.provider);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(wire::kNoMarkers, set, c.provider.c_str());
    const wire::Classified r = wire::classifyProvider(set, c.status, c.body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.expect.c_str(), classSlug(r), label(c).c_str());
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(c.refused, r.refused, label(c).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(c.fileRef, r.fileRef, label(c).c_str());
    ++n;
  }
  TEST_ASSERT_TRUE(n >= 70);
}

// Every row of the section 9 table has at least one fixture.
static void test_every_section9_row_has_a_fixture() {
  static const char* const kRows[] = {"transport", "rate",          "server",           "auth",        "forbidden",
                                      "billing",   "context",       "refused_field",    "file_id",     "unknown_model",
                                      "malformed_request", "refusal", "refusal_2xx", "unparsed_2xx", "cut", "router"};
  std::set<std::string> seen;
  for (const Case& c : allCases()) seen.insert(c.row);
  for (const char* r : kRows) TEST_ASSERT_TRUE_MESSAGE(seen.count(r), r);
}

// Every marker row of the facts table is exercised: a case of its provider whose body carries
// the marker and that classifies to the marker's class. A marker added with no fixture fails.
static void test_every_marker_row_has_a_fixture() {
  const CapsTable& t = builtinCaps();
  const std::vector<Case> cases = allCases();
  for (size_t i = 0; i < t.markerCount; i++) {
    const MarkerRow& m = t.markers[i];
    const std::string text = lowered(poolStr(t, m.text));
    bool found = false;
    for (const Case& c : cases) {
      if (c.routed || wire::markerSetFor(c.provider) != m.markerSet) continue;
      if (lowered(c.body).find(text) == std::string::npos) continue;
      const wire::Classified r = wire::classifyProvider(m.markerSet, c.status, c.body);
      if (r.error && r.cls == m.cls) found = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, (std::string("no fixture for marker ") + poolStr(t, m.text)).c_str());
  }
}

// Billing is decided before 429, and a code marker matches the code field only, exactly.
static void test_billing_before_rate_and_codes_match_exactly() {
  const uint8_t zai = wire::markerSetFor("zai");
  const std::string arrears = R"({"error":{"code":"1113","message":"Your account is in arrears"}})";
  TEST_ASSERT_TRUE(wire::classifyProvider(zai, 429, arrears).cls == ErrClass::Billing);
  const std::string inText = R"({"error":{"code":"1302","message":"not 1113"}})";
  TEST_ASSERT_TRUE(wire::classifyProvider(zai, 429, inText).cls == ErrClass::RateLimited);
  const std::string numeric = R"({"error":{"code":1261,"message":"Prompt exceeds max length"}})";
  TEST_ASSERT_TRUE(wire::classifyProvider(zai, 400, numeric).cls == ErrClass::Context);
  // the same body under another provider's markers is not Z.ai's billing
  TEST_ASSERT_TRUE(wire::classifyProvider(wire::markerSetFor("mistral"), 429, arrears).cls == ErrClass::RateLimited);
  TEST_ASSERT_EQUAL(wire::kNoMarkers, wire::markerSetFor("cumulo"));
}

// The generic resend: once per newly refused field, remembered for the boot, and the row the
// next request reads no longer carries the field.
static void test_a_refused_field_is_resent_once_and_remembered() {
  RefusedFields mem;
  const char* model = "magistral-medium-latest";
  TEST_ASSERT_FALSE(wire::resendWithout(mem, Slot::Mistral, model, false, 0));
  TEST_ASSERT_TRUE(wire::resendWithout(mem, Slot::Mistral, model, false, refused::kEffort));
  TEST_ASSERT_FALSE(wire::resendWithout(mem, Slot::Mistral, model, false, refused::kEffort));
  TEST_ASSERT_TRUE(wire::resendWithout(mem, Slot::Mistral, model, false, refused::kEffort | refused::kImage));
  TEST_ASSERT_EQUAL_UINT16(refused::kEffort | refused::kImage, mem.fieldsFor(Slot::Mistral, model, false));
  TEST_ASSERT_EQUAL_UINT16(0, mem.fieldsFor(Slot::Mistral, model, true));
  const Caps c = capsFor(Slot::Mistral, model, false, &mem);
  TEST_ASSERT_EQUAL_UINT8(0, c.row.effortSet);
  TEST_ASSERT_EQUAL_UINT8(0, c.row.inputKinds & input::kImage);
  // a body that names no refused field asks for no resend
  wire::ErrorObject e = wire::readErrorObject(R"({"error":{"message":"max_tokens exceeds the maximum of 64000"}})");
  TEST_ASSERT_EQUAL_UINT16(0, wire::refusedFieldsOf(e));
  e = wire::readErrorObject(R"({"error":{"message":"Unsupported parameter","code":"unsupported_parameter","param":"reasoning_effort"}})");
  TEST_ASSERT_EQUAL_UINT16(refused::kEffort, wire::refusedFieldsOf(e));
}

// Engine 2's reasons are the cloud contract's 44, engine 1's 35 among them unchanged, and the
// router cases read the contract's action.
static void test_engine2_reasons_equal_the_cloud_contract() {
  const size_t n = sizeof(kRouterReasonsE2) / sizeof(kRouterReasonsE2[0]);
  TEST_ASSERT_EQUAL(44, (int)n);
  for (const RouterReasonRow& old : kRouterReasons) {
    bool same = false;
    for (const RouterReasonRow& r : kRouterReasonsE2)
      if (std::strcmp(r.reason, old.reason) == 0)
        same = r.http == old.http && r.retryable == old.retryable && r.action == old.action;
    TEST_ASSERT_TRUE_MESSAGE(same, old.reason);
  }
  static const char* const kNew[] = {"large_body_busy",       "body_read_timeout", "image_source_not_allowed",
                                     "too_many_images",       "image_too_large",   "image_unsupported_type",
                                     "model_no_image_input",  "file_source_not_allowed", "deadline_too_short"};
  for (const char* r : kNew) {
    TEST_ASSERT_NULL_MESSAGE(routerReasonRow(r), r);   // engine 1's table is not regenerated
    const RouterAnswer a = wire::readRouterAnswerE2(std::string(R"({"reason":")") + r + R"("})");
    TEST_ASSERT_NOT_NULL_MESSAGE(a.row, r);
  }
  int routed = 0;
  for (const Case& c : allCases()) {
    if (!c.routed) continue;
    const RouterAnswer a = wire::readRouterAnswerE2(c.body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.expect.c_str(), routerActionSlug(a.action()), label(c).c_str());
    ++routed;
  }
  TEST_ASSERT_EQUAL(4, routed);
  TEST_ASSERT_TRUE(wire::readRouterAnswerE2(R"({"reason":"large_body_busy","retryable":true})").row->retryable);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_fixture_classifies_as_recorded);
  RUN_TEST(test_every_section9_row_has_a_fixture);
  RUN_TEST(test_every_marker_row_has_a_fixture);
  RUN_TEST(test_billing_before_rate_and_codes_match_exactly);
  RUN_TEST(test_a_refused_field_is_resent_once_and_remembered);
  RUN_TEST(test_engine2_reasons_equal_the_cloud_contract);
  return UNITY_END();
}
