// test_router_embed - CUM-540: long-term memory works on a one-key (Cumulo-only)
// hosted instance, and the hosted page's embedding settings really apply.
//
// The class rules (device parity, src/agent/adapters/embeddings.cpp CUM-302):
//   (a) with NO direct key for the embed provider and a Cumulo key, every
//       embedding call goes through the router on the SAME upstream and model
//       (<router>/router/mistral/v1/embeddings, Cumulo key as the bearer, Mistral's
//       body dialect), so memory.write stores a vector and memory.search finds it;
//       /api/mem/stats reports embedAvailable true (route "router");
//   (b) a direct key wins over the router;
//   (c) with no key at all nothing is sent and stats says so;
//   (d) the device's "cumulo" embed provider is the /router/openai/v1 route;
//   (e) the page's verify-then-save: embedverify runs a real call and saves
//       nothing; embedcfg refuses a model switch while memories exist unless the
//       owner erases them, then applies and persists across a restart.
// Offline: an injected FakeHttpTransport; no network, no real keys.
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

static const char* kKey = "cumulo_sk_test_EMBED";

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
  o.embeddings = true;
  o.embedHost = "mistral";
  o.embedModel = "mistral-embed";
  o.embedDims = 8;
  o.toolLoop = false;
  return o;
}

// An embeddings response with `n` floats (a fixed, non-zero direction).
static Exchange embedOk(const std::string& host, const std::string& path, int n) {
  std::string v;
  for (int i = 0; i < n; i++) v += (i ? "," : "") + std::string(i % 2 ? "0.25" : "0.5");
  Exchange e;
  e.expectHost = host;
  e.expectPathContains = path;
  e.body = "{\"data\":[{\"embedding\":[" + v + "]}],\"usage\":{\"prompt_tokens\":3}}";
  return e;
}

static std::string header(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return std::string();
}

// One /api/* call through the real WebApi with a running engine thread.
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

static void testRouterCarriesMemory(ndtest::Ctx& c) {
  std::printf("  -- (a) Cumulo-only: memory.write and memory.search embed through the router --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));
  tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-a"), &tx);
  c.ok(rig.embedAvailable(), "embeddings are available with only a Cumulo key");
  const std::string w = rig.callTool("memory.write", R"({"content":"Priya is vegetarian"})");
  c.eqi(rig.vectors().size(), 1, "memory.write stored a vector");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.eq(r.host, kCumuloHost, "the embedding goes to the router host");
    c.eq(r.path, "/router/mistral/v1/embeddings", "on the configured upstream (same vector space)");
    c.eq(header(r, "Authorization"), std::string("Bearer ") + kKey, "the Cumulo key is the bearer");
    c.ok(has(r.body, "\"model\":\"mistral-embed\""), "the configured model rides the body");
    c.ok(!has(r.body, "dimensions"), "Mistral's dialect: no OpenAI dimensions field (CUM-469)");
  }
  const std::string q = rig.callTool("memory.search", R"({"query":"what does Priya eat"})");
  c.ok(has(q, "vegetarian"), "memory.search finds the stored fact");
  {
    Web web(rig);
    const ApiResp st = web.call("GET", "/api/mem/stats");
    c.ok(has(st.body, "\"embedAvailable\":true"), "/api/mem/stats reports embedAvailable true");
    c.ok(has(st.body, "\"route\":\"router\""), "/api/mem/stats names the router route");
  }
  clearEnv();
}

static void testDirectKeyWins(ndtest::Ctx& c) {
  std::printf("  -- (b) a direct key wins over the router --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  setenv("MISTRAL_API_KEY", "mk_test_EMBED", 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk("api.mistral.ai", "/v1/embeddings", 8));
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-b"), &tx);
  rig.callTool("memory.write", R"({"content":"direct"})");
  c.ok(!tx.seen.empty() && tx.seen[0].host == "api.mistral.ai", "the direct Mistral key carries the call");
  c.ok(!tx.seen.empty() && tx.seen[0].path == "/v1/embeddings", "on the direct path");
  clearEnv();
}

static void testNoKeyNoRequest(ndtest::Ctx& c) {
  std::printf("  -- (c) no key: nothing is sent and stats says so --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("emb-c"), &tx);
  c.ok(!rig.embedAvailable(), "no embeddings without any key");
  const std::string w = rig.callTool("memory.write", R"({"content":"x"})");
  c.eqi((long)tx.seen.size(), 0, "no request is sent");
  c.eqi(rig.vectors().size(), 0, "nothing stored");
  Web web(rig);
  c.ok(has(web.call("GET", "/api/mem/stats").body, "\"embedAvailable\":false"), "stats reports false");
  clearEnv();
}

static void testCumuloProviderRoute(ndtest::Ctx& c) {
  std::printf("  -- (d) the cumulo embed provider rides /router/openai/v1 --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 8));
  Config cfg;
  NimbusdRig::Options o = opts("emb-d");
  o.embedHost = "cumulo";
  o.embedModel = "text-embedding-3-small";
  NimbusdRig rig(cfg, o, &tx);
  rig.callTool("memory.write", R"({"content":"openai space"})");
  c.ok(!tx.seen.empty() && tx.seen[0].path == "/router/openai/v1/embeddings", "the device's router route");
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, "\"dimensions\":8"), "OpenAI's dialect carries dimensions");
  c.eqi(rig.vectors().size(), 1, "stored");
  clearEnv();
}

static void testVerifyThenSave(ndtest::Ctx& c) {
  std::printf("  -- (e) embedverify saves nothing; embedcfg is locked by memories, reset applies --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  NimbusdRig::Options o = opts("emb-e");
  {
    FakeHttpTransport tx;
    tx.script.push_back(embedOk(kCumuloHost, "/router/mistral/v1/embeddings", 8));    // a memory
    tx.script.push_back(embedOk(kCumuloHost, "/router/openai/v1/embeddings", 16));    // verify
    Config cfg;
    NimbusdRig rig(cfg, o, &tx);
    rig.callTool("memory.write", R"({"content":"keep me"})");
    Web web(rig);
    const std::string cand = "provider=openai&model=text-embedding-3-small&dims=16";
    const ApiResp v = web.call("POST", "/api/mem/embedverify", cand);
    c.eqi(v.status, 200, "embedverify answers 200 for a working candidate");
    c.ok(has(v.body, "\"ok\":true") && has(v.body, "\"dims\":16"), "the live call's width is reported");
    const ApiResp locked = web.call("POST", "/api/mem/embedcfg", cand);
    c.eqi(locked.status, 409, "a switch is refused while memories exist");
    c.ok(has(locked.body, "erase"), "the refusal says how to switch");
    c.eqi(rig.vectors().size(), 1, "the memory is untouched");
    const ApiResp ok = web.call("POST", "/api/mem/embedcfg", cand + "&reset=1");
    c.eqi(ok.status, 200, "reset=1 applies the switch");
    c.ok(has(ok.body, "\"provider\":\"openai\"") && has(ok.body, "\"dims\":16"), "the new config reads back");
    c.eqi(rig.vectors().size(), 0, "the old-model memories were erased");
  }
  {
    Config cfg;   // a restart on the same volume
    NimbusdRig rig(cfg, o);
    c.eq(rig.embedProvider(), "openai", "the embed provider survives a restart");
    c.eqi(rig.embedWidth(), 16, "the width survives a restart");
  }
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== embeddings through the Cumulo router (CUM-540, offline) ===\n");
  testRouterCarriesMemory(c);
  testDirectKeyWins(c);
  testNoKeyNoRequest(c);
  testCumuloProviderRoute(c);
  testVerifyThenSave(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
