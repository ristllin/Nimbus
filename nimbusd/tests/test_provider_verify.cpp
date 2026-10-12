// test_provider_verify - CUM-545 (item 5): a working Cumulo key reads "verified"
// and the router model picker gets the router's catalog.
//
// nimbusd hard-coded verify -1 (the page said "unverified" forever and kept the
// model pickers disabled) and had no GET /api/models. The rules under test:
//   (a) a verify reads GET <router>/router/models with the Cumulo key (unmetered,
//       the device's cumulo verify): 200 -> verify 1, the body becomes the catalog
//       GET /api/models serves grouped by upstream and GET /api/orch's choices;
//   (b) 402 -> -1 "nocredits", 401 -> 0 (rejected); a transient failure never
//       demotes a verified key;
//   (c) a turn that answered on a head marks it verified (no probe needed);
//   (d) POST /api/verify and a key save queue the read through the web layer;
//   (e) the head's [AVAILABLE MODELS] lists the harvested router models, and a
//       spawn naming one of them runs on it (Anthropic Messages under
//       /router/anthropic) instead of being coerced to the configured sub model;
//   (f) a key saved while a check for that provider is still queued (the boot
//       verify, the workspace probe) is checked with the NEW key after it applies.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static const char* kKey = "cumulo_sk_test_VERIFY";
static const char* kCatalog =
    "{\"object\":\"list\",\"data\":["
    "{\"id\":\"openai/gpt-5.6\",\"object\":\"model\",\"owned_by\":\"openai\",\"size_class\":\"large\"},"
    "{\"id\":\"anthropic/claude-sonnet-5\",\"object\":\"model\",\"owned_by\":\"anthropic\",\"size_class\":\"medium\"},"
    "{\"id\":\"mistral/mistral-large-latest\",\"object\":\"model\",\"owned_by\":\"mistral\",\"size_class\":\"large\"}]}";

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

static Exchange routerModels(int status, const std::string& body) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/models";
  e.status = status;
  e.body = body;
  return e;
}

static std::string esc(const std::string& s) {
  std::string out;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out;
}
static Exchange routerHead(const std::string& turnJson) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/responses";
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"" + esc(turnJson) + "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
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
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

// The cumulo provider object out of GET /api/orch (to the next provider key).
static std::string cumuloOf(const std::string& orch) {
  const size_t a = orch.find("\"cumulo\":{");
  if (a == std::string::npos) return std::string();
  return orch.substr(a, orch.find('}', a) - a);
}

static void testVerifyHarvests(ndtest::Ctx& c) {
  std::printf("  -- (a) a 200 from /router/models verifies the key and fills the picker --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(200, kCatalog));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-a"), &tx);
  c.ok(rig.verifyProvider("cumulo"), "the verify succeeds");
  if (!tx.seen.empty()) {
    bool bearer = false;
    for (const auto& h : tx.seen[0].headers) bearer = bearer || (h.first == "Authorization" && h.second == std::string("Bearer ") + kKey);
    c.ok(bearer && tx.seen[0].method == "GET", "a GET with the Cumulo key as the bearer");
  }
  EngineThread eng(&rig);
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  ApiResp orch, models;
  api.handle("GET", "/api/orch", "", orch);
  api.handle("GET", "/api/models", "", models);
  const std::string cu = cumuloOf(orch.body);
  c.ok(has(cu, "\"verify\":1"), "GET /api/orch: cumulo verify 1 (the badge reads verified)");
  c.ok(!has(cu, "\"vts\":0"), "with a verify timestamp");
  c.ok(has(cu, "anthropic/claude-sonnet-5"), "the router models become the picker's choices");
  c.eqi(models.status, 200, "GET /api/models is served (it was a 404)");
  c.ok(has(models.body, "\"id\":\"anthropic/claude-sonnet-5\"") && has(models.body, "\"upstream\":\"anthropic\""),
       "the catalog carries the router ids with their upstream for grouping");
  clearEnv();
}

