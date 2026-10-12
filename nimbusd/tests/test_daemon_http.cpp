// test_daemon_http - offline (T2) proof that the daemon's curl transport keeps
// the error headers a request asks for (HttpRequest::errHeaders), over a real
// loopback socket. Before CUM-465 it recorded no response headers, so on a
// Virtual Nimbus a Mistral 429 fell back to its body text, which reads the same
// for every quota window ("Custom connector rate limit reached." read Unknown).
//
//   (a) the real Mistral head adapter over this transport: a 429 names its window
//       ([rl:day-utc] -> "resets at midnight UTC"), the CUM-465 regression;
//   (b) capture is the shared rule: error status, requested prefix matched
//       case-insensitively, names lowercased, values trimmed, each header once;
//   (c) a healthy response keeps nothing, and nothing is kept unless asked;
//   (d) bounded: a flood of matching headers or a huge value stays capped;
//   (e) only the final header block counts: a 1xx block before the 429 is not an
//       error, so its headers never decide the window;
//   (f) a failure is reported in the provider wire's words (CUM-538): "no
//       response" once the request went out, "request body truncated" when it
//       never fully left, "connect failed" before it was sent;
//   (g) so a hosted Z.ai sub cut after its POST is Timeout (may have started);
//   (h) a POST never rides a kept-alive connection, so libcurl never re-sends one
//       the peer read and then cut (it is sent, and billed, once).
// Every case runs on ONE transport (the persistent handle), so a plain request
// after a capturing one also proves the per-exchange sink never outlives it.
// Offline: a loopback server with canned responses; no keys, no external network.
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_provider_deps.h"
#include "daemon_http.h"
#include "loopback_server.h"
#include "nimbus/harness/rate_limit.h"
#include "test_util.h"

using ndtest::CannedServer;
using ndtest::response;
using nimbusd::DaemonHttpTransport;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static agent::HttpRequest loopbackGet(int port, const char* path) {
  agent::HttpRequest req;
  req.method = "GET";
  req.host = "127.0.0.1";
  req.port = uint16_t(port);
  req.tls = false;
  req.path = path;
  req.timeoutMs = 5000;
  return req;
}
static agent::HttpRequest loopbackGet(const CannedServer& srv) { return loopbackGet(srv.port(), "/v1/x"); }

// (a) The live 2026-09-27 shape of a spent daily Studio connector quota, through
// the real Mistral head adapter. The router base points it at the loopback server
// over plain HTTP; the header path through this transport is the one a direct
// TLS call to api.mistral.ai takes.
static void testMistralHead429NamesTheWindow(ndtest::Ctx& c, DaemonHttpTransport& t) {
  CannedServer srv({response("HTTP/1.1 429 Too Many Requests",
                             {"X-RateLimit-Limit-Custom-Minute: 5", "x-ratelimit-limit-custom-day: 50",
                              "x-ratelimit-remaining-custom-day: 0", "Content-Type: application/json"},
                             "{\"detail\":\"Custom connector rate limit reached.\"}")});
  harness_test::FakeProviderDeps d;
  agent::providers::ProviderDeps pd = d.contract();
  pd.http = &t;
  const std::string base = "http://127.0.0.1:" + std::to_string(srv.port());
  pd.routerBase = [base] { return base; };
  std::string conv, out, err;
  c.ok(!agent::providers::orchTurnMistral(pd, conv, "S", "U", out, err, nullptr, nullptr),
       "(a) the refused Mistral head turn fails");
  srv.join();
  c.ok(srv.requestLines().size() == 1 &&
           srv.requestLines()[0].rfind("POST /router/mistral/v1/conversations ", 0) == 0,
       "(a) the adapter's request reached the loopback server");
  c.eq(err, "conversations HTTP 429 [rl:day-utc]: Custom connector rate limit reached.",
       "(a) the 429 carries its quota window from the x-ratelimit-* headers");
  const std::string reply = agent::rateLimitReply(agent::rateLimitFromError(err));
  c.ok(has(reply, "resets at midnight UTC"), "(a) the owner's reply names the window and its reset");
}

