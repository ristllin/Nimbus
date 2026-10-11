// test_usage_diag - item 4 + CUM-546: usage survives a save, and a hosted turn is
// diagnosable.
//
//   (a) a model save and a key save rebuild the engine; GET /api/orch usage
//       (sessIn/sessOut/turns) and GET /api/state turnCount keep counting across
//       them instead of dropping to 0, and the next turn adds to them;
//   (b) GET /api/lastturn serves the last turn's anatomy (404 before any turn),
//       GET /api/log the engine log, GET /api/errlog/recent its failure lines;
//   (c) the engine log is a bounded ring (it grew for the life of the daemon).
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "nimbus/harness/log.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  return o;
}

static Exchange respTurn(const std::string& path, const std::string& reply) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = path;
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"{\\\"reply\\\":\\\"" + reply +
           "\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":100,\"output_tokens\":7}}";
  return e;
}
static Exchange misTurn(const std::string& reply) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/mistral/v1/conversations";
  e.body = "{\"conversation_id\":\"c\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"{\\\"reply\\\":\\\"" +
           reply + "\\\",\\\"memory\\\":\\\"\\\"}\"}],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":7}}";
  return e;
}

struct Web {
  explicit Web(NimbusdRig& rig) : eng(&rig), api(&rig, &eng, &replies) { eng.start(); }
  ~Web() { eng.stop(); }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    ApiResp out;
    api.handle(m, path, body, out);
    return out;
  }
  void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static void testCountersSurviveSaves(ndtest::Ctx& c) {
  std::printf("  -- (a) usage and turnCount survive a model save and a key save --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "one"));
  tx.script.push_back(respTurn("/router/openai/v1/responses", "two"));
  Exchange verify;   // the key save re-verifies the new key (CUM-545)
  verify.expectHost = kCumuloHost;
  verify.expectPathContains = "/router/models";
  verify.body = "{\"object\":\"list\",\"data\":[]}";
  tx.script.push_back(verify);
  tx.script.push_back(misTurn("three"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-a"), &tx);
  rig.say("owner", "first");
  rig.say("owner", "second");
  {
    Web web(rig);
    const std::string before = web.call("GET", "/api/orch").body;
    c.ok(has(before, "\"sessIn\":200") && has(before, "\"sessOut\":14") && has(before, "\"turns\":2"),
         "two turns counted (200 in / 14 out / 2 turns)");
    c.eqi(web.call("POST", "/api/orch", "orchM_cumulo=mistral/mistral-large-latest").status, 200, "a model save");
    web.settle();
    const std::string afterModel = web.call("GET", "/api/orch").body;
    c.ok(has(afterModel, "\"sessIn\":200") && has(afterModel, "\"turns\":2"), "a model save keeps the counters");
    c.eqi(web.call("POST", "/api/orch", "cumuloKey=cumulo_sk_test_USAGE2").status, 200, "a key save");
    web.settle();
    const std::string afterKey = web.call("GET", "/api/orch").body;
    c.ok(has(afterKey, "\"sessIn\":200") && has(afterKey, "\"turns\":2"), "a key save keeps the counters");
    c.ok(has(web.call("GET", "/api/state").body, "\"turnCount\":2"), "/api/state turnCount keeps counting");
  }
  rig.say("owner", "third");
  Web web(rig);
  const std::string after = web.call("GET", "/api/orch").body;
  c.ok(has(after, "\"sessIn\":300") && has(after, "\"turns\":3"), "the next turn adds to the carried totals");
  c.ok(has(after, "\"lastIn\":100") && has(after, "\"lastOut\":7"), "the last turn's usage is reported");
  clearEnv();
}

static void testDiagnostics(ndtest::Ctx& c) {
  std::printf("  -- (b) /api/lastturn, /api/log and /api/errlog/recent --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "fine"));
  for (int i = 0; i < 2; i++) {   // a failing turn (and the engine's one retry)
    Exchange bad;
    bad.expectHost = kCumuloHost;
    bad.status = 500;
    bad.body = "{}";
    tx.script.push_back(bad);
  }
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-b"), &tx);
  {
    Web web(rig);
    c.eqi(web.call("GET", "/api/lastturn").status, 404, "no anatomy before the first turn");
  }
  rig.say("owner", "PELICAN diagnostics please");
  rig.say("owner", "this one fails");
  Web web(rig);
  const ApiResp lt = web.call("GET", "/api/lastturn");
  c.eqi(lt.status, 200, "GET /api/lastturn answers 200 (it was a 404)");
  c.ok(has(lt.body, "NIMBUS TURN ANATOMY") && has(lt.body, "host: cumulo"), "the anatomy names the head");
  c.ok(has(lt.body, "this one fails") && has(lt.body, "result: FAILED"), "it is the most recent turn, with its result");
  c.ok(has(lt.body, "hosted cloud instance"), "the instructions shown are the ones the head got (hosted text)");
  c.ok(has(web.call("GET", "/api/log").body, "orchestrator:"), "GET /api/log serves the engine log");
  const ApiResp el = web.call("GET", "/api/errlog/recent?bytes=4096");
  c.ok(el.status == 200 && has(el.body, "turn failed"), "GET /api/errlog/recent carries the failure");
  c.ok(!has(el.body, "turn host="), "and only failure lines");
  clearEnv();
}

static void testLogIsBounded(ndtest::Ctx& c) {
  std::printf("  -- (c) the engine log is a bounded ring --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-c"));
  for (int i = 0; i < 2500; i++) agent::hlog::logf("filler line %d", i);
  const auto lines = NimbusdRig::harnessLog();
  c.ok(lines.size() <= 1000, "at most 1000 lines are kept");
  c.ok(!lines.empty() && has(lines.back(), "filler line 2499"), "the newest line is kept");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== usage across saves + diagnostics (item 4, CUM-546, offline) ===\n");
  testCountersSurviveSaves(c);
  testDiagnostics(c);
  testLogIsBounded(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