static void testVerdicts(ndtest::Ctx& c) {
  std::printf("  -- (b) 402 nocredits, 401 rejected, a transient failure keeps verified --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(402, "{\"error\":\"credits_or_subscription_required\"}"));
  tx.script.push_back(routerModels(401, "{\"error\":\"invalid_router_key\"}"));
  tx.script.push_back(routerModels(200, kCatalog));
  tx.script.push_back(routerModels(503, "{}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-b"), &tx);
  rig.verifyProvider("cumulo");
  c.ok(rig.verifyState("cumulo").verify == -1 && rig.verifyState("cumulo").reason == "nocredits",
       "402 -> could not verify, reason nocredits");
  rig.verifyProvider("cumulo");
  c.eqi(rig.verifyState("cumulo").verify, 0, "401 -> rejected");
  rig.verifyProvider("cumulo");
  rig.verifyProvider("cumulo");
  c.eqi(rig.verifyState("cumulo").verify, 1, "a transient 503 never demotes a verified key");
  clearEnv();
}

static void testTurnMarksVerified(ndtest::Ctx& c) {
  std::printf("  -- (c) a turn that answered marks the head verified --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerHead("{\"reply\":\"hi\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-c"), &tx);
  c.eqi(rig.verifyState("cumulo").verify, -1, "unverified before anything ran");
  rig.say("owner", "hello");
  c.eqi(rig.verifyState("cumulo").verify, 1, "verified after a turn answered on the router");
  clearEnv();
}

