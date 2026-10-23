#include <unity.h>

#include <ArduinoJson.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_http.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/vision_caption.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/spend_ceiling.h"

// The fallback photo reader (owner ruling R1) over FakeHttpTransport:
//  - L19-3: a billing refusal on provider 1 is parked through the ONE shared park and
//    the walk moves on to provider 2 in the same photo; the next photos make no
//    request to provider 1; one owner notice per episode, shared with every seam.
//    Counter: a 5xx or a connect failure walks on without parking.
//  - the walk is sticky per page (the provider that answered keeps the album).
//  - the request builder per provider: current models only, detail high, low effort,
//    the token field per model family, the photo before the words.
//  - L19-7: the error line keeps host, model and the provider's code inside its cap.
//  - a 16,385 B reply is a logged failure, not a crash.

using harness_test::Exchange;
using harness_test::FakeHttpTransport;
using harness_test::LogCapture;
namespace vision = agent::vision;
namespace orch = nimbus::orch;

namespace {

// Recorded provider answers (scrubbed of request ids).
const char* kOpenAIQuota429 =
    "{\"error\":{\"message\":\"You exceeded your current quota, please check your plan and billing "
    "details. For more information on this error, read the docs: https://platform.openai.com/docs/"
    "guides/error-codes/api-errors.\",\"type\":\"insufficient_quota\",\"param\":null,\"code\":"
    "\"insufficient_quota\"}}";
const char* kAnthropicCredit400 =
    "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Your credit balance "
    "is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase "
    "credits.\"},\"request_id\":\"req_011CUA\"}";
const char* kMistral422 =
    "{\"object\":\"error\",\"message\":\"Invalid model: pixtral-12b-latest\",\"type\":"
    "\"invalid_request_message_error\",\"param\":null,\"code\":\"1500\"}";

std::string antOk(const std::string& text) {
  JsonDocument d;
  d["content"][0]["type"] = "text";
  d["content"][0]["text"] = text;
  d["usage"]["input_tokens"] = 1600;
  d["usage"]["output_tokens"] = 80;
  std::string out;
  serializeJson(d, out);
  return out;
}
std::string chatOk(const std::string& text) {
  return "{\"choices\":[{\"message\":{\"content\":\"" + text +
         "\"}}],\"usage\":{\"prompt_tokens\":900,\"completion_tokens\":60}}";
}

struct Rig {
  FakeHttpTransport http;
  std::string prio = "openai,anthropic";
  std::vector<std::string> keyed{"openai", "anthropic"};
  bool reachable = true;
  std::vector<std::string> notices;
  uint32_t now = 1000;
  const uint8_t photo[6] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};

  vision::CaptionDeps deps() {
    vision::CaptionDeps d;
    d.http = &http;
    d.key = [this](const std::string& p) {
      for (const auto& k : keyed)
        if (k == p) return std::string("key-") + p;
      return std::string();
    };
    d.priority = [this] { return prio; };
    d.nowMs = [this] { return now; };
    d.ownerReachable = [this] { return reachable; };
    d.notifyOwner = [this](const std::string& w) { notices.push_back(w); };
    return d;
  }
  vision::CaptionResult read(const char* context = "") {
    vision::CaptionRequest in;
    in.bytes = photo;
    in.len = sizeof photo;
    in.mime = "image/jpeg";
    in.context = context;
    return vision::caption(deps(), in);
  }
  size_t requestsTo(const char* host) const {
    size_t n = 0;
    for (const auto& r : http.seen)
      if (r.host == host) ++n;
    return n;
  }
};

bool parked(const char* provider, orch::PaidSeam seam, uint32_t now) {
  return !orch::providerPark()
              .available(provider, seam, orch::parkKeyFingerprint(std::string("key-") + provider), now)
              .open;
}

}  // namespace

void setUp() {
  LogCapture::install();
  orch::providerPark().clear();
  vision::clearLastError();
}
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- L19-3 --------------------------------------------------------------------------

