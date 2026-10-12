// test_usage_diag - item 4 + CUM-546: usage survives a save, and a hosted turn is
// diagnosable.
//
//   (a) a model save and a key save rebuild the engine; GET /api/orch usage
//       (sessIn/sessOut/turns) and GET /api/state turnCount keep counting across
//       them instead of dropping to 0, and the next turn adds to them;
//   (b) GET /api/lastturn serves the last turn's anatomy (404 before any turn),
//       GET /api/log the engine log, GET /api/errlog/recent its failure lines;
//   (c) the engine log is a bounded ring (it grew for the life of the daemon).
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "nimbus/harness/log.h"
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

static Exchange respTurn(const std::string& path, const std::string& reply) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = path;
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"{\\\"reply\\\":\\\"" + reply +
           "\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":100,\"output_tokens\":7}}";
  return e;
}
static Exchange misTurn(const std::string& reply) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/mistral/v1/conversations";
  e.body = "{\"conversation_id\":\"c\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"{\\\"reply\\\":\\\"" +
           reply + "\\\",\\\"memory\\\":\\\"\\\"}\"}],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":7}}";
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
  void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static void testCountersSurviveSaves(ndtest::Ctx& c) {
  std::printf("  -- (a) usage and turnCount survive a model save and a key save --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "one"));
  tx.script.push_back(respTurn("/router/openai/v1/responses", "two"));
  Exchange verify;   // the key save re-verifies the new key (CUM-545)
  verify.expectHost = kCumuloHost;
  verify.expectPathContains = "/router/models";
  verify.body = "{\"object\":\"list\",\"data\":[]}";
  tx.script.push_back(verify);
  tx.script.push_back(misTurn("three"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-a"), &tx);
  rig.say("owner", "first");
  rig.say("owner", "second");
  {
    Web web(rig);
    const std::string before = web.call("GET", "/api/orch").body;
    c.ok(has(before, "\"sessIn\":200") && has(before, "\"sessOut\":14") && has(before, "\"turns\":2"),
         "two turns counted (200 in / 14 out / 2 turns)");
    c.eqi(web.call("POST", "/api/orch", "orchM_cumulo=mistral/mistral-large-latest").status, 200, "a model save");
    web.settle();
    const std::string afterModel = web.call("GET", "/api/orch").body;
    c.ok(has(afterModel, "\"sessIn\":200") && has(afterModel, "\"turns\":2"), "a model save keeps the counters");
    c.eqi(web.call("POST", "/api/orch", "cumuloKey=cumulo_sk_test_USAGE2").status, 200, "a key save");
    web.settle();
    const std::string afterKey = web.call("GET", "/api/orch").body;
    c.ok(has(afterKey, "\"sessIn\":200") && has(afterKey, "\"turns\":2"), "a key save keeps the counters");
    c.ok(has(web.call("GET", "/api/state").body, "\"turnCount\":2"), "/api/state turnCount keeps counting");
  }
  rig.say("owner", "third");
  Web web(rig);
  const std::string after = web.call("GET", "/api/orch").body;
  c.ok(has(after, "\"sessIn\":300") && has(after, "\"turns\":3"), "the next turn adds to the carried totals");
  c.ok(has(after, "\"lastIn\":100") && has(after, "\"lastOut\":7"), "the last turn's usage is reported");
  clearEnv();
}

static void testDiagnostics(ndtest::Ctx& c) {
  std::printf("  -- (b) /api/lastturn, /api/log and /api/errlog/recent --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "fine"));
  for (int i = 0; i < 2; i++) {   // a failing turn (and the engine's one retry)
    Exchange bad;
    bad.expectHost = kCumuloHost;
    bad.status = 500;
    bad.body = "{}";
    tx.script.push_back(bad);
  }
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-b"), &tx);
  {
    Web web(rig);
    c.eqi(web.call("GET", "/api/lastturn").status, 404, "no anatomy before the first turn");
  }
  rig.say("owner", "PELICAN diagnostics please");
  rig.say("owner", "this one fails");
  Web web(rig);
  const ApiResp lt = web.call("GET", "/api/lastturn");
  c.eqi(lt.status, 200, "GET /api/lastturn answers 200 (it was a 404)");
  c.ok(has(lt.body, "NIMBUS TURN ANATOMY") && has(lt.body, "host: cumulo"), "the anatomy names the head");
  c.ok(has(lt.body, "this one fails") && has(lt.body, "result: FAILED"), "it is the most recent turn, with its result");
  c.ok(has(lt.body, "hosted cloud instance"), "the instructions shown are the ones the head got (hosted text)");
  c.ok(has(web.call("GET", "/api/log").body, "orchestrator:"), "GET /api/log serves the engine log");
  const ApiResp el = web.call("GET", "/api/errlog/recent?bytes=4096");
  c.ok(el.status == 200 && has(el.body, "turn failed"), "GET /api/errlog/recent carries the failure");
  c.ok(!has(el.body, "turn host="), "and only failure lines");
  clearEnv();
}

static void testLogIsBounded(ndtest::Ctx& c) {
  std::printf("  -- (c) the engine log is a bounded ring --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-c"));
  for (int i = 0; i < 2500; i++) agent::hlog::logf("filler line %d", i);
  const auto lines = NimbusdRig::harnessLog();
  c.ok(lines.size() <= 1000, "at most 1000 lines are kept");
  c.ok(!lines.empty() && has(lines.back(), "filler line 2499"), "the newest line is kept");
  agent::hlog::logf("resp HTTP 401: Incorrect API key provided: sk-proj-ABCD****WXYZ, cumulo_sk_live1234abcd");
  const std::string tail = NimbusdRig::logTail(4096, true);
  c.ok(has(tail, "[key]") && !has(tail, "ABCD") && !has(tail, "live1234"), "key-shaped text is masked in the log");
}

static void testLastTurnIsMasked(ndtest::Ctx& c) {
  std::printf("  -- (d) /api/lastturn masks keys like the logs do --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  setenv("TAVILY_API_KEY", "tvly-test-HELDSECRET0123", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "noted"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-d"), &tx);
  rig.updateSettings([](HostedSettings& s) { s.directive = "Always use the key sk-proj-ABCDEFGH1234567890 for scripts."; });
  rig.say("owner", "my search key is tvly-test-HELDSECRET0123 and cumulo_sk_live9876abcd is the router one");
  Web web(rig);
  const ApiResp lt = web.call("GET", "/api/lastturn");
  c.eqi(lt.status, 200, "the anatomy is served");
  c.ok(has(lt.body, "[key]"), "key-shaped text is masked");
  c.ok(!has(lt.body, "ABCDEFGH1234567890"), "a key quoted in the directive (the instructions) is masked");
  c.ok(!has(lt.body, "live9876abcd"), "a router key typed in chat (the input) is masked");
  c.ok(!has(lt.body, "HELDSECRET0123"), "a key this instance holds is masked whatever its shape");
  c.ok(has(lt.body, "my search key is"), "the rest of the turn is kept");
  unsetenv("TAVILY_API_KEY");
  clearEnv();
}

static void testHeldKeyMaskedWhole(ndtest::Ctx& c) {
  std::printf("  -- (e) a held key with a key-shaped piece inside is masked whole --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  // The prefix mask alone would rewrite only "sk_..." and leave "webtok7Q" in clear;
  // the exact-value mask must see the value before that happens.
  setenv("NIMBUSD_WEB_TOKEN", "webtok7Qsk_HELDTOKEN99", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "ok"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-e"), &tx);
  rig.say("owner", "the web token is webtok7Qsk_HELDTOKEN99 for reference");
  const std::string lt = rig.lastTurnAnatomy();
  c.ok(has(lt, "the web token is [key] for reference"), "the whole held value reads [key]");
  c.ok(!has(lt, "webtok7Q") && !has(lt, "HELDTOKEN99"), "no piece of it is left in clear");
  unsetenv("NIMBUSD_WEB_TOKEN");
  clearEnv();
}

// CUM-546 gate (High): the held-key mask matched a held key's first kHeldHead
// characters and masked only as far as they matched. Every Anthropic key starts
// with "sk-ant-api03-", so a DIFFERENT Anthropic key typed in chat lost only that
// prefix and its body stayed in clear (and the prefix mask no longer saw "sk-").
// A partial match now masks the whole token it sits in.
static void testSameProviderKeyMaskedWhole(ndtest::Ctx& c) {
  std::printf("  -- (e2) a second key with a held key's prefix is masked whole --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "ok"));
  Config cfg;
  cfg.setOverride("ANTHROPIC_API_KEY", "sk-ant-api03-AAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  NimbusdRig::Options o = opts("ud-e2");
  o.orchHost = "cumulo";   // the turn stays on the scripted router head
  NimbusdRig rig(cfg, o, &tx);
  rig.say("owner", "my new key is sk-ant-api03-BBBBBBBBBBBBBBBBBBBBBBBBBBBB, and the old one ends there");
  const std::string lt = rig.lastTurnAnatomy();
  c.ok(!has(lt, "BBBB"), "no piece of the second key is left in clear");
  c.ok(has(lt, "my new key is [key], and the old one ends there"), "the second key reads [key] and the text around it is kept");
  // The same rule for a held key with no prefix: a different value that only shares
  // its first characters is masked whole too.
  FakeHttpTransport tx2;
  tx2.script.push_back(respTurn("/router/openai/v1/responses", "ok"));
  Config cfg2;
  cfg2.setOverride("NIMBUSD_WEB_TOKEN", "q7KxSHAREDHEAD00AAAAAAAAAAAAAAAA");
  NimbusdRig rig2(cfg2, opts("ud-e3"), &tx2);
  rig2.say("owner", "try q7KxSHAREDHEAD00ZZZZZZZZZZZZZZZZ/next instead");
  const std::string lt2 = rig2.lastTurnAnatomy();
  c.ok(!has(lt2, "ZZZZ") && has(lt2, "try [key] instead"), "a shapeless value sharing a held key's head is masked whole");
  clearEnv();
}

// CUM-546 gate: the anatomy was masked once, when the turn was captured, with the
// keys held then. A key the owner typed in chat before saving it stayed in clear on
// /api/lastturn until the next turn; it is now masked again as the page is served.
// A Tavily key, a long token named a key and a KEY=value are masked even when the
// instance holds none of them.
static void testLastTurnMaskedOnRead(ndtest::Ctx& c) {
  std::printf("  -- (f) /api/lastturn masks a key saved after the turn, and the formats the page takes --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "noted"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-f"), &tx);
  const std::string mistral = "q7KxPLAINMISTRALKEY000000000001";   // no prefix: not recognizable unheld
  rig.say("owner", "for mistral use " + mistral + " from now on, and search with tvly-UNHELD0123456789abcd");
  c.ok(has(rig.lastTurnAnatomy(), mistral), "precondition: a shapeless value is not recognizable before it is held");
  c.ok(!has(rig.lastTurnAnatomy(), "UNHELD0123456789abcd"), "a Tavily key the instance does not hold is masked");
  c.ok(rig.applyProviderKey("mistral", mistral), "the owner saves that key after the turn");
  {
    Web web(rig);
    const ApiResp lt = web.call("GET", "/api/lastturn");
    c.eqi(lt.status, 200, "the anatomy is served");
    c.ok(!has(lt.body, mistral) && !has(lt.body, "PLAINMISTRAL"), "a key saved after the turn is masked as it is served");
    c.ok(has(lt.body, "for mistral use [key] from now on"), "the rest of the turn is kept");
  }
  // A fresh instance (the saved Mistral key would now take the head).
  FakeHttpTransport tx2;
  tx2.script.push_back(respTurn("/router/openai/v1/responses", "noted again"));
  Config cfg2;
  NimbusdRig rig2(cfg2, opts("ud-f2"), &tx2);
  rig2.say("owner", "my other mistral key is q7KxSECONDPLAIN" "KEY00000000000002, and ZAI_TOKEN=zaiPLAINTOKEN1234567890abcdef");
  // A held key cut short (tool-loop and job lines are clipped with an ellipsis
  // before the anatomy is built) is masked as far as it goes.
  {
    FakeHttpTransport tx3;
    tx3.script.push_back(respTurn("/router/openai/v1/responses", "ok"));
    Config cfg3;
    cfg3.setOverride("NIMBUSD_WEB_TOKEN", "q7KxCUTHELDVALUE0000000000000009");   // held, no prefix
    NimbusdRig rig3(cfg3, opts("ud-f3"), &tx3);
    rig3.say("owner", "the brief showed q7KxCUTHELDVALUE00000000000\xE2\x80\xA6 and stopped");
    const std::string lt3 = rig3.lastTurnAnatomy();
    c.ok(!has(lt3, "q7KxCUTHELD") && has(lt3, "the brief showed [key]\xE2\x80\xA6 and stopped"),
         "a held key cut short is masked as far as it goes");
  }
  const std::string lt2 = rig2.lastTurnAnatomy();
  c.ok(!has(lt2, "SECONDPLAINKEY"), "a long token named a key is masked though nothing holds it");
  c.ok(!has(lt2, "zaiPLAINTOKEN"), "the value of a NAME=value named a token is masked");
  c.ok(has(lt2, "my other mistral key is [key], and ZAI_TOKEN=[key]"), "only the values are masked");
  clearEnv();
}

// The bounded rule on its own: what it masks, and the ordinary text it leaves alone.
static void testNamedTokenRule(ndtest::Ctx& c) {
  std::printf("  -- (g) the named-token rule masks key-like tokens only --\n");
  struct Case { const char* in; const char* out; };
  const Case kCases[] = {
      {"the key: q7KxPLAINMISTRALKEY000000000001.", "the key: [key]."},
      {"MISTRAL_API_KEY=q7KxPLAINMISTRALKEY000000000001", "MISTRAL_API_KEY=[key]"},
      {"{\"api_key\":\"q7KxPLAINMISTRALKEY000000000001\"}", "{\"api_key\":\"[key]\"}"},
      {"bot token is 123456789:AAHfakeTokenValue0123456789", "bot token is [key]"},
      {"search key tvly-dev-ABCDEF0123456789", "search key [key]"},
      {"the key insight is internationalization", "the key insight is internationalization"},
      {"keys are 1, 2 and 3; build abc123def456ghi789jkl0 passed", "keys are 1, 2 and 3; build abc123def456ghi789jkl0 passed"},
      {"a monkey ate q7KxPLAINMISTRALKEY000000000001", "a monkey ate q7KxPLAINMISTRALKEY000000000001"},
      {"MONKEY q7KxPLAINMISTRALKEY000000000001", "MONKEY q7KxPLAINMISTRALKEY000000000001"},
      {"apiKey: q7KxPLAINMISTRALKEY000000000001", "apiKey: [key]"},
      {"{\"accessToken\":\"q7KxPLAINMISTRALKEY000000000001\"}", "{\"accessToken\":\"[key]\"}"},
      {"{\\\"api_key\\\":\\\"q7KxPLAINMISTRALKEY000000000001\\\"}", "{\\\"api_key\\\":\\\"[key]\\\"}"},
      {"the key is \xE2\x80\x9Cq7KxPLAINMISTRALKEY000000000001\xE2\x80\x9D", "the key is \xE2\x80\x9C[key]\xE2\x80\x9D"},
      {"Your key is **q7KxPLAINMISTRALKEY000000000001**", "Your key is **[key]**"},
      {"Authorization: Bearer q7KxPLAINMISTRALKEY000000000001", "Authorization: Bearer [key]"},
      {"{\"model\":\"mistral-large\",\"api_key\":\"q7KxPLAINMISTRALKEY000000000001\",\"n\":1}",
       "{\"model\":\"mistral-large\",\"api_key\":\"[key]\",\"n\":1}"},
      {"GET https://x.example/v1?key=q7KxPLAINMISTRALKEY000000000001&alt=json",
       "GET https://x.example/v1?key=[key]&alt=json"},
      // CUM-546 gate: a model id or a URL after "tokens"/"token" is not a key ...
      {"max output tokens for claude-sonnet-5-20260301 is 64000",
       "max output tokens for claude-sonnet-5-20260301 is 64000"},
      {"token budget exceeded calling https://api.openai.com/v1/responses",
       "token budget exceeded calling https://api.openai.com/v1/responses"},
      {"the token endpoint https://api.openai.com/v1/oauth/token2 refused",
       "the token endpoint https://api.openai.com/v1/oauth/token2 refused"},
      {"the key for gpt-4o-2024-08-06 is set", "the key for gpt-4o-2024-08-06 is set"},
      {"the key for anthropic/claude-opus-5-20260416 is set", "the key for anthropic/claude-opus-5-20260416 is set"},
      // ... while a lowercase token with a random part, a UUID (whatever its first
      // character) or a URL whose path carries a secret still is.
      {"slack token xoxb-1234-5678-abcdefghijklmnopqrstuvwx", "slack token [key]"},
      {"the key 3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b", "the key [key]"},
      {"the key e3f2a9c1-7d41-4b0a-9e55-0c1d2e3f4a5b", "the key [key]"},
      {"the key deadbeef-7d41-4b0a-9e55-0c1d2e3f4a5b", "the key [key]"},
      {"sdk key sdk-3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b", "sdk key [key]"},
      {"webhook token https://hooks.slack" ".com/services/T0ABCDEFG/B0ABCDEFG/abcdefghijklmnopqrstuvwx",
       "webhook token [key]"},
      {"bot token https://api.telegram.org/bot123456789:AAHfakeTokenValue0123456789/getMe", "bot token [key]"},
      // CUM-546 gate: the URL exemption read the whole URL for a 16-character run, so
      // a secret in the path broken by '-' or '_' (a UUID-keyed webhook, a Telegram
      // bot token) passed in clear, while a long host label masked a plain URL. Each
      // part after the host gets the secret-shape tests; the host is never a secret.
      {"webhook token https://hooks.example.com/t/3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b", "webhook token [key]"},
      {"webhook token https://n8n.example.com/webhook/3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b?x=1",
       "webhook token [key]"},
      {"bot token https://api.telegram.org/bot123456789:AAHd-Q8f3kLm2n_pQ7rS9tU-vW1xY3zA4b/getMe", "bot token [key]"},
      {"bot token https://api.telegram.org/bot7123456789:AAH-abc_defghijklmn-opqrstuvwxyz012/getMe",
       "bot token [key]"},
      {"token https://bot:Q8f3kLm2n-pQ7rS9tU_vW1xY3zA4b@hooks.example.com/notify", "token [key]"},
      {"api key https://hooks.example.com/notify?sig=Q8f3kLm2n-pQ7rS9tU_vW1xY3zA4b", "api key [key]"},
      {"token endpoint https://generativelanguage.googleapis.com/v1beta/models",
       "token endpoint https://generativelanguage.googleapis.com/v1beta/models"},
      {"key docs at https://platform.openai.com/docs/models/gpt-4o-2024-08-06 say so",
       "key docs at https://platform.openai.com/docs/models/gpt-4o-2024-08-06 say so"},
      {"token https://generativelanguage.googleapis.com:443/v1beta/models/gemini-2.5-pro:generateContent",
       "token https://generativelanguage.googleapis.com:443/v1beta/models/gemini-2.5-pro:generateContent"},
      // Review: the same secrets inside compact JSON or percent-encoded, a random
      // host label (a function or tunnel URL), a path secret next to a named value,
      // a password with an '@' in it or of any shape, no scheme, escaped slashes and
      // a Markdown link; ordinary words with '=' and a user with no password stay.
      {"my bot token {\"api\":\"https://api.telegram.org/bot123456789:AAHd-Q8f3kLm2n_pQ7rS9tU-vW1xY3zA4b\",\"x\":1}",
       "my bot token {\"[key]}"},
      {"my bot token {\"ok\":true,\"url\":\"https://n8n.example.com/webhook/3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b\",\"n\":2}",
       "my bot token {\"[key]}"},
      {"api key https://hooks.example.com/notify?sig=Q8f3kLm2n%2BpQ7rS9tU%2FvW1xY3zA4b%3D", "api key [key]"},
      {"webhook token https://a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6.lambda-url.us-east-1.on.aws/", "webhook token [key]"},
      {"webhook token https://eo1a2b3c4d5e6f7g8h.m.pipedream.net", "webhook token [key]"},
      {"webhook token https://n8n.example.com/webhook/3f2a9c1e-7d41-4b0a-9e55-0c1d2e3f4a5b?token=q7KxPLAINMISTRALKEY000000000001",
       "webhook token [key]"},
      {"token https://user:Q8f3kLm2n@pQ7rS9tU_vW1xY3zA4b@hooks.example.com/x", "token [key]"},
      {"db key postgres://app:Hunter2Prod77@db.internal:5432/app", "db key [key]"},
      {"webhook token hooks.example.com/notify?sig=Q8f3kLm2n-pQ7rS9tU_vW1xY3zA4b", "webhook token [key]"},
      {"the token https:\\/\\/hooks.slack" ".com\\/services\\/T0ABCDEFG\\/B0ABCDEFG\\/abcdefghijklmnopqrstuvwx",
       "the token [key]"},
      {"token [Q8f3kLm2n-pQ7rS9tU_vW1xY3zA4b](https://example.com)", "token [[key])"},
      {"the token count=1500 was spent", "the token count=1500 was spent"},
      {"key docs at https://user@example.com/x say so", "key docs at https://user@example.com/x say so"},
  };
  for (const Case& k : kCases) c.eq(NimbusdRig::redactKeys(k.in), k.out, k.in);
}

// CUM-546 gate: the named-value scan rescanned the whole word for every ':' (a
// separator, a quote character and a token character at once), so one long
// colon-dense word cost seconds on the engine thread at capture and on the HTTP
// thread per GET. Every scan around a separator is now bounded.
static void testMaskStaysLinear(ndtest::Ctx& c) {
  std::printf("  -- (h) masking a long colon-dense word stays linear --\n");
  const std::string colons = "a" + std::string(1 << 17, ':') + "a";
  std::string pairs;
  for (int i = 0; i < 60000; i++) pairs += "ab:";
  std::string tokens;
  for (int i = 0; i < 40000; i++) tokens += "token:";
  tokens += "x";
  std::string url = "token https://x.example/";   // a URL's parts are each read once
  for (int i = 0; i < 60000; i++) url += "ab/";
  const auto t0 = std::chrono::steady_clock::now();
  const bool same = NimbusdRig::redactKeys(colons) == colons && NimbusdRig::redactKeys(pairs) == pairs &&
                    NimbusdRig::redactKeys(tokens) == tokens && NimbusdRig::redactKeys(url) == url;
  const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                      .count();
  c.ok(same, "nothing in those words is a key, and nothing is masked");
  c.ok(ms < 1000, "four ~200 KB separator-dense words are masked in under a second (took " + std::to_string(ms) + " ms)");
  // The bound never costs a real key: a named value at the end of a long run.
  c.eq(NimbusdRig::redactKeys(tokens + ":api_key=q7KxPLAINMISTRALKEY000000000001").substr(tokens.size()),
       ":api_key=[key]", "a NAME=value after a long colon run is still masked");
}

// CUM-546 gate: /api/lastturn is masked again as it is served, but on the copy
// already cut at 256 KB: a key the owner saved after the turn that straddled the
// cut kept its first characters, too few for the held-key mask to recognize. The
// cut now never keeps the head of a word it splits.
static void testCutKeepsNoKeyHead(ndtest::Ctx& c) {
  std::printf("  -- (i) the 256 KB cut never keeps the head of a key it splits --\n");
  std::string t = std::string(100, 'x') + " q7KxCUTKEY0123456789abcdefABCDE tail";
  NimbusdRig::capAnatomy(t, 110);
  c.eq(t, std::string(100, 'x') + " ", "the word the cut splits is dropped");
  std::string run = "a " + std::string(2000, 'z');
  NimbusdRig::capAnatomy(run, 1000);
  c.eqi((long)run.size(), 1000, "a run longer than any key is no key's head and is kept");
  // End to end: a turn whose message puts an unheld 31-character key across the
  // cut (its 8th character at 256 KB), saved by the owner afterwards.
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_USAGE", 1);
  const std::string key = "q7KxSTRADDLEKEY0000000000000007";
  // Where the owner's message starts in the anatomy (a first, identical turn).
  const size_t cut = 256 * 1024;
  size_t mark = 0;
  {
    FakeHttpTransport tx;
    tx.script.push_back(respTurn("/router/openai/v1/responses", "noted"));
    Config cfg;
    NimbusdRig rig(cfg, opts("ud-i1"), &tx);
    rig.say("owner", "MARKHERE " + std::string(cut, 'm'));
    mark = rig.lastTurnAnatomy().find("MARKHERE");
  }
  c.ok(mark != std::string::npos && mark < 200000, "the message's place in the anatomy is known");
  std::string pad;
  while (pad.size() + 4 <= cut - 7 - mark) pad += "pad ";
  pad += std::string(cut - 7 - mark - pad.size(), '.');
  pad.back() = ' ';   // the pad ends in a space, so the key is a word of its own
  FakeHttpTransport tx;
  tx.script.push_back(respTurn("/router/openai/v1/responses", "noted"));
  Config cfg;
  NimbusdRig rig(cfg, opts("ud-i2"), &tx);
  rig.say("owner", pad + key + " and more after it");
  const std::string before = rig.lastTurnAnatomy();
  c.ok(before.size() <= cut, "the anatomy is cut at 256 KB");
  c.ok(rig.applyProviderKey("mistral", key), "the owner saves the key after the turn");
  Web web(rig);
  const ApiResp lt = web.call("GET", "/api/lastturn");
  c.ok(!has(lt.body, "q7KxSTR") && !has(lt.body, "STRADDLE"), "no head of the saved key is served");
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== usage across saves + diagnostics (item 4, CUM-546, offline) ===\n");
  testCountersSurviveSaves(c);
  testDiagnostics(c);
  testLogIsBounded(c);
  testLastTurnIsMasked(c);
  testHeldKeyMaskedWhole(c);
  testSameProviderKeyMaskedWhole(c);
  testLastTurnMaskedOnRead(c);
  testNamedTokenRule(c);
  testMaskStaysLinear(c);
  testCutKeepsNoKeyHead(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