static void testWebTriggers(ndtest::Ctx& c) {
  std::printf("  -- (d) POST /api/verify and a key save queue the read --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(200, kCatalog));
  tx.script.push_back(routerModels(200, kCatalog));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-d"), &tx);
  Web web(rig);
  c.eqi(web.call("POST", "/api/verify", "provider=cumulo").status, 200, "POST /api/verify answers 200");
  for (int i = 0; i < 150 && rig.verifyState("cumulo").verify != 1; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  c.eqi(rig.verifyState("cumulo").verify, 1, "the queued verify ran and verified the key");
  c.eqi(web.call("POST", "/api/orch", "cumuloKey=cumulo_sk_test_NEWKEY").status, 200, "a key save answers 200");
  for (int i = 0; i < 150 && tx.seen.size() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  c.eqi((long)tx.seen.size(), 2, "the new key was verified right after it applied");
  if (tx.seen.size() == 2) {
    bool bearer = false;
    for (const auto& h : tx.seen[1].headers) bearer = bearer || h.second == "Bearer cumulo_sk_test_NEWKEY";
    c.ok(bearer, "with the NEW key");
  }
  clearEnv();
}

static void testSpawnOnHarvestedModel(ndtest::Ctx& c) {
  std::printf("  -- (e) the prompt lists router models; a spawn on one runs on it --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(200, kCatalog));
  tx.script.push_back(routerHead(
      "{\"reply\":\"Asking Claude.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"id\":null,"
      "\"task\":\"second opinion\",\"provider\":\"cumulo\",\"model\":\"anthropic/claude-sonnet-5\"}]}"));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.expectPathContains = "/router/anthropic/v1/messages";
  sub.body = "{\"content\":[{\"type\":\"text\",\"text\":\"Claude says: ship it.\"}],"
             "\"usage\":{\"input_tokens\":5,\"output_tokens\":4}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"Claude says ship it.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-e"), &tx);
  rig.verifyProvider("cumulo");
  std::mutex mu;
  std::vector<std::string> inbox;
  rig.setDeliver([&](const std::string&, const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    inbox.push_back(t);
  });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("owner", "get a second opinion from Claude");
  bool done = false;
  for (int i = 0; i < 750 && !done; i++) {
    {
      std::lock_guard<std::mutex> lk(mu);
      for (const auto& m : inbox) done = done || has(m, "Claude says ship it.");
    }
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  eng.stop();
  c.ok(done, "the synthesis delivered the Claude sub's answer");
  if (tx.seen.size() >= 2) c.ok(has(tx.seen[1].body, "anthropic/claude-sonnet-5"), "[AVAILABLE MODELS] lists the router's Claude model");
  if (tx.seen.size() >= 3) {
    c.eq(tx.seen[2].path, "/router/anthropic/v1/messages", "the sub runs on the Anthropic wire under /router/anthropic");
    c.ok(has(tx.seen[2].body, "\"model\":\"claude-sonnet-5\""), "with the named model (not coerced)");
  }
  c.ok(!rig.jobs().activeCount(), "the job drained");
  clearEnv();
}

static void testZaiBaseIsABareHost(ndtest::Ctx& c) {
  std::printf("  -- (f) the Z.ai verify host is the bare host of NIMBUSD_ZAI_BASE --\n");
  clearEnv();
  setenv("Z_AI_TOKEN", "zai_test_VERIFY", 1);
  setenv("NIMBUSD_ZAI_BASE", "https://open.bigmodel.cn/api/paas/v4", 1);
  FakeHttpTransport tx;
  Exchange e;
  e.expectHost = "open.bigmodel.cn";
  e.expectPathContains = "/api/paas/v4/models";
  e.body = "{\"data\":[]}";
  tx.script.push_back(e);
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-f"), &tx);
  c.ok(rig.verifyProvider("zai"), "the verify reaches the bare host (no scheme in the hostname)");
  unsetenv("NIMBUSD_ZAI_BASE");
  clearEnv();
}

static void testMemberSpawnRunsTheSubModel(ndtest::Ctx& c) {
  std::printf("  -- (g) a member's spawn cannot pick a model on the owner's key --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(200, kCatalog));
  tx.script.push_back(routerHead(
      "{\"reply\":\"Asking.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\",\"id\":null,"
      "\"task\":\"look this up\",\"provider\":\"cumulo\",\"model\":\"anthropic/claude-sonnet-5\"}]}"));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.expectPathContains = "/router/openai/v1/chat/completions";
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"found it\"}}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"Here it is.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-g"), &tx);
  rig.verifyProvider("cumulo");
  std::string err;
  c.ok(rig.telegramAccess().setAllowCsv("424242", err), "an approved Telegram member");
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("424242", "look this up with Claude");
  for (int i = 0; i < 750 && tx.seen.size() < 4; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  eng.stop();
  if (tx.seen.size() >= 3) {
    c.eq(tx.seen[2].path, "/router/openai/v1/chat/completions", "the member's spawn runs the configured sub model's route");
    c.ok(has(tx.seen[2].body, "\"model\":\"gpt-5.6\""), "with the configured sub model, not the member's pick");
  } else {
    c.ok(false, "the member's spawn ran");
  }
  clearEnv();
}

// Every task queued on the engine thread so far has run (FIFO); false on a stall.
static bool drained(EngineThread& eng) {
  auto done = std::make_shared<std::atomic<bool>>(false);   // shared: the task may outlive a stalled wait
  eng.postWork([done] { done->store(true); });
  for (int i = 0; i < 1000 && !done->load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return done->load();
}

static bool bearerIs(const agent::HttpRequest& r, const std::string& key) {
  for (const auto& h : r.headers)
    if (h.first == "Authorization" && h.second == "Bearer " + key) return true;
  return false;
}

static void testKeySavedWhileVerifyQueued(ndtest::Ctx& c) {
  std::printf("  -- (f) a key saved while its check is queued is checked with the new key --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  {
    FakeHttpTransport tx;
    tx.script.push_back(routerModels(200, kCatalog));   // the boot verify (old key)
    tx.script.push_back(routerModels(200, kCatalog));   // the new key's verify
    Config cfg;
    NimbusdRig rig(cfg, opts("pv-f"), &tx);
    Web web(rig);
    web.eng.postWork([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); });   // work ahead
    c.ok(rig.claimVerify("cumulo"), "the boot path claims the verify");
    web.eng.postWork([&rig] { rig.verifyProvider("cumulo"); });   // the boot verify, still queued
    c.eqi(web.call("POST", "/api/orch", "cumuloKey=cumulo_sk_test_NEWKEY").status, 200, "the key save answers 200");
    c.ok(drained(web.eng), "the engine thread ran everything queued");
    web.eng.stop();
    c.eqi((long)tx.seen.size(), 2, "the new key got its own verify after it applied");
    if (tx.seen.size() == 2) c.ok(bearerIs(tx.seen[1], "cumulo_sk_test_NEWKEY"), "with the NEW key");
    c.eqi(rig.verifyState("cumulo").verify, 1, "the badge reads verified");
    c.ok(!rig.verifyState("cumulo").models.empty(), "and the picker has the router catalog");
  }
  clearEnv();
  setenv("MISTRAL_API_KEY", "mk_test_OLDKEY", 1);
  {
    // The sibling check: the Mistral workspace probe of an enabled Studio connector.
    FakeHttpTransport tx;
    auto probe = [] {
      Exchange e;
      e.expectHost = "api.mistral.ai";
      e.expectPathContains = "/v1/connectors";
      e.body = "{\"items\":[{\"id\":\"u-gcal\",\"name\":\"google_calendar\",\"active\":true}]}";
      return e;
    };
    tx.script.push_back(probe());
    tx.script.push_back(probe());
    Exchange models;
    models.expectHost = "api.mistral.ai";
    models.expectPathContains = "/v1/models";
    models.body = "{\"data\":[{\"id\":\"mistral-large-latest\"}]}";
    tx.script.push_back(models);
    Config cfg;
    NimbusdRig rig(cfg, opts("pv-f2"), &tx);
    rig.connectors().replaceBlob(
        "[{\"name\":\"gcal\",\"type\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\","
        "\"cid\":\"connector_googlecalendar\",\"en\":1}]");
    Web web(rig);
    web.eng.postWork([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); });
    c.ok(rig.claimProbe(), "the boot path claims the workspace probe");
    web.eng.postWork([&rig] { rig.refreshMistralWorkspace(); });   // still queued
    c.eqi(web.call("POST", "/api/orch", "mistKey=mk_test_NEWKEY_WEB").status, 200, "the Mistral key save answers 200");
    c.ok(drained(web.eng), "the engine thread ran everything queued");
    web.eng.stop();
    c.ok(tx.seen.size() >= 2 && bearerIs(tx.seen[1], "mk_test_NEWKEY_WEB") &&
             tx.seen[1].path.find("/v1/connectors") != std::string::npos,
         "the workspace is probed again with the NEW key");
    c.ok(rig.workspaceProbed(), "so the Studio connector's answer is the new key's");
  }
  clearEnv();
}

// CUM-530 gate (critic): the device bounds the router catalog it keeps, the hosted
// consumer of the same untrusted body did not, so a body of thousands of ids was
// held whole and served on every GET /api/orch. It now takes the device's rule:
// at most 128 rows, the router default first, the flagships kept wherever they are
// listed, and no id that no upstream runs.
static void testHostedCatalogIsBounded(ndtest::Ctx& c) {
  std::printf("  -- (h) the hosted router catalog is bounded like the device's --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  std::string body = "{\"object\":\"list\",\"data\":[";
  for (int i = 0; i < 4000; ++i) body += "{\"id\":\"zai/m" + std::to_string(i) + "\"},";
  body += "{\"id\":\"mistral/mistral/zai-glm-5-3\"},{\"id\":\"openai/gpt-5.6\"},"
          "{\"id\":\"anthropic/claude-opus-5\"},{\"id\":\"zai/glm-5.3\"}]}";
  FakeHttpTransport tx;
  tx.script.push_back(routerModels(200, body));
  Config cfg;
  NimbusdRig rig(cfg, opts("pv-h"), &tx);
  c.ok(rig.verifyProvider("cumulo"), "the verify succeeds");
  const auto models = rig.verifyState("cumulo").models;
  c.ok(!models.empty() && models.size() <= 128, "at most 128 rows are kept (got " + std::to_string(models.size()) + ")");
  c.ok(!models.empty() && models[0].id == "openai/gpt-5.6", "the router default comes first");
  std::string all = ",";
  for (const auto& m : models) all += m.id + ",";
  c.ok(has(all, ",anthropic/claude-opus-5,") && has(all, ",zai/glm-5.3,"), "the flagships listed last are kept");
  c.ok(!has(all, "zai-glm-5-3"), "an id no upstream runs is not offered");
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== provider verify + router model catalog (CUM-545, offline) ===\n");
  testVerifyHarvests(c);
  testVerdicts(c);
  testTurnMarksVerified(c);
  testWebTriggers(c);
  testSpawnOnHarvestedModel(c);
  testZaiBaseIsABareHost(c);
  testHostedCatalogIsBounded(c);
  testMemberSpawnRunsTheSubModel(c);
  testKeySavedWhileVerifyQueued(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
