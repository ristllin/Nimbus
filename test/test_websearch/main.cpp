// test_websearch - the portable web.search path (agent::websearch) over a
// scripted transport.
//
// The suite exists because of a field bug that never once worked: the device
// adapter read the Tavily response into a buffer capped at 6000 bytes, while
// real replies for the shipped call shape (max_results=5 + include_answer)
// measure 7-8 KB. Every search truncated mid-JSON, failed to parse, and surfaced
// as "web search failed (network / no results)" - a message that was wrong about
// the cause in every one of the six ways the old code could fail.
//
// So the assertions here are mostly about honesty: a large body must parse, and
// each distinct failure must NAME itself.
#include <unity.h>

#include <string>

#include "../support/fake_http.h"
#include "nimbus/harness/websearch.h"

using harness_test::Exchange;
using harness_test::FakeHttpTransport;

namespace {

// A response shaped exactly like Tavily's, padded to `bytes` via the per-result
// `content` fields - the same way a real reply gets big.
std::string bigTavilyBody(size_t bytes) {
  std::string filler;
  const char* lorem =
      "The summit concluded with a joint communique covering trade and security. ";
  while (filler.size() < bytes) filler += lorem;

  std::string b = R"({"query":"world news","follow_up_questions":null,)"
                  R"("answer":"Several major stories developed today.",)"
                  R"("images":[],"results":[)";
  for (int i = 0; i < 5; i++) {
    if (i) b += ",";
    b += R"({"title":"Headline )" + std::to_string(i) +
         R"(","url":"https://example.com/)" + std::to_string(i) +
         R"(","score":0.9,"raw_content":null,"content":")" +
         filler.substr(0, bytes / 5) + R"("})";
  }
  b += R"(],"response_time":1.2,"request_id":"abc-123"})";
  return b;
}

const char* kSmallBody =
    R"({"answer":"Paris is the capital of France.","results":[)"
    R"({"title":"France","url":"https://ex.com/fr","content":"France is a country in Europe."})"
    R"(],"response_time":0.4})";

}  // namespace

// --- the regression this suite was written for -----------------------------

void test_large_response_parses(void) {
  // ~14 KB - comfortably past both the old 6000-byte cap and a real reply.
  const std::string body = bigTavilyBody(14000);
  TEST_ASSERT_GREATER_THAN_UINT32(6000, (uint32_t)body.size());

  FakeHttpTransport http;
  http.script.push_back(Exchange{"api.tavily.com", "/search", 200, body, ""});

  auto r = agent::websearch::search(http, "k", "world news", 5);

  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.err.c_str());
  TEST_ASSERT_TRUE(r.digest.find("Several major stories") != std::string::npos);
  TEST_ASSERT_TRUE(r.digest.find("Headline 0") != std::string::npos);
  TEST_ASSERT_TRUE(r.digest.find("Headline 4") != std::string::npos);
}

void test_very_large_response_still_parses(void) {
  // A deep-search reply can reach tens of KB. The streaming filter means size
  // simply is not a failure mode any more.
  //
  // ⚠ This test asserts the LAST hit, not the first. Asserting "Headline 0"
  // alone passed even with the 6000-byte cap reintroduced, because the head of
  // the document survives truncation - a test that could not see the very bug
  // the suite exists for.
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 200, bigTavilyBody(120000), ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.err.c_str());
  TEST_ASSERT_TRUE(r.digest.find("Headline 0") != std::string::npos);
  TEST_ASSERT_TRUE(r.digest.find("Headline 4") != std::string::npos);
}

// A body that arrives but does not parse must be an ERROR, never a partial
// digest. ArduinoJson retains everything it read before the break, so the
// fragment looks like a valid thin answer - the model would have been handed a
// headline with its sources silently dropped.
void test_truncated_body_is_an_error_not_a_partial_answer(void) {
  const std::string full = bigTavilyBody(14000);
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 200, full.substr(0, 6000), ""});

  auto r = agent::websearch::search(http, "k", "q", 5);

  TEST_ASSERT_FALSE_MESSAGE(r.ok, "a truncated response was reported as success");
  TEST_ASSERT_TRUE(r.err.find("parse") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(r.digest.empty(),
                           "no digest may be rendered from a partial document");
}

// An HTML error page must not read as "the web had nothing to say". Filtered
// parsing of a non-object body returns Ok with an EMPTY document, so this case
// is invisible to a parse-error check alone and needs the shape check.
void test_html_error_page_is_not_reported_as_no_results(void) {
  for (const char* body : {"<html>502 Bad Gateway</html>",
                           "<!DOCTYPE html><title>Sign in to the network</title>",
                           "not json at all"}) {
    FakeHttpTransport http;
    http.script.push_back(Exchange{"", "", 200, body, ""});
    auto r = agent::websearch::search(http, "k", "q", 5);
    TEST_ASSERT_FALSE_MESSAGE(r.ok, body);
    TEST_ASSERT_TRUE(r.digest.empty());
  }
}

// The digest handed to the model must stay small no matter how fat the response
// was - that bound belongs on the OUTPUT, which is what the old cap confused.
void test_digest_is_bounded_regardless_of_response_size(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 200, bigTavilyBody(120000), ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_LESS_THAN_UINT32(3000, (uint32_t)r.digest.size());
}

// --- request shape ---------------------------------------------------------