static void test_billing_429_parks_walks_on_and_notices_once() {
  Rig rig;
  rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 429, kOpenAIQuota429});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("A nutrition label: 190 kcal.")});
  const vision::CaptionResult r = rig.read("dinner");
  TEST_ASSERT_EQUAL_STRING("A nutrition label: 190 kcal.", r.text.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.provider.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", r.model.c_str());
  TEST_ASSERT_EQUAL(2, r.requests);
  TEST_ASSERT_EQUAL(1u, rig.requestsTo("api.anthropic.com"));   // provider 2 called once
  // Provider 1 is parked through the one shared park: for every seam, not just photos.
  TEST_ASSERT_TRUE(parked("openai", orch::PaidSeam::Vision, rig.now));
  TEST_ASSERT_TRUE(parked("openai", orch::PaidSeam::Chat, rig.now));
  TEST_ASSERT_TRUE(parked("openai", orch::PaidSeam::Embeddings, rig.now));
  TEST_ASSERT_EQUAL(1u, rig.notices.size());
  TEST_ASSERT_NOT_NULL(strstr(rig.notices[0].c_str(), "openai says the account is out of credit (HTTP 429)"));
  TEST_ASSERT_NOT_NULL(strstr(rig.notices[0].c_str(), "photos are read by anthropic"));
  // The next N photos: zero requests to provider 1, no second notice (a new page too).
  const int kNext = 5;
  for (int i = 0; i < kNext; ++i) {
    vision::endPage();
    rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("photo " + std::to_string(i))});
    TEST_ASSERT_EQUAL_STRING(("photo " + std::to_string(i)).c_str(), rig.read().text.c_str());
  }
  TEST_ASSERT_EQUAL(1u, rig.requestsTo("api.openai.com"));
  TEST_ASSERT_EQUAL((size_t)(1 + kNext), rig.requestsTo("api.anthropic.com"));
  TEST_ASSERT_EQUAL(1u, rig.notices.size());
  // The episode latch is the shared one: another seam meeting the same refusal now
  // sends no second notice.
  TEST_ASSERT_FALSE(orch::billingNoticeDue("openai"));
}

// Counter-test: a 5xx and a connect failure walk on WITHOUT parking.
static void test_5xx_and_connect_failure_walk_on_without_parking() {
  for (int kind = 0; kind < 2; ++kind) {
    orch::providerPark().clear();
    vision::endPage();
    Rig rig;
    if (kind == 0)
      rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 503,
                                 "{\"error\":{\"message\":\"The server is overloaded\",\"type\":\"server_error\"}}"});
    else
      rig.http.script.push_back(harness_test::transportCut(0, "connect failed"));
    rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("a receipt")});
    const vision::CaptionResult r = rig.read();
    TEST_ASSERT_EQUAL_STRING("a receipt", r.text.c_str());
    TEST_ASSERT_FALSE(parked("openai", orch::PaidSeam::Vision, rig.now));
    TEST_ASSERT_FALSE(parked("openai", orch::PaidSeam::Chat, rig.now));
    TEST_ASSERT_TRUE(rig.notices.empty());
    // A new page starts from the top of the order again: provider 1 is asked.
    vision::endPage();
    rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 200, chatOk("back again")});
    TEST_ASSERT_EQUAL_STRING("back again", rig.read().text.c_str());
  }
}

// Sticky per page: once provider 2 answered, the rest of the album goes there first.
static void test_walk_is_sticky_per_page() {
  Rig rig;
  rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 503, "{}"});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("photo 1")});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("photo 2")});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("photo 3")});
  for (int i = 1; i <= 3; ++i) TEST_ASSERT_EQUAL_STRING(("photo " + std::to_string(i)).c_str(), rig.read().text.c_str());
  TEST_ASSERT_EQUAL(1u, rig.requestsTo("api.openai.com"));
}

// A provider already closed (out of credit on another seam) is skipped with no request.
static void test_closed_provider_is_skipped_with_no_request() {
  Rig rig;
  orch::providerPark().park("openai", orch::PaidSeam::Embeddings, orch::parkKeyFingerprint("key-openai"),
                            orch::ParkKind::Billing, 429, rig.now);
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("a bus stop sign")});
  TEST_ASSERT_EQUAL_STRING("a bus stop sign", rig.read().text.c_str());
  TEST_ASSERT_EQUAL(0u, rig.requestsTo("api.openai.com"));
}

// Every keyed provider refused: the typed class of the most telling refusal, its
// words naming the provider and the HTTP status (never the body).
static void test_all_refused_gives_the_typed_billing_result() {
  Rig rig;
  rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 429, kOpenAIQuota429});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 400, kAnthropicCredit400});
  const vision::CaptionResult r = rig.read();
  TEST_ASSERT_TRUE(r.text.empty());
  TEST_ASSERT_EQUAL((int)vision::ReadFail::Billing, (int)r.fail);
  TEST_ASSERT_EQUAL_STRING("openai out of credit (HTTP 429)", r.words.c_str());
  TEST_ASSERT_EQUAL(1u, rig.notices.size());
  TEST_ASSERT_NOT_NULL(strstr(rig.notices[0].c_str(), "Photos cannot be read until credit is added to openai."));
  const vision::LastError e = vision::lastError();
  TEST_ASSERT_TRUE(e.atMs > 0);
  TEST_ASSERT_EQUAL_STRING("openai", e.provider.c_str());
  TEST_ASSERT_EQUAL((int)vision::ReadFail::Billing, (int)e.fail);
  TEST_ASSERT_EQUAL(429, e.status);
}

