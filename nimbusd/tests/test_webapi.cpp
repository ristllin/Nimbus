// test_webapi - offline (T2) proof of the Virtual Nimbus web surface (CUM-265):
// every panel the served web app renders, driven over a real loopback socket
// through the daemon_http transport (the same seam the relay sidecar forwards
// each tunneled frame to). No provider keys, no external network.
//
// It asserts the honest-virtual contract end to end:
//   * the app shell + logo are served ungated; the token seed is injected;
//   * the X-Nimbus-Token header authenticates the data routes (device compat);
//   * the assistant panels (state/health/orch/memory/tools/themes/qr/docs/chat)
//     answer with REAL engine data in the shapes the app reads;
//   * the hardware panels answer honestly ("not on a hosted instance") and never
//     fake a reading or leave a dead control.
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

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

struct Out { int status = 0; std::string body; };

// One request, authenticated the way the web app is - the X-Nimbus-Token header
// (empty tok = send none). Grouped into a struct so the helper stays within the
// argument-count gate.
struct Call { std::string method, path, tok, body; };

static Out doReq(DaemonHttpTransport& c, int port, const Call& k) {
  agent::HttpRequest r;
  r.method = k.method;
  r.host = "127.0.0.1";
  r.port = port;
  r.tls = false;
  r.path = k.path;
  r.timeoutMs = 8000;
  if (!k.tok.empty()) r.headers.push_back({"X-Nimbus-Token", k.tok});
  if (k.method != "GET") {
    r.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    r.body = k.body;
  }
  agent::HttpResponse resp;
  std::string err;
  Out out;
  if (c.exec(r, resp, err)) { out.status = resp.status; out.body = resp.body; }
  return out;
}

static Out G(DaemonHttpTransport& c, int port, const std::string& path, const std::string& tok) {
  return doReq(c, port, {"GET", path, tok, ""});
}
static Out P(DaemonHttpTransport& c, int port, const std::string& path, const std::string& tok,
             const std::string& body = "") {
  return doReq(c, port, {"POST", path, tok, body});
}

// Home + Assistant snapshots: honest virtual state, real health/orch shapes.
static void checkStatus(ndtest::Ctx& c, DaemonHttpTransport& cl, int port, const std::string& tok) {
  // The X-Nimbus-Token header gate (device compat): missing -> 401, present -> 200.
  c.ok(G(cl, port, "/api/state", "").status == 401, "GET /api/state without a token -> 401");
  Out s = G(cl, port, "/api/state", tok);
  c.ok(s.status == 200, "GET /api/state with X-Nimbus-Token -> 200 (header auth)");
  c.ok(has(s.body, "\"virtual\":true"), "state marks the instance virtual");
  c.ok(has(s.body, "\"needsOnboarding\":false"), "a provisioned VN is not sent to onboarding");
  c.ok(has(s.body, "\"mode\":1"), "mode is Orchestrator (assistant)");
  c.ok(has(s.body, "\"hasRing\":false"), "no LED ring is claimed");
  c.ok(has(s.body, "\"valid\":false") && has(s.body, "\"onExtPower\":true"),
       "battery is honestly absent (on external power), never a faked voltage");
  c.ok(has(s.body, "\"synced\":true"), "the host clock is reported synced (routines work)");
  c.ok(has(s.body, "\"storeSD\":true"), "durable volume plays the SD role (no false 'no SD' alarm)");
  c.ok(!has(s.body, "\"ota\":"), "no ESP-OTA fields (the platform rolls the image, not device OTA)");
  // params must be a real (empty) array, never null - the app does d.params.forEach.
  c.ok(has(s.body, "\"params\":[]"), "params is an empty array, not null (app iterates it)");

  Out h = G(cl, port, "/api/health", tok);
  c.ok(h.status == 200 && has(h.body, "\"components\":["), "GET /api/health -> components list");
  c.ok(has(h.body, "\"label\":\"Engine\"") && has(h.body, "\"state\":\"ok\""),
       "the engine reports ok");
  c.ok(has(h.body, "\"label\":\"Battery\"") && has(h.body, "\"state\":\"absent\""),
       "hardware components are honestly absent, not faked ok");

  Out o = G(cl, port, "/api/orch", tok);
  c.ok(o.status == 200 && has(o.body, "\"running\":true"), "GET /api/orch -> running");
  c.ok(has(o.body, "\"mistral\"") && has(o.body, "\"hasKey\":false"),
       "providers are listed with an honest hasKey (keyless here)");
  c.ok(has(o.body, "\"usage\""), "orch carries the real session usage block");
}

// Memory dashboard: real engine reads in the shapes the app renders.
static void checkMemory(ndtest::Ctx& c, DaemonHttpTransport& cl, int port, const std::string& tok) {
  Out st = G(cl, port, "/api/mem/stats", tok);
  c.ok(st.status == 200 && has(st.body, "\"vectors\":2"),
       "mem/stats reports the 2 seeded memories (real store)");
  c.ok(has(st.body, "\"sdPresent\":true"), "mem/stats reports durable storage present (honest)");

  Out sc = G(cl, port, "/api/mem/scratchpad", tok);
  c.ok(sc.status == 200 && has(sc.body, "writing tests"),
       "mem/scratchpad returns the real active task");

  Out cfgGet = G(cl, port, "/api/mem/config", tok);
  c.ok(cfgGet.status == 200 && has(cfgGet.body, "\"retrieval_count\""),
       "mem/config GET returns the retrieval knobs");
  Out cfgPut = doReq(cl, port, {"PUT", "/api/mem/config", tok, "retrieval_count=7&max_vectors=1234"});
  c.ok(cfgPut.status == 200, "mem/config PUT -> 200");
  c.ok(has(G(cl, port, "/api/mem/config", tok).body, "\"retrieval_count\":7"),
       "the PUT persisted (retrieval_count is now 7)");

  Out vg = G(cl, port, "/api/mem/vector?limit=10", tok);
  c.ok(vg.status == 200 && has(vg.body, "\"mode\":\"browse\"") && has(vg.body, "alpha memory"),
       "mem/vector GET browses the real entries");

  Out ep = G(cl, port, "/api/mem/episodic?limit=50", tok);
  c.ok(ep.status == 200 && has(ep.body, "\"messages\":["), "mem/episodic GET -> messages list");

  Out tools = G(cl, port, "/api/tools", tok);
  c.ok(tools.status == 200 && has(tools.body, "\"tools\":[") && has(tools.body, "\"count\":"),
       "tools lists the real registry");
}

