// test_mistral_workspace - offline (T1/T2) proof that a hosted instance gates
// Mistral Studio connectors exactly like the device: usable once the key's
// workspace LISTS the connector as active in GET /v1/connectors (is_authenticated is
// a hint only - it reads false for connectors that demonstrably work), fail-closed
// before the first answer, and one auth-stamped view behind the model catalog,
// GET /api/connectors and the /api/tools Capabilities rows so they cannot disagree.
//
// Class rules covered:
//   (a) the probe request (host/path/bearer) and the listed => usable flip;
//   (b) no signal (non-200, transport error, junk) keeps the last answer;
//   (c) no request at all without a Mistral key or an enabled Studio connector;
//   (d) a Mistral key change drops the answer (device resetMistralConnectorsProbe);
//   (e) the head the catalog names is the RESOLVED head (first keyed / pinned);
//   (f) the web surface: /api/connectors auth, /api/connectors/catalog, /api/tools
//       rows, /api/orch routing fields, and the background probes queued by
//       /api/verify, a connectors write and a Mistral key write;
//   (g) agreement: for every connector, the badge, the catalog note and the
//       Capabilities availability tell the same story.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY",
                        "TAVILY_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag, const std::string& prio) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.priority = prio;
  return o;
}

// The live shape (2026-09-27): every item active, google_calendar and notion
// is_authenticated:false, document_library true. Trimmed to the fields we read.
static const char* kListing =
    "{\"items\":["
    "{\"id\":\"u-notion\",\"name\":\"notion\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"u-slack\",\"name\":\"slack\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"u-gcal\",\"name\":\"google_calendar\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"u-lib\",\"name\":\"document_library\",\"is_authenticated\":true,\"active\":true}"
    "],\"pagination\":{\"next_cursor\":null,\"page_size\":100}}";

static Exchange ok200(const char* body) {
  Exchange e;
  e.expectHost = "api.mistral.ai";
  e.expectPathContains = "/v1/connectors";
  e.status = 200;
  e.body = body;
  return e;
}

// A Lumi-shaped registry: gcal (web-UI default cid), notion, slack on Mistral, a
// Studio connector the workspace does not offer, and a Mistral built-in.
static const char* kBlob =
    "[{\"name\":\"gcal\",\"type\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\","
    "\"cid\":\"connector_googlecalendar\",\"en\":1},"
    "{\"name\":\"notion\",\"type\":\"notion\",\"prov\":\"mistral\",\"kind\":\"connector\",\"cid\":\"notion\",\"en\":1},"
    "{\"name\":\"slack\",\"type\":\"slack\",\"prov\":\"mistral\",\"kind\":\"connector\",\"cid\":\"slack\",\"en\":1},"
    "{\"name\":\"ghost\",\"type\":\"ghost\",\"prov\":\"mistral\",\"kind\":\"connector\",\"cid\":\"not_offered\",\"en\":1},"
    "{\"name\":\"web_search\",\"type\":\"web_search\",\"prov\":\"mistral\",\"kind\":\"builtin\",\"en\":1}]";

static const nimbus::orch::ConnectorInfo* find(const std::vector<nimbus::orch::ConnectorInfo>& cs,
                                               const char* name) {
  for (const auto& c : cs)
    if (c.name == name) return &c;
  return nullptr;
}

static int authOf(const NimbusdRig& rig, const char* name) {
  auto cs = rig.connectorsView();
  const auto* c = find(cs, name);
  return c ? c->auth : -9;
}

