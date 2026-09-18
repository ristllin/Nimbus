// test_orch_model - CUM-425: in-app per-head model picks on a hosted instance,
// device parity with store::setOrchModel ("" -> provider default).
//
// The class rules under test (not one instance):
//   (a) applyOrchModel changes what modelFor resolves, bumps the change counter,
//       and rejects an unknown slug or a malformed selector at the seam.
//   (b) the pick is DURABLE: a fresh rig on the same data dir (a "pod restart")
//       still resolves it (models.txt), and clearing restores the default.
//   (c) the web POST /api/orch accepts the SAME field the device form posts
//       (orchM_<slug> / clr_orchM_<slug>) and GET /api/orch reports the resolved
//       model per head, so the shared web page reads one truth on both surfaces.
//   (d) the pick changes the WIRE: with only a Cumulo key, setting the cumulo
//       head's model to "zai/glm-4.5-flash" routes the next turn to the zai
//       upstream (/router/zai/v1) with the bare model id in the request body -
//       the same resolveRouterRoute rule the device head follows.
//
// Offline: injected FakeHttpTransport; no network, no real keys.
#include <cstdio>
#include <string>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static std::string chatBody(const std::string& content) {
  return std::string("{\"choices\":[{\"message\":{\"content\":\"") + content +
         "\"}}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";
}

static void clearProviderEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY",
                        "TAVILY_API_KEY", "CUMULO_API_KEY"})
    unsetenv(k);
}

static NimbusdRig::Options baseOpt(const std::string& tag) {
  NimbusdRig::Options opt;
  opt.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  opt.embeddings = false;
  opt.embedDims = 64;
  return opt;
}

// (a) The rig seam: apply, reject, and observe.
static void testApplySeam(ndtest::Ctx& c) {
  std::printf("  -- (a) applyOrchModel: applies, validates, bumps counter --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("orchm-a"));
  c.eq(rig.modelFor("cumulo"), "gpt-5.6", "cumulo head starts on the router default");
  const uint32_t gen0 = rig.keyGen();

  c.ok(rig.applyOrchModel("cumulo", "zai/glm-4.5-flash"), "a router selector applies");
  c.eq(rig.modelFor("cumulo"), "zai/glm-4.5-flash", "modelFor resolves the pick");
  c.ok(rig.keyGen() == gen0 + 1, "the change counter bumped (web poll observes it)");

  c.ok(!rig.applyOrchModel("nope", "gpt-5.6"), "an unknown provider slug is rejected");
  c.ok(!rig.applyOrchModel("cumulo", "bad model with spaces"),
       "a selector with spaces is rejected");
  c.ok(!rig.applyOrchModel("cumulo", std::string(65, 'a')),
       "an oversize selector is rejected");
  c.eq(rig.modelFor("cumulo"), "zai/glm-4.5-flash",
       "a rejected write never clobbers the stored pick");
}

// (b) Durability + clear-to-default across a restart.
static void testDurableAndClear(ndtest::Ctx& c) {
  std::printf("  -- (b) durable across a restart; empty clears to default --\n");
  clearProviderEnv();
  NimbusdRig::Options opt = baseOpt("orchm-b");
  {
    Config cfg;
    NimbusdRig rig(cfg, opt);
    c.ok(rig.applyOrchModel("cumulo", "zai/glm-4.5-flash"), "set the pick");
  }
  {
    Config cfg;                       // a brand-new rig == a pod restart
    NimbusdRig rig(cfg, opt);
    c.eq(rig.modelFor("cumulo"), "zai/glm-4.5-flash",
         "the pick survived the restart (models.txt)");
    c.ok(rig.applyOrchModel("cumulo", ""), "an empty model clears the pick");
    c.eq(rig.modelFor("cumulo"), "gpt-5.6", "cleared -> back on the router default");
  }
  {
    Config cfg;
    NimbusdRig rig(cfg, opt);
    c.eq(rig.modelFor("cumulo"), "gpt-5.6", "the clear is durable too");
  }
}

// (c) The web seam: the device form's field names, and an honest GET.
static void testWebSeam(ndtest::Ctx& c) {
  std::printf("  -- (c) POST /api/orch orchM_<slug> + GET reports orchModel --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("orchm-c"));
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  ApiResp out;
  c.ok(api.handle("POST", "/api/orch", "orchM_cumulo=zai/glm-4.5-flash", out),
       "POST /api/orch with a model field is handled");
  c.ok(out.body.find("\"applied\":1") != std::string::npos,
       "the response reports one write applied");
  c.eq(rig.modelFor("cumulo"), "zai/glm-4.5-flash", "the pick applied through the web");

  ApiResp got;
  api.handle("GET", "/api/orch", "", got);
  c.ok(got.body.find("\"orchModel\":\"zai/glm-4.5-flash\"") != std::string::npos,
       "GET /api/orch reports the resolved cumulo model");

  ApiResp cleared;
  api.handle("POST", "/api/orch", "clr_orchM_cumulo=1", cleared);
  c.eq(rig.modelFor("cumulo"), "gpt-5.6", "clr_orchM_<slug> restores the default");
  eng.stop();
}

// (d) The class rule that matters on the wire: the pick re-routes the turn.
static void testPickChangesTheWire(ndtest::Ctx& c) {
  std::printf("  -- (d) cumulo pick zai/<m> routes the turn to /router/zai --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_ORCHM", 1);

  FakeHttpTransport tx;
  Exchange e;
  e.status = 200;
  e.body = chatBody("{\\\"reply\\\":\\\"hi from zai\\\",\\\"memory\\\":\\\"\\\"}");
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("orchm-d"), &tx);
  c.ok(rig.applyOrchModel("cumulo", "zai/glm-4.5-flash"), "set the router selector");

  rig.say("owner", "hello");
  c.ok(!tx.seen.empty(), "the turn dispatched a request");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.ok(r.path.find("/router/zai/chat/completions") != std::string::npos,
         "the dispatch path rides the zai upstream (no /v1 - Z.ai base carries its prefix)");
    c.ok(r.body.find("\"model\":\"glm-4.5-flash\"") != std::string::npos,
         "the request body carries the BARE model id (router prices it)");
    // The upstream prefix must never ride the model FIELD (the router would price
    // "zai/glm-4.5-flash" as unknown -> 403). It DOES appear elsewhere in the body,
    // in the [YOUR MODEL] context line that now honestly names the cumulo head's
    // selector - so the check is scoped to the model field, not the whole body.
    c.ok(r.body.find("\"model\":\"zai/") == std::string::npos,
         "the upstream prefix never leaks into the model field");
  }
  clearProviderEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== in-app model picks (T1, offline, CUM-425) ===\n");
  testApplySeam(c);
  testDurableAndClear(c);
  testWebSeam(c);
  testPickChangesTheWire(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