// The monthly spend ceiling (lane DEF): a frozen device sends no caption request at all.
static void test_spend_freeze_sends_nothing() {
  Rig rig;
  orch::installSpendFreeze(+[] { return true; });
  const vision::CaptionResult r = rig.read();
  orch::installSpendFreeze(nullptr);
  TEST_ASSERT_EQUAL((int)vision::ReadFail::Closed, (int)r.fail);
  TEST_ASSERT_EQUAL(0, r.requests);
  TEST_ASSERT_TRUE(rig.http.seen.empty());
  TEST_ASSERT_NOT_NULL(strstr(r.words.c_str(), "paused"));
}

static void test_no_key_no_request() {
  Rig rig;
  rig.keyed.clear();
  const vision::CaptionResult r = rig.read();
  TEST_ASSERT_EQUAL((int)vision::ReadFail::NoProvider, (int)r.fail);
  TEST_ASSERT_EQUAL(0, r.requests);
  TEST_ASSERT_TRUE(rig.http.seen.empty());
}

// A quiet background turn (no owner reachable) leaves the shared latch for the next
// owner turn instead of spending it on a notice nobody receives.
static void test_quiet_turn_leaves_the_notice_latch() {
  Rig rig;
  rig.reachable = false;
  rig.http.script.push_back({"api.openai.com", "/v1/chat/completions", 429, kOpenAIQuota429});
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk("ok")});
  rig.read();
  TEST_ASSERT_TRUE(rig.notices.empty());
  TEST_ASSERT_TRUE(orch::billingNoticeDue("openai"));
}

// ---- the request builder ----------------------------------------------------------

static void test_request_builder_per_provider() {
  const uint8_t img[4] = {1, 2, 3, 4};
  vision::CaptionRequest in;
  in.bytes = img;
  in.len = sizeof img;
  in.mime = "image/png";
  in.context = "what is the batch number?";
  struct Row {
    const char* provider;
    const char* model;
    const char* host;
    const char* path;
  } rows[] = {
      {"anthropic", vision::captionModel("anthropic"), "api.anthropic.com", "/v1/messages"},
      {"openai", vision::captionModel("openai"), "api.openai.com", "/v1/chat/completions"},
      {"mistral", vision::captionModel("mistral"), "api.mistral.ai", "/v1/chat/completions"},
  };
  for (const Row& row : rows) {
    agent::HttpRequest req;
    TEST_ASSERT_TRUE(vision::buildCaptionRequest(row.provider, row.model, "k", in, "", req));
    TEST_ASSERT_EQUAL_STRING(row.host, req.host.c_str());
    TEST_ASSERT_EQUAL_STRING(row.path, req.path.c_str());
    TEST_ASSERT_EQUAL(1u, req.splices.size());   // the photo is spliced, never a JSON string
    const std::string body = agent::materializeBody(req);
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, body));
    TEST_ASSERT_EQUAL_STRING(row.model, d["model"] | "");
    JsonArrayConst c = d["messages"][0]["content"];
    TEST_ASSERT_EQUAL(2, (int)c.size());   // the photo, then the words
    TEST_ASSERT_NOT_NULL(strstr(c[1]["text"] | "", "The sender wrote: what is the batch number?"));
    const std::string p = row.provider;
    if (p == "anthropic") {
      TEST_ASSERT_EQUAL_STRING("image", c[0]["type"] | "");
      TEST_ASSERT_EQUAL_STRING("image/png", c[0]["source"]["media_type"] | "");
      TEST_ASSERT_EQUAL_STRING("AQIDBA==", c[0]["source"]["data"] | "");
      TEST_ASSERT_TRUE(d["max_tokens"].is<int>());
      TEST_ASSERT_EQUAL_STRING("low", d["output_config"]["effort"] | "");
      bool apiKey = false;
      for (const auto& h : req.headers) apiKey |= h.first == "x-api-key" && h.second == "k";
      TEST_ASSERT_TRUE(apiKey);
    } else if (p == "openai") {
      TEST_ASSERT_EQUAL_STRING("data:image/png;base64,AQIDBA==", c[0]["image_url"]["url"] | "");
      TEST_ASSERT_EQUAL_STRING("high", c[0]["image_url"]["detail"] | "");   // detail high
      TEST_ASSERT_TRUE(d["max_completion_tokens"].is<int>());   // the gpt-5 family's token field
      TEST_ASSERT_TRUE(d["max_tokens"].isNull());
      TEST_ASSERT_EQUAL_STRING("low", d["reasoning_effort"] | "");
    } else {
      TEST_ASSERT_EQUAL_STRING("data:image/png;base64,AQIDBA==", c[0]["image_url"] | "");
      TEST_ASSERT_TRUE(d["max_tokens"].is<int>());
      TEST_ASSERT_EQUAL_STRING("none", d["reasoning_effort"] | "");   // Mistral's low setting
    }
  }
  // The token field per model family: an OpenAI chat model (no reasoning) takes max_tokens.
  agent::HttpRequest req;
  TEST_ASSERT_TRUE(vision::buildCaptionRequest("openai", "gpt-4.1-mini", "k", in, "", req));
  JsonDocument d;
  deserializeJson(d, agent::materializeBody(req));
  TEST_ASSERT_TRUE(d["max_tokens"].is<int>());
  TEST_ASSERT_TRUE(d["max_completion_tokens"].isNull());
  TEST_ASSERT_TRUE(d["reasoning_effort"].isNull());
  // Through the router: Bearer with the router key, /router/<provider>/v1/...
  TEST_ASSERT_TRUE(vision::buildCaptionRequest("anthropic", "claude-haiku-5-5", "rk", in, "app.example", req));
  TEST_ASSERT_EQUAL_STRING("app.example", req.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/router/anthropic/v1/messages", req.path.c_str());
  bool bearer = false, apiKey = false;
  for (const auto& h : req.headers) {
    bearer |= h.first == "Authorization" && h.second == "Bearer rk";
    apiKey |= h.first == "x-api-key";
  }
  TEST_ASSERT_TRUE(bearer);
  TEST_ASSERT_FALSE(apiKey);
}

