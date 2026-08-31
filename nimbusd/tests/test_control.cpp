// test_control - offline (T2) proof of the daemon runtime: the engine thread
// (mailbox + snapshot) plus the 127.0.0.1 HTTP control surface, driven over a
// real loopback socket. No provider keys, no external network. Proves the
// contract the Phase-1 sidecar forwards to: health, readiness, snapshot reads
// that don't need the engine, MCP dispatch onto the engine thread, and the
// web-token gate.
#include <chrono>
#include <string>
#include <thread>

#include "daemon_http.h"
#include "engine_thread.h"
#include "http_control.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;

// A loopback GET/POST helper over the daemon's own curl transport.
static bool httpGet(DaemonHttpTransport& c, int port, const std::string& path,
                    const std::string& authToken, int& status, std::string& body) {
  agent::HttpRequest req;
  req.method = "GET";
  req.host = "127.0.0.1";
  req.port = port;
  req.tls = false;
  req.path = path;
  req.timeoutMs = 5000;
  if (!authToken.empty()) req.headers.push_back({"Authorization", "Bearer " + authToken});
  agent::HttpResponse resp;
  std::string err;
  if (!c.exec(req, resp, err)) return false;
  status = resp.status;
  body = resp.body;
  return true;
}

// Out-params for a request, grouped so the helpers stay within the arg-count gate.
struct HttpOut { int status = 0; std::string body; };

static bool httpPost(DaemonHttpTransport& c, int port, const std::string& path,
                     const std::string& authToken, const std::string& reqBody, HttpOut& out) {
  agent::HttpRequest req;
  req.method = "POST";
  req.host = "127.0.0.1";
  req.port = port;
  req.tls = false;
  req.path = path;
  req.timeoutMs = 5000;
  req.headers.push_back({"Content-Type", "application/json"});
  if (!authToken.empty()) req.headers.push_back({"Authorization", "Bearer " + authToken});
  req.body = reqBody;
  agent::HttpResponse resp;
  std::string err;
  if (!c.exec(req, resp, err)) return false;
  out.status = resp.status;
  out.body = resp.body;
  return true;
}

