#pragma once
// loopback_server - canned HTTP servers on 127.0.0.1 for the offline transport
// tests. CannedServer: each scripted response answers one connection after that
// connection's whole request was read, then the connection closes. KeepAliveServer:
// connections stay open across requests, as a provider gateway's do. In both, an
// empty response answers nothing (the peer cuts the exchange after the request
// went out).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ndtest {

// The total size of the request at the front of `buf` (headers plus a
// Content-Length body) once its headers are in, else npos.
inline size_t requestSize(const std::string& buf) {
  const size_t end = buf.find("\r\n\r\n");
  if (end == std::string::npos) return std::string::npos;
  std::string head = buf.substr(0, end);
  for (char& ch : head) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  const size_t cl = head.find("content-length:");
  return end + 4 + (cl == std::string::npos ? 0 : std::strtoul(head.c_str() + cl + 15, nullptr, 10));
}

// A raw HTTP/1.1 response; every one closes its connection.
inline std::string response(const std::string& statusLine, const std::vector<std::string>& headers,
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
  // `holdMs`: how long an empty response holds the connection open (read, then
  // silence) before closing it; 0 closes at once.
  explicit CannedServer(std::vector<std::string> script, int holdMs = 0)
      : script_(std::move(script)), holdMs_(holdMs) {
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
      if (want == std::string::npos) want = requestSize(req);
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
      if (resp.empty() && holdMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(holdMs_));
      (void)send(c, resp.data(), resp.size(), 0);
      close(c);
    }
  }

  std::vector<std::string> script_;
  int holdMs_ = 0;
  std::vector<std::string> lines_;
  int fd_ = -1;
  int port_ = 0;
  std::thread th_;
};

// A raw HTTP/1.1 response that leaves its connection open (keep-alive is the
// HTTP/1.1 default), so the client may send its next request on the same socket.
inline std::string keepAliveResponse(const std::string& statusLine, const std::string& body) {
  return statusLine + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// A server that keeps connections alive, the way a provider gateway does: every
// request read (on any connection) takes the next scripted response; a non-empty
// one is sent and the connection stays open for the next request, an empty one
// (or a request past the end of the script) closes that connection without a
// status line. Each connection is served on its own thread, so a client that
// opens a new connection while an earlier one idles is answered at once. It keeps
// accepting until stop(), so a request a client re-sends on a new connection is
// seen and recorded too.
class KeepAliveServer {
 public:
  struct Seen {
    int conn;          // 1 = the first connection accepted
    std::string line;  // the request line
  };

  explicit KeepAliveServer(std::vector<std::string> script) : script_(std::move(script)) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(fd_, (sockaddr*)&a, sizeof a) != 0 || listen(fd_, 8) != 0 ||
        getsockname(fd_, (sockaddr*)&a, &len) != 0)
      return;
    port_ = ntohs(a.sin_port);
    acceptor_ = std::thread([this] { acceptLoop(); });
  }
  ~KeepAliveServer() {
    stop();
    close(fd_);
  }
  int port() const { return port_; }
  // Stops accepting and closes every open connection; call it once the client's
  // exchange has returned (any re-send happens inside that exchange).
  void stop() {
    stop_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    for (auto& t : conns_)
      if (t.joinable()) t.join();
    conns_.clear();
  }
  // Every request read, in order; read it only after stop().
  const std::vector<Seen>& seen() const { return seen_; }

 private:
  // Waits for `fd` to be readable in short slices, so stop() is never kept
  // waiting; false on stop or after 5 s of silence.
  bool readable(int fd) const {
    for (int waited = 0; waited < 5000 && !stop_; waited += 25) {
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, 25) > 0) return true;
    }
    return false;
  }
  // One whole request (headers + Content-Length body) off `c`; bytes past it stay
  // in `buf` for the next one. Empty when the peer closed or went quiet.
  std::string readRequest(int c, std::string& buf) const {
    char chunk[4096];
    for (;;) {
      const size_t want = requestSize(buf);
      if (want != std::string::npos && buf.size() >= want) {
        std::string req = buf.substr(0, want);
        buf.erase(0, want);
        return req;
      }
      if (!readable(c)) return std::string();
      const ssize_t got = recv(c, chunk, sizeof chunk, 0);
      if (got <= 0) return std::string();
      buf.append(chunk, size_t(got));
    }
  }
  void serveConnection(int c, int conn) {
    std::string buf;
    for (;;) {
      const std::string req = readRequest(c, buf);
      if (req.empty()) break;
      std::string resp;
      {
        std::lock_guard<std::mutex> lk(mu_);
        seen_.push_back({conn, req.substr(0, req.find("\r\n"))});
        if (next_ < script_.size()) resp = script_[next_];
        next_++;
      }
      if (resp.empty()) break;  // read the whole request, then cut it
      (void)send(c, resp.data(), resp.size(), 0);
    }
    close(c);
  }
  void acceptLoop() {
    int conn = 0;
    while (!stop_) {
      if (!readable(fd_)) continue;
      const int c = accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      const int id = ++conn;
      conns_.emplace_back([this, c, id] { serveConnection(c, id); });
    }
  }

  std::vector<std::string> script_;
  size_t next_ = 0;
  std::mutex mu_;
  std::vector<Seen> seen_;
  std::vector<std::thread> conns_;
  std::atomic<bool> stop_{false};
  int fd_ = -1;
  int port_ = 0;
  std::thread acceptor_;
};

// A loopback port with nothing listening on it (bound, then closed).
inline int closedLoopbackPort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof a;
  int port = 0;
  if (bind(fd, (sockaddr*)&a, sizeof a) == 0 && getsockname(fd, (sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
  close(fd);
  return port;
}

}  // namespace ndtest