// (b) The shared capture rule, through execJson (the path every adapter uses).
static void testErrorHeadersCapturedOnce(ndtest::Ctx& c, DaemonHttpTransport& t) {
  CannedServer srv({response("HTTP/1.1 429 Too Many Requests",
                             {"X-RateLimit-Remaining-Custom-Day:   0  ", "Content-Type: application/json",
                              "x-ratelimitish: 1", "X-RATELIMIT-LIMIT-CUSTOM-DAY:50"},
                             "{\"message\":\"slow down\"}")});
  agent::HeaderList rl;
  agent::HttpRequest req = loopbackGet(srv);
  req.errHeaderPrefix = "x-ratelimit-";
  req.errHeaders = &rl;
  JsonDocument doc, filter;
  filter["message"] = true;
  std::string err;
  c.eqi(t.execJson(req, doc, filter, err), 429, "(b) execJson returns the 429");
  c.eq(doc["message"] | "", "slow down", "(b) the body still parses");
  c.eqi((long)rl.size(), 2, "(b) exactly the two prefixed headers, each once (no double capture)");
  if (rl.size() != 2) return;
  c.eq(rl[0].first, "x-ratelimit-remaining-custom-day", "(b) name lowercased");
  c.eq(rl[0].second, "0", "(b) value trimmed");
  c.eq(rl[1].first, "x-ratelimit-limit-custom-day", "(b) a mixed-case name matches the prefix");
  c.eq(rl[1].second, "50", "(b) a value with no space after the colon");
}

// (c) A healthy response keeps nothing; a request that asks for nothing gets
// nothing, even on a 429 (and runs right after a capturing request, on the same
// persistent handle).
static void testNothingKeptUnlessErrorAndAsked(ndtest::Ctx& c, DaemonHttpTransport& t) {
  const std::vector<std::string> quota = {"x-ratelimit-remaining-req-minute: 0"};
  CannedServer srv({response("HTTP/1.1 200 OK", quota, "{}"),
                    response("HTTP/1.1 429 Too Many Requests", quota, "{}"),
                    response("HTTP/1.1 429 Too Many Requests", quota, "{}")});
  agent::HeaderList rl;
  agent::HttpRequest asked = loopbackGet(srv);
  asked.errHeaderPrefix = "x-ratelimit";
  asked.errHeaders = &rl;
  agent::HttpResponse resp;
  std::string err;
  c.ok(t.exec(asked, resp, err) && resp.status == 200 && rl.empty(),
       "(c) a healthy response keeps no header");
  agent::HttpRequest noPrefix = loopbackGet(srv);
  noPrefix.errHeaders = &rl;
  c.ok(t.exec(noPrefix, resp, err) && resp.status == 429 && rl.empty(),
       "(c) no prefix, nothing kept on a 429");
  const agent::HttpRequest plain = loopbackGet(srv);
  c.ok(t.exec(plain, resp, err) && resp.status == 429 && resp.headers.empty() && resp.body == "{}",
       "(c) a plain request on the reused handle gets its 429 and no headers");
}

// (d) A response is untrusted input: 40 matching headers and a 5000-byte value.
static void testCaptureIsBounded(ndtest::Ctx& c, DaemonHttpTransport& t) {
  std::vector<std::string> flood = {"x-ratelimit-remaining-custom-day: " + std::string(5000, '9')};
  for (int i = 0; i < 40; i++) flood.push_back("x-ratelimit-limit-x" + std::to_string(i) + "-minute: 5");
  CannedServer srv({response("HTTP/1.1 429 Too Many Requests", flood, "{}")});
  agent::HeaderList rl;
  agent::HttpRequest req = loopbackGet(srv);
  req.errHeaderPrefix = "x-ratelimit";
  req.errHeaders = &rl;
  agent::HttpResponse resp;
  std::string err;
  c.ok(t.exec(req, resp, err) && resp.status == 429, "(d) the flood arrives as a 429");
  c.eqi((long)rl.size(), (long)agent::kMaxErrHeaders, "(d) the header count is capped");
  bool capped = true;
  for (const auto& h : rl)
    capped = capped && h.first.size() <= agent::kMaxErrHeaderLen && h.second.size() <= agent::kMaxErrHeaderLen;
  c.ok(capped, "(d) every kept name and value is capped in length");
}

// (e) An informational block's headers would read as a 0-request plan
// (NotAllowed); only the final 429's headers may decide the window (DailyUtc).
static void testOnlyTheFinalBlockCounts(ndtest::Ctx& c, DaemonHttpTransport& t) {
  CannedServer srv({"HTTP/1.1 100 Continue\r\nx-ratelimit-limit-req-minute: 0\r\n\r\n" +
                    response("HTTP/1.1 429 Too Many Requests", {"x-ratelimit-remaining-custom-day: 0"}, "{}")});
  agent::HeaderList rl;
  agent::HttpRequest req = loopbackGet(srv);
  req.errHeaderPrefix = "x-ratelimit";
  req.errHeaders = &rl;
  agent::HttpResponse resp;
  std::string err;
  c.ok(t.exec(req, resp, err) && resp.status == 429, "(e) the final status is the 429");
  c.ok(rl.size() == 1 && rl[0].first == "x-ratelimit-remaining-custom-day",
       "(e) only the final block's header is kept");
  c.eqi((long)agent::rateLimitFromHeaders(rl), (long)agent::RateLimit::DailyUtc,
        "(e) the window is the 429's, not the 1xx block's");
}