// The web chat surface: the static page (ungated) plus the gated replies route.
// Assumes an owner prompt ("hi") was already posted, so the ring holds one user
// entry to read back. Kept out of main() so each stays within the complexity gate.
static void checkWebChat(ndtest::Ctx& c, DaemonHttpTransport& client, int port,
                         const std::string& token) {
  int status = 0;
  std::string body;
  c.ok(httpGet(client, port, "/", "", status, body) && status == 200,
       "GET / -> 200 without a token (the app shell is ungated)");
  c.ok(body.find("<!doctype html") != std::string::npos, "/ serves an HTML document");
  c.ok(body.find("class=tabs>") != std::string::npos && body.find("id=pane-dash") != std::string::npos,
       "/ serves the REAL Nimbus web app (nav + Home pane), not the bare chat page");
  c.ok(body.find("nimbusTok','" + token + "'") != std::string::npos,
       "the served page seeds this instance's web token (tunnel auto sign-in)");
  // Ordering: the token seed must run BEFORE the app script reads nimbusTok().
  c.ok(body.find("setItem('nimbusTok'") < body.find("function nimbusTok"),
       "the token seed is injected ahead of the app script");
  c.ok(httpGet(client, port, "/logo.svg", "", status, body) && status == 200 &&
           body.find("<svg") != std::string::npos,
       "GET /logo.svg -> 200 SVG (ungated brand mark)");
  c.ok(httpGet(client, port, "/index.html", "", status, body) && status == 200 &&
           body.find("<!doctype html") != std::string::npos,
       "GET /index.html -> 200 HTML (alias of /)");

  // /api/replies is a DATA route: gated exactly like /api/state.
  c.ok(httpGet(client, port, "/api/replies?after=0", "", status, body) && status == 401,
       "GET /api/replies without a token -> 401 (gated)");
  // The owner prompt posted earlier ("hi") was recorded as a user entry.
  c.ok(httpGet(client, port, "/api/replies?after=0", token, status, body) && status == 200,
       "GET /api/replies with token -> 200");
  c.ok(body.find("\"role\":\"user\"") != std::string::npos &&
           body.find("\"text\":\"hi\"") != std::string::npos,
       "the recorded owner prompt is returned (role user, text hi)");
  c.ok(body.find("\"providerConfigured\":false") != std::string::npos,
       "keyless instance reports providerConfigured:false (CUM-211 honest state)");
  size_t p = body.find("\"lastSeq\":");
  long last = p == std::string::npos ? -1 : std::atol(body.c_str() + p + 10);
  c.ok(last >= 1, "lastSeq advanced past the owner prompt");
  const std::string after = "/api/replies?after=" + std::to_string(last);
  c.ok(httpGet(client, port, after, token, status, body) && status == 200 &&
           body.find("\"replies\":[]") != std::string::npos,
       "after=<lastSeq> returns an empty replies set (seq filter works)");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== daemon control surface (T2, offline, loopback) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY"})
    unsetenv(k);

  const std::string dataDir = ndtest::scratchDir("ctl") + "/data";
  ndtest::rmTree(ndtest::scratchDir("ctl"));

  Config cfg;
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.devName = "CtlTest";
  NimbusdRig rig(cfg, opt);

  EngineThread eng(&rig);
  eng.start();

  const std::string token = "s3kret-web-token";
  ReplyBuffer replies;
  HttpControl http(&eng, "127.0.0.1", 0, token, dataDir, &replies, &rig);
  const int port = http.start();
  c.ok(port > 0, "control surface bound a loopback port");

  DaemonHttpTransport client;
  int status = 0;
  std::string body;

  // ---- health is never gated ------------------------------------------------
  c.ok(httpGet(client, port, "/healthz", "", status, body) && status == 200,
       "GET /healthz -> 200 without a token");
  c.eq(body, "ok", "/healthz body");

  // ---- the gate: no token -> 401 -------------------------------------------
  c.ok(httpGet(client, port, "/api/state", "", status, body) && status == 401,
       "GET /api/state without a token -> 401");

  // ---- with the token: readiness + snapshot --------------------------------
  c.ok(httpGet(client, port, "/readyz", token, status, body) && status == 200,
       "GET /readyz with token -> 200 (engine running)");
  c.ok(httpGet(client, port, "/api/state", token, status, body) && status == 200,
       "GET /api/state with token -> 200");
  c.ok(body.find("\"name\":\"CtlTest\"") != std::string::npos,
       "state JSON carries the instance name");
  c.ok(body.find("\"running\":true") != std::string::npos, "state reports running");

  // ---- MCP dispatch runs on the engine thread ------------------------------
  HttpOut mcp;
  c.ok(httpPost(client, port, "/mcp", token,
                R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", mcp) &&
           mcp.status == 200,
       "POST /mcp tools/list -> 200");
  c.ok(mcp.body.find("memory.write") != std::string::npos,
       "the MCP tool list includes memory.write (engine dispatch worked)");

  // ---- a snapshot read returns immediately even with a turn queued ----------
  // Queue a (keyless -> fast no-op) message, then read state without blocking.
  HttpOut msg;
  c.ok(httpPost(client, port, "/api/message", token, R"({"chat_id":"owner","text":"hi"})", msg) &&
           msg.status == 202,
       "POST /api/message -> 202 (enqueued, not blocked on)");
  c.ok(httpGet(client, port, "/api/state", token, status, body) && status == 200,
       "GET /api/state still answers immediately (snapshot read)");
  c.ok(body.find("\"provVerified\":false") != std::string::npos,
       "state reports provVerified:false (keyless instance, honest)");
  c.ok(body.find("\"virtual\":true") != std::string::npos,
       "state carries the honest Virtual Nimbus marker");

  // ---- the web chat surface: static page (ungated) + replies (gated) --------
  checkWebChat(c, client, port, token);

  // ---- backup: flush-and-stream a consistent tar of the mem tree -----------
  // Seed a vector so the flushed vectors.bin has content, then pull /backup.
  rig.vectors().configure(64);
  { orch::VecEntry e; e.id = "b1"; e.content = "backup me"; e.vec.assign(64, 0); e.vec[1] = 100; rig.vectors().add(e); }
  c.ok(httpGet(client, port, "/backup", token, status, body) && status == 200,
       "GET /backup with token -> 200");
  c.ok(body.size() > 0 && body.find("vectors.bin") != std::string::npos,
       "the backup tar contains mem/vectors.bin");
  c.ok(httpGet(client, port, "/backup", "", status, body) && status == 401,
       "GET /backup without a token -> 401 (gated)");

  http.stop();
  eng.stop();
  ndtest::rmTree(ndtest::scratchDir("ctl"));
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