// Current models only: no retired id can be the caption model of any provider.
static void test_caption_models_are_current() {
  for (const char* p : {"anthropic", "openai", "mistral"}) {
    const std::string m = vision::captionModel(p);
    TEST_ASSERT_FALSE_MESSAGE(m.empty(), p);
    for (const char* retired : {"gpt-4o-mini", "claude-3-5-haiku-latest", "pixtral-12b-latest", "claude-haiku-4-5"})
      TEST_ASSERT_TRUE_MESSAGE(m != retired, m.c_str());
  }
  TEST_ASSERT_EQUAL_STRING("", vision::captionModel("zai"));
}

// The sender's words can carry the marker text: the photo is still spliced where it
// belongs (its part comes first) and the words reach the provider intact.
static void test_sender_text_cannot_move_the_photo() {
  const uint8_t img[3] = {9, 9, 9};
  vision::CaptionRequest in;
  in.bytes = img;
  in.len = sizeof img;
  in.context = "nimg:caption \"},{\"type\":\"x";
  for (const char* p : {"anthropic", "openai", "mistral"}) {
    agent::HttpRequest req;
    TEST_ASSERT_TRUE(vision::buildCaptionRequest(p, vision::captionModel(p), "k", in, "", req));
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, agent::materializeBody(req)));
    TEST_ASSERT_NOT_NULL(strstr(d["messages"][0]["content"][1]["text"] | "", "nimg:caption \"},{\"type\":\"x"));
  }
}

// image.look: the ref is exactly 16 lowercase hex characters (a blob hash) and the
// question is present and bounded; anything else is refused before any request.
static void test_look_args() {
  std::string err;
  TEST_ASSERT_TRUE(vision::lookArgsValid("0123456789abcdef", "what is the batch number?", err));
  for (const char* bad : {"", "0123456789abcde", "0123456789abcdef0", "0123456789ABCDEF", "0123456789abcdeg",
                          "../../etc/passwd", "0123456789abcde/"}) {
    err.clear();
    TEST_ASSERT_FALSE_MESSAGE(vision::lookArgsValid(bad, "q", err), bad);
    TEST_ASSERT_NOT_NULL(strstr(err.c_str(), "16 hex"));
  }
  TEST_ASSERT_FALSE(vision::lookArgsValid("0123456789abcdef", "", err));
  TEST_ASSERT_TRUE(vision::lookArgsValid("0123456789abcdef", std::string(vision::kLookQuestionMax, 'q'), err));
  TEST_ASSERT_FALSE(vision::lookArgsValid("0123456789abcdef", std::string(vision::kLookQuestionMax + 1, 'q'), err));
}

