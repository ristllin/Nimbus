// test_cumulo_key - CUM-286: the Virtual Nimbus "one key, one balance" path.
//
// The daemon must consume the canonical env CUMULO_API_KEY and feed it into the
// SAME router seam the DEVICE uses (src/agent/orchestrator.cpp cumulo head): a
// keyed VN routes through the Cumulo router identically to a keyed device. These
// are class tests over the seam, not one instance:
//   (a) key set   -> health Provider ok AND a turn dispatches to the ROUTER wire
//                    (fixed base + /router/openai/v1 path + bearer of the key).
//   (b) key absent-> health Provider degraded, with the honest copy.
//   (c) BOTH a direct BYOK key and the Cumulo key -> the BYOK head wins, exactly
//       as the device's head resolution does (a keyed BYOK head beats the router
//       fallback). This pins the routing PRIORITY, not just the keyed case.
//
// Offline: the provider wire is an injected FakeHttpTransport; no network, no
// real keys. The turn's reply is canned orch_turn JSON.
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

// An OpenAI chat-completions response whose message content is the orch_turn
// JSON the custom/router head parses (same shape as the harness custom-wire
// suite). `content` must already be quote-escaped for embedding in JSON.
static std::string chatBody(const std::string& content) {
  return std::string("{\"choices\":[{\"message\":{\"content\":\"") + content +
         "\"}}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";
}

static std::string headerOf(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return std::string();
}

// Clear every provider-key env so a dev machine's exported keys can never make
// this test lie (the router-only cases MUST see no BYOK key).
static void clearProviderEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY",
                        "TAVILY_API_KEY", "CUMULO_API_KEY"})
    unsetenv(k);
}

static NimbusdRig::Options baseOpt(const std::string& tag) {
  NimbusdRig::Options opt;
  opt.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  opt.embeddings = false;   // no embedder -> no extra HTTP; the turn is one call
  opt.embedDims = 64;
  return opt;
}

// The exact "Provider" health detail the web surface renders (web_api.h). One
// GET /api/health through the real WebApi, so the honest copy is asserted, not
// just the boolean behind it.
static std::string providerHealthDetail(NimbusdRig& rig) {
  EngineThread eng(&rig);
  eng.start();   // start() refreshes the snapshot synchronously before the thread
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  ApiResp out;
  api.handle("GET", "/api/health", "", out);
  eng.stop();
  // The health JSON lists components; return the Provider component's detail.
  const std::string& b = out.body;
  const size_t p = b.find("\"label\":\"Provider\"");
  if (p == std::string::npos) return std::string("(no Provider component)");
  const size_t d = b.find("\"detail\":\"", p);
  if (d == std::string::npos) return std::string("(no detail)");
  const size_t s = d + 10;
  return b.substr(s, b.find('"', s) - s);
}

// (a) A Cumulo-only instance: health ok + the turn rides the router wire.
static void testKeyedRoutesToRouter(ndtest::Ctx& c) {
  std::printf("  -- (a) CUMULO_API_KEY set: health ok + router dispatch --\n");
  clearProviderEnv();
  const char* kKey = "cumulo_sk_test_ABCD1234";
  setenv("CUMULO_API_KEY", kKey, 1);

  FakeHttpTransport tx;
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/chat/completions";
  e.status = 200;
  e.body = chatBody("{\\\"reply\\\":\\\"hi from the router\\\",\\\"memory\\\":\\\"\\\"}");
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("cumkey-a"), &tx);

  c.ok(rig.hasCumulo(), "the rig sees the Cumulo router key from env");
  c.ok(rig.anyProviderConfigured(),
       "health input: a Cumulo-only instance IS provider-configured");
  c.eq(providerHealthDetail(rig), "at least one provider key configured",
       "health Provider reads ok with only a Cumulo key");

  auto t = rig.say("owner", "hello there");

  c.ok(!tx.seen.empty(), "the turn dispatched an HTTP request");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.eq(r.host, kCumuloHost, "dispatch host is the fixed router base");
    c.ok(r.path.find("/router/openai/v1/chat/completions") != std::string::npos,
         "dispatch path carries the router prefix (/router/openai/v1)");
    c.eq(headerOf(r, "Authorization"), std::string("Bearer ") + kKey,
         "the router bearer is the Cumulo key");
    c.ok(r.tls, "the router exchange is TLS");
  }
  c.eqi((long)tx.seen.size(), 1, "the router head is single-shot (exactly one call)");
  c.ok(t.ok, "the turn produced a reply");
  c.ok(t.reply.find("hi from the router") != std::string::npos,
       "the router's reply was delivered to the owner");

  clearProviderEnv();
}

// (b) No key at all: health degraded, honest next-step copy.
static void testUnkeyedDegradedHonest(ndtest::Ctx& c) {
  std::printf("  -- (b) no provider key: honest degraded --\n");
  clearProviderEnv();

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("cumkey-b"));   // real transport unused: no dispatch

  c.ok(!rig.hasCumulo(), "no Cumulo key present");
  c.ok(!rig.anyProviderConfigured(),
       "health input: a keyless instance is NOT provider-configured");
  c.eq(providerHealthDetail(rig), "no provider key - add one to reply",
       "health Provider stays honestly degraded with no key");
}

