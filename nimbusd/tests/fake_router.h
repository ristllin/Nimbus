#pragma once
// fake_router - a local Cumulo router on 127.0.0.1 for the offline tests: a real HTTP
// server that answers with the exact JSON of the router refusal contract
// (docs-internal/router-refusal-contract.md in the cloud repo), and RouterRedirect, the
// real libcurl transport with its router host pointed there. A Virtual Nimbus reaches
// the router over HTTPS on a fixed host, so the bytes on the socket are the only thing
// faked: the rig, its transport, the wire, the park and the engine are all the real ones.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "daemon_http.h"
#include "loopback_server.h"
#include "nimbus/harness/http.h"

namespace ndtest {

class FakeRouter {
 public:
  struct Seen {
    std::string method, path, body;
  };
  struct Answer {
    int status = 200;
    std::string body;
  };
  // Decides each request. Runs on the server thread, under the server's lock.
  using Handler = std::function<Answer(const Seen&)>;

  explicit FakeRouter(Handler h) : handler_(std::move(h)) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(fd_, (sockaddr*)&a, sizeof a) != 0 || listen(fd_, 8) != 0 || getsockname(fd_, (sockaddr*)&a, &len) != 0)
      return;
    port_ = ntohs(a.sin_port);
    th_ = std::thread([this] { loop(); });
  }
  ~FakeRouter() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    close(fd_);
  }
  int port() const { return port_; }
  void setHandler(Handler h) {
    std::lock_guard<std::mutex> lk(mu_);
    handler_ = std::move(h);
  }
  std::vector<Seen> seen() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_;
  }
  void clearSeen() {
    std::lock_guard<std::mutex> lk(mu_);
    seen_.clear();
  }
  int paidRequests() const {   // the POSTs: GET /router/models is a free read
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (const Seen& s : seen_) n += s.method == "POST" ? 1 : 0;
    return n;
  }

 private:
  void loop() {
    while (!stop_) {
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, 25) <= 0) continue;
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      serve(c);
      close(c);
    }
  }
  void serve(int c) {
    std::string buf;
    char chunk[4096];
    for (int waited = 0; waited < 5000;) {
      const size_t want = requestSize(buf);
      if (want != std::string::npos && buf.size() >= want) break;
      pollfd p{c, POLLIN, 0};
      if (poll(&p, 1, 25) <= 0) {
        waited += 25;
        continue;
      }
      const ssize_t got = recv(c, chunk, sizeof chunk, 0);
      if (got <= 0) return;
      buf.append(chunk, size_t(got));
    }
    const size_t eol = buf.find("\r\n");
    const size_t sp1 = buf.find(' ');
    const size_t sp2 = buf.find(' ', sp1 + 1);
    if (eol == std::string::npos || sp1 == std::string::npos || sp2 == std::string::npos) return;
    Seen s;
    s.method = buf.substr(0, sp1);
    s.path = buf.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t head = buf.find("\r\n\r\n");
    s.body = head == std::string::npos ? std::string() : buf.substr(head + 4);
    Answer a;
    {
      std::lock_guard<std::mutex> lk(mu_);
      seen_.push_back(s);
      a = handler_(s);
    }
    const std::string out = response("HTTP/1.1 " + std::to_string(a.status) + " X", {"Content-Type: application/json"}, a.body);
    (void)send(c, out.data(), out.size(), 0);
  }

  mutable std::mutex mu_;
  Handler handler_;
  std::vector<Seen> seen_;
  std::atomic<bool> stop_{false};
  int fd_ = -1;
  int port_ = 0;
  std::thread th_;
};

// The real transport, with every router request sent to the fake one in cleartext.
class RouterRedirect : public agent::HttpTransport {
 public:
  RouterRedirect(std::string routerHost, int port) : host_(std::move(routerHost)), port_(port) {}
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    agent::HttpRequest r = req;
    if (r.host == host_) {
      r.host = "127.0.0.1";
      r.port = (uint16_t)port_;
      r.tls = false;
    }
    return real_.exec(r, out, err);
  }

 private:
  nimbusd::DaemonHttpTransport real_;
  std::string host_;
  int port_;
};

}  // namespace ndtest
