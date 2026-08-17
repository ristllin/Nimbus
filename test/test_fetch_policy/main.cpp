#include <unity.h>

#include <string>

#include "nimbus/orch/fetch_policy.h"

// fetch_policy - the trust ladder for files.fetch (W18). Under test: the policy
// decisions (off refuses, approve queues, scan/yolo are pump-ready), the
// owner-approval state machine, queue bounds/dedup, and the https-only URL
// parsing + redirect rules the download engine trusts.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static void test_off_refuses_and_names_the_setting() {
  FetchQueue q;
  std::string err;
  TEST_ASSERT_EQUAL(0, q.request(FetchPolicy::Off, "https://x.com/a.pdf", "p", "a.pdf", "web", err));
  TEST_ASSERT_TRUE_MESSAGE(err.find("turned off by the owner") != std::string::npos,
                           "the refusal must say WHO turned it off and where");
}

static void test_approve_queues_pending_then_owner_gates() {
  FetchQueue q;
  std::string err;
  uint32_t id = q.request(FetchPolicy::Approve, "https://x.com/a.pdf", "p", "a.pdf", "12345", err);
  TEST_ASSERT_TRUE(id > 0);
  TEST_ASSERT_EQUAL((int)FetchState::PendingApproval, (int)q.find(id)->state);
  // The pump must NOT see it as workable before the owner acts.
  TEST_ASSERT_NULL(q.firstIn(FetchState::Ready));
  TEST_ASSERT_TRUE(q.approve(id));
  TEST_ASSERT_NOT_NULL(q.firstIn(FetchState::Ready));
  // Approving twice is a no-op refusal, not a state corruption.
  TEST_ASSERT_FALSE(q.approve(id));
}

static void test_deny_finalizes() {
  FetchQueue q;
  std::string err;
  uint32_t id = q.request(FetchPolicy::Approve, "https://x.com/a.pdf", "p", "a.pdf", "w", err);
  TEST_ASSERT_TRUE(q.deny(id));
  TEST_ASSERT_EQUAL((int)FetchState::Denied, (int)q.find(id)->state);
  TEST_ASSERT_FALSE(q.approve(id));   // a denied request cannot be revived
}

static void test_scan_and_yolo_are_pump_ready() {
  FetchQueue q;
  std::string err;
  uint32_t a = q.request(FetchPolicy::Scan, "https://x.com/a.pdf", "p", "a.pdf", "w", err);
  uint32_t b = q.request(FetchPolicy::Yolo, "https://x.com/b.pdf", "p", "b.pdf", "w", err);
  TEST_ASSERT_TRUE(a > 0 && b > 0);
  TEST_ASSERT_EQUAL((int)FetchState::Ready, (int)q.find(a)->state);
  TEST_ASSERT_EQUAL((int)FetchState::Ready, (int)q.find(b)->state);
}

static void test_dedup_and_bounds() {
  FetchQueue q;
  std::string err;
  uint32_t id = q.request(FetchPolicy::Approve, "https://x.com/a.pdf", "p", "a.pdf", "w", err);
  TEST_ASSERT_TRUE(id > 0);
  // Same URL while live -> refused with the existing id named.
  TEST_ASSERT_EQUAL(0, q.request(FetchPolicy::Approve, "https://x.com/a.pdf", "p2", "b.pdf", "w", err));
  TEST_ASSERT_TRUE(err.find(std::to_string(id)) != std::string::npos);
  // Fill to kMax live requests -> the next is refused as full.
  for (int i = 0; i < (int)FetchQueue::kMax - 1; i++)
    TEST_ASSERT_TRUE(q.request(FetchPolicy::Approve,
        "https://x.com/f" + std::to_string(i), "p", "f" + std::to_string(i), "w", err) > 0);
  TEST_ASSERT_EQUAL(0, q.request(FetchPolicy::Approve, "https://x.com/over", "p", "o", "w", err));
  TEST_ASSERT_TRUE(err.find("full") != std::string::npos);
  // Finishing one frees a slot, and finished rows are trimmed at kDoneKeep.
  q.finish(id, FetchState::Failed, "test", 0);
  TEST_ASSERT_TRUE(q.request(FetchPolicy::Approve, "https://x.com/over", "p", "o", "w", err) > 0);
}