// Static / pure panels.
static void checkStatic(ndtest::Ctx& c, DaemonHttpTransport& cl, int port, const std::string& tok) {
  Out th = G(cl, port, "/api/themes", tok);
  c.ok(th.status == 200 && has(th.body, "\"themes\":[") && has(th.body, "\"roles\":["),
       "themes returns the palette + status legend");
  Out qr = G(cl, port, "/api/qr?data=hello", tok);
  c.ok(qr.status == 200 && has(qr.body, "<svg"), "qr renders an SVG for the data");
  c.ok(G(cl, port, "/api/qr", tok).status == 400, "qr with no data -> 400 (honest)");
  Out ds = G(cl, port, "/api/docs/search?q=memory", tok);
  c.ok(ds.status == 200 && has(ds.body, "\"results\":["), "docs/search returns a results list");
}

// Chat: a real turn round-trips through the engine and reply ring (keyless here,
// so the engine's own honest 'no provider' reply comes back - never silence).
static void checkChat(ndtest::Ctx& c, DaemonHttpTransport& cl, int port, const std::string& tok) {
  Out post = P(cl, port, "/api/chat", tok, "text=hello");
  c.ok(post.status == 200 && has(post.body, "\"pending\":true"), "chat POST accepts the turn");
  Out get{};
  for (int i = 0; i < 40; i++) {
    get = G(cl, port, "/api/chat", tok);
    if (has(get.body, "\"pending\":false")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  c.ok(has(get.body, "\"pending\":false"), "chat GET settles (turn completed)");
  c.ok(has(get.body, "provider"), "the keyless instance replies honestly (no provider), not silence");
}

// Hardware panels: honest 'not on a hosted instance', never a faked reading and
// never a dead control (each returns a truthful body the app can surface).
static void checkHardware(ndtest::Ctx& c, DaemonHttpTransport& cl, int port, const std::string& tok) {
  Out ota = P(cl, port, "/api/ota/check", tok);
  c.ok(ota.status == 200 && has(ota.body, "\"ok\":false") && has(ota.body, "platform"),
       "OTA check honestly defers to the platform image roll");
  Out beep = P(cl, port, "/api/audio/beep", tok);
  c.ok(beep.status == 200 && has(beep.body, "\"ok\":false"),
       "speaker tone honestly reports no audio hardware");
  Out wifi = G(cl, port, "/api/wifi", tok);
  c.ok(wifi.status == 200 && has(wifi.body, "platform"),
       "wifi honestly reports platform-managed networking");
  Out loops = G(cl, port, "/api/loops", tok);
  c.ok(loops.status == 200 && loops.body == "[]", "loops returns a well-formed empty list");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== virtual nimbus web surface (T2, offline, loopback) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY"})
    unsetenv(k);

  const std::string dataDir = ndtest::scratchDir("webapi") + "/data";
  ndtest::rmTree(ndtest::scratchDir("webapi"));

  Config cfg;
  NimbusdRig::Options opt;
  opt.dataDir = dataDir;
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.devName = "VNTest";
  NimbusdRig rig(cfg, opt);

  // Seed real memory so the dashboard reads non-empty (proves it is the engine).
  rig.vectors().configure(64);
  { orch::VecEntry e; e.id = "a"; e.content = "alpha memory"; e.vec.assign(64, 0); e.vec[1] = 100; rig.vectors().add(e); }
  { orch::VecEntry e; e.id = "b"; e.content = "beta memory"; e.vec.assign(64, 0); e.vec[2] = 100; rig.vectors().add(e); }
  rig.scratchpad().setActiveTask("writing tests");

  EngineThread eng(&rig);
  eng.start();

  const std::string token = "vn-web-token";
  ReplyBuffer replies;
  // Wire reply delivery into the ring exactly as the daemon does, so the web chat
  // surface sees every reply the engine produces (including the honest keyless one).
  rig.setDeliver([&replies](const std::string&, const std::string& t) { replies.push("assistant", t); });
  HttpControl http(&eng, "127.0.0.1", 0, token, &replies, &rig);
  const int port = http.start();
  c.ok(port > 0, "control surface bound a loopback port");

  DaemonHttpTransport cl;

  // The app shell + logo are ungated; the token is seeded into the served page.
  Out shell = G(cl, port, "/", "");
  c.ok(shell.status == 200 && has(shell.body, "class=tabs>") && has(shell.body, "id=pane-dash"),
       "GET / serves the real web app (ungated shell)");
  c.ok(has(shell.body, "setItem('nimbusTok','" + token + "')"),
       "the served page seeds this instance's web token");
  c.ok(G(cl, port, "/logo.svg", "").status == 200, "GET /logo.svg -> 200 (ungated)");

  checkStatus(c, cl, port, token);
  checkMemory(c, cl, port, token);
  checkStatic(c, cl, port, token);
  checkChat(c, cl, port, token);
  checkHardware(c, cl, port, token);

  http.stop();
  eng.stop();
  ndtest::rmTree(ndtest::scratchDir("webapi"));
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