// (c) BOTH a direct provider key (Mistral) and the Cumulo key: the BYOK head
// wins, mirroring the device's head resolution (engine.cpp: a keyed priority
// head beats routerFallbackHost). The first dispatch must go to the BYOK
// provider, NOT the router.
static void testByokBeatsRouter(ndtest::Ctx& c) {
  std::printf("  -- (c) BYOK + Cumulo: the direct provider wins --\n");
  clearProviderEnv();
  setenv("MISTRAL_API_KEY", "mistral_sk_test_ZZZZ", 1);
  setenv("CUMULO_API_KEY", "cumulo_sk_test_ABCD1234", 1);

  FakeHttpTransport tx;
  // Accept whatever the FIRST dispatch is; we assert on where it went. A 200 with
  // an empty-ish body may fail the turn parse, but seen[0] still records the host.
  Exchange e;
  e.status = 200;
  e.body = chatBody("{\\\"reply\\\":\\\"ok\\\",\\\"memory\\\":\\\"\\\"}");
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("cumkey-c"), &tx);
  c.ok(rig.hasCumulo() && !cfg.providerKey("mistral").empty(),
       "both a BYOK (Mistral) key and the Cumulo key are present");

  rig.say("owner", "hello");

  c.ok(!tx.seen.empty(), "the turn dispatched an HTTP request");
  if (!tx.seen.empty()) {
    c.eq(tx.seen[0].host, std::string("api.mistral.ai"),
         "the keyed BYOK head wins - dispatch went to Mistral");
    c.ok(tx.seen[0].host != std::string(kCumuloHost),
         "the router did NOT win while a BYOK key is present");
  }

  clearProviderEnv();
}

// (d) CUM-288: a Cumulo-only instance must FOLD (compact its context) via the
// router wire, not just chat. The fold host ladder used to consult only the keyed
// BYOK heads, so a router-only instance could never fold - context grew unbounded
// until turns failed. The fold must reach the SAME router seam a turn does (fixed
// base + /router/openai/v1 path + bearer of the key), and modelFor("cumulo") must
// resolve the router default model (not "") so the fold budget derives honestly.
static void testCumuloOnlyFoldsViaRouter(ndtest::Ctx& c) {
  std::printf("  -- (d) CUMULO_API_KEY set: the fold rides the router wire --\n");
  clearProviderEnv();
  const char* kKey = "cumulo_sk_test_ABCD1234";
  setenv("CUMULO_API_KEY", kKey, 1);

  FakeHttpTransport tx;
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/chat/completions";
  e.status = 200;
  // The fold reply IS the anchored summary. The router head still returns the
  // full strict orch_turn shape (response_format json_schema); the fold keeps
  // only reply and ignores every other field.
  e.body = chatBody("{\\\"reply\\\":\\\"1. Owner intent: testing the fold\\\","
                    "\\\"ask\\\":\\\"\\\",\\\"memory\\\":\\\"\\\",\\\"device\\\":[],"
                    "\\\"mem_write\\\":[],\\\"mem_query\\\":[],\\\"session_ops\\\":[]}");
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("cumkey-d"), &tx);
  c.ok(rig.hasCumulo(), "the rig sees the Cumulo router key from env");

  // modelFor("cumulo") (wired into the provider deps as orchModel, the seam the
  // engine's budget derivation reads) resolves the router default, not "".
  c.eq(rig.modelFor("cumulo"), std::string(kCumuloModel),
       "modelFor(cumulo) resolves the router default model, not empty");

  auto& eng = rig.engine();
  // The pre-notice gate opens and the candidate ladder is the router head.
  c.ok(eng.canFoldNow(), "canFoldNow() is true for a Cumulo-only instance");
  const auto cands = eng.foldHostCandidates();
  c.eqi((long)cands.size(), 1, "the fold ladder has exactly the router head");
  if (!cands.empty())
    c.eq(cands[0], std::string(kCumuloSlug), "the fold candidate is the Cumulo router");

  std::string sum;
  auto fr = eng.runFold("owner", "prev summary", "- user: hi\n", sum);
  c.ok(fr == agent::TurnEngine::FoldResult::Ok, "runFold succeeded on the router head");
  c.ok(sum.find("Owner intent") != std::string::npos,
       "the router's compaction reply became the summary");

  c.ok(!tx.seen.empty(), "the fold dispatched an HTTP request");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.eq(r.host, kCumuloHost, "fold dispatch host is the fixed router base");
    c.ok(r.path.find("/router/openai/v1/chat/completions") != std::string::npos,
         "fold dispatch path carries the router prefix (/router/openai/v1)");
    c.eq(headerOf(r, "Authorization"), std::string("Bearer ") + kKey,
         "the fold's router bearer is the Cumulo key");
    c.ok(r.tls, "the fold exchange is TLS");
  }

  clearProviderEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== nimbusd Cumulo one-key path (CUM-286, offline) ===\n");

  testKeyedRoutesToRouter(c);
  testUnkeyedDegradedHonest(c);
  testByokBeatsRouter(c);
  testCumuloOnlyFoldsViaRouter(c);

  clearProviderEnv();
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
