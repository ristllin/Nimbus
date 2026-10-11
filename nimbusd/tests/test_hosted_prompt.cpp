// test_hosted_prompt - CUM-542: every hosted head is told the truth about the
// instance it runs on.
//
// The composer's identity block said "Current date-time: UNKNOWN (device clock not
// synced)" on every hosted turn (nimbusd never set ComposeInputs.now) and described
// an ESP32 desk device with "Connectivity: offline", so a Virtual Nimbus refused
// time questions and called itself offline. The class rule, over EVERY head this
// daemon registers (openai, anthropic, mistral, cumulo, zai): the instructions carry
// the real clock in the owner's time zone, the engine version and a hosted identity,
// and never the desk-device or offline text.
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

// One turn on a rig whose ONLY key is `env`; returns the first request body.
static std::string firstHeadBody(const char* env, const std::string& tag) {
  clearEnv();
  setenv(env, "test_key_PROMPT", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});   // the turn fails; we read the request
  Config cfg;
  NimbusdRig rig(cfg, opts(tag), &tx);
  rig.say("owner", "what time is it?");
  clearEnv();
  return tx.seen.empty() ? std::string() : tx.seen[0].body;
}

static void testEveryHeadGetsTheHostedTruth(ndtest::Ctx& c) {
  std::printf("  -- every head: real clock, version, hosted identity --\n");
  const char* kEnvs[][2] = {{"OPENAI_API_KEY", "openai"},   {"ANTHROPIC_API_KEY", "anthropic"},
                            {"MISTRAL_API_KEY", "mistral"}, {"CUMULO_API_KEY", "cumulo"},
                            {"Z_AI_TOKEN", "zai"}};
  for (const auto& e : kEnvs) {
    const std::string body = firstHeadBody(e[0], std::string("hp-") + e[1]);
    const std::string who = std::string(e[1]) + " head: ";
    c.ok(!body.empty(), who + "dispatched");
    c.ok(has(body, "Current date-time: 20"), who + "the prompt carries the real date-time");
    c.ok(!has(body, "date-time: UNKNOWN"), who + "never the unsynced-clock line");
    c.ok(has(body, "Firmware: " NIMBUS_FW_VERSION), who + "the engine version");
    c.ok(has(body, "hosted cloud instance that is always online"), who + "the hosted identity");
    c.ok(!has(body, "ESP32"), who + "no desk-device (ESP32) text");
    c.ok(!has(body, "Connectivity: offline"), who + "never told it is offline");
    c.ok(!has(body, "-LED ring") && !has(body, "color touchscreen"), who + "no ring or screen claimed");
  }
}

static void testClockFollowsTheOwnersZone(ndtest::Ctx& c) {
  std::printf("  -- the prompt's clock follows the saved time zone --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_PROMPT", 1);
  FakeHttpTransport tx;
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});
  Config cfg;
  NimbusdRig rig(cfg, opts("hp-tz"), &tx);
  {
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);
    ApiResp out;
    api.handle("POST", "/api/config", "devTz=QQT-4", out);
    c.eqi(out.status, 200, "the zone saves");
    eng.stop();
  }
  rig.say("owner", "what time is it?");
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, " QQT (trust THIS"), "the next turn's date-time is in the owner's zone");
  {
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);
    ApiResp out;
    api.handle("POST", "/api/config", "devTz=", out);   // back to UTC for the rest of the run
    eng.stop();
  }
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted prompt truth (CUM-542, offline) ===\n");
  testEveryHeadGetsTheHostedTruth(c);
  testClockFollowsTheOwnersZone(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
