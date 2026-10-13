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
//   (c) a keyed BYOK provider in the configured ladder still wins over the router;
//   (d) the Joiner-VN3 case: a spawn that NAMES a provider with no key (openai,
//       gpt-5.6) runs on the router as openai/gpt-5.6 instead of "spawn failed 2",
//       and one naming an anthropic model the verified router catalog lists runs
//       on the router's Anthropic wire; one the router does not offer is refused
//       with the real reason and sends nothing; an approved member naming a
//       provider with no key runs the configured router sub model (only the owner
//       picks a spawn's model on the owner's key).
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
  // The synthesis turn has no user row of its own: the owner's request must still
  // be in its conversation window (the in-flight row id may not outlive its turn).
  if (tx.seen.size() >= 3)
    c.ok(has(tx.seen[2].body, "- user: research the FCA Consumer Duty"),
         "the synthesis turn's window still carries the owner's request");
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

// A head turn that spawns one helper naming `provider` and `model`.
static std::string spawnTurn(const char* provider, const char* model) {
  return std::string("{\"reply\":\"I've started one helper.\",\"memory\":\"\",\"ask\":\"\",\"session_ops\":[{\"op\":"
                     "\"spawn\",\"id\":null,\"task\":\"Summarise the UK FCA Consumer Duty\",\"provider\":\"") +
         provider + "\",\"model\":\"" + model + "\"}]}";
}

static const char* kRouterCatalog =
    "{\"object\":\"list\",\"data\":["
    "{\"id\":\"openai/gpt-5.6\",\"object\":\"model\",\"owned_by\":\"openai\",\"size_class\":\"large\"},"
    "{\"id\":\"anthropic/claude-sonnet-5\",\"object\":\"model\",\"owned_by\":\"anthropic\",\"size_class\":\"medium\"}]}";

static bool bearerIs(const agent::HttpRequest& r, const std::string& key) {
  for (const auto& h : r.headers)
    if (h.first == "Authorization" && h.second == "Bearer " + key) return true;
  return false;
}

