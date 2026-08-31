#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "web_api.h"
#include "web_ui.h"

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
//   GET  /              the assembled Nimbus web app (UNGATED - static shell)
//   GET  /index.html    alias of "/"
//   GET  /logo.svg      the brand mark (UNGATED - a logo is not sensitive)
//   GET  /healthz       liveness  (never gated - the kubelet probe)
//   GET  /readyz        readiness (200 iff the engine thread is running)
//   GET  /api/state     the state snapshot as JSON (gated)
//   GET  /api/replies?after=<seq>  last-N chat entries newer than <seq> (gated)
//   POST /api/message   {"chat_id","text"} -> 202, enqueues a turn (gated)
//   POST /mcp           JSON-RPC to the tool registry, run on the engine (gated)
//
// Auth decision (CUM-263 / CUM-265): "/", "/index.html" and "/logo.svg" are
// served UNGATED. The page is a static shell that carries no instance data -
// every byte of data it shows is fetched from the gated /api/* routes, which stay
// gated exactly as before. The relay sidecar injects the web token on every
// forwarded request, so ungating the shell only affects first paint (it renders
// before the API answers), never data exposure. The served shell also seeds the
// browser with this instance's web token (web_ui.h) so the app's client-side
// sign-in gate is satisfied inside the tunnel without a second sign-in; the DATA
// routes still require the token (X-Nimbus-Token header, ?t=, or Bearer).
namespace nimbusd {

class HttpControl {
 public:
  // `token` empty = no gate (dev). Non-empty = require Authorization: Bearer or
  // ?token= on every path except /healthz.
  HttpControl(EngineThread* eng, const std::string& bindAddr, int port,
              std::string token, std::string dataDir = "/data",
              ReplyBuffer* replies = nullptr, NimbusdRig* rig = nullptr)
      : eng_(eng), bindAddr_(bindAddr), port_(port), token_(std::move(token)),
        dataDir_(std::move(dataDir)), replies_(replies),
        page_(buildWebUiPage(token_)), api_(rig, eng, replies) {
    api_.setWebToken(token_);
  }
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

  // One route: method + path -> handler (fd, path, body). Gated iff !ungated.
  struct Route {
    const char* method;
    const char* path;
    void (HttpControl::*fn)(int, const std::string&, const std::string&);
    bool ungated;  // true = served without the token gate (health + static page)
  };

  void route(int fd, const std::string& method, const std::string& path,
             const std::string& head, const std::string& body) {
    static const Route kRoutes[] = {
        {"GET", "/", &HttpControl::handleIndex, true},
        {"GET", "/index.html", &HttpControl::handleIndex, true},
        {"GET", "/logo.svg", &HttpControl::handleLogo, true},
        {"GET", "/healthz", &HttpControl::handleHealthz, true},
        {"GET", "/readyz", &HttpControl::handleReadyz, false},
        {"GET", "/api/replies", &HttpControl::handleReplies, false},
        {"GET", "/backup", &HttpControl::handleBackup, false},
        {"POST", "/api/message", &HttpControl::handleMessage, false},
        {"POST", "/mcp", &HttpControl::handleMcp, false},
    };
    for (const auto& r : kRoutes) {
      if (method != r.method || !pathIs(path, r.path)) continue;
      if (!r.ungated && !authorized(path, head)) {
        respond(fd, 401, "application/json", R"({"error":"unauthorized"})");
        return;
      }
      (this->*r.fn)(fd, path, body);
      return;
    }
    // The web API surface (all token-gated data, except the pre-auth sign-in code
    // exchange). Handled by WebApi with honest Virtual Nimbus semantics.
    const std::string apiBase = path.substr(0, path.find('?'));
    if (apiBase.rfind("/api/", 0) == 0) {
      const bool preAuth = apiBase == "/api/signin/exchange";
      if (!preAuth && !authorized(path, head)) {
        respond(fd, 401, "application/json", R"({"error":"unauthorized"})");
        return;
      }
      ApiResp r;
      if (api_.handle(method, path, body, r)) {
        respond(fd, r.status, r.ctype.c_str(), r.body);
        return;
      }
    }
    respond(fd, 404, "application/json", R"({"error":"not found"})");
  }

