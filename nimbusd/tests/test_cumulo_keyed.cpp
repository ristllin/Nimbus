// test_cumulo_keyed - CUM-541: a Cumulo-only hosted instance treats its own router
// key as a keyed provider, so the head is told it has models and a spawn runs on
// the router instead of "No provider is configured to run that agent".
//
//   (a) the head prompt lists cumulo in [AVAILABLE MODELS] (never "(none
//       configured)") on an instance whose only key is the Cumulo key;
//   (b) a spawn that names no provider runs on the router (the configured ladder
//       has no keyed entry, so the router key is the next rung), on the
//       OpenAI-compatible sub wire with the Cumulo key, and the synthesis turn
//       delivers its result;
//   (c) a keyed BYOK provider in the configured ladder still wins over the router.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;

static const char* kKey = "cumulo_sk_test_KEYED";

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
  o.toolLoop = false;   // single-shot heads: one scripted exchange per turn
  return o;
}

static std::string esc(const std::string& s) {
  std::string out;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out;
}

// A router head turn (default selector: OpenAI Responses over /router/openai).
static Exchange routerHead(const std::string& turnJson) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/responses";
  e.body = "{\"id\":\"r1\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":"
           "[{\"type\":\"output_text\",\"text\":\"" + esc(turnJson) +
           "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
  return e;
}

struct Inbox {
  std::mutex mu;
  std::vector<std::string> msgs;
  void push(const std::string& t) {
    std::lock_guard<std::mutex> lk(mu);
    msgs.push_back(t);
  }
  bool waitFor(const std::string& needle, int ms) {
    for (int i = 0; i < ms / 20; i++) {
      {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& m : msgs)
          if (has(m, needle)) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

static void testPromptListsCumulo(ndtest::Ctx& c) {
  std::printf("  -- (a) a Cumulo-only head is told it has router models --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerHead("{\"reply\":\"hi\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-a"), &tx);
  rig.say("owner", "which models can you use?");
  c.ok(!tx.seen.empty(), "the turn dispatched");
  if (!tx.seen.empty()) {
    const std::string& b = tx.seen[0].body;
    c.ok(has(b, "cumulo: gpt-5.6;"), "[AVAILABLE MODELS] lists the router with its model");
    c.ok(!has(b, "(none configured)"), "the head is never told it has no models");
  }
  clearEnv();
}

static void testSpawnRunsOnRouter(ndtest::Ctx& c) {
  std::printf("  -- (b) a spawn with no provider runs on the router key --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerHead(
      "{\"reply\":\"Starting a researcher.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":\"spawn\","
      "\"id\":null,\"task\":\"Summarize the FCA Consumer Duty\",\"provider\":null,\"model\":null}]}"));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.expectPathContains = "/router/openai/v1/chat/completions";
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"Consumer Duty: fair value, understanding, outcomes.\"}}],"
             "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"Here is the summary: fair value first.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-b"), &tx);
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("owner", "research the FCA Consumer Duty");
  c.ok(inbox.waitFor("Starting a researcher.", 5000), "the head turn replied and spawned");
  c.ok(!inbox.waitFor("No provider is configured", 300), "the spawn is not refused for want of a key");
  c.ok(inbox.waitFor("fair value first", 15000), "the synthesis turn delivered the router sub's result");
  eng.stop();
  c.eqi((long)tx.seen.size(), 3, "three router calls: head, sub, synthesis");
  if (tx.seen.size() >= 2) {
    const agent::HttpRequest& r = tx.seen[1];
    c.eq(r.host, kCumuloHost, "the sub goes to the router host");
    c.ok(has(r.body, "\"model\":\"gpt-5.6\""), "the sub runs the router model");
    c.ok(has(r.body, "Summarize the FCA Consumer Duty"), "the sub carries the spawned task");
    bool bearer = false;
    for (const auto& h : r.headers) bearer = bearer || (h.first == "Authorization" && h.second == std::string("Bearer ") + kKey);
    c.ok(bearer, "the sub's bearer is the Cumulo key");
  }
  clearEnv();
}

static void testByokLadderWins(ndtest::Ctx& c) {
  std::printf("  -- (c) a keyed BYOK provider in the ladder still wins --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  setenv("MISTRAL_API_KEY", "mk_test_KEYED", 1);
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-c"));
  c.ok(rig.keyedForEngine("cumulo") && rig.keyedForEngine("mistral"), "both keys are seen");
  c.eq(rig.subLadder(), "mistral,openai,anthropic,cumulo", "the router is the LAST rung, after the configured order");
  // The JobEngine takes the FIRST keyed rung: mistral, ahead of the router.
  std::string first;
  for (const char* p : {"mistral", "openai", "anthropic", "cumulo"})
    if (first.empty() && rig.keyedForEngine(p)) first = p;
  c.eq(first, "mistral", "a keyed BYOK provider wins the default spawn");
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== Cumulo key is a keyed provider (CUM-541, offline) ===\n");
  testPromptListsCumulo(c);
  testSpawnRunsOnRouter(c);
  testByokLadderWins(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
