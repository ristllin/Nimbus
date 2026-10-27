#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_http.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/provider_park.h"

// L19-4: one label per photo outcome, so the head can tell "the provider is out of
// credit" from "the photo is too large" from "the device ran low on memory", and is
// never left to guess (on 19 Oct every failure read "(couldn't be described)" and the
// head blamed the owner's Telegram app).
//  - every failure class yields its own label; the billing one names the provider and
//    the HTTP status;
//  - property: over hostile provider answers, a label never carries the response
//    body, a key or a URL;
//  - counter-test: the pre-R1 placeholder still reads as before for the legacy class
//    (older stored rows), and no typed class reads like it.

using harness_test::FakeHttpTransport;
using harness_test::LogCapture;
namespace vision = agent::vision;
namespace orch = nimbus::orch;

void setUp() {
  LogCapture::install();
  orch::providerPark().clear();
  vision::clearLastError();
}
void tearDown() { agent::hlog::setSink(nullptr); }

static std::string notReadLabel(vision::ReadFail f, const char* provider, int status) {
  orch::MediaLabel l;
  l.k = 1;
  l.n = 1;
  l.ref = "0123456789abcdef";
  l.cls = orch::MediaClass::NotRead;
  l.detail = vision::failWords(f, provider, status);
  return orch::mediaSegment(l);
}

static void test_every_failure_class_has_its_own_label() {
  std::vector<std::string> seen;
  for (int f = 1; f < (int)vision::ReadFail::Count; ++f) {
    const std::string lab = notReadLabel((vision::ReadFail)f, "openai", 429);
    TEST_ASSERT_EQUAL(0, (int)lab.find("[image 1/1 ref=0123456789abcdef] (not read: "));
    for (const auto& s : seen) TEST_ASSERT_TRUE_MESSAGE(s != lab, lab.c_str());
    seen.push_back(lab);
  }
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0123456789abcdef] (not read: openai out of credit (HTTP 429))",
                           notReadLabel(vision::ReadFail::Billing, "openai", 429).c_str());
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0123456789abcdef] (not read: too large)",
                           notReadLabel(vision::ReadFail::TooLarge, "openai", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0123456789abcdef] (not read: device low on memory)",
                           notReadLabel(vision::ReadFail::LowMemory, "openai", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=0123456789abcdef] (not read: provider error)",
                           notReadLabel(vision::ReadFail::ProviderError, "openai", 500).c_str());
}

// The R1 classes: the photo itself, another model's transcription (named), a kept
// photo its turn has not settled, a photo the head saw (with its own note, or without);
// each its own suffix.
static void test_r1_classes() {
  orch::MediaLabel l;
  l.k = 2;
  l.n = 3;
  l.ref = "fedcba9876543210";
  l.cls = orch::MediaClass::Attached;
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (attached: the photo itself is in this message)",
                           orch::mediaSegment(l).c_str());
  l.cls = orch::MediaClass::Transcribed;
  l.by = "anthropic";
  l.detail = "Batch 4417, net 250 g";
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (transcribed by anthropic) Batch 4417, net 250 g",
                           orch::mediaSegment(l).c_str());
  l.cls = orch::MediaClass::Pending;
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (kept; image.look shows it)",
                           orch::mediaSegment(l).c_str());
  l.cls = orch::MediaClass::Seen;
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (seen in its turn; image.look shows it again)",
                           orch::mediaSegment(l).c_str());
  l.cls = orch::MediaClass::Noted;
  l.detail = "receipt, 3 items, total 12.40";
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (seen; note: receipt, 3 items, total 12.40)",
                           orch::mediaSegment(l).c_str());
  l.detail.clear();   // a note never written reads as seen
  TEST_ASSERT_EQUAL_STRING("[image 2/3 ref=fedcba9876543210] (seen in its turn; image.look shows it again)",
                           orch::mediaSegment(l).c_str());
  TEST_ASSERT_EQUAL_STRING(" (attached: the photo itself is in this message)", orch::kAttachedSuffix);
  TEST_ASSERT_EQUAL_STRING(" (seen in its turn; image.look shows it again)", orch::kSeenSuffix);
}

