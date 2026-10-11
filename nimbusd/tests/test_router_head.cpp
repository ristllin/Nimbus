// test_router_head - CUM-539: the hosted Cumulo head runs the SELECTED upstream's
// native wire over the router (device parity, CUM-242), and a router refusal never
// leaves the assistant dead.
//
// The class rules under test, each over the injected FakeHttpTransport (offline,
// no keys, canned provider bodies):
//   (a) an "anthropic/<model>" selector round-trips on the Anthropic Messages wire
//       via /router/anthropic/v1/messages: Cumulo key as the bearer, no x-api-key,
//       the bare model in the body;
//   (b) with the loop on, the registry tools ride the request and a mid-turn tool
//       call runs - every round through the router;
//   (c) a refused pick (403 model_not_priced) is answered on the router default in
//       the SAME turn, the reply names the refusal and the fix, the pick goes back
//       to the model in use (GET /api/orch shows it), and the next turn answers;
//   (d) the fallback is the last selector that WORKED, not always the default;
//   (e) an account-wide refusal (402 credits) says what to do instead of "rejected
//       this device's key", keeps the pick, and the next turn still answers;
//   (f) a paused upstream (402 funding_cap_reached) answers on the default and keeps
//       the pick;
//   (g) zai keeps the single-shot chat-completions path with json_object;
//   (h) a refusal AFTER a tool ran is not re-run (no repeated side effects).
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

static const char* kKey = "cumulo_sk_test_HEAD";

static void clearProviderEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY",
                        "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options baseOpt(const std::string& tag, bool loop) {
  NimbusdRig::Options opt;
  opt.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.toolLoop = loop;
  return opt;
}

static std::string headerOf(const agent::HttpRequest& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.first == name) return h.second;
  return std::string();
}

static Exchange ex(const std::string& path, int status, const std::string& body) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = path;
  e.status = status;
  e.body = body;
  return e;
}

// Canned single-shot answers per upstream wire (the tool input / output_text /
// message.output IS the turn).
static std::string antBody(const std::string& reply) {
  return "{\"content\":[{\"type\":\"tool_use\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"" + reply +
         "\",\"memory\":\"\"}}],\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,"
         "\"output_tokens\":2}}";
}
static std::string respBody(const std::string& reply) {
  return "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":"
         "[{\"type\":\"output_text\",\"text\":\"{\\\"reply\\\":\\\"" + reply +
         "\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}";
}
static std::string misBody(const std::string& reply) {
  return "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\",\"content\":"
         "\"{\\\"reply\\\":\\\"" + reply + "\\\",\\\"memory\\\":\\\"\\\"}\"}],\"usage\":"
         "{\"prompt_tokens\":10,\"completion_tokens\":2}}";
}
static std::string chatBody(const std::string& reply) {
  return "{\"choices\":[{\"message\":{\"content\":\"{\\\"reply\\\":\\\"" + reply +
         "\\\",\\\"memory\\\":\\\"\\\"}\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";
}
static std::string refusal(const char* reason) {
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason +
         "\",\"charged\":false,\"message\":\"refused\"}";
}

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// GET /api/orch through the real WebApi (snapshot reads only, no engine thread).
static std::string orchJson(NimbusdRig& rig) {
  EngineThread eng(&rig);
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  ApiResp out;
  api.handle("GET", "/api/orch", "", out);
  return out.body;
}

