#pragma once
// loopback_server - a canned HTTP server on 127.0.0.1 for the offline transport
// tests: each scripted response answers one connection after that connection's
// whole request was read, then the connection closes. An empty response answers
// nothing (the peer cuts the exchange after the request went out).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace ndtest {

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