// Property: whatever the provider answers (its error text echoing a key, a URL, the
// request, a huge body), the label carries only the typed words.
static void test_label_never_carries_body_key_or_url() {
  const std::string key = std::string("sk-") + "proj-" + "AbCdEf0123456789";   // split: never a secret-shaped literal
  const std::string bodies[] = {
      "{\"error\":{\"message\":\"Incorrect API key provided: " + key +
          ". You can find your API key at https://platform.openai.com/account/api-keys.\",\"code\":\"invalid_api_key\"}}",
      "{\"error\":{\"message\":\"You exceeded your current quota, see https://platform.openai.com/docs\","
      "\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}",
      "{\"error\":{\"message\":\"" + std::string(5000, 'Z') + " http://evil.example/x\"}}",
      "<html><body>502 Bad Gateway https://cdn.example</body></html>",
  };
  const int statuses[] = {401, 429, 400, 502};
  for (size_t i = 0; i < 4; ++i) {
    orch::providerPark().clear();
    vision::endPage();
    FakeHttpTransport http;
    http.script.push_back({"api.openai.com", "", statuses[i], bodies[i]});
    vision::CaptionDeps d;
    d.http = &http;
    d.key = [&key](const std::string& p) { return p == "openai" ? key : std::string(); };
    d.priority = [] { return std::string("openai"); };
    d.nowMs = [] { return 5000u; };
    const uint8_t img[2] = {1, 2};
    vision::CaptionRequest in;
    in.bytes = img;
    in.len = 2;
    const vision::CaptionResult r = vision::caption(d, in);
    orch::MediaLabel l;
    l.ref = "0123456789abcdef";
    l.cls = orch::MediaClass::NotRead;
    l.detail = r.words;
    const std::string lab = orch::mediaSegment(l);
    TEST_ASSERT_TRUE_MESSAGE(lab.find("sk-") == std::string::npos, lab.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lab.find("://") == std::string::npos, lab.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lab.find("ZZZZ") == std::string::npos, lab.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lab.find("{") == std::string::npos, lab.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lab.find("quota") == std::string::npos, lab.c_str());   // the provider's own words
    TEST_ASSERT_TRUE_MESSAGE(lab.size() < 120, lab.c_str());
  }
}

// Counter-test: the legacy class keeps the pre-R1 placeholder (rows stored before this
// change still read as they did), and no typed class reads like it.
static void test_legacy_placeholder_is_only_the_legacy_class() {
  orch::MediaLabel l;
  l.ref = "0123456789abcdef";
  l.cls = orch::MediaClass::NotDescribed;
  TEST_ASSERT_EQUAL_STRING("[image ref=0123456789abcdef] (couldn't be described)", orch::mediaSegment(l).c_str());
  for (int f = 1; f < (int)vision::ReadFail::Count; ++f)
    TEST_ASSERT_TRUE(notReadLabel((vision::ReadFail)f, "openai", 429).find("couldn't be described") ==
                     std::string::npos);
}

// The recorded fixture S2's cause-attribution eval reads (L19-11) carries the label the
// device writes today, byte for byte: a change to the words fails here first.
static void test_recorded_billing_fixture_matches_the_label() {
  FILE* f = fopen("test/support/fixtures/photo_labels_2026-10/billing_label.json", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "the recorded billing label fixture is missing");
  std::string json;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) json.append(buf, n);
  fclose(f);
  vision::CaptionResult r;
  r.fail = vision::ReadFail::Billing;
  r.words = vision::failWords(r.fail, "openai", 429);
  orch::MediaLabel l;
  l.k = l.n = 1;
  l.ref = "0123456789abcdef";
  l.cls = orch::MediaClass::NotRead;
  l.detail = vision::notReadWords(r);
  std::string want = orch::mediaSegment(l);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json.c_str(), ("\"label\": \"" + want + "\"").c_str()), want.c_str());
  TEST_ASSERT_TRUE(vision::rereadLater(vision::ReadFail::Billing));
  TEST_ASSERT_FALSE(vision::rereadLater(vision::ReadFail::TooLarge));   // a resend can help there
  TEST_ASSERT_EQUAL_STRING("too large", [] {
    vision::CaptionResult t;
    t.fail = vision::ReadFail::TooLarge;
    t.words = "too large";
    return vision::notReadWords(t);
  }().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_failure_class_has_its_own_label);
  RUN_TEST(test_r1_classes);
  RUN_TEST(test_label_never_carries_body_key_or_url);
  RUN_TEST(test_legacy_placeholder_is_only_the_legacy_class);
  RUN_TEST(test_recorded_billing_fixture_matches_the_label);
  return UNITY_END();
}
