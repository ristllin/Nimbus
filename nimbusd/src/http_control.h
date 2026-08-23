#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "engine_thread.h"

// http_control - the daemon's LOCAL control surface, bound 127.0.0.1 ONLY.
//
// This is the seam the Phase-1 relay sidecar forwards each tunneled HTTP frame
// to (injecting the instance's web token); nimbusd keeps the token gate on
// everything here, so the sidecar's forward is authenticated exactly like a
// physical device's web surface. Bound to loopback so nothing but the co-located
// sidecar can reach it (no hostNetwork, §3.4).
//
// A deliberately tiny HTTP/1.0 server (one request per connection, Connection:
// close) - enough for the sidecar's one-request-per-frame model and the Docker
// healthcheck. State/health reads are served from the engine-thread SNAPSHOT, so
// they never queue behind a turn; /mcp is dispatched onto the engine thread and
// awaited.
//
//   GET  /healthz    liveness  (never gated - the kubelet probe)
//   GET  /readyz     readiness (200 iff the engine thread is running)
//   GET  /api/state  the state snapshot as JSON (gated)
//   POST /api/message {"chat_id","text"} -> 202, enqueues a turn (gated)
//   POST /mcp        JSON-RPC to the tool registry, run on the engine (gated)
namespace nimbusd {

class HttpControl {
 public:
  // `token` empty = no gate (dev). Non-empty = require Authorization: Bearer or
  // ?token= on every path except /healthz.
  HttpControl(EngineThread* eng, const std::string& bindAddr, int port,
              std::string token)
      : eng_(eng), bindAddr_(bindAddr), port_(port), token_(std::move(token)) {}
  ~HttpControl() { stop(); }