void test_request_shape(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"api.tavily.com", "/search", 200, kSmallBody, ""});
  agent::websearch::search(http, "secret-key", "capital of France", 3);

  TEST_ASSERT_EQUAL_size_t(1, http.seen.size());
  const auto& req = http.seen[0];
  TEST_ASSERT_EQUAL_STRING("POST", req.method.c_str());
  TEST_ASSERT_EQUAL_STRING("api.tavily.com", req.host.c_str());
  TEST_ASSERT_EQUAL_STRING("/search", req.path.c_str());
  TEST_ASSERT_TRUE(req.tls);

  bool auth = false, ident = false;
  for (const auto& h : req.headers) {
    if (h.first == "Authorization" && h.second == "Bearer secret-key") auth = true;
    if (h.first == "Accept-Encoding" && h.second == "identity") ident = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(auth, "key must ride the Authorization header");
  TEST_ASSERT_TRUE_MESSAGE(ident, "must ask for an uncompressed body");

  TEST_ASSERT_TRUE(req.body.find("capital of France") != std::string::npos);
  TEST_ASSERT_TRUE(req.body.find("\"max_results\":3") != std::string::npos);
  // The key must NOT be duplicated into the body now that it rides the header.
  TEST_ASSERT_TRUE(req.body.find("secret-key") == std::string::npos);
}

void test_max_results_is_clamped(void) {
  for (int req : {0, -5, 99}) {
    FakeHttpTransport http;
    http.script.push_back(Exchange{"", "", 200, kSmallBody, ""});
    agent::websearch::search(http, "k", "q", req);
    const std::string& b = http.seen[0].body;
    const int want = req > 10 ? 10 : 1;
    TEST_ASSERT_TRUE(b.find("\"max_results\":" + std::to_string(want)) !=
                     std::string::npos);
  }
}

// --- every failure must name itself ----------------------------------------

void test_missing_key_says_so(void) {
  FakeHttpTransport http;
  auto r = agent::websearch::search(http, "", "q", 5);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.err.find("key") != std::string::npos);
  // ...and it must not have spent a network call to find out.
  TEST_ASSERT_EQUAL_size_t(0, http.seen.size());
}

void test_transport_failure_reports_the_transport_reason(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 0, "", "connect failed"});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.err.find("connect failed") != std::string::npos);
}

void test_401_reports_the_providers_message(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{
      "", "", 401, R"({"detail":{"error":"Unauthorized: missing or invalid API key."}})", ""});
  auto r = agent::websearch::search(http, "bad", "q", 5);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.err.find("401") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(r.err.find("invalid API key") != std::string::npos,
                           "the provider's own explanation must survive to the caller");
}

void test_429_is_distinguishable_from_a_network_failure(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 429, "{}", ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.err.find("429") != std::string::npos);
  TEST_ASSERT_TRUE(r.err.find("network") == std::string::npos);
}

void test_empty_query_is_refused_without_a_call(void) {
  FakeHttpTransport http;
  auto r = agent::websearch::search(http, "k", "", 5);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL_size_t(0, http.seen.size());
}

// --- "no results" is a SUCCESS, not a failure ------------------------------
// The old code returned "" for both, so the caller could not tell "the web has
// nothing" from "the search never ran".

void test_genuinely_empty_result_set_is_success(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 200, R"({"answer":"","results":[]})", ""});
  auto r = agent::websearch::search(http, "k", "asdkjhasd", 5);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, "an empty result set is an answer, not an error");
  TEST_ASSERT_TRUE(r.digest.find("no results") != std::string::npos);
  TEST_ASSERT_TRUE(r.err.empty());
}

void test_answer_only_response_is_usable(void) {
  FakeHttpTransport http;
  http.script.push_back(
      Exchange{"", "", 200, R"({"answer":"42.","results":[]})", ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.digest.find("42.") != std::string::npos);
}

void test_results_without_an_answer_are_usable(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{
      "", "", 200,
      R"({"results":[{"title":"T","url":"https://u","content":"C"}]})", ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.digest.find("https://u") != std::string::npos);
}

// --- rendering -------------------------------------------------------------

void test_digest_honours_max_results(void) {
  FakeHttpTransport http;
  http.script.push_back(Exchange{"", "", 200, bigTavilyBody(2000), ""});
  auto r = agent::websearch::search(http, "k", "q", 2);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.digest.find("Headline 0") != std::string::npos);
  TEST_ASSERT_TRUE(r.digest.find("Headline 1") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(r.digest.find("Headline 2") == std::string::npos,
                           "more hits rendered than the caller asked for");
}

void test_missing_fields_do_not_produce_garbage(void) {
  FakeHttpTransport http;
  http.script.push_back(
      Exchange{"", "", 200, R"({"results":[{"url":"https://u"}]})", ""});
  auto r = agent::websearch::search(http, "k", "q", 5);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.digest.find("(untitled)") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_large_response_parses);
  RUN_TEST(test_very_large_response_still_parses);
  RUN_TEST(test_truncated_body_is_an_error_not_a_partial_answer);
  RUN_TEST(test_html_error_page_is_not_reported_as_no_results);
  RUN_TEST(test_digest_is_bounded_regardless_of_response_size);
  RUN_TEST(test_request_shape);
  RUN_TEST(test_max_results_is_clamped);
  RUN_TEST(test_missing_key_says_so);
  RUN_TEST(test_transport_failure_reports_the_transport_reason);
  RUN_TEST(test_401_reports_the_providers_message);
  RUN_TEST(test_429_is_distinguishable_from_a_network_failure);
  RUN_TEST(test_empty_query_is_refused_without_a_call);
  RUN_TEST(test_genuinely_empty_result_set_is_success);
  RUN_TEST(test_answer_only_response_is_usable);
  RUN_TEST(test_results_without_an_answer_are_usable);
  RUN_TEST(test_digest_honours_max_results);
  RUN_TEST(test_missing_fields_do_not_produce_garbage);
  return UNITY_END();
}