// ---- L19-7: the error line ----------------------------------------------------------

static void test_error_line_keeps_host_model_code_inside_the_cap() {
  struct Case {
    const char* provider;
    int status;
    const char* body;
    const char* host;
    const char* code;
  } cases[] = {
      {"openai", 429, kOpenAIQuota429, "api.openai.com", "insufficient_quota"},
      {"anthropic", 400, kAnthropicCredit400, "api.anthropic.com", "invalid_request_error"},
      {"mistral", 422, kMistral422, "api.mistral.ai", "1500"},
  };
  for (const Case& c : cases) {
    orch::providerPark().clear();
    vision::endPage();
    LogCapture::lines().clear();
    Rig rig;
    rig.prio = c.provider;
    rig.keyed = {c.provider};
    rig.http.script.push_back({c.host, "", c.status, c.body});
    rig.read();
    std::string line;
    for (const auto& l : LogCapture::lines())
      if (l.rfind(std::string("vision: ") + c.host, 0) == 0) line = l;
    TEST_ASSERT_FALSE_MESSAGE(line.empty(), c.host);
    TEST_ASSERT_TRUE_MESSAGE(line.size() <= vision::kErrLineMax, line.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line.c_str(), c.host), line.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line.c_str(), vision::captionModel(c.provider)), line.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line.c_str(), c.code), line.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line.c_str(), (" HTTP " + std::to_string(c.status)).c_str()), line.c_str());
    TEST_ASSERT_NULL(strchr(line.c_str(), '\n'));
  }
}

// ---- raised limits ------------------------------------------------------------------

// A 16,385 B reply (one byte over kMaxCaptionText) is a logged failure, not a crash
// and not a description; a reply at the limit is kept.
static void test_overlong_reply_is_a_logged_failure() {
  Rig rig;
  rig.prio = "anthropic";
  rig.keyed = {"anthropic"};
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk(std::string(16385, 'x'))});
  const vision::CaptionResult r = rig.read();
  TEST_ASSERT_TRUE(r.text.empty());
  TEST_ASSERT_EQUAL((int)vision::ReadFail::ProviderError, (int)r.fail);
  TEST_ASSERT_TRUE(LogCapture::contains("the answer is too long"));
  TEST_ASSERT_FALSE(parked("anthropic", orch::PaidSeam::Vision, rig.now));   // a 200 is no refusal
  vision::endPage();
  rig.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antOk(std::string(16384, 'y'))});
  TEST_ASSERT_EQUAL(16384u, rig.read().text.size());
}

// ---- the typed words ----------------------------------------------------------------

static void test_fail_words_per_class() {
  std::vector<std::string> seen;
  for (int f = 1; f < (int)vision::ReadFail::Count; ++f) {
    const std::string w = vision::failWords((vision::ReadFail)f, "openai", 429);
    TEST_ASSERT_FALSE(w.empty());
    for (const auto& s : seen) TEST_ASSERT_TRUE_MESSAGE(s != w, w.c_str());
    seen.push_back(w);
    TEST_ASSERT_TRUE(std::string(vision::readFailSlug((vision::ReadFail)f)) != "?");
  }
  TEST_ASSERT_EQUAL_STRING("openai out of credit (HTTP 429)",
                           vision::failWords(vision::ReadFail::Billing, "openai", 429).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_billing_429_parks_walks_on_and_notices_once);
  RUN_TEST(test_5xx_and_connect_failure_walk_on_without_parking);
  RUN_TEST(test_walk_is_sticky_per_page);
  RUN_TEST(test_closed_provider_is_skipped_with_no_request);
  RUN_TEST(test_all_refused_gives_the_typed_billing_result);
  RUN_TEST(test_no_key_no_request);
  RUN_TEST(test_spend_freeze_sends_nothing);
  RUN_TEST(test_quiet_turn_leaves_the_notice_latch);
  RUN_TEST(test_request_builder_per_provider);
  RUN_TEST(test_caption_models_are_current);
  RUN_TEST(test_sender_text_cannot_move_the_photo);
  RUN_TEST(test_look_args);
  RUN_TEST(test_error_line_keeps_host_model_code_inside_the_cap);
  RUN_TEST(test_overlong_reply_is_a_logged_failure);
  RUN_TEST(test_fail_words_per_class);
  return UNITY_END();
}
