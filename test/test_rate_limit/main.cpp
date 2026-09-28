#include <unity.h>

#include <set>
#include <string>

#include "../support/fake_http.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/rate_limit.h"

// CUM-460b - an HTTP 429 names the quota window that refused, and the owner hears
// the real thing: "wait a minute" only for a per-minute window; a spent daily
// quota says when it resets; a plan that allows 0 requests says waiting won't
// help. Pure classification + copy (lib/harness/src/rate_limit.cpp) and the
// transport's error-header capture seam (http.h).

using agent::HeaderList;
using agent::RateLimit;

void setUp() {}
void tearDown() {}

// ---- headers (Mistral x-ratelimit-*) ----------------------------------------

static void test_headers_measured_live_shapes() {
  // chat/completions on the personal key: the plan allows 0 requests per minute.
  TEST_ASSERT_EQUAL((int)RateLimit::NotAllowed,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-limit-req-minute", "0"},
                                                      {"x-ratelimit-remaining-req-minute", "0"}}));
  // Studio connectors after 50 calls in a day (reset observed at midnight UTC).
  TEST_ASSERT_EQUAL((int)RateLimit::DailyUtc,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-limit-custom-minute", "5"},
                                                      {"x-ratelimit-remaining-custom-minute", "4"},
                                                      {"x-ratelimit-limit-custom-day", "50"},
                                                      {"x-ratelimit-remaining-custom-day", "0"}}));
  // Only the Studio connector day window's reset was measured (midnight UTC); any
  // other exhausted day window is daily without a promised reset time.
  TEST_ASSERT_EQUAL((int)RateLimit::Daily,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-limit-tokens-day", "100000"},
                                                      {"x-ratelimit-remaining-tokens-day", "0"}}));
  // Conversations 20K tokens/min spent by a connector-heavy synthesis turn.
  TEST_ASSERT_EQUAL((int)RateLimit::PerMinute,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-limit-tokens-minute", "20000"},
                                                      {"x-ratelimit-remaining-tokens-minute", "0"}}));
}

static void test_headers_rules() {
  // A token window can refuse with tokens left (the request needs more than is left).
  TEST_ASSERT_EQUAL((int)RateLimit::PerMinute,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-limit-tokens-minute", "20000"},
                                                      {"x-ratelimit-remaining-tokens-minute", "3100"}}));
  // Connector per-minute (5/min) spent while the day still has room.
  TEST_ASSERT_EQUAL((int)RateLimit::PerMinute,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-remaining-custom-minute", "0"},
                                                      {"x-ratelimit-remaining-custom-day", "31"}}));
  // A spent month outranks a day and a minute; a zero limit outranks everything.
  TEST_ASSERT_EQUAL((int)RateLimit::Quota,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-remaining-tokens-month", "0"},
                                                      {"x-ratelimit-remaining-custom-day", "0"},
                                                      {"x-ratelimit-remaining-tokens-minute", "0"}}));
  TEST_ASSERT_EQUAL((int)RateLimit::NotAllowed,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-remaining-custom-day", "0"},
                                                      {"x-ratelimit-limit-req-minute", "0"}}));
  // The by-size family, mixed-case names, padded values.
  TEST_ASSERT_EQUAL((int)RateLimit::Quota,
                    (int)agent::rateLimitFromHeaders({{"X-RateLimitBySize-Remaining-Month", " 0"},
                                                      {"x-ratelimitbysize-limit-minute", "500000"}}));
  // Nothing recognizable: unknown, never a guess.
  TEST_ASSERT_EQUAL((int)RateLimit::Unknown, (int)agent::rateLimitFromHeaders({}));
  TEST_ASSERT_EQUAL((int)RateLimit::Unknown,
                    (int)agent::rateLimitFromHeaders({{"x-ratelimit-reset-custom-day", "0"},
                                                      {"x-ratelimit-limit-custom-day", "n/a"},
                                                      {"retry-after", "0"},
                                                      {"x-ratelimit-limit-requests", "0x"}}));
}

// ---- provider text (OpenAI / Anthropic / Mistral bodies) --------------------

