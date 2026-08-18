// test_http_replay - loopback request assembly (token injection/override, hop-by-hop
// strip, Content-Length) and the response parser (Content-Length, chunked, until-close,
// header allowlist, byte caps, truncation).
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/cloud/http_replay.h"

using namespace nimbus::cloud::http_replay;

void setUp() {}
void tearDown() {}

static std::string bodyStr(const ResponseParser& p) {
  return std::string(p.body().begin(), p.body().end());
}

static void test_request_head_injects_token() {
  Headers h = {{"User-Agent", "Mozilla"}, {"X-Nimbus-Token", "ATTACKER"}, {"Host", "evil"},
               {"Connection", "keep-alive"}, {"Content-Length", "999"}};
  std::string head = buildRequestHead("POST", "/api/config?t=x", h, 12, "REALTOKEN");
  TEST_ASSERT_TRUE(head.find("POST /api/config?t=x HTTP/1.1\r\n") == 0);
  TEST_ASSERT_TRUE(head.find("Host: 127.0.0.1\r\n") != std::string::npos);
  // Our token wins; the attacker-supplied one is dropped.
  TEST_ASSERT_TRUE(head.find("X-Nimbus-Token: REALTOKEN\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(head.find("ATTACKER") == std::string::npos);
  TEST_ASSERT_TRUE(head.find("Host: evil") == std::string::npos);
  TEST_ASSERT_TRUE(head.find("Connection: keep-alive") == std::string::npos);
  TEST_ASSERT_TRUE(head.find("Content-Length: 12\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(head.find("Content-Length: 999") == std::string::npos);
  TEST_ASSERT_TRUE(head.find("Connection: close\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(head.find("User-Agent: Mozilla\r\n") != std::string::npos);
  TEST_ASSERT_TRUE(head.rfind("\r\n\r\n") == head.size() - 4);
}

static void test_response_content_length() {
  ResponseParser p(4096);
  std::string r =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
      "Connection: close\r\n\r\nhello world";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_INT(200, p.status());
  TEST_ASSERT_EQUAL_STRING("hello world", bodyStr(p).c_str());
  // Allowlist: content-type kept, connection/content-length dropped.
  bool sawCT = false, sawConn = false, sawCL = false;
  for (auto& h : p.headers()) {
    std::string n = h.first;
    for (char& c : n) c = (char)tolower(c);
    if (n == "content-type") sawCT = true;
    if (n == "connection") sawConn = true;
    if (n == "content-length") sawCL = true;
  }
  TEST_ASSERT_TRUE(sawCT);
  TEST_ASSERT_FALSE(sawConn);
  TEST_ASSERT_FALSE(sawCL);
}

static void test_response_chunked() {
  ResponseParser p(4096);
  // Two chunks + terminator, exactly what AsyncWebServer emits for chunked routes.
  std::string r =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_STRING("Hello World", bodyStr(p).c_str());
}

static void test_response_chunked_dribbled() {
  ResponseParser p(4096);
  std::string r =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "4\r\nabcd\r\n0\r\n\r\n";
  for (char c : r) p.feed(reinterpret_cast<const uint8_t*>(&c), 1);
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_STRING("abcd", bodyStr(p).c_str());
}

static void test_response_until_close() {
  ResponseParser p(4096);
  std::string r = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nstreamed";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_FALSE(p.complete());  // no length/chunked -> waits for close
  p.endOfStream();
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_STRING("streamed", bodyStr(p).c_str());
}

static void test_response_no_body_204() {
  ResponseParser p(4096);
  std::string r = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_EQUAL_INT(204, p.status());
  TEST_ASSERT_EQUAL_UINT(0, p.body().size());
}

static void test_response_truncated_is_error() {
  ResponseParser p(4096);
  std::string r = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly-ten!!";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_FALSE(p.complete());
  p.endOfStream();  // socket closed early
  TEST_ASSERT_TRUE(p.error());
}

static void test_response_overflow_caps() {
  ResponseParser p(8);  // 8-byte cap
  std::string r = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n01234567890123456789";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_TRUE(p.complete());
  TEST_ASSERT_TRUE(p.overflow());
  TEST_ASSERT_EQUAL_UINT(8, p.body().size());
}

// Security: a CRLF in a header value must not inject a new header line, and a control
// char in the method/path must fail the whole request (request-smuggling defense).
static void test_request_head_rejects_crlf() {
  Headers h = {{"X-Evil", "a\r\nTransfer-Encoding: chunked"}, {"Good", "ok"}};
  std::string head = buildRequestHead("GET", "/api/state", h, 0, "T");
  TEST_ASSERT_FALSE(head.empty());
  TEST_ASSERT_TRUE(head.find("Transfer-Encoding") == std::string::npos);  // injection dropped
  TEST_ASSERT_TRUE(head.find("X-Evil") == std::string::npos);             // the bad header skipped
  TEST_ASSERT_TRUE(head.find("Good: ok\r\n") != std::string::npos);       // clean header kept
  // CRLF in the request line fails the whole request.
  TEST_ASSERT_TRUE(buildRequestHead("GET", "/x\r\nInjected: 1", h, 0, "T").empty());
  TEST_ASSERT_TRUE(buildRequestHead("GE\r\nT", "/x", h, 0, "T").empty());
}

// Security: an absurd chunk size must be rejected (not wrap the 32-bit length math into
// a heap over-read), and the overflow copy stays within the chunk's actual bytes.
static void test_chunked_rejects_huge_size() {
  ResponseParser p(4096);
  std::string r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFF\r\nAAAA";
  p.feed(reinterpret_cast<const uint8_t*>(r.data()), r.size());
  TEST_ASSERT_TRUE(p.error());       // rejected, no OOB read
  TEST_ASSERT_FALSE(p.complete());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_request_head_injects_token);
  RUN_TEST(test_request_head_rejects_crlf);
  RUN_TEST(test_response_content_length);
  RUN_TEST(test_response_chunked);
  RUN_TEST(test_response_chunked_dribbled);
  RUN_TEST(test_response_until_close);
  RUN_TEST(test_response_no_body_204);
  RUN_TEST(test_response_truncated_is_error);
  RUN_TEST(test_response_overflow_caps);
  RUN_TEST(test_chunked_rejects_huge_size);
  return UNITY_END();
}