// (f) CUM-538 gate: a failed exchange is reported in the words the provider wire
// reads, as the device transport does. curl's own texts ("Server returned
// nothing", "Timeout was reached") matched none of them, so a hosted Z.ai or
// Cumulo sub whose POST went out and was then cut read Network ("did not start")
// and the head was told to spawn the paid work again.
static agent::HttpRequest loopbackPost(int port, uint32_t timeoutMs) {
  agent::HttpRequest req;
  req.method = "POST";
  req.host = "127.0.0.1";
  req.port = uint16_t(port);
  req.tls = false;
  req.path = "/api/paas/v4/chat/completions";
  req.headers = {{"Content-Type", "application/json"}};
  req.body = "{\"model\":\"glm-5.3\",\"messages\":[]}";
  req.timeoutMs = timeoutMs;
  return req;
}
static void testFailuresSpeakTheWireWords(ndtest::Ctx& c, DaemonHttpTransport& t) {
  {
    CannedServer srv({""});   // reads the whole request, then closes: no status line
    agent::HttpResponse out;
    std::string err;
    c.ok(!t.exec(loopbackPost(srv.port(), 5000), out, err) && out.status == 0, "(f) a cut exchange fails");
    srv.join();
    c.eq(err, "Server returned nothing (no headers, no data): no response",
         "(f) sent, then cut: \"no response\" (the run may have started)");
  }
  {
    CannedServer srv({""}, 1500);   // reads the request, then silence past the deadline
    agent::HttpResponse out;
    std::string err;
    c.ok(!t.exec(loopbackPost(srv.port(), 400), out, err), "(f) a request past its deadline fails");
    srv.join();
    c.ok(err.rfind("Timeout was reached", 0) == 0 && has(err, ": no response"),
         "(f) sent, then the deadline: \"no response\" (got \"" + err + "\")");
  }
  {
    agent::HttpResponse out;
    std::string err;
    c.ok(!t.exec(loopbackPost(ndtest::closedLoopbackPort(), 5000), out, err), "(f) a refused connection fails");
    c.ok(err.rfind("connect failed: ", 0) == 0 && !has(err, "no response") && !has(err, "truncated"),
         "(f) nothing sent: \"connect failed\" (got \"" + err + "\")");
  }
  c.eq(DaemonHttpTransport::failureText(CURLE_SEND_ERROR, DaemonHttpTransport::Sent::Partly),
       "Failed sending data to the peer: request body truncated", "(f) a send error mid-body: never fully left");
  c.eq(DaemonHttpTransport::failureText(CURLE_RECV_ERROR, DaemonHttpTransport::Sent::Partly),
       "Failure when receiving data from the peer: request body truncated", "(f) a short upload: never fully left");
  c.eq(DaemonHttpTransport::failureText(CURLE_SEND_ERROR, DaemonHttpTransport::Sent::Whole),
       "Failed sending data to the peer: no response",
       "(f) a send error after the whole request went out (HTTP/2 frames mid-wait): it may have started");
}

// (g) The real Z.ai sub dispatch over this transport: a POST that went out and was
// cut is Timeout (the head is told it may have started), a refused connection is
// Network (it did not start).
static void testCutSubMayHaveStarted(ndtest::Ctx& c, DaemonHttpTransport& t) {
  harness_test::FakeProviderDeps d;
  agent::providers::ProviderDeps pd = d.contract();
  pd.http = &t;
  agent::Directive dir;
  dir.category = "research";
  dir.instruction = "summarize the GLM release notes";
  char job[72] = {0};
  CannedServer srv({""});
  agent::providers::CompatEndpoint ep;
  const std::string host = "127.0.0.1";
  ep.host = host.c_str();
  ep.port = uint16_t(srv.port());
  ep.tls = false;
  ep.basePath = "/api/paas/v4";
  ep.key = "zai_sk_TEST_CUT";
  ep.model = "glm-5.3";
  ep.backendTag = "zai";
  c.eqi((long)agent::providers::openaiCompatDispatch(pd, ep, dir, job), (long)agent::FabricErr::Timeout,
        "(g) a Z.ai sub cut after its POST went out is Timeout (may have started)");
  srv.join();
  c.eqi((long)srv.requestLines().size(), 1, "(g) the POST reached the server once");
  ep.port = uint16_t(ndtest::closedLoopbackPort());
  c.eqi((long)agent::providers::openaiCompatDispatch(pd, ep, dir, job), (long)agent::FabricErr::Network,
        "(g) a sub whose connection was refused is Network (did not start)");
}