static void testNamedKeylessProviderRunsOnRouter(ndtest::Ctx& c) {
  std::printf("  -- (d) a spawn naming openai on a Cumulo-only instance runs on the router --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(routerHead(spawnTurn("openai", "gpt-5.6")));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.expectPathContains = "/router/openai/v1/chat/completions";
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"Consumer Duty: good outcomes for retail customers.\"}}],"
             "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"The helper is back: good outcomes for retail customers.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-d"), &tx);
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("owner", "spawn one helper to summarise the UK FCA Consumer Duty");
  c.ok(inbox.waitFor("I've started one helper.", 5000), "the head turn replied and spawned");
  c.ok(inbox.waitFor("good outcomes for retail customers.", 15000),
       "the synthesis turn delivered the router sub's result");
  c.ok(!inbox.waitFor("Couldn't start that agent", 200), "the spawn is not refused");
  eng.stop();
  c.eqi((long)tx.seen.size(), 3, "three router calls: head, sub, synthesis (none to api.openai.com)");
  if (tx.seen.size() >= 2) {
    const agent::HttpRequest& r = tx.seen[1];
    c.eq(r.host, kCumuloHost, "the sub goes to the router host");
    c.ok(has(r.path, "/router/openai/v1/chat/completions"), "on the router's openai upstream");
    c.ok(has(r.body, "\"model\":\"gpt-5.6\""), "with the model the spawn named");
    c.ok(bearerIs(r, kKey), "and the Cumulo key as the bearer");
  }
  clearEnv();
}

static void testNamedAnthropicRunsOnRouterCatalog(ndtest::Ctx& c) {
  std::printf("  -- (d2) an anthropic model the router catalog lists runs on its Anthropic wire --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  Exchange models;
  models.expectHost = kCumuloHost;
  models.expectPathContains = "/router/models";
  models.body = kRouterCatalog;
  tx.script.push_back(models);
  tx.script.push_back(routerHead(spawnTurn("anthropic", "claude-sonnet-5")));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.expectPathContains = "/router/anthropic/v1/messages";
  sub.body = "{\"content\":[{\"type\":\"text\",\"text\":\"Consumer Duty: fair value.\"}],"
             "\"usage\":{\"input_tokens\":20,\"output_tokens\":8}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"Done: fair value.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-d2"), &tx);
  c.ok(rig.verifyProvider("cumulo"), "the router catalog is harvested");
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("owner", "spawn one helper on Claude");
  c.ok(inbox.waitFor("Done: fair value.", 15000), "the router sub's result came back");
  eng.stop();
  c.eqi((long)tx.seen.size(), 4, "verify, head, sub, synthesis");
  if (tx.seen.size() >= 3) {
    c.ok(has(tx.seen[2].path, "/router/anthropic/v1/messages"), "the sub runs on the router's Anthropic wire");
    c.ok(has(tx.seen[2].body, "\"model\":\"claude-sonnet-5\""), "with the model the spawn named");
  }
  clearEnv();
}

static void testNamedProviderNothingOffers(ndtest::Ctx& c) {
  std::printf("  -- (d3) a model neither a key nor the router offers is refused with the reason --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  // Neither the named model nor mistral's own sub model (mistral-large-latest) is
  // offered: the instance holds no router catalog, so the router offers only its
  // default (an openai model named for openai is carried, see (d)).
  tx.script.push_back(routerHead(spawnTurn("mistral", "mistral-medium-9")));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-d3"), &tx);
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("owner", "spawn a helper on mistral");
  c.ok(inbox.waitFor("Couldn't start that agent on mistral. No mistral key is set, and the Cumulo "
                     "router does not offer mistral/mistral-medium-9.", 8000),
       "the owner hears the real reason");
  eng.stop();
  c.eqi((long)tx.seen.size(), 1, "only the head turn: no keyless or unoffered sub request");
  clearEnv();
}

static void testMemberKeylessProviderRunsTheSubModel(ndtest::Ctx& c) {
  std::printf("  -- (d4) a member naming anthropic runs the configured router sub model --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  Exchange models;
  models.expectHost = kCumuloHost;
  models.expectPathContains = "/router/models";
  models.body = "{\"object\":\"list\",\"data\":["
                "{\"id\":\"openai/gpt-5.6\",\"object\":\"model\",\"size_class\":\"large\"},"
                "{\"id\":\"anthropic/claude-sonnet-4-6\",\"object\":\"model\",\"size_class\":\"large\"}]}";
  tx.script.push_back(models);
  tx.script.push_back(routerHead(spawnTurn("anthropic", "claude-sonnet-4-6")));
  Exchange sub;
  sub.expectHost = kCumuloHost;
  sub.body = "{\"choices\":[{\"message\":{\"content\":\"Consumer Duty: outcomes.\"}}],"
             "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}";
  tx.script.push_back(sub);
  tx.script.push_back(routerHead("{\"reply\":\"Member summary: outcomes.\",\"memory\":\"\"}"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ck-d4"), &tx);
  c.ok(rig.verifyProvider("cumulo"), "the router catalog lists anthropic/claude-sonnet-4-6");
  std::string err;
  c.ok(rig.telegramAccess().approve("424242", "Ana", err), "a member chat is approved");
  Inbox inbox;
  rig.setDeliver([&inbox](const std::string&, const std::string& t) { inbox.push(t); });
  EngineThread eng(&rig);
  eng.start();
  eng.postMessage("424242", "spawn a helper on Claude to summarise the Consumer Duty");
  c.ok(inbox.waitFor("Member summary: outcomes.", 15000), "the member's helper ran and reported");
  eng.stop();
  if (tx.seen.size() >= 3) {
    c.ok(has(tx.seen[2].path, "/router/openai/v1/chat/completions"), "on the configured router sub model's upstream");
    c.ok(has(tx.seen[2].body, "\"model\":\"gpt-5.6\"") && !has(tx.seen[2].body, "claude"),
         "the router sub model, never the anthropic model the member's provider named");
  } else {
    c.ok(false, "the sub request was sent");
  }
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== Cumulo key is a keyed provider (CUM-541, offline) ===\n");
  testPromptListsCumulo(c);
  testSpawnRunsOnRouter(c);
  testByokLadderWins(c);
  testNamedKeylessProviderRunsOnRouter(c);
  testNamedAnthropicRunsOnRouterCatalog(c);
  testNamedProviderNothingOffers(c);
  testMemberKeylessProviderRunsTheSubModel(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