  void handleIndex(int fd, const std::string&, const std::string&) {
    respond(fd, 200, "text/html; charset=utf-8", page_);
  }

  void handleLogo(int fd, const std::string&, const std::string&) {
    respond(fd, 200, "image/svg+xml", kLogoSvg);
  }

  void handleHealthz(int fd, const std::string&, const std::string&) { respond(fd, 200, "text/plain", "ok"); }

  void handleReadyz(int fd, const std::string&, const std::string&) {
    const bool ready = eng_ && eng_->running();
    respond(fd, ready ? 200 : 503, "text/plain", ready ? "ready" : "not ready");
  }

  // Read-only poll of the recent chat ring. `after` filters to entries with a
  // higher sequence; the response also carries the live turn/provider state the
  // page needs for its honest indicators. Never clears on read (multi-tab safe).
  void handleReplies(int fd, const std::string& path, const std::string&) {
    uint64_t after = queryU64(path, "after");
    const StateSnapshot s = eng_ ? eng_->snapshot() : StateSnapshot{};
    std::string arr = replies_ ? replies_->sinceJsonArray(after) : std::string("[]");
    uint64_t last = replies_ ? replies_->lastSeq() : 0;
    std::string j = "{";
    j += "\"replies\":" + arr + ",";
    j += "\"lastSeq\":" + std::to_string(last) + ",";
    j += "\"turnInFlight\":" + std::string(s.turnInFlight ? "true" : "false") + ",";
    j += "\"turnCount\":" + std::to_string(s.turnCount) + ",";
    j += "\"providerConfigured\":" + std::string(s.providerConfigured ? "true" : "false");
    j += "}";
    respond(fd, 200, "application/json", j);
  }

  void handleMessage(int fd, const std::string&, const std::string& body) {
    const std::string chat = jsonField(body, "chat_id");
    const std::string text = jsonField(body, "text");
    if (text.empty()) { respond(fd, 400, "application/json", R"({"error":"missing text"})"); return; }
    const std::string chatId = chat.empty() ? "owner" : chat;
    // Record the owner's prompt synchronously so every polling tab renders it
    // (the assistant reply is recorded later by the engine delivery hook).
    if (replies_) replies_->push("user", text);
    eng_->postMessage(chatId, text);
    respond(fd, 202, "application/json", R"({"queued":true})");
  }

  void handleBackup(int fd, const std::string&, const std::string&) {
    // Flush to a consistent on-disk state, then stream a tar of the mem tree.
    // nimbusd owns the write discipline, so only it can produce a consistent
    // archive (plan §3.4); a CronJob drives this to GCS nightly.
    eng_->flushNow();
    std::string tar;
    if (!makeBackupTar(tar)) { respond(fd, 500, "text/plain", "backup failed"); return; }
    respond(fd, 200, "application/x-tar", tar);
  }

  void handleMcp(int fd, const std::string&, const std::string& body) {
    auto fut = eng_->dispatchMcp(body, nimbus::orch::principalForRole("owner", nimbus::orch::Role::Admin));
    if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
      respond(fd, 503, "application/json", R"({"error":"engine busy"})");
      return;
    }
    respond(fd, 200, "application/json", fut.get());
  }

  bool authorized(const std::string& path, const std::string& head) const {
    if (token_.empty()) return true;  // no gate configured (dev)
    // Authorization: Bearer <token> - match the header VALUE exactly (trimmed),
    // not a substring, so a token can't be accepted merely for appearing somewhere
    // in the header.
    size_t p = ciFind(head, "authorization:");
    if (p != std::string::npos) {
      size_t eol = head.find("\r\n", p);
      std::string val = head.substr(p + 14, (eol == std::string::npos ? head.size() : eol) - (p + 14));
      val = trim(val);
      if (val == "Bearer " + token_) return true;
    }
    // X-Nimbus-Token: <token> - the header the device web app's fetch shim sends
    // on every request (CUM-45). Kept compatible so the same page authenticates
    // against a hosted instance exactly as against the device. Exact value match.
    if (headerValueIs(head, "x-nimbus-token:", token_)) return true;
    // ?token=<token> as an exact query parameter value (the sidecar may inject
    // either; both are the same secret). Match "token=<tok>" bounded by ? & or end.
    const std::string needle = "token=" + token_;
    size_t qs = path.find('?');
    if (qs != std::string::npos) {
      const std::string query = path.substr(qs + 1);
      size_t at = 0;
      while (at < query.size()) {
        size_t amp = query.find('&', at);
        const std::string kv = query.substr(at, (amp == std::string::npos ? query.size() : amp) - at);
        if (kv == needle) return true;
        if (amp == std::string::npos) break;
        at = amp + 1;
      }
    }
    return false;
  }