static void testClaudeOnMessagesWire(ndtest::Ctx& c) {
  std::printf("  -- (a) anthropic/<model> rides /router/anthropic/v1/messages --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/anthropic/v1/messages", 200, antBody("hi from claude")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-a", false), &tx);
  c.ok(rig.applyOrchModel("cumulo", "anthropic/claude-sonnet-5"), "pick a Claude router model");
  const TurnRecord t = rig.say("owner", "hello");
  c.eqi((long)tx.seen.size(), 1, "one request");
  if (!tx.seen.empty()) {
    const agent::HttpRequest& r = tx.seen[0];
    c.eq(r.host, kCumuloHost, "the request goes to the router host");
    c.eq(r.path, "/router/anthropic/v1/messages", "the Anthropic Messages wire under /router/anthropic");
    c.eq(headerOf(r, "Authorization"), std::string("Bearer ") + kKey, "the Cumulo key is the bearer");
    c.eq(headerOf(r, "x-api-key"), "", "no client x-api-key rides the router request");
    c.ok(has(r.body, "\"model\":\"claude-sonnet-5\""), "the body carries the bare model id");
  }
  c.ok(has(t.reply, "hi from claude"), "the Claude answer reaches the owner");
  clearProviderEnv();
}

static void testToolsAttachAndRun(ndtest::Ctx& c) {
  std::printf("  -- (b) the loop advertises the registry tools and runs one mid-turn --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/anthropic/v1/messages", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
      "\"input\":{\"query\":\"tea\"}}],\"stop_reason\":\"tool_use\","
      "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}"));
  tx.script.push_back(ex("/router/anthropic/v1/messages", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
      "\"input\":{\"reply\":\"looked it up\",\"memory\":\"\"}}],\"stop_reason\":\"tool_use\","
      "\"usage\":{\"input_tokens\":60,\"output_tokens\":15}}"));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-b", true), &tx);
  rig.applyOrchModel("cumulo", "anthropic/claude-opus-5");
  const TurnRecord t = rig.say("owner", "what tea do I like?");
  c.eqi((long)tx.seen.size(), 2, "two rounds: the tool call, then the final turn");
  if (!tx.seen.empty()) {
    c.ok(has(tx.seen[0].body, "\"name\":\"memory_write\""), "memory_write is advertised on the wire");
    c.ok(has(tx.seen[0].body, "\"name\":\"memory_search\""), "memory_search is advertised on the wire");
  }
  bool allRouter = !tx.seen.empty();
  for (const auto& r : tx.seen) allRouter = allRouter && r.path.rfind("/router/anthropic/", 0) == 0;
  c.ok(allRouter, "every round travels through /router/anthropic");
  c.ok(!t.toolCalls.empty() && has(t.toolCalls[0], "memory_search("), "the mid-turn tool ran");
  c.ok(has(t.reply, "looked it up"), "the loop's final reply reaches the owner");
  clearProviderEnv();
}

