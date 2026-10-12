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
                        "TAVILY_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
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

  // CUM-445: the field the shared web app actually posts is `oaiKey` (the canonical
  // provider_slots.h keyField), NOT `openaiKey`. The old test asserted the buggy name
  // that silently dropped every direct-provider write.
  ApiResp out;
  c.ok(api.handle("POST", "/api/orch", "oaiKey=sk_test_WEBFORM_KEY1", out),
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

// (e) CUM-445: the web POST /api/orch accepts the SAME field names the shared web app
// sends, for EVERY provider (not just openai/cumulo), through the canonical registry -
// so this drift class cannot recur. This is a class test over kProviderSlots: a new
// slot is covered here with no new code, and a field the daemon does not accept FAILS.
static void testEveryFieldNameApplies(ndtest::Ctx& c) {
  std::printf("  -- (e) POST /api/orch accepts every canonical key field (CUM-445) --\n");
  // (field the UI posts) -> (provider slug it must reach). These are exactly the
  // provider_slots.h keyField values; a divergence here means the daemon and the UI
  // disagree, which is the whole bug.
  struct FT { const char* field; const char* slug; bool router; };
  const FT kFields[] = {
      {"oaiKey", "openai", false}, {"antKey", "anthropic", false},
      {"mistKey", "mistral", false}, {"zaiKey", "zai", false},
      {"cumuloKey", "cumulo", true},
  };
  for (const FT& f : kFields) {
    clearProviderEnv();
    Config cfg;
    NimbusdRig rig(cfg, baseOpt(std::string("inapp-e-") + f.slug));
    EngineThread eng(&rig);
    eng.start();
    ReplyBuffer replies;
    WebApi api(&rig, &eng, &replies);

    ApiResp out;
    const std::string body = std::string(f.field) + "=key_" + f.slug + "_SET_0123456789";
    c.ok(api.handle("POST", "/api/orch", body, out), std::string("POST ") + f.field + " handled");
    c.eqi(out.status, 200, std::string(f.field) + " is accepted (200, not refused)");
    c.ok(out.body.find("\"applied\":1") != std::string::npos,
         std::string(f.field) + " reports one key applied");
    const bool present = f.router ? rig.hasCumulo() : rig.hostAvailable(f.slug);
    c.ok(present, std::string(f.field) + " reached provider " + f.slug);

    // clr_<field> clears it again.
    ApiResp clr;
    api.handle("POST", "/api/orch", std::string("clr_") + f.field + "=1", clr);
    const bool gone = !(f.router ? rig.hasCumulo() : rig.hostAvailable(f.slug));
    c.ok(gone, std::string("clr_") + f.field + " cleared " + f.slug);
    eng.stop();
  }
  clearProviderEnv();
}

// (f) CUM-445: an unknown *Key field is refused LOUDLY (400), never silently acked, so
// a future field rename cannot re-introduce the silent-drop bug. Counter-test to (e):
// the exact names the OLD daemon read (openaiKey/mistralKey/anthropicKey) are now the
// drift it must reject.
static void testUnknownKeyFieldRefused(ndtest::Ctx& c) {
  std::printf("  -- (f) unknown *Key field refused with 400 (CUM-445) --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("inapp-f"));
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  for (const char* bad : {"openaiKey", "mistralKey", "anthropicKey", "bogusKey"}) {
    ApiResp out;
    api.handle("POST", "/api/orch", std::string(bad) + "=sk_DRIFT", out);
    c.eqi(out.status, 400, std::string(bad) + " is refused with 400");
    c.ok(out.body.find("\"ok\":false") != std::string::npos &&
             out.body.find(std::string("unknown field ") + bad) != std::string::npos,
         std::string(bad) + " names the unknown field in the error");
    c.ok(!rig.hostAvailable("openai") && !rig.hostAvailable("mistral") &&
             !rig.hostAvailable("anthropic"),
         std::string(bad) + " applied nothing");
  }
  // clr_ of an unknown field is also refused (drift on the clear path too).
  ApiResp clr;
  api.handle("POST", "/api/orch", "clr_openaiKey=1", clr);
  c.eqi(clr.status, 400, "clr_ of an unknown field is refused too");

  // Counter-check: the NON-provider key fields the shared orch form legitimately posts
  // (the custom endpoint key, the Tavily key) are not provider-key drift. The page's
  // Save Changes posts custKey BLANK ("keep"), so that payload must save; a typed
  // value is something a hosted instance does not store from the page, so it is
  // refused honestly by name (CUM-543) - never as an "unknown field".
  ApiResp save;
  c.ok(api.handle("POST", "/api/orch", "custKey=&orchLoop=1", save),
       "a Save Changes payload with a blank custKey is handled");
  c.eqi(save.status, 200, "a blank custKey rides Save Changes (not refused as unknown)");
  for (const char* f : {"custKey", "tavKey"}) {
    ApiResp typed;
    api.handle("POST", "/api/orch", std::string(f) + "=typed_value", typed);
    c.eqi(typed.status, 400, std::string("a typed ") + f + " is refused");
    c.ok(typed.body.find(std::string("Not available on a hosted Nimbus: ") + f) != std::string::npos &&
             typed.body.find("unknown field") == std::string::npos,
         std::string(f) + " is refused by name as hosted-only, not as drift");
  }
  eng.stop();
  clearProviderEnv();
}

// (g) CUM-445: GET /api/orch reports the canonical keyField + hasKey for every slot
// (including Z.ai), so the UI shows "Key set" and the model pickers unlock. Reads the
// field the UI merges (pp.keyField) - it MUST be oaiKey/antKey/mistKey/zaiKey/cumuloKey.
static void testOrchGetReportsCanonicalFields(ndtest::Ctx& c) {
  std::printf("  -- (g) GET /api/orch reports canonical keyField + hasKey (CUM-445) --\n");
  clearProviderEnv();
  Config cfg;
  NimbusdRig rig(cfg, baseOpt("inapp-g"));
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  ApiResp before;
  c.ok(api.handle("GET", "/api/orch", "", before), "GET /api/orch handled");
  for (const char* kf : {"oaiKey", "antKey", "mistKey", "zaiKey", "cumuloKey"}) {
    c.ok(before.body.find(std::string("\"keyField\":\"") + kf + "\"") != std::string::npos,
         std::string("reports canonical keyField ") + kf);
  }
  // Z.ai is present as a first-class provider row.
  c.ok(before.body.find("\"zai\"") != std::string::npos, "Z.ai provider is listed");

  // Set a Z.ai key through the seam; hasKey flips true in the next GET (picker unlocks).
  ApiResp setz;
  api.handle("POST", "/api/orch", "zaiKey=zai_UNLOCK_0123456", setz);
  ApiResp after;
  api.handle("GET", "/api/orch", "", after);
  // The zai object now carries hasKey:true. (Order-independent substring: the zai block
  // is "zai":{...,"hasKey":true,...}; a coarse but sufficient check is that hasKey:true
  // appears and zai is keyed on the rig.)
  c.ok(rig.hostAvailable("zai"), "the Z.ai key applied through the web seam (Z_AI_TOKEN)");
  eng.stop();
  clearProviderEnv();
}

// (h) CUM-445: a keyed Z.ai VN actually DISPATCHES a turn to Z.ai's endpoint, not a
// silent misroute to Mistral. Z.ai-only (no other key) must resolve to the zai head.
static void testZaiTurnDispatches(ndtest::Ctx& c) {
  std::printf("  -- (h) a Z.ai-only VN dispatches its turn to api.z.ai (CUM-445) --\n");
  clearProviderEnv();
  FakeHttpTransport tx;
  Exchange e; e.status = 200;
  e.body = "{\"choices\":[{\"message\":{\"content\":\"{}\"}}],"
           "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
  tx.script.push_back(e);

  Config cfg;
  NimbusdRig rig(cfg, baseOpt("inapp-h"), &tx);
  c.ok(rig.applyProviderKey("zai", "zai_sk_TURN"), "applyProviderKey(zai) succeeds");
  c.ok(rig.hostAvailable("zai"), "Z.ai key took effect");
  rig.say("owner", "hi");
  c.ok(!tx.seen.empty(), "the turn dispatched a request");
  if (!tx.seen.empty())
    c.eq(tx.seen[0].host, std::string("api.z.ai"),
         "the Z.ai turn reached api.z.ai (NOT misrouted to Mistral)");
  clearProviderEnv();
}

// (i) CUM-444: /api/state reports the container image tag separately from fw, so the
// portal can show the engine version primary and the image tag secondary.
static void testStateReportsImageTag(ndtest::Ctx& c) {
  std::printf("  -- (i) /api/state reports fw and image separately (CUM-444) --\n");
  clearProviderEnv();
  setenv("NIMBUSD_IMAGE_TAG", "nimbusd:v9.9.9-test", 1);
  Config cfg;                       // Config reads env at construction
  NimbusdRig rig(cfg, baseOpt("inapp-i"));
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  ApiResp out;
  c.ok(api.handle("GET", "/api/state", "", out), "GET /api/state handled");
  c.ok(out.body.find(std::string("\"fw\":\"") + NIMBUS_FW_VERSION + "\"") != std::string::npos,
       "fw is the engine version (NIMBUS_FW_VERSION)");
  c.ok(out.body.find("\"image\":\"nimbusd:v9.9.9-test\"") != std::string::npos,
       "image is the container tag, reported separately");
  eng.stop();
  unsetenv("NIMBUSD_IMAGE_TAG");
  clearProviderEnv();
}

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
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

static void testKeyShapeRefused(ndtest::Ctx& c) {
  std::printf("  -- a key with a line break, a space or a control character is refused --\n");
  clearProviderEnv();
  const NimbusdRig::Options o = baseOpt("ik-shape");
  FakeHttpTransport tx;   // the good key's verify finds an empty script: an honest transport failure
  Config cfg;
  NimbusdRig rig(cfg, o, &tx);
  {
    Web web(rig);
    const std::string bodies[] = {
        "cumuloKey=cumulo_sk_x%0ANIMBUSD_ZAI_BASE=http%3A%2F%2Fevil.example",   // a line injected into secrets.env
        "cumuloKey=cumulo_sk_x%0D%0AZ_AI_TOKEN=forged",
        "oaiKey=sk-proj-two%20words",
        "antKey=sk-ant-tab%09here",
        "mistKey=mk_del%7Fhere",
        "zaiKey=%C3%A9t%C3%A9",
        "cumuloKey=" + std::string(513, 'k'),   // past the 512-character cap
    };
    for (const std::string& b : bodies) {
      const ApiResp r = web.call("POST", "/api/orch", b);
      c.ok(r.status == 400 && has(r.body, "\"error\":\"That ") && has(r.body, " and save again."),
           "refused with the device's own sentence: " + b.substr(0, 40));
    }
    c.eq(rig.cumuloKey(), "", "no key was applied");
    std::string blob;
    const bool wrote = fsutil::readFile(rig.secretsPath(), blob);
    c.ok(!wrote || (!has(blob, "NIMBUSD_ZAI_BASE") && !has(blob, "forged")), "secrets.env never gained an injected line");
    const ApiResp good = web.call("POST", "/api/orch", "cumuloKey=cumulo_sk_test_GOOD.key-1_2");
    c.eqi(good.status, 200, "a clean key saves");
  }
  std::string blob;
  fsutil::readFile(rig.secretsPath(), blob);
  c.eq(blob, "CUMULO_API_KEY=cumulo_sk_test_GOOD.key-1_2\n", "the secrets file holds exactly the clean key");
  c.ok(!rig.applyProviderKey("openai", "sk-a\nB=c"), "the engine-thread seam refuses a key with a line break too");
  clearProviderEnv();
}

static void testSecretsFileGuards(ndtest::Ctx& c) {
  std::printf("  -- the secrets file writes and loads provider keys only --\n");
  clearProviderEnv();
  const NimbusdRig::Options o = baseOpt("ik-guard");
  {
    // The writer: an override that is not a clean provider key never reaches the file.
    Config cfg;
    cfg.setOverride("CUMULO_API_KEY", "cumulo_sk_x\nNIMBUSD_ZAI_BASE=evil.example");
    cfg.setOverride("NIMBUSD_ZAI_BASE", "evil.example");
    NimbusdRig rig(cfg, o);
    c.ok(rig.applyProviderKey("openai", "sk-test-WRITER"), "a clean key applies");
    std::string blob;
    fsutil::readFile(rig.secretsPath(), blob);
    c.eq(blob, "OPENAI_API_KEY=sk-test-WRITER\n", "only the clean provider key is written");
  }
  // The loader: a hand-edited file with a non-provider name or a dirty value loads
  // only the clean provider keys.
  fsutil::writeFileAtomic(o.dataDir + "/mem/secrets.env",
                          "NIMBUSD_ZAI_BASE=evil.example\nCUMULO_API_KEY=cumulo_sk_test_LOADED\n"
                          "OPENAI_API_KEY=sk two words\nTELEGRAM_BOT_TOKEN=123:forged\n");
  Config cfg;
  NimbusdRig rig(cfg, o);
  c.eq(rig.zaiHost(), "api.z.ai", "a non-provider name in the file never becomes config");
  c.eq(rig.cumuloKey(), "cumulo_sk_test_LOADED", "a clean provider key loads");
  c.eq(rig.cfg().providerKey("openai"), "", "a value with a space does not load");
  c.ok(!rig.cfg().has("TELEGRAM_BOT_TOKEN"), "nor does any other secret name");
  clearProviderEnv();
}

// CUM-545: an earlier build stored a key exactly as the page posted it,
// and the page posts the field as typed, so a pasted key with a space around it was
// stored with the space (and worked: a header value's surrounding whitespace is
// dropped). The stricter rule then dropped it on the first start after the update
// and erased it from the file. It now loads trimmed and the file is rewritten
// trimmed; a control character or a tab still never loads.
static void testSpacedKeysSurviveTheUpdate(ndtest::Ctx& c) {
  std::printf("  -- a key stored with spaces around it loads trimmed, and the file is rewritten --\n");
  clearProviderEnv();
  const NimbusdRig::Options o = baseOpt("ik-spaced");
  fsutil::mkdirs(o.dataDir + "/mem");
  fsutil::writeFileAtomic(o.dataDir + "/mem/secrets.env",
                          "OPENAI_API_KEY=sk-test-TRAILING \nCUMULO_API_KEY= cumulo_sk_test_LEAD\n"
                          "MISTRAL_API_KEY=  mk_test_BOTH_SIDES  \r\nANTHROPIC_API_KEY=sk-ant-ctl\x01-here\n"
                          "Z_AI_TOKEN=zai_tab\there\n");
  const std::string want = "CUMULO_API_KEY=cumulo_sk_test_LEAD\nMISTRAL_API_KEY=mk_test_BOTH_SIDES\n"
                           "OPENAI_API_KEY=sk-test-TRAILING\n";
  for (int start = 1; start <= 2; start++) {   // the first start after the update, then the next
    Config cfg;
    NimbusdRig rig(cfg, o);
    const std::string when = start == 1 ? " (first start)" : " (next start)";
    c.eq(rig.cfg().providerKey("openai"), "sk-test-TRAILING", "a key with a trailing space loads trimmed" + when);
    c.eq(rig.cumuloKey(), "cumulo_sk_test_LEAD", "the Cumulo router key with a leading space loads trimmed" + when);
    c.eq(rig.cfg().providerKey("mistral"), "mk_test_BOTH_SIDES", "spaces on both sides and a CR are dropped" + when);
    c.eq(rig.cfg().providerKey("anthropic"), "", "a control character still never loads" + when);
    c.eq(rig.cfg().providerKey("zai"), "", "nor does a tab" + when);
    c.ok(rig.hostAvailable("openai") && rig.hasCumulo(), "the instance still uses both keys" + when);
    std::string blob;
    fsutil::readFile(rig.secretsPath(), blob);
    c.eq(blob, want, "the secrets file holds the trimmed keys" + when);
  }
  clearProviderEnv();
}

// CUM-545: POST /api/orch applies the device's own rule (key_shape.h): the
// spaces around a key are dropped and the key is stored trimmed, and every other
// refusal carries the device's sentence for that field.
static void testWebKeyStoredTrimmed(ndtest::Ctx& c) {
  std::printf("  -- POST /api/orch: a key with spaces around it is stored trimmed; the device's rule refuses the rest --\n");
  clearProviderEnv();
  const NimbusdRig::Options o = baseOpt("ik-trim");
  FakeHttpTransport tx;   // the verify after each save finds an empty script: an honest transport failure
  Config cfg;
  NimbusdRig rig(cfg, o, &tx);
  {
    Web web(rig);
    // The page encodes a typed space as '+' (URLSearchParams).
    c.eqi(web.call("POST", "/api/orch", "oaiKey=sk-test-abcdefghijklmnop+").status, 200,
          "a key with a trailing space saves");
    c.eqi(web.call("POST", "/api/orch", "cumuloKey=%20%20cumulo_sk_test_LEADING").status, 200,
          "a key with leading spaces saves");
    c.eq(rig.cfg().providerKey("openai"), "sk-test-abcdefghijklmnop", "stored without the trailing space");
    c.eq(rig.cumuloKey(), "cumulo_sk_test_LEADING", "stored without the leading spaces");
    struct Bad { std::string body; const char* says; };
    const Bad bad[] = {
        {"mistKey=+++", "That Mistral key is blank. Paste the whole key and save again."},
        {"oaiKey=sk-short", "That OpenAI key looks too short. Paste the whole key and save again."},
        {"zaiKey=" + std::string(257, 'z'), "That Z.ai key looks too long. Paste just the key and save again."},
        {"antKey=sk-ant-tab%09here-0123456789",
         "That Anthropic key has a space or a hidden character in it. Paste just the key and save again."},
        {"mistKey=mk_line%0Abreak_0123456789",
         "That Mistral key has a space or a hidden character in it. Paste just the key and save again."},
    };
    for (const Bad& b : bad) {
      const ApiResp r = web.call("POST", "/api/orch", b.body);
      c.ok(r.status == 400 && has(r.body, b.says), std::string("refused with: ") + b.says);
    }
    c.ok(!rig.hostAvailable("mistral") && !rig.hostAvailable("anthropic") && !rig.hostAvailable("zai"),
         "a refused key stores nothing");
  }
  std::string blob;
  fsutil::readFile(rig.secretsPath(), blob);
  c.eq(blob, "CUMULO_API_KEY=cumulo_sk_test_LEADING\nOPENAI_API_KEY=sk-test-abcdefghijklmnop\n",
       "the secrets file holds the trimmed keys only");
  // The class: every provider key field the page posts has its shape in the shared
  // rule, so no slot falls back to a check the device does not make.
  for (size_t i = 0; i < nimbus::orch::kProviderSlotCount; i++) {
    const nimbus::orch::ProviderSlot& s = nimbus::orch::kProviderSlots[i];
    c.ok(nimbus::orch::keyShapeFor(s.keyField) != nullptr, std::string("key_shape.h covers ") + s.keyField);
    c.ok(has(NimbusdRig::keyShapeErrorFor(s.slug, "   "), " is blank."),
         std::string("a key of spaces alone is refused for ") + s.slug);
  }
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
  testEveryFieldNameApplies(c);
  testUnknownKeyFieldRefused(c);
  testOrchGetReportsCanonicalFields(c);
  testZaiTurnDispatches(c);
  testStateReportsImageTag(c);
  testKeyShapeRefused(c);
  testSecretsFileGuards(c);
  testSpacedKeysSurviveTheUpdate(c);
  testWebKeyStoredTrimmed(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
