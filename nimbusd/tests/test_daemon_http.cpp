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
//       error, so its headers never decide the window.
// Every case runs on ONE transport (the persistent handle), so a plain request
// after a capturing one also proves the per-exchange sink never outlives it.
// Offline: a loopback server with canned responses; no keys, no external network.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_provider_deps.h"
#include "daemon_http.h"
#include "nimbus/harness/rate_limit.h"
#include "test_util.h"

using nimbusd::DaemonHttpTransport;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// A raw HTTP/1.1 response; every one closes its connection.
static std::string response(const std::string& statusLine, const std::vector<std::string>& headers,
                            const std::string& body) {
  std::string r = statusLine + "\r\n";
  for (const auto& h : headers) r += h + "\r\n";
  return r + "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// Serves each scripted response to one loopback connection, after reading that
// connection's whole request, then closes it. Never hangs the suite: an accept
// waits at most 5 s and a read is capped by SO_RCVTIMEO.
class CannedServer {
 public:
  explicit CannedServer(std::vector<std::string> script) : script_(std::move(script)) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof a;
    if (bind(fd_, (sockaddr*)&a, sizeof a) != 0 || listen(fd_, 4) != 0 ||
        getsockname(fd_, (sockaddr*)&a, &len) != 0)
      return;
    port_ = ntohs(a.sin_port);
    th_ = std::thread([this] { serve(); });
  }
  ~CannedServer() {
    join();
    close(fd_);
  }
  void join() {
    if (th_.joinable()) th_.join();
  }
  int port() const { return port_; }
  // The first line of each request served; read it only after join().
  const std::vector<std::string>& requestLines() const { return lines_; }

 private:
  static std::string readRequest(int c) {
    std::string req;
    char buf[4096];
    size_t want = std::string::npos;  // total bytes once the headers are in
    while (req.size() < want) {
      const ssize_t got = recv(c, buf, sizeof buf, 0);
      if (got <= 0) break;
      req.append(buf, size_t(got));
      const size_t end = req.find("\r\n\r\n");
      if (want != std::string::npos || end == std::string::npos) continue;
      std::string head = req.substr(0, end);
      for (char& ch : head) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      const size_t cl = head.find("content-length:");
      want = end + 4 + (cl == std::string::npos ? 0 : std::strtoul(head.c_str() + cl + 15, nullptr, 10));
    }
    return req;
  }
  void serve() {
    for (const auto& resp : script_) {
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, 5000) <= 0) return;
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0) return;
      timeval tv{5, 0};
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      const std::string req = readRequest(c);
      lines_.push_back(req.substr(0, req.find("\r\n")));
      (void)send(c, resp.data(), resp.size(), 0);
      close(c);
    }
  }

  std::vector<std::string> script_;
  std::vector<std::string> lines_;
  int fd_ = -1;
  int port_ = 0;
  std::thread th_;
};

static agent::HttpRequest loopbackGet(const CannedServer& srv) {
  agent::HttpRequest req;
  req.method = "GET";
  req.host = "127.0.0.1";
  req.port = uint16_t(srv.port());
  req.tls = false;
  req.path = "/v1/x";
  req.timeoutMs = 5000;
  return req;
}

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
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