  // Bind + listen synchronously (so a caller/test knows the port is ready), then
  // serve on a background thread. Returns the bound port (useful when port==0),
  // or -1 on failure.
  int start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return -1;
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port_);
    addr.sin_addr.s_addr = inet_addr(bindAddr_.c_str());
    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) != 0) { ::close(listenFd_); listenFd_ = -1; return -1; }
    if (listen(listenFd_, 16) != 0) { ::close(listenFd_); listenFd_ = -1; return -1; }
    // Learn the actual port (port 0 -> ephemeral).
    socklen_t sl = sizeof(addr);
    if (getsockname(listenFd_, (sockaddr*)&addr, &sl) == 0) port_ = ntohs(addr.sin_port);
    // A short accept timeout so the loop can observe stop().
    timeval tv{1, 0};
    setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    running_.store(true);
    thread_ = std::thread([this] { serve(); });
    return port_;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    if (listenFd_ >= 0) { ::shutdown(listenFd_, SHUT_RDWR); ::close(listenFd_); listenFd_ = -1; }
    if (thread_.joinable()) thread_.join();
  }

  int port() const { return port_; }

 private:
  void serve() {
    while (running_.load()) {
      int fd = accept(listenFd_, nullptr, nullptr);
      if (fd < 0) continue;  // timeout or interrupt -> re-check running_
      handle(fd);
      ::close(fd);
    }
  }

  void handle(int fd) {
    std::string req;
    // Read until we have headers; then the body per Content-Length.
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) return;
      req.append(buf, (size_t)n);
      headerEnd = req.find("\r\n\r\n");
      if (req.size() > 1u * 1024 * 1024) break;  // header flood guard
    }
    if (headerEnd == std::string::npos) { respond(fd, 400, "text/plain", "bad request"); return; }

    const std::string head = req.substr(0, headerEnd);
    size_t contentLen = 0;
    { size_t p = ciFind(head, "content-length:");
      if (p != std::string::npos) contentLen = (size_t)std::atol(head.c_str() + p + 15); }
    std::string body = req.substr(headerEnd + 4);
    while (body.size() < contentLen) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      body.append(buf, (size_t)n);
    }

    // Parse the request line: METHOD SP PATH SP VERSION.
    std::string method, path;
    { size_t s1 = head.find(' '); size_t s2 = head.find(' ', s1 + 1);
      if (s1 == std::string::npos || s2 == std::string::npos) { respond(fd, 400, "text/plain", "bad request"); return; }
      method = head.substr(0, s1);
      path = head.substr(s1 + 1, s2 - s1 - 1); }

    route(fd, method, path, head, body);
  }

  void route(int fd, const std::string& method, const std::string& path,
             const std::string& head, const std::string& body) {
    // Liveness is never gated - it must answer even before readiness/token setup.
    if (method == "GET" && pathIs(path, "/healthz")) { respond(fd, 200, "text/plain", "ok"); return; }

    if (!authorized(path, head)) {
      respond(fd, 401, "application/json", R"({"error":"unauthorized"})");
      return;
    }

    if (method == "GET" && pathIs(path, "/readyz")) {
      const bool ready = eng_ && eng_->running();
      respond(fd, ready ? 200 : 503, "text/plain", ready ? "ready" : "not ready");
      return;
    }
    if (method == "GET" && pathIs(path, "/api/state")) {
      respond(fd, 200, "application/json", stateJson());
      return;
    }
    if (method == "POST" && pathIs(path, "/api/message")) {
      const std::string chat = jsonField(body, "chat_id");
      const std::string text = jsonField(body, "text");
      if (text.empty()) { respond(fd, 400, "application/json", R"({"error":"missing text"})"); return; }
      eng_->postMessage(chat.empty() ? "owner" : chat, text);
      respond(fd, 202, "application/json", R"({"queued":true})");
      return;
    }
    if (method == "POST" && pathIs(path, "/mcp")) {
      auto fut = eng_->dispatchMcp(body, nimbus::orch::principalForRole("owner", nimbus::orch::Role::Admin));
      if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        respond(fd, 503, "application/json", R"({"error":"engine busy"})");
        return;
      }
      respond(fd, 200, "application/json", fut.get());
      return;
    }
    respond(fd, 404, "application/json", R"({"error":"not found"})");
  }

  bool authorized(const std::string& path, const std::string& head) const {
    if (token_.empty()) return true;  // no gate configured (dev)
    // Authorization: Bearer <token>
    size_t p = ciFind(head, "authorization:");
    if (p != std::string::npos) {
      const std::string line = head.substr(p, head.find("\r\n", p) - p);
      if (line.find(token_) != std::string::npos) return true;
    }
    // ?token=<token> (the sidecar may inject either; both are the same secret)
    const std::string q = "token=" + token_;
    if (path.find(q) != std::string::npos) return true;
    return false;
  }

  std::string stateJson() const {
    StateSnapshot s = eng_->snapshot();
    const uint64_t up = (uint64_t)time(nullptr) - s.startedEpoch;
    std::string j = "{";
    j += "\"name\":" + quote(s.devName) + ",";
    j += "\"running\":" + std::string(s.running ? "true" : "false") + ",";
    j += "\"turnInFlight\":" + std::string(s.turnInFlight ? "true" : "false") + ",";
    j += "\"turnCount\":" + std::to_string(s.turnCount) + ",";
    j += "\"vectors\":" + std::to_string(s.vectors) + ",";
    j += "\"episodicMessages\":" + std::to_string(s.episodicMessages) + ",";
    j += "\"tokensIn\":" + std::to_string(s.sessionTokensIn) + ",";
    j += "\"tokensOut\":" + std::to_string(s.sessionTokensOut) + ",";
    j += "\"uptimeSeconds\":" + std::to_string(up);
    j += "}";
    return j;
  }

  // ---- tiny helpers ---------------------------------------------------------
  static bool pathIs(const std::string& path, const char* p) {
    const std::string base = path.substr(0, path.find('?'));
    return base == p;
  }
  static size_t ciFind(const std::string& hay, const std::string& needleLower) {
    std::string low = hay;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low.find(needleLower);
  }
  static std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
    return o + "\"";
  }
  // Minimal "\"key\":\"value\"" or "\"key\":value" extractor for tiny bodies.
  static std::string jsonField(const std::string& body, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return std::string();
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return std::string();
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p < body.size() && body[p] == '"') {
      size_t e = p + 1; std::string out;
      for (; e < body.size() && body[e] != '"'; e++) {
        if (body[e] == '\\' && e + 1 < body.size()) { e++; if (body[e] == 'n') { out += '\n'; continue; } }
        out += body[e];
      }
      return out;
    }
    size_t e = p;
    while (e < body.size() && body[e] != ',' && body[e] != '}' && body[e] != ' ') e++;
    return body.substr(p, e - p);
  }

  void respond(int fd, int status, const char* ctype, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted"
                         : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized"
                         : status == 404 ? "Not Found" : status == 503 ? "Service Unavailable"
                         : "OK";
    std::string h = "HTTP/1.0 " + std::to_string(status) + " " + reason + "\r\n";
    h += "Content-Type: " + std::string(ctype) + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += "Connection: close\r\n\r\n";
    ::send(fd, h.data(), h.size(), 0);
    ::send(fd, body.data(), body.size(), 0);
  }

  EngineThread* eng_;
  std::string bindAddr_;
  int port_;
  std::string token_;
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nimbusd