static void test_text_rules() {
  struct Case { const char* text; RateLimit want; };
  const Case cases[] = {
      {"Rate limit reached for gpt-4o-mini in organization org-x on requests per min "
       "(RPM): Limit 3, Used 3, Requested 1.", RateLimit::PerMinute},
      {"Rate limit reached for gpt-4o in organization org-x on tokens per min (TPM): "
       "Limit 30000, Used 29000, Requested 2000.", RateLimit::PerMinute},
      {"Rate limit reached for gpt-4o-mini in organization org-x on requests per day "
       "(RPD): Limit 200, Used 200, Requested 1. Please try again in 7m12s.", RateLimit::Daily},
      {"You exceeded your current quota, please check your plan and billing details.",
       RateLimit::Quota},
      {"insufficient_quota", RateLimit::Quota},
      // One request bigger than the per-minute window: waiting never helps.
      {"Request too large for gpt-4o in organization org-x on tokens per min (TPM): "
       "Limit 30000, Requested 36858. The input or output tokens must be reduced in "
       "order to run successfully.", RateLimit::NotAllowed},
      {"This request would exceed the rate limit for your organization of 50,000 input "
       "tokens per minute.", RateLimit::PerMinute},
      // Mistral bodies say nothing about the window: the headers must.
      {"Custom connector rate limit reached.", RateLimit::Unknown},
      {"Requests rate limit exceeded", RateLimit::Unknown},
      {"", RateLimit::Unknown},
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_MESSAGE((int)c.want, (int)agent::rateLimitFromText(c.text), c.text);
}

// ---- the tag an adapter carries to the engine --------------------------------

static void test_tag_round_trips_every_kind() {
  std::set<std::string> slugs;
  for (int i = 0; i < int(RateLimit::kCount); i++) {
    const RateLimit k = RateLimit(i);
    TEST_ASSERT_TRUE_MESSAGE(slugs.insert(agent::rateLimitSlug(k)).second, "duplicate slug");
    if (k == RateLimit::Unknown) {
      TEST_ASSERT_EQUAL_STRING("", agent::rateLimitTag(k).c_str());
      continue;
    }
    const std::string err = "conversations HTTP 429 " + agent::rateLimitTag(k) +
                            ": Requests rate limit exceeded";
    TEST_ASSERT_EQUAL_MESSAGE(i, (int)agent::rateLimitFromError(err), err.c_str());
  }
  // The tag wins over the quoted text; no tag falls back to the text; a mangled
  // tag is ignored rather than trusted.
  TEST_ASSERT_EQUAL((int)RateLimit::DailyUtc,
                    (int)agent::rateLimitFromError("x HTTP 429 [rl:day-utc]: per min (RPM)"));
  TEST_ASSERT_EQUAL((int)RateLimit::Daily,
                    (int)agent::rateLimitFromError("resp HTTP 429: requests per day (RPD)"));
  TEST_ASSERT_EQUAL((int)RateLimit::Unknown,
                    (int)agent::rateLimitFromError("x HTTP 429 [rl:bogus]: busy"));
  TEST_ASSERT_EQUAL((int)RateLimit::Unknown, (int)agent::rateLimitFromError("x HTTP 429 [rl:"));
}

// ---- the owner copy: the class rule over EVERY kind --------------------------
// A new kind without its own reply, or a reply that breaks the device copy rules
// or promises the wrong wait, fails here.
static bool printableAscii(const std::string& s) {
  for (unsigned char c : s)
    if (c < 0x20 || c > 0x7e) return false;
  return true;
}

static void test_reply_copy_class_rules() {
  std::set<std::string> seen;
  for (int i = 0; i < int(RateLimit::kCount); i++) {
    const RateLimit k = RateLimit(i);
    const std::string r = agent::rateLimitReply(k);
    const char* slug = agent::rateLimitSlug(k);
    TEST_ASSERT_TRUE_MESSAGE(!r.empty() && r.size() < 200, slug);
    TEST_ASSERT_TRUE_MESSAGE(printableAscii(r), slug);            // no em dash, no emoji
    TEST_ASSERT_TRUE_MESSAGE(r.find(" - ") == std::string::npos, slug);
    TEST_ASSERT_TRUE_MESSAGE(r.find('!') == std::string::npos, slug);
    TEST_ASSERT_EQUAL_MESSAGE('.', r.back(), slug);
    // Every failure says nothing is left running (no phantom background work).
    TEST_ASSERT_TRUE_MESSAGE(r.find("Nothing is still running") != std::string::npos, slug);
    // Only a per-minute window may name a minute at all (no "a few minutes" either)...
    TEST_ASSERT_EQUAL_MESSAGE(k == RateLimit::PerMinute,
                              r.find("minute") != std::string::npos, slug);
    TEST_ASSERT_EQUAL_MESSAGE(k == RateLimit::PerMinute,
                              r.find("wait a minute") != std::string::npos, slug);
    // ...only the Mistral day window names its reset...
    TEST_ASSERT_EQUAL_MESSAGE(k == RateLimit::DailyUtc,
                              r.find("midnight UTC") != std::string::npos, slug);
    // ...and only the 0-request plan says waiting cannot help.
    TEST_ASSERT_EQUAL_MESSAGE(k == RateLimit::NotAllowed,
                              r.find("waiting won't help") != std::string::npos, slug);
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(r).second, slug);          // each kind its own words
  }
}