  // Parse an unsigned integer query parameter (?key=<n>) from a request path.
  // Returns 0 when absent or non-numeric.
  static uint64_t queryU64(const std::string& path, const std::string& key) {
    size_t qs = path.find('?');
    if (qs == std::string::npos) return 0;
    const std::string query = path.substr(qs + 1);
    const std::string needle = key + "=";
    size_t at = 0;
    while (at < query.size()) {
      size_t amp = query.find('&', at);
      const std::string kv = query.substr(at, (amp == std::string::npos ? query.size() : amp) - at);
      if (kv.rfind(needle, 0) == 0)
        return (uint64_t)std::strtoull(kv.c_str() + needle.size(), nullptr, 10);
      if (amp == std::string::npos) break;
      at = amp + 1;
    }
    return 0;
  }

  // ---- tiny helpers ---------------------------------------------------------
  static bool pathIs(const std::string& path, const char* p) {
    const std::string base = path.substr(0, path.find('?'));
    return base == p;
  }
  // True iff the request carries header `headerLower` (case-insensitive) whose
  // trimmed value equals `want` exactly.
  static bool headerValueIs(const std::string& head, const std::string& headerLower,
                            const std::string& want) {
    size_t p = ciFind(head, headerLower);
    if (p == std::string::npos) return false;
    size_t vs = p + headerLower.size();
    size_t eol = head.find("\r\n", vs);
    std::string val = head.substr(vs, (eol == std::string::npos ? head.size() : eol) - vs);
    return trim(val) == want;
  }
  static size_t ciFind(const std::string& hay, const std::string& needleLower) {
    std::string low = hay;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low.find(needleLower);
  }
  static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
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
    if (p < body.size() && body[p] == '"') return jsonQuotedValue(body, p + 1);
    return jsonBareValue(body, p);
  }
  // Read a double-quoted value starting at `p` (the char after the opening ").
  static std::string jsonQuotedValue(const std::string& body, size_t p) {
    std::string out;
    for (; p < body.size() && body[p] != '"'; p++) {
      if (body[p] == '\\' && p + 1 < body.size()) {
        p++;
        if (body[p] == 'n') { out += '\n'; continue; }
      }
      out += body[p];
    }
    return out;
  }
  // Read a bare (unquoted) value up to a delimiter.
  static std::string jsonBareValue(const std::string& body, size_t p) {
    size_t e = p;
    while (e < body.size() && body[e] != ',' && body[e] != '}' && body[e] != ' ') e++;
    return body.substr(p, e - p);
  }

  // Produce a tar of the durable mem tree via the system tar (present in the
  // runtime image). The daemon has already flushed, and the archive is read
  // wholly into memory - fine for the small mem set at MVP scale (a chunked
  // stream is a later optimization). Returns false on any failure.
  bool makeBackupTar(std::string& out) {
    out.clear();
    // -C <dataDir> mem: archive the "mem" subtree with stable relative paths.
    const std::string cmd = "tar -cf - -C '" + shellEscape(dataDir_) + "' mem 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    const int rc = pclose(p);
    return rc == 0 && !out.empty();
  }
  static std::string shellEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o;
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
  std::string dataDir_;
  ReplyBuffer* replies_ = nullptr;
  std::string page_;  // the assembled web app, token seeded, built once
  WebApi api_;        // the /api/* surface (honest Virtual Nimbus semantics)
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nimbusd