// (a) + (b): the probe flips listed connectors usable; no-signal keeps the answer.
static void testProbeFlipsAndKeeps(ndtest::Ctx& c) {
  std::printf("  -- (a)(b) probe: listed => usable; no signal keeps the last answer --\n");
  clearEnv();
  FakeHttpTransport tx;
  tx.script.push_back(ok200(kListing));
  Exchange unauth = ok200("{\"detail\":\"Unauthorized\"}");
  unauth.status = 401;
  tx.script.push_back(unauth);
  Exchange down; down.status = 0;   // transport failure
  tx.script.push_back(down);
  tx.script.push_back(ok200("<html>bad gateway</html>"));

  Config cfg;
  NimbusdRig rig(cfg, opts("mws-a", "mistral,openai,anthropic"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_KEY_1");
  c.eq(rig.connectors().replaceBlob(kBlob), "", "the Lumi-shaped registry saves");

  // Fail-closed before any answer, and the catalog says why.
  c.eqi(authOf(rig, "gcal"), 2, "unprobed: gcal is NOT usable (fail-closed)");
  c.ok(has(rig.connectorsCatalog(), "gcal (not usable yet: the Mistral workspace has not been checked"),
       "unprobed: the catalog says the workspace was not checked (not 'enable it in Mistral')");
  c.ok(!rig.workspaceProbed(), "no workspace answer yet");

  c.ok(rig.refreshMistralWorkspace(), "a 200 listing lands an answer");
  c.eqi((long)tx.seen.size(), 1, "exactly one probe request");
  if (!tx.seen.empty()) {
    const auto& r = tx.seen[0];
    c.eq(r.method, "GET", "the probe is a GET");
    c.ok(has(r.path, "/v1/connectors") && has(r.path, "page_size=100"),
         "the probe asks for /v1/connectors with the device's page size");
    bool bearer = false;
    for (const auto& h : r.headers)
      if (h.first == "Authorization" && h.second == "Bearer mk_TEST_KEY_1") bearer = true;
    c.ok(bearer, "the probe carries the configured Mistral key as the bearer");
  }
  c.eqi(authOf(rig, "gcal"), 1, "listed + is_authenticated:false => gcal USABLE");
  c.eqi(authOf(rig, "notion"), 1, "listed notion => usable");
  c.eqi(authOf(rig, "slack"), 1, "listed slack => usable");
  c.eqi(authOf(rig, "ghost"), 2, "a Studio connector the workspace does not list => refused");
  c.eqi(authOf(rig, "web_search"), -1, "a built-in stays provider-side (-1)");
  const std::string cat = rig.connectorsCatalog();
  c.ok(has(cat, "gcal (Mistral reports it not signed in: results may come back empty"),
       "the catalog carries the not-signed-in hint for gcal");
  c.ok(!has(cat, "gcal (not usable"), "the hint never gates: gcal is not called unusable");
  c.ok(has(cat, "ghost (not usable until the owner enables it in their Mistral Studio account)"),
       "the unlisted connector points the owner at Mistral");

  c.ok(!rig.refreshMistralWorkspace(), "a 401 is no signal");
  c.eqi(rig.workspaceProbe().status, 401, "the probe outcome records the 401");
  c.ok(!rig.refreshMistralWorkspace(), "a transport failure is no signal");
  c.eqi(rig.workspaceProbe().status, 0, "the probe outcome records the transport failure");
  c.ok(!rig.refreshMistralWorkspace(), "an unparseable 200 is no signal");
  c.eqi(authOf(rig, "gcal"), 1, "after three no-signal probes gcal is STILL usable (last answer kept)");
  clearEnv();
}

// (c) + (d): no request without a key or an enabled Studio connector; a key change
// drops the answer until a fresh probe.
static void testProbeGatesAndKeyReset(ndtest::Ctx& c) {
  std::printf("  -- (c)(d) probe gating + Mistral key change resets --\n");
  clearEnv();
  FakeHttpTransport tx;   // EMPTY script: any request fails the test loudly
  Config cfg;
  NimbusdRig rig(cfg, opts("mws-c", "mistral"), &tx);
  c.eq(rig.connectors().replaceBlob(kBlob), "", "registry saved");
  c.ok(!rig.refreshMistralWorkspace(), "no Mistral key: no probe");
  rig.applyProviderKey("mistral", "mk_TEST_KEY_2");
  c.eq(rig.connectors().replaceBlob(
           "[{\"name\":\"gcal\",\"prov\":\"mistral\",\"kind\":\"connector\",\"en\":0},"
           "{\"name\":\"web_search\",\"prov\":\"mistral\",\"kind\":\"builtin\",\"en\":1}]"),
       "", "only a disabled Studio connector + a built-in");
  c.ok(!rig.refreshMistralWorkspace(), "no ENABLED Studio connector: no probe");
  c.eqi((long)tx.seen.size(), 0, "no request was made in either case");

  tx.script.push_back(ok200(kListing));
  c.eq(rig.connectors().replaceBlob(kBlob), "", "registry with enabled Studio connectors");
  c.ok(rig.refreshMistralWorkspace(), "now the probe runs");
  c.eqi(authOf(rig, "gcal"), 1, "gcal usable");
  rig.applyProviderKey("mistral", "mk_TEST_KEY_3");
  c.eqi(authOf(rig, "gcal"), 2, "a new Mistral key drops the answer (fail-closed until re-probe)");
  c.ok(!rig.workspaceProbed(), "the workspace reads unchecked again");
  clearEnv();
}

// (e) the catalog names the head the turn actually runs on.
static void testResolvedHead(ndtest::Ctx& c) {
  std::printf("  -- (e) the catalog's 'YOU are here' is the resolved head --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("mws-e", "openai,anthropic,mistral,cumulo"), &tx);
  rig.applyProviderKey("mistral", "mk_TEST_KEY_4");
  c.eq(rig.hostBadge(), "mistral",
       "priority led by unkeyed openai/anthropic resolves to the first KEYED provider");
  c.eq(rig.connectors().replaceBlob(kBlob), "", "registry saved");
  c.ok(has(rig.connectorsCatalog(), "You are currently running on mistral"),
       "the catalog says the turn runs on mistral (not the unkeyed first token)");
  rig.applyProviderKey("openai", "sk_TEST_KEY_5");
  c.eq(rig.hostBadge(), "openai", "once openai is keyed it heads the priority");

  NimbusdRig::Options pinned = opts("mws-e2", "openai,mistral");
  pinned.orchHost = "mistral";
  pinned.subPriority = "openai,mistral,anthropic";
  NimbusdRig rig2(cfg, pinned, &tx);
  c.eq(rig2.hostBadge(), "mistral", "an explicit orchHost pin wins");
  c.eq(rig2.subPriority(), "openai,mistral,anthropic", "the sub ladder is its own setting");
  NimbusdRig rig3(cfg, opts("mws-e3", "openai,mistral"), &tx);
  c.eq(rig3.subPriority(), "openai,mistral", "an unset sub ladder follows the head priority");
  clearEnv();
}

static std::string get(WebApi& api, const std::string& path) {
  ApiResp out;
  api.handle("GET", path, "", out);
  return out.body;
}
static ApiResp post(WebApi& api, const std::string& path, const std::string& body) {
  ApiResp out;
  api.handle("POST", path, body, out);
  return out;
}

// Wait (bounded) for the background probe numbered `run` to land on the engine thread.
static bool waitForProbe(const NimbusdRig& rig, uint32_t run, int status) {
  for (int i = 0; i < 150; i++) {
    const auto p = rig.workspaceProbe();
    if (p.runs >= run) return p.status == status;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// The (connector name) -> Capabilities availability map out of /api/tools.
static std::string availabilityOf(const std::string& toolsBody, const std::string& name) {
  JsonDocument d;
  if (deserializeJson(d, toolsBody)) return "";
  for (JsonObjectConst o : d["tools"].as<JsonArrayConst>())
    if (std::string(o["group"] | "") == "connector" && name == (const char*)(o["name"] | ""))
      return (const char*)(o["availability"] | "");
  return "";
}
static int badgeAuthOf(const std::string& connBody, const std::string& name) {
  JsonDocument d;
  if (deserializeJson(d, connBody)) return -9;
  for (JsonObjectConst o : d["configured"].as<JsonArrayConst>())
    if (name == (const char*)(o["name"] | "")) return o["auth"].is<int>() ? o["auth"].as<int>() : -8;
  return -9;
}

static std::string formEncode(const std::string& key, const std::string& v) {
  std::string out = key + "=";
  for (char ch : v) {
    if (isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.') { out += ch; continue; }
    char b[4];
    std::snprintf(b, sizeof b, "%%%02X", (unsigned char)ch);
    out += b;
  }
  return out;
}

// GET /api/tools, retried past the honest 503 it answers while the engine is busy.
static std::string toolsWhenIdle(WebApi& api) {
  for (int i = 0; i < 100; i++) {
    ApiResp r;
    api.handle("GET", "/api/tools", "", r);
    if (r.status == 200) return r.body;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return "";
}

// (g) agreement, per connector: badge auth <=> Capabilities availability <=> catalog.
static void checkSurfacesAgree(ndtest::Ctx& c, WebApi& api, const std::string& conns) {
  const std::string tools = toolsWhenIdle(api);
  const std::string cat = get(api, "/api/connectors/catalog");
  c.ok(has(cat, "[PROVIDERS & CONNECTORS]"), "GET /api/connectors/catalog returns the model block");
  for (const char* n : {"gcal", "notion", "slack", "ghost"}) {
    const int a = badgeAuthOf(conns, n);
    const std::string av = availabilityOf(tools, n);
    const bool catalogUsable = !has(cat, std::string(n) + " (not usable");
    const bool usable = a == 1;
    c.ok(usable == (av == "orchestrator-direct") && usable == catalogUsable,
         std::string(n) + ": badge auth, Capabilities availability and catalog agree (" +
             std::to_string(a) + "/" + av + ")");
  }
}

// (f) + (g): the web surface, driven through the real WebApi + engine thread.
static void testWebSurface(ndtest::Ctx& c) {
  std::printf("  -- (f)(g) web surface: badges, catalog, capabilities, background probes --\n");
  clearEnv();
  FakeHttpTransport tx;
  tx.script.push_back(ok200("{\"items\":[]}"));   // probe #1 (connectors write): empty workspace
  tx.script.push_back(ok200(kListing));           // probe #2 (/api/verify)
  tx.script.push_back(ok200(kListing));           // probe #3 (after a Mistral key write)
  Config cfg;
  NimbusdRig::Options o = opts("mws-f", "openai,anthropic,mistral,cumulo");
  o.subPriority = "openai,mistral,anthropic";
  NimbusdRig rig(cfg, o, &tx);
  rig.applyProviderKey("mistral", "mk_TEST_KEY_6");
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);

  const std::string orch = get(api, "/api/orch");
  c.ok(has(orch, "\"provPrio\":\"openai,anthropic,mistral,cumulo\"") &&
           has(orch, "\"subPrio\":\"openai,mistral,anthropic\"") && has(orch, "\"orchHost\":\"\""),
       "/api/orch reports the head ladder, the sub ladder and the (unpinned) orchHost");

  // A connectors write with the workspace unchecked queues a probe; the empty
  // workspace is a REAL answer, so every Studio connector reads refused.
  c.eqi(post(api, "/api/connectors", formEncode("blob", kBlob)).status, 200, "POST /api/connectors saves the registry");
  c.ok(waitForProbe(rig, 1, 200), "the write queued a workspace probe (it was unchecked)");
  c.eqi(badgeAuthOf(get(api, "/api/connectors"), "gcal"), 2,
        "an empty workspace: the gcal badge reads refused (auth 2)");

  // POST /api/verify provider=mistral re-probes; the live-shaped listing flips gcal.
  c.eqi(post(api, "/api/verify", "provider=mistral").status, 200, "POST /api/verify accepted");
  c.ok(waitForProbe(rig, 2, 200) && rig.workspaceProbe().noted, "verify ran the workspace probe");
  c.eqi(post(api, "/api/verify", "provider=openai").status, 200, "a non-mistral verify is acknowledged");
  const std::string conns = get(api, "/api/connectors");
  c.eqi(badgeAuthOf(conns, "gcal"), 1, "after verify the gcal badge reads usable (auth 1)");
  c.ok(has(conns, "\"host\":\"mistral\""), "the badge host is the resolved head (mistral)");
  c.ok(!has(conns, "mk_TEST_KEY"), "no key material in the connectors view");

  checkSurfacesAgree(c, api, conns);

  // A Mistral key write resets the answer and queues a fresh probe.
  c.eqi(post(api, "/api/orch", "mistKey=mk_TEST_KEY_7").status, 200, "POST /api/orch mistKey saves");
  c.ok(waitForProbe(rig, 3, 200), "the key write queued a fresh workspace probe");
  c.eqi(authOf(rig, "gcal"), 1, "the re-probe under the new key restores gcal");
  bool newBearer = false;
  for (const auto& h : tx.seen.back().headers)
    if (h.first == "Authorization" && h.second == "Bearer mk_TEST_KEY_7") newBearer = true;
  c.ok(newBearer, "the re-probe used the NEW key");
  c.eqi((long)tx.seen.size(), 3, "three probes total (write, verify, key change); no stray requests");
  eng.stop();
  clearEnv();
}

// (h) probe coalescing + a hostile prov never reaches the page as markup + a slow
// background probe never turns a queued settings save into a false failure.
static void testCoalesceTagAndQueuedSave(ndtest::Ctx& c) {
  std::printf("  -- (h) coalesced probes, safe capability tags, queued saves --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("mws-h", "mistral"), &tx);
  c.ok(rig.claimProbe(), "the first probe request claims the slot");
  c.ok(!rig.claimProbe(), "a second request while one is queued is coalesced");
  rig.refreshMistralWorkspace();   // no key: no request, but the claim is released
  c.ok(rig.claimProbe(), "once the probe runs, a new request can queue one again");
  c.eqi((long)tx.seen.size(), 0, "no request without a key");

  c.eq(rig.connectors().replaceBlob(
           "[{\"name\":\"x\",\"prov\":\"<img src=x onerror=alert(1)>\",\"kind\":\"builtin\",\"en\":1}]"),
       "", "a blob with a markup-shaped prov saves (the device accepts it too)");
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  std::string tools;
  for (int i = 0; i < 100 && tools.empty(); i++) {
    ApiResp r;
    api.handle("GET", "/api/tools", "", r);
    if (r.status == 200) tools = r.body;
    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  c.ok(!has(tools, "<img") && has(tools, "\"tag\":\"custom builtin\""),
       "the capability tag never carries markup (it reads custom)");

  eng.postWork([] { std::this_thread::sleep_for(std::chrono::milliseconds(6500)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ApiResp busyRead;
  api.handle("GET", "/api/tools", "", busyRead);
  c.eqi(busyRead.status, 503, "a read during background work answers busy at once");
  ApiResp save;
  api.handle("POST", "/api/orch", "orchM_mistral=mistral-small-latest", save);
  c.eqi(save.status, 200, "a save queued behind a long probe is not a 503");
  c.ok(has(save.body, "\"queued\":true"), "it says it is queued (and it does apply)");
  // A read queued now runs AFTER the save (the engine thread is FIFO).
  auto after = eng.dispatchRead([&rig] { return rig.modelFor("mistral"); });
  const bool ran = after.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  c.ok(ran && after.get() == "mistral-small-latest", "the queued model save applied");
  eng.stop();
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  c.suite = "mistral workspace gating (device parity)";
  std::printf("=== %s ===\n", c.suite);
  testProbeFlipsAndKeeps(c);
  testProbeGatesAndKeyReset(c);
  testResolvedHead(c);
  testWebSurface(c);
  testCoalesceTagAndQueuedSave(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