// ---- the transport seam: error headers captured, healthy responses untouched --

static void test_default_transport_captures_error_headers_only() {
  harness_test::FakeHttpTransport http;
  harness_test::Exchange ok;
  ok.status = 200;
  ok.body = "{}";
  ok.headers = {{"x-ratelimit-remaining-tokens-minute", "19000"}};
  harness_test::Exchange limited;
  limited.status = 429;
  limited.body = "{}";
  limited.headers = {{"X-RateLimit-Remaining-Custom-Day", "0"},
                     {"Content-Type", "application/json"},
                     {"x-ratelimitish", "1"},
                     {"X-RATELIMIT-LIMIT-CUSTOM-DAY", "50"}};
  http.script = {ok, limited};

  JsonDocument doc, filter;
  filter["x"] = true;
  std::string err;
  HeaderList rl;
  agent::HttpRequest req;
  req.errHeaderPrefix = "x-ratelimit-";
  req.errHeaders = &rl;
  TEST_ASSERT_EQUAL(200, http.execJson(req, doc, filter, err));
  TEST_ASSERT_EQUAL(0, (int)rl.size());   // a healthy response keeps nothing
  TEST_ASSERT_EQUAL(429, http.execJson(req, doc, filter, err));
  TEST_ASSERT_EQUAL(2, (int)rl.size());   // prefix match, case-insensitive, lowercased
  TEST_ASSERT_EQUAL_STRING("x-ratelimit-remaining-custom-day", rl[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("0", rl[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("x-ratelimit-limit-custom-day", rl[1].first.c_str());

  // Off by default: a request that asks for nothing gets nothing.
  agent::HttpRequest plain;
  TEST_ASSERT_FALSE(agent::wantsErrHeader(plain, 429, "x-ratelimit-limit-req-minute"));
}

// A response is untrusted input and device heap is scarce: a flood of matching
// headers, or a huge one, is kept bounded (count and length).
static void test_error_header_capture_is_bounded() {
  harness_test::FakeHttpTransport http;
  harness_test::Exchange flood;
  flood.status = 429;
  flood.body = "{}";
  for (int i = 0; i < 40; i++)
    flood.headers.push_back({"x-ratelimit-limit-x" + std::to_string(i) + "-minute", "5"});
  flood.headers.insert(flood.headers.begin(),
                       {"x-ratelimit-remaining-custom-day", std::string(5000, '9')});
  http.script = {flood};
  JsonDocument doc, filter;
  filter["x"] = true;
  std::string err;
  HeaderList rl;
  agent::HttpRequest req;
  req.errHeaderPrefix = "x-ratelimit";
  req.errHeaders = &rl;
  TEST_ASSERT_EQUAL(429, http.execJson(req, doc, filter, err));
  TEST_ASSERT_EQUAL((int)agent::kMaxErrHeaders, (int)rl.size());
  for (const auto& h : rl) {
    TEST_ASSERT_TRUE(h.first.size() <= agent::kMaxErrHeaderLen);
    TEST_ASSERT_TRUE(h.second.size() <= agent::kMaxErrHeaderLen);
  }
  // A clipped huge value is still not 0, so it never reads as "spent".
  TEST_ASSERT_EQUAL((int)RateLimit::PerMinute, (int)agent::rateLimitFromHeaders(rl));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_headers_measured_live_shapes);
  RUN_TEST(test_headers_rules);
  RUN_TEST(test_text_rules);
  RUN_TEST(test_tag_round_trips_every_kind);
  RUN_TEST(test_reply_copy_class_rules);
  RUN_TEST(test_default_transport_captures_error_headers_only);
  RUN_TEST(test_error_header_capture_is_bounded);
  UNITY_END();
  return 0;
}