static void testRefusedPickFallsBack(ndtest::Ctx& c) {
  std::printf("  -- (c) a refused pick answers on the default and the next turn works --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/anthropic/v1/messages", 403, refusal("model_not_priced")));
  tx.script.push_back(ex("/router/openai/v1/responses", 200, respBody("answered by the default")));
  tx.script.push_back(ex("/router/openai/v1/responses", 200, respBody("still here")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-c", false), &tx);
  rig.applyOrchModel("cumulo", "anthropic/claude-sonnet-5");
  const TurnRecord t1 = rig.say("owner", "which model are you?");
  c.ok(has(t1.reply, "does not offer anthropic/claude-sonnet-5"), "the reply names the refused model");
  c.ok(has(t1.reply, "Choose another model under Models"), "the reply names the fix");
  c.ok(has(t1.reply, "answered by the default"), "the same turn is answered on the router default");
  c.ok(!has(t1.reply, "rejected this device's key"), "the refusal is never blamed on the key");
  c.eq(rig.modelFor("cumulo"), "gpt-5.6", "the pick goes back to the model in use");
  c.ok(has(orchJson(rig), "\"orchModel\":\"gpt-5.6\""), "GET /api/orch shows the model in use");
  const TurnRecord t2 = rig.say("owner", "and now?");
  c.ok(has(t2.reply, "still here"), "the next turn answers");
  c.eqi((long)tx.seen.size(), 3, "the next turn goes straight to the working model (no retry of the refusal)");
  if (tx.seen.size() == 3) c.eq(tx.seen[2].path, "/router/openai/v1/responses", "next turn on the default");
  clearProviderEnv();
}

static void testFallbackIsLastWorking(ndtest::Ctx& c) {
  std::printf("  -- (d) a refused pick goes back to the last selector that worked --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/mistral/v1/conversations", 200, misBody("mistral here")));
  tx.script.push_back(ex("/router/anthropic/v1/messages", 403, refusal("endpoint_not_allowed")));
  tx.script.push_back(ex("/router/mistral/v1/conversations", 200, misBody("mistral again")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-d", false), &tx);
  rig.applyOrchModel("cumulo", "mistral/mistral-large-latest");
  c.ok(has(rig.say("owner", "hi").reply, "mistral here"), "the Mistral pick works");
  rig.applyOrchModel("cumulo", "anthropic/claude-sonnet-5");
  const TurnRecord t = rig.say("owner", "hi again");
  c.ok(has(t.reply, "so mistral/mistral-large-latest answered"), "the notice names the model that answered");
  c.ok(has(t.reply, "mistral again"), "answered on the last working selector");
  c.eq(rig.modelFor("cumulo"), "mistral/mistral-large-latest", "the pick is the last working selector");
  clearProviderEnv();
}

static void testAccountRefusal(ndtest::Ctx& c) {
  std::printf("  -- (e) a 402 credits refusal says what to do; the next turn answers --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  // The engine retries a failed head once on a fresh conversation: both refused.
  tx.script.push_back(ex("/router/openai/v1/responses", 402, refusal("credits_or_subscription_required")));
  tx.script.push_back(ex("/router/openai/v1/responses", 402, refusal("credits_or_subscription_required")));
  tx.script.push_back(ex("/router/openai/v1/responses", 200, respBody("topped up")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-e", false), &tx);
  const TurnRecord t = rig.say("owner", "hello");
  c.ok(has(t.reply, "needs credits or a plan"), "the reply names the account refusal");
  c.ok(has(t.reply, "Add credits in the portal"), "the reply names the fix");
  c.ok(!has(t.reply, "rejected this device's key"), "never blamed on the key");
  c.eq(rig.modelFor("cumulo"), "gpt-5.6", "the pick is unchanged");
  c.ok(has(rig.say("owner", "again").reply, "topped up"), "the next turn answers");
  clearProviderEnv();
}

static void testUpstreamPausedKeepsPick(ndtest::Ctx& c) {
  std::printf("  -- (f) a paused upstream answers on the default and keeps the pick --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/anthropic/v1/messages", 402, refusal("funding_cap_reached")));
  tx.script.push_back(ex("/router/openai/v1/responses", 200, respBody("default meanwhile")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-f", false), &tx);
  rig.applyOrchModel("cumulo", "anthropic/claude-sonnet-5");
  const TurnRecord t = rig.say("owner", "hello");
  c.ok(has(t.reply, "has paused anthropic/claude-sonnet-5"), "the notice names the paused model");
  c.ok(has(t.reply, "Your model choice is kept"), "the notice says the pick stays");
  c.ok(has(t.reply, "default meanwhile"), "answered on the default");
  c.eq(rig.modelFor("cumulo"), "anthropic/claude-sonnet-5", "the pick is kept");
  clearProviderEnv();
}

static void testZaiKeepsCustomPath(ndtest::Ctx& c) {
  std::printf("  -- (g) zai keeps single-shot chat completions with json_object --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/zai/chat/completions", 200, chatBody("glm here")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-g", true), &tx);
  rig.applyOrchModel("cumulo", "zai/glm-5.3");
  const TurnRecord t = rig.say("owner", "hello");
  c.eqi((long)tx.seen.size(), 1, "single shot (no tool loop on this path)");
  if (!tx.seen.empty()) {
    c.eq(tx.seen[0].path, "/router/zai/chat/completions", "the zai path has no /v1");
    c.ok(has(tx.seen[0].body, "json_object"), "json_object, not json_schema (CUM-495)");
    c.ok(has(tx.seen[0].body, "\"model\":\"glm-5.3\""), "the bare model id");
  }
  c.ok(has(t.reply, "glm here"), "the answer reaches the owner");
  clearProviderEnv();
}

static void testMidLoopRefusalNotRerun(ndtest::Ctx& c) {
  std::printf("  -- (h) a refusal after a tool ran is not re-run --\n");
  clearProviderEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeHttpTransport tx;
  tx.script.push_back(ex("/router/anthropic/v1/messages", 200,
      "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
      "\"input\":{\"query\":\"tea\"}}],\"stop_reason\":\"tool_use\","
      "\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}"));
  tx.script.push_back(ex("/router/anthropic/v1/messages", 403, refusal("model_not_priced")));
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("rh-h", true), &tx);
  rig.applyOrchModel("cumulo", "anthropic/claude-opus-5");
  const TurnRecord t = rig.say("owner", "look it up");
  c.eqi((long)tx.seen.size(), 2, "no fallback request after a tool already ran");
  c.ok(!t.toolCalls.empty(), "the tool did run");
  c.eq(rig.modelFor("cumulo"), "anthropic/claude-opus-5", "the pick is not touched mid-loop");
  c.ok(!has(t.reply, "rejected this device's key"), "the failure is not blamed on the key");
  clearProviderEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== Cumulo router head parity (CUM-539, offline) ===\n");
  testClaudeOnMessagesWire(c);
  testToolsAttachAndRun(c);
  testRefusedPickFallsBack(c);
  testFallbackIsLastWorking(c);
  testAccountRefusal(c);
  testUpstreamPausedKeepsPick(c);
  testZaiKeepsCustomPath(c);
  testMidLoopRefusalNotRerun(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
