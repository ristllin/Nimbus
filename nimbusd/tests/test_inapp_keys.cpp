// test_inapp_keys - CUM-279: in-app provider keys, device parity.
//
// The owner ruling: "you set the keys inside the nimbus ui... not externally." So a
// key set through the running web app MUST take effect on THIS instance, exactly as
// a key set in a device's own UI does. These are class tests over the rig seam and
// the /api/orch web surface (not one instance):
//   (a) applyProviderKey flips hostAvailable, bumps the change counter, and the very
//       next turn dispatches to that provider - it took effect with no restart.
//   (b) the key is DURABLE: a fresh rig on the same data dir (a "pod restart") still
//       has it (loaded from the owner-only secrets file, authoritative over env).
//   (c) clearing removes it (hostAvailable false again).
//   (d) the same fields the device's Providers & keys form posts, driven through the
//       real WebApi POST /api/orch, apply the key (openai and the flagship cumulo).
//
// Offline: the provider wire is an injected FakeHttpTransport; no network, no real
// keys. Secrets never printed (assertions read booleans, never the key bytes).
#include <sys/stat.h>

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

// (a) A key set in-app takes effect immediately - no restart.
static void testApplyTakesEffect(ndtest::Ctx& c) {
  std::printf("  -- (a) applyProviderKey: takes effect, next turn dispatches --\n");
  clearProviderEnv();
  FakeHttpTransport tx;
  Exchange e; e.status = 200;
  e.body = "{\"choices\":[{\"message\":{\"content\":\"{}\"}}],"
           "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig::Options opt = baseOpt("inapp-a");
  NimbusdRig rig(cfg, opt, &tx);
  c.ok(!rig.hostAvailable("mistral"), "starts with no Mistral key");
  const uint32_t gen0 = rig.keyGen();

  c.ok(rig.applyProviderKey("mistral", "mistral_sk_test_INAPP"),
       "applyProviderKey(mistral, <key>) succeeds");
  c.ok(rig.hostAvailable("mistral"), "Mistral key is now available (took effect)");
  c.ok(rig.keyGen() == gen0 + 1, "the key-change counter bumped");

  // The plaintext-key file is owner-only (0600), with no world-readable window.
  struct stat stt{};
  const std::string secrets = opt.dataDir + "/mem/secrets.env";
  c.ok(::stat(secrets.c_str(), &stt) == 0, "the secrets file was written");
  c.eqi(stt.st_mode & 0777, 0600, "the secrets file is 0600 (owner-only)");

  rig.say("owner", "hi");
  c.ok(!tx.seen.empty(), "the next turn dispatched a request");
  if (!tx.seen.empty())
    c.eq(tx.seen[0].host, std::string("api.mistral.ai"),
         "the turn dispatched to the newly-keyed provider");

  c.ok(!rig.applyProviderKey("nope", "x"), "an unknown provider slug is rejected");
  clearProviderEnv();
}

// (b) The key persists across a process restart (durable secrets file).
static void testKeyIsDurable(ndtest::Ctx& c) {
  std::printf("  -- (b) durable across a restart (same data dir) --\n");
  clearProviderEnv();
  NimbusdRig::Options opt = baseOpt("inapp-b");
  {
    Config cfg;
    NimbusdRig rig(cfg, opt);
    c.ok(rig.applyProviderKey("openai", "sk_test_DURABLE"), "set an OpenAI key in-app");
    c.ok(rig.hostAvailable("openai"), "key present in the first process");
  }
  {
    Config cfg;                       // a brand-new rig == a pod restart
    NimbusdRig rig(cfg, opt);
    c.ok(rig.hostAvailable("openai"),
         "the in-app key survived the restart (loaded from secrets)");
  }
  clearProviderEnv();
}

// (c) Clearing a key removes it.
static void testClearRemovesKey(ndtest::Ctx& c) {
  std::printf("  -- (c) clearing a key removes it --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("inapp-c"));
  rig.applyProviderKey("anthropic", "sk-ant-CLEARME");
  c.ok(rig.hostAvailable("anthropic"), "key set");
  rig.applyProviderKey("anthropic", "");   // empty key clears
  c.ok(!rig.hostAvailable("anthropic"), "empty key cleared the override");
  clearProviderEnv();
}

// (d) The web POST /api/orch seam the device form uses applies the key.
static void testWebPostAppliesKey(ndtest::Ctx& c) {
  std::printf("  -- (d) POST /api/orch applies a key (device-parity form) --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("inapp-d"));
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  ApiResp out;
  c.ok(api.handle("POST", "/api/orch", "openaiKey=sk_test_WEBFORM", out),
       "POST /api/orch is handled");
  c.ok(out.body.find("\"applied\":1") != std::string::npos,
       "the response reports one key applied");
  c.ok(rig.hostAvailable("openai"), "the OpenAI key applied through the web seam");

  // The flagship one-key path: the Cumulo router key set the same way.
  ApiResp out2;
  api.handle("POST", "/api/orch", "cumuloKey=cumulo_sk_WEBFORM", out2);
  c.ok(rig.hasCumulo(), "the Cumulo router key applied through the web seam");

  // A non-key orch save is still acked (does not fail).
  ApiResp out3;
  c.ok(api.handle("POST", "/api/orch", "orchLoop=1", out3) &&
           out3.body.find("\"ok\":true") != std::string::npos,
       "a non-key orch setting is acked honestly");

  eng.stop();
  clearProviderEnv();
}

int main() {
  ndtest::Ctx c;
  c.suite = "in-app provider keys (CUM-279)";
  std::printf("=== %s ===\n", c.suite);
  testApplyTakesEffect(c);
  testKeyIsDurable(c);
  testClearRemovesKey(c);
  testWebPostAppliesKey(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