// (h) CUM-538 gate (High): libcurl re-sends a request on a new connection when a
// REUSED keep-alive connection dies before any answer, and it does that for a POST
// too. A gateway that read the POST, ran the paid sub and then closed would bill it
// twice while the owner is told once that it may have started. Connection 1
// answers two GETs (a model list, a poll) and stays open; the POST that follows
// is read and cut without a status line, and so is anything sent after it.
static int postsSeen(const ndtest::KeepAliveServer& srv) {
  int n = 0;
  for (const auto& s : srv.seen()) n += s.line.rfind("POST ", 0) == 0;
  return n;
}
static void testPostNeverResentOnAReusedConnection(ndtest::Ctx& c, DaemonHttpTransport& t) {
  using ndtest::keepAliveResponse;
  {
    ndtest::KeepAliveServer srv({keepAliveResponse("HTTP/1.1 200 OK", "{\"data\":[]}"),
                                 keepAliveResponse("HTTP/1.1 200 OK", "{\"data\":[]}")});
    const agent::HttpRequest get = loopbackGet(srv.port(), "/api/paas/v4/models");
    agent::HttpResponse out;
    std::string err;
    const bool g1 = t.exec(get, out, err) && out.status == 200;
    const bool g2 = t.exec(get, out, err) && out.status == 200;
    const bool sent = t.exec(loopbackPost(srv.port(), 5000), out, err);
    srv.stop();
    const auto& seen = srv.seen();
    c.ok(g1 && g2 && seen.size() >= 2 && seen[0].conn == 1 && seen[1].conn == 1,
         "(h) the GETs share one kept-alive connection (the warm cache still works)");
    c.eqi(postsSeen(srv), 1, "(h) the cut POST reached the server exactly once (never re-sent)");
    c.ok(seen.size() == 3 && seen[2].line.rfind("POST ", 0) == 0 && seen[2].conn == 2,
         "(h) the POST went out on a new connection, not the kept-alive one");
    c.ok(!sent && out.status == 0 && has(err, ": no response"),
         "(h) the cut POST reads \"no response\" (got \"" + err + "\")");
  }
  {
    // The same through the real Z.ai sub dispatch: a warm connection to the
    // gateway, then the sub's POST is cut. Once, and Timeout (may have started).
    ndtest::KeepAliveServer srv({keepAliveResponse("HTTP/1.1 200 OK", "{\"data\":[]}")});
    harness_test::FakeProviderDeps d;
    agent::providers::ProviderDeps pd = d.contract();
    pd.http = &t;
    const agent::HttpRequest warm = loopbackGet(srv.port(), "/api/paas/v4/models");
    agent::HttpResponse out;
    std::string err;
    const bool warmed = t.exec(warm, out, err) && out.status == 200;
    agent::Directive dir;
    dir.category = "research";
    dir.instruction = "summarize the GLM release notes";
    char job[72] = {0};
    agent::providers::CompatEndpoint ep;
    const std::string host = "127.0.0.1";
    ep.host = host.c_str();
    ep.port = uint16_t(srv.port());
    ep.tls = false;
    ep.basePath = "/api/paas/v4";
    ep.key = "zai_sk_TEST_REUSE";
    ep.model = "glm-5.3";
    ep.backendTag = "zai";
    const agent::FabricErr e = agent::providers::openaiCompatDispatch(pd, ep, dir, job);
    srv.stop();
    c.ok(warmed, "(h) the gateway connection was warm before the sub");
    c.eqi(postsSeen(srv), 1, "(h) the Z.ai sub cut after its POST was sent once (billed once)");
    c.eqi((long)e, (long)agent::FabricErr::Timeout, "(h) and it is Timeout (may have started), never respawned");
  }
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  ndtest::Ctx c;
  std::printf("=== daemon_http error-header capture (T2, loopback) ===\n");
  DaemonHttpTransport t;
  testMistralHead429NamesTheWindow(c, t);
  testErrorHeadersCapturedOnce(c, t);
  testNothingKeptUnlessErrorAndAsked(c, t);
  testCaptureIsBounded(c, t);
  testOnlyTheFinalBlockCounts(c, t);
  testFailuresSpeakTheWireWords(c, t);
  testCutSubMayHaveStarted(c, t);
  testPostNeverResentOnAReusedConnection(c, t);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