static void test_url_parse_https_only() {
  ParsedUrl p = parseHttpsUrl("https://arxiv.org/pdf/2401.1234v1.pdf");
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL_STRING("arxiv.org", p.host.c_str());
  TEST_ASSERT_EQUAL(443, p.port);
  TEST_ASSERT_EQUAL_STRING("/pdf/2401.1234v1.pdf", p.path.c_str());
  TEST_ASSERT_TRUE(parseHttpsUrl("https://h:8443/x").ok);
  TEST_ASSERT_EQUAL(8443, parseHttpsUrl("https://h:8443/x").port);
  TEST_ASSERT_EQUAL_STRING("/", parseHttpsUrl("https://h").path.c_str());
  // Refusals: plain http (MITM write primitive), creds-in-url, garbage ports.
  TEST_ASSERT_FALSE(parseHttpsUrl("http://x.com/a.pdf").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("https://user:pw@x.com/a").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h:99999/x").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h:abc/x").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("ftp://x/a").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("").ok);
}

static void test_redirect_rules() {
  ParsedUrl from = parseHttpsUrl("https://arxiv.org/pdf/x.pdf");
  // Absolute https: followed. Same-host absolute path: resolved.
  TEST_ASSERT_EQUAL_STRING("https://export.arxiv.org/pdf/x.pdf",
      resolveRedirect(from, "https://export.arxiv.org/pdf/x.pdf").c_str());
  TEST_ASSERT_EQUAL_STRING("https://arxiv.org/abs/x",
      resolveRedirect(from, "/abs/x").c_str());
  // http downgrade / protocol-relative / relative path: refused.
  TEST_ASSERT_EQUAL_STRING("", resolveRedirect(from, "http://evil.com/x").c_str());
  TEST_ASSERT_EQUAL_STRING("", resolveRedirect(from, "//evil.com/x").c_str());
  TEST_ASSERT_EQUAL_STRING("", resolveRedirect(from, "other.pdf").c_str());
  // Non-443 source port survives a same-host path redirect.
  ParsedUrl alt = parseHttpsUrl("https://h:8443/a");
  TEST_ASSERT_EQUAL_STRING("https://h:8443/b", resolveRedirect(alt, "/b").c_str());
}


// ⚠ prism (header injection): the path is printf'd into the HTTP request line,
// so ANY control byte in the URL is a request-smuggling primitive ("GET /x
// HTTP/1.0\r\nHost: other-vhost" serves a DIFFERENT origin than the one the
// owner approved and the scanner saw). Every byte < 0x21 and DEL must refuse.
static void test_control_bytes_in_url_refused() {
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h.com/x\r\nHost: evil.com\r\n\r\nGET /y").ok);
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h.com/x y").ok);        // space
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h.com/x\ty").ok);       // tab
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h.com/x\x7f").ok);      // DEL
  TEST_ASSERT_FALSE(parseHttpsUrl("https://h\rn.com/x").ok);       // host CR
  TEST_ASSERT_TRUE(parseHttpsUrl("https://h.com/a%20b?q=1&r=2").ok);  // encoded stays fine
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_off_refuses_and_names_the_setting);
  RUN_TEST(test_approve_queues_pending_then_owner_gates);
  RUN_TEST(test_deny_finalizes);
  RUN_TEST(test_scan_and_yolo_are_pump_ready);
  RUN_TEST(test_dedup_and_bounds);
  RUN_TEST(test_url_parse_https_only);
  RUN_TEST(test_redirect_rules);
  RUN_TEST(test_control_bytes_in_url_refused);
  return UNITY_END();
}
