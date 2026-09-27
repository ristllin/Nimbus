#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/connectors_wire.h"

using nimbus::orch::attachAnthropicWire;
using nimbus::orch::attachMistralWire;
using nimbus::orch::attachOpenAIWire;
using nimbus::orch::catalogText;
using nimbus::orch::ConnectorInfo;
using nimbus::orch::knownCatalogJson;
using nimbus::orch::knownConnectors;
using nimbus::orch::ProviderState;
using nimbus::orch::CapScope;
using nimbus::orch::capScopeSlug;
using nimbus::orch::connectorScope;
using nimbus::orch::urlRoutableToProviderHead;
using nimbus::orch::forwardsToProviderHead;
using nimbus::orch::connectorConfigError;
using nimbus::orch::mistralConnectorId;
using nimbus::orch::connectorAuthFor;
using nimbus::orch::mistralWorkspaceState;
using nimbus::orch::noteMistralWorkspaceProbe;
using nimbus::orch::parseMistralWorkspaceConnectors;

void setUp() {}
void tearDown() {}

static ConnectorInfo mk(const char* name, const char* prov, const char* kind,
                        const char* url = "", const char* cid = "", bool en = true) {
  ConnectorInfo c;
  c.name = name;
  c.prov = prov;
  c.kind = kind;
  c.url = url;
  c.connectorId = cid;
  c.type = name;
  c.enabled = en;
  return c;
}

static std::string dump(const JsonDocument& d) {
  std::string s;
  serializeJson(d, s);
  return s;
}
static bool has(const JsonDocument& d, const char* needle) {
  return dump(d).find(needle) != std::string::npos;
}

// A bearer closure that returns a fixed token for a named connector.
static nimbus::orch::BearerFn fixedBearer(const char* forName, const char* tok) {
  std::string n = forName, t = tok;
  return [n, t](const ConnectorInfo& c) -> std::string { return c.name == n ? t : std::string(); };
}

// ---- golden helper (same contract as test_harness_goldens) ------------------
// GOLDEN_UPDATE=1 blesses; in compare mode a missing golden is a FAILURE and a
// drift dumps the current text to test/golden/out/<name> for `diff`.
static const char* kGoldenDir = "test/golden";
static const char* kOutDir = "test/golden/out";
static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}
static bool readFileG(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
static void writeFileG(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
static void checkTextGolden(const char* name, const std::string& current) {
  std::string goldenPath = std::string(kGoldenDir) + "/" + name;
  if (blessMode()) {
    writeFileG(goldenPath, current);
    TEST_MESSAGE((std::string("blessed ") + goldenPath).c_str());
    return;
  }
  std::string blessed;
  if (!readFileG(goldenPath, blessed)) {
    writeFileG(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("missing golden ") + goldenPath).c_str());
    return;
  }
  if (blessed != current) {
    writeFileG(std::string(kOutDir) + "/" + name, current);
    TEST_FAIL_MESSAGE((std::string("catalog drift vs ") + goldenPath +
                       " - diff it against " + kOutDir + "/" + name).c_str());
  }
}

// ---- OpenAI attach ----------------------------------------------------------

static void test_openai_first_party_connector_id() {
  std::vector<ConnectorInfo> cs = {mk("gmail", "openai", "connector", "", "connector_gmail")};
  JsonDocument d;
  // First-party connectors attach only WITH a bearer (below: without one).
  attachOpenAIWire(d, cs, [](const ConnectorInfo&) { return std::string("tok123"); });
  TEST_ASSERT_EQUAL(1, d["tools"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("mcp", d["tools"][0]["type"]);
  TEST_ASSERT_EQUAL_STRING("connector_gmail", d["tools"][0]["connector_id"]);
  TEST_ASSERT_EQUAL_STRING("never", d["tools"][0]["require_approval"]);
  TEST_ASSERT_FALSE(d["tools"][0]["server_url"].is<const char*>());
  // REGRESSION (live, nimbus-5 2026-08-07): server_label is required by the
  // OpenAI API on EVERY mcp tool - the connector_id branch omitted it, so one
  // enabled first-party connector 400'd every head turn on that provider.
  TEST_ASSERT_EQUAL_STRING("gmail", d["tools"][0]["server_label"]);
}

static void test_openai_remote_mcp_with_bearer() {
  std::vector<ConnectorInfo> cs = {mk("github", "openai", "mcp", "https://api.githubcopilot.com/mcp/")};
  JsonDocument d;
  attachOpenAIWire(d, cs, fixedBearer("github", "ghp_secret"));
  TEST_ASSERT_EQUAL_STRING("github", d["tools"][0]["server_label"]);
  TEST_ASSERT_EQUAL_STRING("https://api.githubcopilot.com/mcp/", d["tools"][0]["server_url"]);
  TEST_ASSERT_EQUAL_STRING("ghp_secret", d["tools"][0]["authorization"]);
}

static void test_openai_prov_filtering() {
  std::vector<ConnectorInfo> cs = {
      mk("a", "mistral", "builtin"),          // wrong provider -> skip
      mk("b", "any", "mcp", "https://x"),     // "any" -> included
      mk("c", "openai", "mcp", "https://y"),  // openai -> included
      mk("d", "openai", "mcp", "https://z", "", false),  // disabled -> skip
  };
  JsonDocument d;
  attachOpenAIWire(d, cs, nullptr);
  TEST_ASSERT_EQUAL(2, d["tools"].as<JsonArrayConst>().size());
  TEST_ASSERT_FALSE(has(d, "\"a\""));
  TEST_ASSERT_TRUE(has(d, "https://x"));
  TEST_ASSERT_TRUE(has(d, "https://y"));
  TEST_ASSERT_FALSE(has(d, "https://z"));
}

// ---- CUM-255: prov-routing guard (LAN MCP URL must never reach a head) -------

// A device-dialed MCP with a private (LAN) URL. mk() leaves deviceDialed false,
// so set it (and appr) the way the connectors blob "dev":1/"appr":1 flags parse.
static ConnectorInfo mkDeviceDialed(const char* name, const char* prov, const char* url) {
  ConnectorInfo c = mk(name, prov, "mcp", url);
  c.deviceDialed = true;
  c.approved = true;
  return c;
}

// THE REGRESSION (CUM-61 live validation -> CUM-255): a device-dialed MCP entry
// with `prov` omitted parses to prov=="any"; the old provMatches() then forwarded
// its LAN URL to the OpenAI head as server_url, which OpenAI's cloud cannot reach
// (HTTP 424) - killing the WHOLE turn. The guard keeps a device-dialed default
// entry device-side: zero tools attached to any head, so the turn survives and
// only the device-side client dials it.
static void test_cum255_device_dialed_lan_not_forwarded_to_head() {
  ConnectorInfo lan = mkDeviceDialed("myfs", "any", "http://192.168.50.145:8765/mcp");
  std::vector<ConnectorInfo> cs = {lan};
  JsonDocument oa;
  attachOpenAIWire(oa, cs, nullptr);
  TEST_ASSERT_FALSE(oa["tools"].is<JsonArrayConst>());   // nothing forwarded -> turn survives
  JsonDocument an;
  attachAnthropicWire(an, cs, nullptr);
  TEST_ASSERT_FALSE(an["mcp_servers"].is<JsonArrayConst>());
  JsonDocument mi;
  attachMistralWire(mi, cs);
  TEST_ASSERT_FALSE(mi["tools"].is<JsonArrayConst>());
  // The guard predicate agrees for every head.
  TEST_ASSERT_FALSE(forwardsToProviderHead(lan, "openai"));
  TEST_ASSERT_FALSE(forwardsToProviderHead(lan, "anthropic"));
  TEST_ASSERT_FALSE(forwardsToProviderHead(lan, "mistral"));
}

// urlRoutableToProviderHead: only a public http(s) host is routable from a
// provider's cloud. Loopback, RFC-1918, link-local, .local, non-http => false.
static void test_url_routable_predicate() {
  TEST_ASSERT_TRUE(urlRoutableToProviderHead("https://api.githubcopilot.com/mcp/"));
  TEST_ASSERT_TRUE(urlRoutableToProviderHead("https://mcp.linear.app/mcp"));
  TEST_ASSERT_TRUE(urlRoutableToProviderHead("http://93.184.216.34/mcp"));  // public IP
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://192.168.1.20:3111/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://10.0.0.5/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://172.16.4.4/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://172.31.255.1/mcp"));
  TEST_ASSERT_TRUE(urlRoutableToProviderHead("http://172.15.0.1/mcp"));     // just outside 172.16/12
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://169.254.10.10/mcp")); // link-local
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://127.0.0.1:8080/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://localhost:3000/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://nimbus-6.local/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://0.0.0.0/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://[::1]:9000/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("ftp://example.com/mcp"));    // non-http scheme
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://user@192.168.1.1/mcp"));  // userinfo can't hide a LAN host
  TEST_ASSERT_FALSE(urlRoutableToProviderHead(""));
  // Evasion variants a review confirmed (CUM-255 hardening) - all must be caught:
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://127.0.0.1?x=1"));      // query, no path
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://10.0.0.5#frag"));      // fragment, no path
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://[::ffff:10.0.0.1]/mcp"));   // IPv4-mapped private
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://[::ffff:127.0.0.1]/mcp"));  // IPv4-mapped loopback
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://[0:0:0:0:0:0:0:1]/mcp"));   // non-canonical loopback
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://[0::1]/mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://localhost./mcp"));     // trailing-dot FQDN
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://printer.local./mcp"));
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://10.0.0.1./mcp"));      // trailing-dot IPv4
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://2130706433/mcp"));     // decimal 127.0.0.1
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://0x7f000001/mcp"));     // hex 127.0.0.1
  TEST_ASSERT_FALSE(urlRoutableToProviderHead("http://3232235521/mcp"));     // decimal 192.168.0.1
  // A public IPv4-mapped IPv6 and a public integer host stay routable.
  TEST_ASSERT_TRUE(urlRoutableToProviderHead("http://[::ffff:93.184.216.34]/mcp"));
}

// A kind=="connector" entry with an EMPTY connector_id falls into attachOpenAIWire's
// server_url branch, so a private URL there must ALSO be blocked - the guard keys on
// URL presence, not kind=="mcp". (Review-confirmed bypass; CUM-255 hardening.)
static void test_cum255_connector_kind_empty_cid_lan_blocked() {
  ConnectorInfo c = mk("x", "openai", "connector", "http://192.168.1.10/mcp", "");  // cid empty
  std::vector<ConnectorInfo> cs = {c};
  JsonDocument d;
  attachOpenAIWire(d, cs, [](const ConnectorInfo&) { return std::string("tok"); });
  TEST_ASSERT_FALSE(d["tools"].is<JsonArrayConst>());   // LAN url never emitted as server_url
  TEST_ASSERT_FALSE(forwardsToProviderHead(c, "openai"));
  // The same shape with a PUBLIC url still forwards (server_url branch works).
  ConnectorInfo pub = mk("y", "openai", "connector", "https://api.example.com/mcp", "");
  std::vector<ConnectorInfo> cs2 = {pub};
  JsonDocument d2;
  attachOpenAIWire(d2, cs2, [](const ConnectorInfo&) { return std::string("tok"); });
  TEST_ASSERT_EQUAL(1, d2["tools"].as<JsonArrayConst>().size());
}

// CLASS GUARD (the invariant, not the instance): over the WHOLE prov-resolution
// table, a private/LAN MCP URL is forwarded to NO head for ANY prov value, and a
// new/unknown prov value routes to NO head (fail-closed). A public URL keeps its
// explicit routing. If a future prov is added without an explicit routing rule,
// this test FAILS rather than silently forwarding it.
static void test_prov_routing_table_is_fail_closed() {
  const char* heads[] = {"openai", "anthropic", "mistral"};
  // Every prov value a blob might carry, including an unknown/future one.
  const char* provs[] = {"openai", "anthropic", "mistral", "any", "device", "", "grok"};

  for (const char* p : provs) {
    // 1. A private/LAN MCP URL must NEVER be forwarded, whatever the prov says.
    ConnectorInfo lan = mk("srv", p, "mcp", "http://192.168.1.50:3111/mcp");
    for (const char* h : heads)
      TEST_ASSERT_FALSE_MESSAGE(forwardsToProviderHead(lan, h),
                                "a LAN MCP URL must never reach a provider head");

    // 2. A device-dialed entry at this prov never reaches a head unless the prov
    //    EXPLICITLY names that head (public URL, so the URL guard is not the cause).
    ConnectorInfo dev = mkDeviceDialed("srv", p, "https://public.example.com/mcp");
    for (const char* h : heads) {
      bool expect = (std::string(p) == h);   // explicit head name only; never via "any"
      TEST_ASSERT_EQUAL_MESSAGE(expect, forwardsToProviderHead(dev, h),
                                "device-dialed routes to a head only on an explicit prov");
    }

    // 3. A plain (not device-dialed) public MCP: only the explicit head or "any".
    ConnectorInfo pub = mk("srv", p, "mcp", "https://public.example.com/mcp");
    for (const char* h : heads) {
      bool expect = (std::string(p) == h) || (std::string(p) == "any");
      TEST_ASSERT_EQUAL_MESSAGE(expect, forwardsToProviderHead(pub, h),
                                "an unknown/future prov must route to NO head");
    }
  }
}

// The config-time validator: an entry that WOULD be forwarded to a head with an
// unroutable URL is rejected with a clear error; every safe shape passes.
static void test_connector_config_error() {
  // A plain LAN MCP with prov omitted (parses to "any") would be forwarded -> reject.
  TEST_ASSERT_FALSE(connectorConfigError(mk("x", "any", "mcp", "http://192.168.1.9/mcp")).empty());
  // Explicitly routed to a head with a LAN URL -> reject (names the next step).
  std::string e = connectorConfigError(mk("x", "openai", "mcp", "http://10.1.2.3/mcp"));
  TEST_ASSERT_FALSE(e.empty());
  TEST_ASSERT_TRUE(e.find("device-dialed") != std::string::npos);
  // A device-dialed LAN entry at the default prov "any" is device-side -> OK.
  TEST_ASSERT_TRUE(connectorConfigError(mkDeviceDialed("x", "any", "http://192.168.1.9/mcp")).empty());
  // A device-dialed entry with an EXPLICIT head prov + LAN url is still device-side
  // (the attach guard blocks the head path); do not flag it with "turn on
  // device-dialed" when it already is - review-confirmed wording bug.
  TEST_ASSERT_TRUE(connectorConfigError(mkDeviceDialed("x", "openai", "http://192.168.1.9/mcp")).empty());
  // A public MCP URL routed to a head -> OK.
  TEST_ASSERT_TRUE(connectorConfigError(mk("x", "openai", "mcp", "https://api.example.com/mcp")).empty());
  // Non-mcp kinds carry no dialed URL -> never this error.
  TEST_ASSERT_TRUE(connectorConfigError(mk("gmail", "openai", "connector", "", "connector_gmail")).empty());
  TEST_ASSERT_TRUE(connectorConfigError(mk("web_search", "mistral", "builtin")).empty());
  // An unknown/future prov targets no head, so a LAN URL there is not a config
  // error (it simply attaches nowhere) - the attach guard still keeps it off heads.
  TEST_ASSERT_TRUE(connectorConfigError(mk("x", "grok", "mcp", "http://192.168.1.9/mcp")).empty());
}

// ---- Mistral attach ---------------------------------------------------------

static void test_mistral_builtin_and_studio_connector() {
  ConnectorInfo notion = mk("notion", "mistral", "connector");  // Studio connector by name
  ConnectorInfo custom = mk("wiki", "mistral", "connector");
  custom.connectorId = "conn_abc123";                           // custom MCP by UUID
  std::vector<ConnectorInfo> cs = {
      mk("web_search", "mistral", "builtin"),
      notion,
      custom,
      mk("github", "openai", "mcp", "https://x"),               // wrong provider -> skip
  };
  JsonDocument d;
  attachMistralWire(d, cs);
  TEST_ASSERT_EQUAL(3, d["tools"].as<JsonArrayConst>().size());
  // built-in tool: {type:"web_search"}
  TEST_ASSERT_EQUAL_STRING("web_search", d["tools"][0]["type"]);
  TEST_ASSERT_FALSE(d["tools"][0]["connector_id"].is<const char*>());
  // Studio connector by name: {type:"connector", connector_id:"notion"}
  TEST_ASSERT_EQUAL_STRING("connector", d["tools"][1]["type"]);
  TEST_ASSERT_EQUAL_STRING("notion", d["tools"][1]["connector_id"]);
  // custom connector by UUID takes connectorId over name
  TEST_ASSERT_EQUAL_STRING("connector", d["tools"][2]["type"]);
  TEST_ASSERT_EQUAL_STRING("conn_abc123", d["tools"][2]["connector_id"]);
}

// ---- Anthropic attach -------------------------------------------------------

static void test_anthropic_mcp_servers_with_token() {
  std::vector<ConnectorInfo> cs = {
      mk("notion", "anthropic", "mcp", "https://mcp.notion.com"),
      mk("gmail", "anthropic", "connector", "", "connector_gmail"),  // no url -> skip
      mk("web_search", "mistral", "builtin"),                        // wrong provider
  };
  JsonDocument d;
  attachAnthropicWire(d, cs, fixedBearer("notion", "secret_ntn"));
  TEST_ASSERT_EQUAL(1, d["mcp_servers"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("url", d["mcp_servers"][0]["type"]);
  TEST_ASSERT_EQUAL_STRING("https://mcp.notion.com", d["mcp_servers"][0]["url"]);
  TEST_ASSERT_EQUAL_STRING("notion", d["mcp_servers"][0]["name"]);
  TEST_ASSERT_EQUAL_STRING("secret_ntn", d["mcp_servers"][0]["authorization_token"]);
}

static void test_anthropic_no_tools_key_polluted() {
  // Anthropic connectors must NOT land in tools[] (that is the agent toolset).
  std::vector<ConnectorInfo> cs = {mk("notion", "any", "mcp", "https://mcp.notion.com")};
  JsonDocument d;
  attachAnthropicWire(d, cs, nullptr);
  TEST_ASSERT_FALSE(d["tools"].is<JsonArrayConst>());
  TEST_ASSERT_EQUAL(1, d["mcp_servers"].as<JsonArrayConst>().size());
}

// ---- catalog text -----------------------------------------------------------

static void test_catalog_marks_current_provider() {
  std::vector<ConnectorInfo> cs = {
      mk("github", "openai", "mcp", "https://x"),
      mk("web_search", "mistral", "builtin"),
  };
  ProviderState ps;
  ps.openaiKeyed = ps.mistralKeyed = true;
  ps.currentHost = "openai";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("currently running on openai") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("openai (YOU are here)") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("Enabled connectors: github") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("web_search") != std::string::npos);
  // The current host is NOT anthropic, so anthropic is not the "you" line.
  TEST_ASSERT_TRUE(t.find("anthropic (YOU are here)") == std::string::npos);
}

// A Mistral connector (Studio connector or hosted built-in) has no device
// credential - it is authorized in the owner's Mistral account. So an enabled-but-
// unauthorized (auth==2) Mistral non-mcp connector must point at Mistral Studio, NOT
// tell the owner to "add a credential" here; a device-token MCP still says the latter.
static void test_catalog_mistral_studio_points_at_studio_not_device() {
  ConnectorInfo studio = mk("gcal", "mistral", "connector");  // Studio connector by name
  studio.enabled = true; studio.auth = 2;                     // enabled, not authorized
  ConnectorInfo mcp = mk("slack", "mistral", "mcp");          // device-token MCP
  mcp.enabled = true; mcp.auth = 2;
  std::vector<ConnectorInfo> cs = {studio, mcp};
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("Mistral Studio account") != std::string::npos);          // gcal
  TEST_ASSERT_TRUE(t.find("NO credential - not usable until the owner adds one")
                   != std::string::npos);                                            // slack
}

static void test_catalog_no_connectors() {
  std::vector<ConnectorInfo> cs;
  ProviderState ps;
  ps.anthropicKeyed = true;
  ps.currentHost = "anthropic";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("no connectors configured") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("NO KEY") != std::string::npos);  // openai/mistral unkeyed
}

// ---- known catalog ----------------------------------------------------------

static void test_known_catalog_json_shape() {
  std::string j = knownCatalogJson();
  JsonDocument d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, j).code());
  TEST_ASSERT_TRUE(d.is<JsonArrayConst>());
  int n = 0;
  knownConnectors(n);
  TEST_ASSERT_EQUAL(n, (int)d.as<JsonArrayConst>().size());
  TEST_ASSERT_TRUE(j.find("\"github\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"gmail\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("mistral-builtins") != std::string::npos);
  // Every row carries the join + link fields + the honest capability caps.
  for (JsonObjectConst o : d.as<JsonArrayConst>()) {
    TEST_ASSERT_TRUE(o["id"].is<const char*>());
    TEST_ASSERT_TRUE(o["docs"].is<const char*>());
    TEST_ASSERT_TRUE(o["cred"].is<const char*>());
    TEST_ASSERT_TRUE(o["caps"].is<const char*>());
  }
  // The honest Gmail caveat (draft-only, no send) is carried in the catalog.
  TEST_ASSERT_TRUE(j.find("NO send") != std::string::npos);
}

// A caps'd, ENABLED connector surfaces its real capability/limit line to the model
// so it can't hallucinate an action the connector has no tool for (the Gmail-send
// hallucination regression).
static void test_catalog_surfaces_connector_caps() {
  std::vector<ConnectorInfo> cs = {mk("gmail", "mistral", "connector")};
  ProviderState ps;
  ps.mistralKeyed = true;
  ps.currentHost = "mistral";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("capabilities/limits") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("NO send tool") != std::string::npos);
}

// GitHub caps honesty - verified LIVE against api.githubcopilot.com/mcp/
// tools/list (44 tools): repo creation + file pushes EXIST; Actions/Gists do
// not. The old string was silent on creation and claimed Actions/Gists - the
// model either refused something that works or promised something absent
// (the eval-baseline confabulated-422 class).
static void test_github_caps_state_repo_creation() {
  std::vector<ConnectorInfo> cs = {mk("github", "openai", "mcp", "https://x")};
  ProviderState ps;
  ps.openaiKeyed = true;
  ps.currentHost = "openai";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("CREATE repositories") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("repo scope") != std::string::npos);
  // The clone/build/run limit is still stated; the false blanket "cannot push"
  // is gone (the API pushes file contents; it does not git-push a clone).
  TEST_ASSERT_TRUE(t.find("cannot CLONE, BUILD, or RUN") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("No sub can push") == std::string::npos);
}

// Discoverability: available-but-unconfigured catalog entries are named in ONE
// line so "could you do X?" is answerable when nothing is set up - and a
// configured id must NOT appear in that line.
static void test_catalog_names_unconfigured_connectors() {
  ProviderState ps;
  ps.openaiKeyed = true;
  ps.currentHost = "openai";
  // Nothing configured: the line lists the catalog (github among it).
  std::string t0 = catalogText({}, ps);
  size_t at = t0.find("Not configured (owner can add");
  TEST_ASSERT_TRUE(at != std::string::npos);
  std::string line0 = t0.substr(at, t0.find('\n', at) - at);
  TEST_ASSERT_TRUE(line0.find("github") != std::string::npos);
  // github configured: it leaves the line; others (e.g. notion) stay.
  std::vector<ConnectorInfo> cs = {mk("github", "openai", "mcp", "https://x")};
  std::string t1 = catalogText(cs, ps);
  at = t1.find("Not configured (owner can add");
  TEST_ASSERT_TRUE(at != std::string::npos);
  std::string line1 = t1.substr(at, t1.find('\n', at) - at);
  TEST_ASSERT_TRUE(line1.find("github") == std::string::npos);
  TEST_ASSERT_TRUE(line1.find("notion") != std::string::npos);
}

// GRID-BORN (2026-08-07): a first-party connector with NO auth token must be
// SKIPPED, not attached - OpenAI rejects connector_id without authorization,
// which poisons the whole turn (the nimbus-5 field-failure class).
static void test_openai_first_party_without_bearer_is_skipped() {
  std::vector<ConnectorInfo> cs = {mk("gmail", "openai", "connector", "", "connector_gmail")};
  JsonDocument d;
  attachOpenAIWire(d, cs, nullptr);
  TEST_ASSERT_EQUAL(0, d["tools"].as<JsonArrayConst>().size());
}

// GRID-BORN: Mistral connector ids are Mistral's OWN namespace. An unknown id
// is silently IGNORED by the API (200, no tools) - so an OpenAI-namespace or
// legacy id must map to the workspace-listed canonical id.
static void test_mistral_ids_map_to_the_mistral_namespace() {
  std::vector<ConnectorInfo> cs = {
      mk("github", "mistral", "connector", "", ""),                    // blank -> canonical
      mk("gdrive", "mistral", "connector", "", "connector_googledrive"),  // OpenAI ns -> canonical
      mk("gcal", "mistral", "connector", "", "connector_googlecalendar"),
      mk("notion", "mistral", "connector", "", "0198f11d-uuid"),       // explicit id WINS
  };
  JsonDocument d;
  attachMistralWire(d, cs);
  TEST_ASSERT_EQUAL_STRING("github_app", d["tools"][0]["connector_id"]);
  TEST_ASSERT_EQUAL_STRING("google_drive_mcp", d["tools"][1]["connector_id"]);
  TEST_ASSERT_EQUAL_STRING("google_calendar", d["tools"][2]["connector_id"]);
  TEST_ASSERT_EQUAL_STRING("0198f11d-uuid", d["tools"][3]["connector_id"]);
}

// GRID-BORN: bare document_library 422s the whole Mistral request (it needs a
// library id) - skipped until one can ride the blob.
static void test_mistral_document_library_bare_is_skipped() {
  std::vector<ConnectorInfo> cs = {mk("document_library", "mistral", "builtin", "", "")};
  JsonDocument d;
  attachMistralWire(d, cs);
  TEST_ASSERT_EQUAL(0, d["tools"].as<JsonArrayConst>().size());
}

// ---- W3: verified marking + custom MCP + [SUB-AGENT CAPABILITIES] ----------

static void test_catalog_marks_verified_state() {
  std::vector<ConnectorInfo> cs;
  ProviderState ps;
  ps.openaiKeyed = true;    ps.openaiVerified = 1;    // verified
  ps.anthropicKeyed = true; ps.anthropicVerified = 0; // rejected
  ps.mistralKeyed = true;   ps.mistralVerified = -1;  // unchecked
  ps.currentHost = "openai";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("available, VERIFIED") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("REJECTED on last check") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("not yet verified") != std::string::npos);
}

// W7b: the OpenAI code_interpreter BUILTIN attaches as its own tool type (with
// the auto container), never the mcp shape - and an unknown openai builtin is
// skipped rather than emitted as a doomed mcp entry.
// CUM-49: the CODE SANDBOX availability line must be gated on a KEYED provider, not
// just an enabled code_interpreter builtin (the toggle injects the builtin regardless
// of keys). Off when no key; available only when keyed.
static void test_code_sandbox_availability_key_gated() {
  std::vector<ConnectorInfo> cs = {mk("code_interpreter", "openai", "builtin")};
  ProviderState off;                          // no provider keyed
  std::string toff = catalogText(cs, off);
  TEST_ASSERT_TRUE(toff.find("CODE SANDBOX: off") != std::string::npos);
  TEST_ASSERT_TRUE(toff.find("CODE SANDBOX: available") == std::string::npos);
  ProviderState on;
  on.openaiKeyed = true;                      // now a keyed provider can run it
  std::string ton = catalogText(cs, on);
  TEST_ASSERT_TRUE(ton.find("CODE SANDBOX: available") != std::string::npos);
}

static void test_openai_code_interpreter_builtin_attaches() {
  std::vector<ConnectorInfo> cs = {
      mk("code_interpreter", "openai", "builtin"),
      mk("image_generation", "openai", "builtin"),   // not wired -> skipped
  };
  JsonDocument d;
  attachOpenAIWire(d, cs, nullptr);
  TEST_ASSERT_EQUAL(1, d["tools"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("code_interpreter", d["tools"][0]["type"]);
  TEST_ASSERT_EQUAL_STRING("auto", d["tools"][0]["container"]["type"]);
  TEST_ASSERT_FALSE(d["tools"][0]["server_label"].is<const char*>());  // no mcp shape
}

// W12: enabled is a checkbox, not health - the catalog surfaces the two states
// that need intervention (failed sign-in, missing credential) and leaves a
// healthy connector as a bare name.
static void test_catalog_marks_connector_auth_problems() {
  ConnectorInfo ok = mk("notion", "mistral", "mcp", "https://mcp.notion.com");
  ok.auth = 1;
  ConnectorInfo failed = mk("gmail", "mistral", "connector");
  failed.auth = 0;
  ConnectorInfo missing = mk("linear", "mistral", "mcp", "https://mcp.linear.app/mcp");
  missing.auth = 2;
  std::vector<ConnectorInfo> cs = {ok, failed, missing};
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("gmail (sign-in FAILED - tell the owner)") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("linear (NO credential - not usable") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("notion,") != std::string::npos ||
                   t.find("notion\n") != std::string::npos);   // bare name, no marker
  TEST_ASSERT_TRUE(t.find("notion (") == std::string::npos);
  // W12 coding guidance is present
  TEST_ASSERT_TRUE(t.find("For CODING tasks") != std::string::npos);
}

// W3b: with validation OFF (capProbe==0) the catalog makes NO verified/rejected
// claim even when the cache has one - it reports key-presence and trusts it.
static void test_catalog_validation_off_makes_no_claim() {
  std::vector<ConnectorInfo> cs;
  ProviderState ps;
  ps.openaiKeyed = true; ps.openaiVerified = 1;   // cache SAYS verified...
  ps.capProbe = 0;                                // ...but validation is OFF
  ps.currentHost = "openai";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("validation off - trusting key presence") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("available, VERIFIED") == std::string::npos);  // no claim
}

static void test_catalog_custom_mcp_gets_a_line() {
  std::vector<ConnectorInfo> cs = {
      mk("mywiki", "mistral", "mcp", "https://wiki.example/mcp")};  // not in kKnown
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("mywiki (custom MCP at https://wiki.example/mcp)") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("do not assume what it can do") != std::string::npos);
}

static void test_subagent_capabilities_block_is_generated() {
  std::vector<ConnectorInfo> cs = {
      mk("web_search", "mistral", "builtin"),
      mk("notion", "mistral", "connector"),
  };
  ProviderState ps;
  ps.mistralKeyed = true; ps.anthropicKeyed = true;   // openai NOT keyed
  ps.currentHost = "mistral";
  std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("[SUB-AGENT CAPABILITIES]") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("returns TEXT ONLY") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("YOU (the head) do") != std::string::npos);
  // per-keyed-provider spawn lines, built from the live enabled set
  TEST_ASSERT_TRUE(t.find("spawn on mistral:") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("web_search, notion") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("spawn on anthropic:") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("cloud sandbox") != std::string::npos);
  // openai unkeyed -> no spawn-on-openai line
  TEST_ASSERT_TRUE(t.find("spawn on openai:") == std::string::npos);
}

// A representative LIVE catalog block, as the model sees it on the grid board
// (Nimbus-4): mistral host, all three providers keyed AND verified, the full
// enabled connector set. This golden is the human-reviewable render of the
// [PROVIDERS & CONNECTORS] + [SUB-AGENT CAPABILITIES] dynamic input block. Any
// wording drift is a red diff (re-bless only intentionally).
static void test_catalog_full_golden() {
  std::vector<ConnectorInfo> cs = {
      mk("web_search", "mistral", "builtin"),
      mk("code_interpreter", "mistral", "builtin"),
      // The OpenAI-side sandbox is a SEPARATE entry - it is what makes the PDF
      // path real (Mistral's sandbox errors on PDF/CSV). A board with only the
      // mistral one gets the honest "not possible right now" text instead.
      mk("code_interpreter", "openai", "builtin"),
      mk("image_generation", "mistral", "builtin"),
      mk("github", "mistral", "connector"),
      mk("gmail", "mistral", "connector"),
      mk("gcal", "mistral", "connector"),
      mk("gdrive", "mistral", "connector"),
      mk("notion", "mistral", "connector"),
      mk("slack", "mistral", "connector"),
      mk("linear", "mistral", "connector"),
  };
  ProviderState ps;
  ps.openaiKeyed = ps.anthropicKeyed = ps.mistralKeyed = true;
  ps.openaiVerified = ps.anthropicVerified = ps.mistralVerified = 1;
  ps.currentHost = "mistral";
  checkTextGolden("orch_catalog_full.txt", catalogText(cs, ps));
}

// ⚠ Found live on Nimbus-4 (2026-08-09): asked for an AI-news digest, the model
// sent a multi-source sweep to a MISTRAL sub. Mistral's Conversations API is
// synchronous, the device's read deadline is 60 s, and the sub's work was simply
// LOST - no file, no error the head could see, and the head then invented a
// filename for output that never existed. The cap was disclosed NOWHERE in the
// prompt, so the model had no way to route around it. Routing is the only lever
// (the cap is a provider limit, and on-device concurrency is banned).
static void test_mistral_60s_cap_is_disclosed_and_routes_elsewhere() {
  std::vector<ConnectorInfo> cs = {mk("web_search", "mistral", "builtin")};
  ProviderState ps;
  ps.mistralKeyed = ps.openaiKeyed = ps.anthropicKeyed = true;
  ps.currentHost = "mistral";
  const std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE_MESSAGE(t.find("~60 SECONDS") != std::string::npos,
                           "the mistral sub duration cap must be disclosed");
  TEST_ASSERT_TRUE_MESSAGE(t.find("LOST") != std::string::npos,
                           "must say the work is lost, not merely delayed");
  TEST_ASSERT_TRUE_MESSAGE(t.find("spawn on openai or anthropic") != std::string::npos,
                           "must name where to send long work");
  // With no long-running provider keyed there is nowhere to route, so the advice
  // becomes "split it into waves" rather than naming a provider that isn't there.
  ProviderState only;
  only.mistralKeyed = true;
  only.currentHost = "mistral";
  const std::string t2 = catalogText(cs, only);
  TEST_ASSERT_TRUE(t2.find("~60 SECONDS") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(t2.find("spawn on openai") == std::string::npos,
                           "must not route to a provider with no key");
  TEST_ASSERT_TRUE(t2.find("waves") != std::string::npos);
}

// The device cannot render a PDF, but a sub CAN and the firmware captures the
// file (W7/W7b). On hardware the model twice told the owner "the device cannot
// create PDFs" and downgraded to markdown, because the recipe lived only in a
// per-connector footnote. State it where decomposition is planned.
static void test_pdf_recipe_is_stated_and_routed_to_openai() {
  // The recipe holds only when an OPENAI-reachable code_interpreter is enabled.
  std::vector<ConnectorInfo> cs = {mk("code_interpreter", "openai", "builtin")};
  ProviderState ps;
  ps.openaiKeyed = ps.mistralKeyed = true;
  ps.currentHost = "mistral";
  const std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE_MESSAGE(t.find("TO DELIVER A PDF") != std::string::npos,
                           "the PDF recipe must be present");
  TEST_ASSERT_TRUE_MESSAGE(t.find("Do NOT say you cannot produce a PDF") != std::string::npos,
                           "the observed failure must be named explicitly");
  TEST_ASSERT_TRUE(t.find("code_interpreter") != std::string::npos);
  // No OpenAI key => the honest answer is that PDF is unavailable, not silence.
  ProviderState nokey;
  nokey.mistralKeyed = true;
  nokey.currentHost = "mistral";
  const std::string t2 = catalogText(cs, nokey);
  TEST_ASSERT_TRUE(t2.find("needs an OpenAI key") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(t2.find("Spawn on openai with a project") == std::string::npos,
                           "must not advertise a PDF path that has no key");
}

// ⚠ Live on Board 1 (2026-08-09): openai was keyed and verified, but the board's
// only code_interpreter was registered under MISTRAL. The first version of this
// guidance promised the openai PDF path anyway; the model spawned on openai, the
// sub had no sandbox, and it returned prose plus a retry sub that came back
// "(completed, no text output)". Promising a capability the board is not
// configured for is the same lie the block exists to remove.
static void test_pdf_recipe_requires_an_openai_reachable_code_interpreter() {
  std::vector<ConnectorInfo> mistralOnly = {mk("code_interpreter", "mistral", "builtin")};
  ProviderState ps;
  ps.openaiKeyed = ps.mistralKeyed = true;
  ps.currentHost = "mistral";
  const std::string t = catalogText(mistralOnly, ps);
  TEST_ASSERT_TRUE_MESSAGE(t.find("not possible right now") != std::string::npos,
                           "must admit PDF is unavailable when openai has no sandbox");
  TEST_ASSERT_TRUE_MESSAGE(t.find("Code Interpreter enabled for OpenAI") != std::string::npos,
                           "must name the exact thing the owner has to enable");
  TEST_ASSERT_TRUE_MESSAGE(t.find("Spawn on openai with a project") == std::string::npos,
                           "must NOT advertise the openai PDF recipe with no openai sandbox");
  // A disabled openai entry is no better than none.
  std::vector<ConnectorInfo> off = {mk("code_interpreter", "openai", "builtin", "", "", false)};
  TEST_ASSERT_TRUE(catalogText(off, ps).find("not possible right now") != std::string::npos);
}

// ---- CUM-44: parseConnectorsJson - the no-silent-drop blob parser -----------
using nimbus::orch::parseConnectorsJson;

// REGRESSION (Board 1, 2026-08-09): a hardcoded Info[8] silently dropped every
// blob entry past the eighth from the catalog + wire attach while /api/connectors
// showed them. The cap is now kMaxConnectors=24 and the parser must return ALL
// entries up to it - here nine, the exact count that used to vanish.
static void test_parse_connectors_reads_past_eight() {
  std::string blob = "[";
  for (int i = 0; i < 9; i++) {
    if (i) blob += ",";
    blob += "{\"name\":\"c" + std::to_string(i) + "\",\"kind\":\"mcp\",\"en\":1}";
  }
  blob += "]";
  std::vector<ConnectorInfo> out;
  int total = -1;
  int n = parseConnectorsJson(blob.c_str(), out, 24, &total);
  TEST_ASSERT_EQUAL(9, n);
  TEST_ASSERT_EQUAL(9, (int)out.size());
  TEST_ASSERT_EQUAL(9, total);
  TEST_ASSERT_EQUAL_STRING("c8", out[8].name.c_str());  // the 9th, once dropped
}

// Over the cap: the parser writes maxN and reports the true total so the caller
// can see (and LOUD-LOG) that a drop happened - never a silent truncation.
static void test_parse_connectors_caps_and_reports_drop() {
  std::string blob = "[";
  for (int i = 0; i < 30; i++) {
    if (i) blob += ",";
    blob += "{\"name\":\"c" + std::to_string(i) + "\"}";
  }
  blob += "]";
  std::vector<ConnectorInfo> out;
  int total = 0;
  int n = parseConnectorsJson(blob.c_str(), out, 24, &total);
  TEST_ASSERT_EQUAL(24, n);
  TEST_ASSERT_EQUAL(30, total);       // the drop is observable: total > n
  TEST_ASSERT_TRUE(total > n);
}

static void test_parse_connectors_skips_nameless_but_counts_it() {
  const char* blob = "[{\"name\":\"a\"},{\"kind\":\"mcp\"},{\"name\":\"b\"}]";
  std::vector<ConnectorInfo> out;
  int total = 0;
  int n = parseConnectorsJson(blob, out, 24, &total);
  TEST_ASSERT_EQUAL(2, n);            // the nameless middle entry is not written
  TEST_ASSERT_EQUAL(3, total);        // but it IS counted (array had 3)
  TEST_ASSERT_EQUAL_STRING("a", out[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("b", out[1].name.c_str());
}

static void test_parse_connectors_defaults_and_flags() {
  const char* blob =
      "[{\"name\":\"srv\",\"url\":\"https://x/mcp\",\"tok\":\"abcd1234\",\"en\":1,\"dev\":1,\"appr\":1},"
      "{\"name\":\"o\",\"oauth\":{\"rurl\":\"https://t\"}}]";
  std::vector<ConnectorInfo> out;
  int n = parseConnectorsJson(blob, out, 24, nullptr);
  TEST_ASSERT_EQUAL(2, n);
  // defaults: prov -> "any", kind -> "mcp", type -> name
  TEST_ASSERT_EQUAL_STRING("any", out[0].prov.c_str());
  TEST_ASSERT_EQUAL_STRING("mcp", out[0].kind.c_str());
  TEST_ASSERT_EQUAL_STRING("srv", out[0].type.c_str());
  TEST_ASSERT_TRUE(out[0].enabled);
  TEST_ASSERT_TRUE(out[0].deviceDialed);   // "dev":1
  TEST_ASSERT_TRUE(out[0].approved);       // "appr":1
  TEST_ASSERT_TRUE(out[0].hasToken);       // presence only - no secret carried
  TEST_ASSERT_FALSE(out[0].hasOauth);
  // secret VALUE is never copied into the portable struct
  TEST_ASSERT_FALSE(out[1].hasToken);
  TEST_ASSERT_TRUE(out[1].hasOauth);
  TEST_ASSERT_FALSE(out[1].deviceDialed);
  TEST_ASSERT_FALSE(out[1].approved);      // fail-closed: absent -> not approved
}

static void test_parse_connectors_malformed_and_empty() {
  std::vector<ConnectorInfo> out;
  int total = 5;
  TEST_ASSERT_EQUAL(0, parseConnectorsJson("{not json", out, 24, &total));
  TEST_ASSERT_EQUAL(0, total);
  TEST_ASSERT_EQUAL(0, parseConnectorsJson("", out, 24, &total));
  TEST_ASSERT_EQUAL(0, parseConnectorsJson("{\"name\":\"x\"}", out, 24, &total));  // object, not array
  TEST_ASSERT_EQUAL(0, total);
  TEST_ASSERT_EQUAL(0, parseConnectorsJson("[{\"name\":\"a\"}]", out, 0, nullptr));  // maxN 0
}

// --- capability scope (CUM-159) ----------------------------------------------
static ProviderState psAllKeyed(const char* host) {
  ProviderState ps;
  ps.openaiKeyed = ps.anthropicKeyed = ps.mistralKeyed = true;
  ps.currentHost = host;
  return ps;
}

static void test_capscope_slugs_are_frozen() {
  TEST_ASSERT_EQUAL_STRING("orchestrator-direct", capScopeSlug(CapScope::OrchestratorDirect));
  TEST_ASSERT_EQUAL_STRING("subsessions-only", capScopeSlug(CapScope::SubsessionsOnly));
  TEST_ASSERT_EQUAL_STRING("unavailable", capScopeSlug(CapScope::Unavailable));
}

static void test_capscope_on_host_is_orchestrator_direct() {
  ProviderState ps = psAllKeyed("openai");
  ConnectorInfo c = mk("github", "openai", "mcp", "https://x/", "");
  TEST_ASSERT_EQUAL(int(CapScope::OrchestratorDirect), int(connectorScope(c, ps)));
}

static void test_capscope_off_host_is_subsessions_only() {
  // Keyed + enabled but on a provider that is NOT the current head: the head can
  // only reach it by spawning a sub-agent on that provider.
  ProviderState ps = psAllKeyed("openai");
  ConnectorInfo c = mk("notion", "anthropic", "mcp", "https://x/", "");
  TEST_ASSERT_EQUAL(int(CapScope::SubsessionsOnly), int(connectorScope(c, ps)));
}

static void test_capscope_unkeyed_provider_is_unavailable() {
  ProviderState ps;                 // only openai keyed
  ps.openaiKeyed = true;
  ps.currentHost = "openai";
  ConnectorInfo c = mk("notion", "anthropic", "mcp");
  TEST_ASSERT_EQUAL(int(CapScope::Unavailable), int(connectorScope(c, ps)));
}

static void test_capscope_disabled_is_unavailable() {
  ProviderState ps = psAllKeyed("openai");
  ConnectorInfo c = mk("github", "openai", "mcp", "", "", /*en=*/false);
  TEST_ASSERT_EQUAL(int(CapScope::Unavailable), int(connectorScope(c, ps)));
}

static void test_capscope_auth_failed_is_unavailable() {
  ProviderState ps = psAllKeyed("openai");
  ConnectorInfo c = mk("gmail", "openai", "connector", "", "connector_gmail");
  c.auth = 0;                       // last OAuth sign-in FAILED
  TEST_ASSERT_EQUAL(int(CapScope::Unavailable), int(connectorScope(c, ps)));
}

static void test_capscope_any_provider_rides_the_host() {
  ProviderState ps = psAllKeyed("mistral");
  ConnectorInfo c = mk("web_search", "any", "builtin");
  TEST_ASSERT_EQUAL(int(CapScope::OrchestratorDirect), int(connectorScope(c, ps)));
}

// CUM-242 x1 item 2: a Cumulo-routed session (builtinsOnly=true) advertises the
// provider BUILT-IN tools only; a direct-key session (false) advertises built-ins
// PLUS the account's private connectors. The advertised sets must provably differ,
// so the orchestrator can distinguish cumulo/<model> from direct <model>.
static void test_cumulo_route_advertises_builtins_only_openai() {
  std::vector<ConnectorInfo> cs = {
      mk("code_interpreter", "openai", "builtin"),
      mk("github", "openai", "mcp", "https://api.githubcopilot.com/mcp/"),
  };
  auto bearer = fixedBearer("github", "ghp_secret");
  // Direct: built-in + private MCP = 2 tools.
  JsonDocument direct;
  attachOpenAIWire(direct, cs, bearer, /*builtinsOnly=*/false);
  TEST_ASSERT_EQUAL(2, direct["tools"].as<JsonArrayConst>().size());
  // Cumulo route: only the code_interpreter built-in survives.
  JsonDocument routed;
  attachOpenAIWire(routed, cs, bearer, /*builtinsOnly=*/true);
  TEST_ASSERT_EQUAL(1, routed["tools"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("code_interpreter", routed["tools"][0]["type"]);
  // The sets provably DIFFER.
  TEST_ASSERT_NOT_EQUAL(direct["tools"].as<JsonArrayConst>().size(),
                        routed["tools"].as<JsonArrayConst>().size());
}

static void test_cumulo_route_advertises_builtins_only_mistral() {
  std::vector<ConnectorInfo> cs = {
      mk("web_search", "mistral", "builtin"),
      mk("github", "mistral", "connector", "", "github_app"),
  };
  JsonDocument direct;
  attachMistralWire(direct, cs, /*builtinsOnly=*/false);
  TEST_ASSERT_EQUAL(2, direct["tools"].as<JsonArrayConst>().size());
  JsonDocument routed;
  attachMistralWire(routed, cs, /*builtinsOnly=*/true);
  TEST_ASSERT_EQUAL(1, routed["tools"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL_STRING("web_search", routed["tools"][0]["type"]);
}

static void test_cumulo_route_anthropic_drops_private_mcp() {
  std::vector<ConnectorInfo> cs = {
      mk("github", "anthropic", "mcp", "https://api.githubcopilot.com/mcp/"),
  };
  auto bearer = fixedBearer("github", "ghp_secret");
  JsonDocument direct;
  attachAnthropicWire(direct, cs, bearer, /*builtinsOnly=*/false);
  TEST_ASSERT_EQUAL(1, direct["mcp_servers"].as<JsonArrayConst>().size());
  // Anthropic has no built-in kind, so a cumulo route attaches nothing at all.
  JsonDocument routed;
  attachAnthropicWire(routed, cs, bearer, /*builtinsOnly=*/true);
  TEST_ASSERT_FALSE(routed["mcp_servers"].is<JsonArrayConst>());
}

// The shared device->Mistral-workspace id map (used by both the wire attach and the
// connector-verify probe, so they can never drift). Owner-set ids win unless they are
// an OpenAI-namespace default; those map through the canonical table by type/name.
static void test_mistral_connector_id_shared_map() {
  auto mk = [](const char* type, const char* name, const char* cid) {
    ConnectorInfo c; c.type = type; c.name = name; c.connectorId = cid; c.prov = "mistral";
    c.kind = "connector"; return c;
  };
  // Canonical remaps when the cid is blank or an OpenAI "connector_*" default.
  TEST_ASSERT_EQUAL_STRING("google_calendar", mistralConnectorId(mk("gcal", "gcal", "connector_googlecalendar")).c_str());
  TEST_ASSERT_EQUAL_STRING("google_calendar", mistralConnectorId(mk("gcal", "gcal", "")).c_str());
  TEST_ASSERT_EQUAL_STRING("github_app",      mistralConnectorId(mk("github", "github", "")).c_str());
  TEST_ASSERT_EQUAL_STRING("google_drive_mcp", mistralConnectorId(mk("gdrive", "gdrive", "")).c_str());
  // A workspace-native name passes through; an owner-set explicit id wins.
  TEST_ASSERT_EQUAL_STRING("notion",  mistralConnectorId(mk("notion", "notion", "notion")).c_str());
  TEST_ASSERT_EQUAL_STRING("my-uuid", mistralConnectorId(mk("gcal", "gcal", "my-uuid")).c_str());
}

// ---- Mistral workspace probe: listed + active => usable ----------------------
// Measured live (2026-09-27): GET /v1/connectors listed google_calendar with
// is_authenticated:false, active:true - and the SAME key's Conversations call ran
// google_calendar_list_events server-side and returned the owner's real events. So
// the workspace LISTING (active) is the usability signal and is_authenticated is a
// hint only. kLiveListing mirrors that live response: the 18 items in the order
// Mistral returned them, with name/active/is_authenticated as observed (ids are
// placeholders; the ~300 KB of per-item metadata is dropped).
static const char* kLiveListing =
    "{\"items\":["
    "{\"id\":\"0198e70f-0000-7000-8000-000000000001\",\"name\":\"atlassian\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"0198f11d-0000-7000-8000-000000000002\",\"name\":\"notion\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"0198f11d-0000-7000-8000-000000000003\",\"name\":\"box\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"0198f11e-0000-7000-8000-000000000004\",\"name\":\"stripe\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"0198f11e-0000-7000-8000-000000000005\",\"name\":\"linear\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019cfb41-0000-7000-8000-000000000006\",\"name\":\"slack\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019d8b52-0000-7000-8000-000000000007\",\"name\":\"github_app\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019dafec-0000-7000-8000-000000000008\",\"name\":\"outlook_calendar\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019df75b-0000-7000-8000-000000000009\",\"name\":\"gmail\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019df75b-0000-7000-8000-000000000010\",\"name\":\"outlook\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019df75b-0000-7000-8000-000000000011\",\"name\":\"google_calendar\",\"is_authenticated\":false,\"active\":true,"
    "\"description\":\"cal\",\"tools\":[],\"connection_config\":{\"type\":\"mcp\"}},"
    "{\"id\":\"019e641f-0000-7000-8000-000000000012\",\"name\":\"document_library\",\"is_authenticated\":true,\"active\":true},"
    "{\"id\":\"019e693b-0000-7000-8000-000000000013\",\"name\":\"slack_http\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019e721b-0000-7000-8000-000000000014\",\"name\":\"github_http\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019eab87-0000-7000-8000-000000000015\",\"name\":\"sharepoint_mcp\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019ed053-0000-7000-8000-000000000016\",\"name\":\"bigquery_\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"019f4b9c-0000-7000-8000-000000000017\",\"name\":\"google_drive_mcp\",\"is_authenticated\":false,\"active\":true},"
    "{\"id\":\"01a0d3b2-0000-7000-8000-000000000018\",\"name\":\"gitlab_http\",\"is_authenticated\":false,\"active\":true}"
    "],\"pagination\":{\"next_cursor\":null,\"page_size\":100}}";

using nimbus::orch::MistralWorkspace;
using W = ConnectorInfo::Workspace;

static MistralWorkspace probed(const char* body) {
  MistralWorkspace ws;
  TEST_ASSERT_TRUE(noteMistralWorkspaceProbe(ws, body));
  return ws;
}

// A Studio connector card as the web UI saves it (the OpenAI-namespace default cid
// for gcal/gdrive, remapped by mistralConnectorId).
static ConnectorInfo studio(const char* type, const char* cid = "") {
  return mk(type, "mistral", "connector", "", cid);
}

static void test_workspace_listed_is_usable_even_when_not_signed_in() {
  MistralWorkspace ws = probed(kLiveListing);
  ConnectorInfo gcal = studio("gcal", "connector_googlecalendar");
  // The live case: listed + active + is_authenticated:false => USABLE (auth 1).
  TEST_ASSERT_EQUAL(1, connectorAuthFor(gcal, ws, -1));
  TEST_ASSERT_EQUAL((int)W::ListedNotSignedIn, (int)mistralWorkspaceState(gcal, ws));
  for (const char* t : {"notion", "slack", "gmail", "github", "gdrive", "linear"})
    TEST_ASSERT_EQUAL_MESSAGE(1, connectorAuthFor(studio(t), ws, -1), t);
  // The one item reporting is_authenticated:true reads SignedIn (same auth).
  ConnectorInfo lib = studio("document_library");
  TEST_ASSERT_EQUAL((int)W::ListedSignedIn, (int)mistralWorkspaceState(lib, ws));
  TEST_ASSERT_EQUAL(1, connectorAuthFor(lib, ws, -1));
}

// Class guard for the old fixed 16-slot table: EVERY item of the live 18-item
// listing must read listed (google_drive_mcp is item 17), by name AND by id.
static void test_workspace_keeps_every_listed_item() {
  MistralWorkspace ws = probed(kLiveListing);
  std::vector<std::string> ids;
  TEST_ASSERT_TRUE(parseMistralWorkspaceConnectors(kLiveListing, ids));
  TEST_ASSERT_EQUAL(36, (int)ids.size());   // 18 names + 18 ids
  for (const std::string& id : ids) {
    ConnectorInfo c = studio("custom", id.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, connectorAuthFor(c, ws, -1), id.c_str());
  }
  TEST_ASSERT_EQUAL(1, connectorAuthFor(studio("gdrive", "connector_googledrive"), ws, -1));
}

// Unlisted or explicitly inactive => refused (auth 2, "enable it in Mistral").
static void test_workspace_unlisted_or_inactive_is_refused() {
  MistralWorkspace ws = probed(
      "{\"items\":[{\"name\":\"slack\",\"is_authenticated\":true,\"active\":false},"
      "{\"name\":\"gmail\",\"is_authenticated\":false}]}");   // active absent => active
  ConnectorInfo slack = studio("slack");
  ConnectorInfo notion = studio("notion");
  TEST_ASSERT_EQUAL(2, connectorAuthFor(slack, ws, -1));   // inactive, even though signed in
  TEST_ASSERT_EQUAL((int)W::NotListed, (int)mistralWorkspaceState(slack, ws));
  TEST_ASSERT_EQUAL(2, connectorAuthFor(notion, ws, -1));  // not in the listing
  TEST_ASSERT_EQUAL(1, connectorAuthFor(studio("gmail"), ws, -1));
  std::vector<ConnectorInfo> cs = {slack, notion};
  nimbus::orch::applyConnectorAuth(cs, ws);
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  const std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("slack (not usable until the owner enables it in their Mistral "
                          "Studio account)") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("notion (not usable until the owner enables it") != std::string::npos);
  for (const ConnectorInfo& c : cs)
    TEST_ASSERT_EQUAL((int)CapScope::Unavailable, (int)connectorScope(c, ps));
}

// Fail-closed: before any workspace answer a Studio connector is NOT usable, and the
// catalog says why (not checked) instead of sending the owner to Mistral for nothing.
static void test_workspace_unprobed_is_fail_closed() {
  MistralWorkspace ws;   // never probed
  std::vector<ConnectorInfo> cs = {studio("gcal", "connector_googlecalendar")};
  nimbus::orch::applyConnectorAuth(cs, ws);
  TEST_ASSERT_EQUAL(2, cs[0].auth);
  TEST_ASSERT_EQUAL((int)W::Unprobed, (int)cs[0].workspace);
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  const std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("gcal (not usable yet: the Mistral workspace has not been checked")
                   != std::string::npos);
  TEST_ASSERT_EQUAL((int)CapScope::Unavailable, (int)connectorScope(cs[0], ps));
}

// A non-parseable / wrong-shape body is "no signal", never "nothing listed": the
// parse returns false and the last good answer survives (a transient HTTP error must
// not silently strip a working connector). An EMPTY listing is a real answer.
static void test_workspace_bad_body_is_no_signal() {
  const char* bad[] = {"not json at all", "{\"error\":\"unauthorized\"}",
                       "{\"detail\":\"Unauthorized\"}", "{\"items\":{}}", ""};
  for (const char* b : bad) {
    std::vector<std::string> ids;
    TEST_ASSERT_FALSE_MESSAGE(parseMistralWorkspaceConnectors(b, ids), b);
    TEST_ASSERT_EQUAL(0, (int)ids.size());
  }
  std::vector<std::string> none;
  TEST_ASSERT_FALSE(parseMistralWorkspaceConnectors(nullptr, none));
  MistralWorkspace ws = probed(kLiveListing);
  ConnectorInfo gcal = studio("gcal");
  for (const char* b : bad) TEST_ASSERT_FALSE(noteMistralWorkspaceProbe(ws, b));
  TEST_ASSERT_EQUAL(1, connectorAuthFor(gcal, ws, -1));   // last good answer kept
  TEST_ASSERT_TRUE(noteMistralWorkspaceProbe(ws, "{\"items\":[]}"));
  TEST_ASSERT_TRUE(ws.probed);
  TEST_ASSERT_EQUAL(2, connectorAuthFor(gcal, ws, -1));   // empty workspace: nothing usable
}

// Each probe is a FULL replace: a connector removed in Mistral stops being usable.
static void test_workspace_probe_is_full_replace() {
  MistralWorkspace ws = probed("{\"items\":[{\"name\":\"google_calendar\",\"is_authenticated\":true}]}");
  ConnectorInfo gcal = studio("gcal");
  TEST_ASSERT_EQUAL((int)W::ListedSignedIn, (int)mistralWorkspaceState(gcal, ws));
  TEST_ASSERT_TRUE(noteMistralWorkspaceProbe(ws, "{\"items\":[{\"name\":\"notion\"}]}"));
  TEST_ASSERT_EQUAL((int)W::NotListed, (int)mistralWorkspaceState(gcal, ws));
  TEST_ASSERT_EQUAL(1, connectorAuthFor(studio("notion"), ws, -1));
}

// The Conversations API accepts a uuid as connector_id, so an owner-set uuid that
// the workspace lists is usable; an id containing a newline can never match.
static void test_workspace_matches_uuid_and_rejects_newline_ids() {
  MistralWorkspace ws = probed(kLiveListing);
  TEST_ASSERT_EQUAL(1, connectorAuthFor(studio("notion", "0198f11d-0000-7000-8000-000000000002"), ws, -1));
  TEST_ASSERT_EQUAL(2, connectorAuthFor(studio("notion", "0198f11d-ffff-7000-8000-000000000002"), ws, -1));
  TEST_ASSERT_FALSE(ws.isListed("notion\nslack"));
  TEST_ASSERT_FALSE(ws.isListed(""));
}

// THE CLASS RULE, over every (listed?, active, is_authenticated) combination:
// usable <=> listed and not explicitly inactive; is_authenticated NEVER changes
// auth or capability scope, only the hint.
static void test_workspace_signed_in_never_gates_class() {
  const char* actives[] = {"", ",\"active\":true", ",\"active\":false"};
  const char* signs[] = {"", ",\"is_authenticated\":true", ",\"is_authenticated\":false"};
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "openai"; ps.openaiKeyed = true;
  int combos = 0;
  for (int listed = 0; listed < 2; listed++)
    for (const char* a : actives)
      for (const char* s : signs) {
        std::string body = "{\"items\":[{\"name\":\"";
        body += listed ? "google_calendar" : "outlook";
        body += "\"" + std::string(a) + s + "}]}";
        MistralWorkspace ws = probed(body.c_str());
        std::vector<ConnectorInfo> cs = {studio("gcal")};
        nimbus::orch::applyConnectorAuth(cs, ws);
        const bool usable = listed && std::strcmp(a, ",\"active\":false") != 0;
        TEST_ASSERT_EQUAL_MESSAGE(usable ? 1 : 2, cs[0].auth, body.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(usable ? (int)CapScope::SubsessionsOnly : (int)CapScope::Unavailable,
                                  (int)connectorScope(cs[0], ps), body.c_str());
        if (usable) {
          const bool signedIn = std::strcmp(s, ",\"is_authenticated\":true") == 0;
          TEST_ASSERT_EQUAL_MESSAGE(signedIn ? (int)W::ListedSignedIn : (int)W::ListedNotSignedIn,
                                    (int)cs[0].workspace, body.c_str());
        }
        combos++;
      }
  TEST_ASSERT_EQUAL(18, combos);
}

// The hint: usable + not signed in => the catalog cautions that results may come
// back empty, but never calls it unusable; signed in => a bare, healthy name.
static void test_catalog_hint_when_listed_but_not_signed_in() {
  MistralWorkspace ws = probed(
      "{\"items\":[{\"name\":\"google_calendar\",\"is_authenticated\":false,\"active\":true},"
      "{\"name\":\"notion\",\"is_authenticated\":true,\"active\":true}]}");
  std::vector<ConnectorInfo> cs = {studio("gcal", "connector_googlecalendar"), studio("notion")};
  nimbus::orch::applyConnectorAuth(cs, ws);
  ProviderState ps; ps.mistralKeyed = true; ps.currentHost = "mistral";
  const std::string t = catalogText(cs, ps);
  TEST_ASSERT_TRUE(t.find("gcal (Mistral reports it not signed in: results may come back "
                          "empty until the owner connects it in Mistral") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("gcal (not usable") == std::string::npos);
  TEST_ASSERT_TRUE(t.find("notion\n") != std::string::npos || t.find("notion,") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("notion (") == std::string::npos);
  for (const ConnectorInfo& c : cs)
    TEST_ASSERT_EQUAL((int)CapScope::OrchestratorDirect, (int)connectorScope(c, ps));
}

// The LIVE body nests 26 levels deep inside fields the probe does not read (tool
// input schemas), and a filtered ArduinoJson parse still descends into them: at its
// default limit it answered TooDeep, so a real probe NEVER landed an answer (caught
// by the first live hosted e2e, 2026-09-27). The class: any depth, any junk inside
// ignored fields, brackets/quotes inside strings, nested keys that reuse our field
// names - only an item's own top-level id/name/active/is_authenticated count.
static std::string deepValue(int depth) {
  std::string v;
  for (int i = 0; i < depth; i++) v += (i % 2) ? "[" : "{\"k\":";
  v += "\"leaf \\\" }] {[ \"";
  for (int i = depth - 1; i >= 0; i--) v += (i % 2) ? "]" : "}";
  return v;
}

static void test_workspace_parses_any_depth_and_ignores_nested_names() {
  std::string body = "{\"pagination\":{\"next_cursor\":null,\"deep\":" + deepValue(60) +
                     "},\"items\":[";
  const char* names[] = {"notion", "google_calendar", "document_library", "slack"};
  for (int i = 0; i < 4; i++) {
    if (i) body += ",";
    body += "{\"locale\":{\"name\":{\"en\":\"Display " + std::to_string(i) + "\"}},";
    body += "\"tools\":[{\"name\":\"tool_x\",\"input_schema\":" + deepValue(40) + "}],";
    body += "\"description\":\"has \\\"quotes\\\" and ]} brackets\",";
    body += std::string("\"name\":\"") + names[i] + "\",\"id\":\"uuid-" + std::to_string(i) + "\",";
    body += std::string("\"is_authenticated\":") + (i == 2 ? "true" : "false") + ",\"active\":true}";
  }
  body += "]}";
  std::vector<std::string> ids, signedIn;
  TEST_ASSERT_TRUE(parseMistralWorkspaceConnectors(body.c_str(), ids, &signedIn));
  TEST_ASSERT_EQUAL(8, (int)ids.size());          // 4 names + 4 ids, nothing nested
  TEST_ASSERT_EQUAL(2, (int)signedIn.size());     // document_library name + id
  auto has = [](const std::vector<std::string>& v, const char* n) {
    for (const auto& x : v) if (x == n) return true;
    return false;
  };
  TEST_ASSERT_TRUE(has(ids, "google_calendar"));
  TEST_ASSERT_TRUE(has(ids, "uuid-3"));
  TEST_ASSERT_FALSE(has(ids, "tool_x"));          // a nested "name" is not an item name
  TEST_ASSERT_FALSE(has(ids, "Display 0"));
  TEST_ASSERT_TRUE(has(signedIn, "document_library"));
  MistralWorkspace ws = probed(body.c_str());
  TEST_ASSERT_EQUAL(1, connectorAuthFor(studio("gcal"), ws, -1));
}

// Structure: a truncated or wrong-shape body is no signal; key order and a
// non-bool active do not matter; an absent items[] is no signal.
static void test_workspace_scanner_structure() {
  std::string full = kLiveListing;
  std::vector<std::string> ids;
  for (size_t cut : {full.size() / 3, full.size() / 2, full.size() - 3}) {
    ids.clear();
    TEST_ASSERT_FALSE(parseMistralWorkspaceConnectors(full.substr(0, cut).c_str(), ids));
    TEST_ASSERT_EQUAL(0, (int)ids.size());        // nothing leaks from a half parse
  }
  const char* noSignal[] = {"[]", "{\"pagination\":{}}", "{\"items\":\"x\"}",
                            "{\"items\":[{\"name\":\"a\" \"id\":\"b\"}]}", "   "};
  for (const char* b : noSignal) {
    ids.clear();
    TEST_ASSERT_FALSE_MESSAGE(parseMistralWorkspaceConnectors(b, ids), b);
  }
  ids.clear();
  TEST_ASSERT_TRUE(parseMistralWorkspaceConnectors(
      " {\n \"pagination\" : {\"page_size\":100} ,\"items\" : [ 7, null,"
      "{\"active\":null,\"name\":\"a\"},{\"active\":\"false\",\"name\":\"b\"},"
      "{\"active\":false,\"name\":\"c\"}] } ", ids));
  TEST_ASSERT_EQUAL(2, (int)ids.size());          // a, b (non-bool active = absent); c inactive
  TEST_ASSERT_EQUAL_STRING("a", ids[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b", ids[1].c_str());
}

// The catalog's "YOU are here" must be the head the turn engine dispatches to. The
// class: pin > first KEYED priority token (any slug, router ones included) > router
// fallback > bare first token. A BYOK-only keyed check named mistral on a
// cumulo-first device that really runs on cumulo; the raw first token named an
// unkeyed openai on a mistral-only one.
static void test_resolve_head_host_matches_the_engine_rule() {
  using nimbus::orch::resolveHeadHost;
  auto keyedSet = [](std::vector<std::string> ks) {
    return [ks](const std::string& h) {
      for (const auto& k : ks) if (k == h) return true;
      return false;
    };
  };
  TEST_ASSERT_EQUAL_STRING("cumulo",
      resolveHeadHost("", "cumulo,mistral", keyedSet({"cumulo", "mistral"}), "cumulo").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral",
      resolveHeadHost("", "openai,anthropic,mistral,cumulo", keyedSet({"mistral"}), "").c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic",
      resolveHeadHost(" anthropic ", "openai,mistral", keyedSet({"openai"}), "").c_str());
  TEST_ASSERT_EQUAL_STRING("zai",
      resolveHeadHost("", "openai, mistral", keyedSet({}), "zai").c_str());
  TEST_ASSERT_EQUAL_STRING("openai",
      resolveHeadHost("", " openai , mistral", keyedSet({}), "").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral",
      resolveHeadHost("", "openai,,  mistral ", keyedSet({"mistral"}), "").c_str());
  TEST_ASSERT_EQUAL_STRING("", resolveHeadHost("", "", keyedSet({}), "").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral", resolveHeadHost("", "mistral", nullptr, "").c_str());
}

// A full page (page_size 100) is never truncated; a runaway body is bounded.
static void test_workspace_item_cap() {
  auto body = [](int n) {
    std::string b = "{\"items\":[";
    for (int i = 0; i < n; i++) b += (i ? ",{\"name\":\"c" : "{\"name\":\"c") + std::to_string(i) + "\"}";
    return b + "]}";
  };
  std::vector<std::string> ids;
  TEST_ASSERT_TRUE(parseMistralWorkspaceConnectors(body(100).c_str(), ids));
  TEST_ASSERT_EQUAL(100, (int)ids.size());
  ids.clear();
  TEST_ASSERT_TRUE(parseMistralWorkspaceConnectors(body(200).c_str(), ids));
  TEST_ASSERT_EQUAL(nimbus::orch::kMistralWorkspaceMaxItems, (int)ids.size());
  MistralWorkspace ws = probed(body(100).c_str());
  TEST_ASSERT_TRUE(ws.isListed("c99"));
}

// The shared credential rule every surface (device + hosted) reads.
static void test_connector_auth_rule_table() {
  MistralWorkspace ws = probed(kLiveListing);
  ConnectorInfo bi = mk("web_search", "mistral", "builtin");
  ConnectorInfo tok = mk("github", "openai", "mcp", "https://api.githubcopilot.com/mcp/");
  tok.hasToken = true;
  ConnectorInfo oa = mk("gmail", "openai", "connector", "", "connector_gmail");
  oa.hasOauth = true;
  ConnectorInfo bare = mk("linear", "openai", "mcp", "https://mcp.linear.app/mcp");
  ConnectorInfo anyConn = mk("gcal", "any", "connector");
  TEST_ASSERT_EQUAL(-1, connectorAuthFor(bi, ws, -1));
  TEST_ASSERT_EQUAL(1, connectorAuthFor(tok, ws, -1));
  TEST_ASSERT_EQUAL(0, connectorAuthFor(oa, ws, 0));
  TEST_ASSERT_EQUAL(1, connectorAuthFor(oa, ws, 1));
  TEST_ASSERT_EQUAL(-1, connectorAuthFor(oa, ws, -1));
  TEST_ASSERT_EQUAL(2, connectorAuthFor(bare, ws, -1));
  TEST_ASSERT_EQUAL(2, connectorAuthFor(anyConn, ws, -1));   // only prov=mistral reads the workspace
  std::vector<ConnectorInfo> cs = {bi, tok, oa, bare, studio("gcal")};
  nimbus::orch::applyConnectorAuth(cs, ws, [](const ConnectorInfo&) { return (int8_t)0; });
  TEST_ASSERT_EQUAL(0, cs[2].auth);   // the OAuth callback feeds the rule
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL((int)W::NotApplicable, (int)cs[i].workspace);
  TEST_ASSERT_EQUAL((int)W::ListedNotSignedIn, (int)cs[4].workspace);
}

static void test_wants_probe_only_for_enabled_studio_connectors() {
  using nimbus::orch::wantsMistralWorkspaceProbe;
  std::vector<ConnectorInfo> none = {mk("web_search", "mistral", "builtin"),
                                     mk("slack", "mistral", "mcp", "https://x.example/mcp"),
                                     mk("gcal", "openai", "connector"),
                                     mk("gcal", "mistral", "connector", "", "", false)};
  TEST_ASSERT_FALSE(wantsMistralWorkspaceProbe(none));
  none.push_back(studio("notion"));
  TEST_ASSERT_TRUE(wantsMistralWorkspaceProbe(none));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mistral_connector_id_shared_map);
  RUN_TEST(test_workspace_listed_is_usable_even_when_not_signed_in);
  RUN_TEST(test_workspace_keeps_every_listed_item);
  RUN_TEST(test_workspace_unlisted_or_inactive_is_refused);
  RUN_TEST(test_workspace_unprobed_is_fail_closed);
  RUN_TEST(test_workspace_bad_body_is_no_signal);
  RUN_TEST(test_workspace_probe_is_full_replace);
  RUN_TEST(test_workspace_matches_uuid_and_rejects_newline_ids);
  RUN_TEST(test_workspace_signed_in_never_gates_class);
  RUN_TEST(test_catalog_hint_when_listed_but_not_signed_in);
  RUN_TEST(test_workspace_parses_any_depth_and_ignores_nested_names);
  RUN_TEST(test_workspace_scanner_structure);
  RUN_TEST(test_workspace_item_cap);
  RUN_TEST(test_resolve_head_host_matches_the_engine_rule);
  RUN_TEST(test_connector_auth_rule_table);
  RUN_TEST(test_wants_probe_only_for_enabled_studio_connectors);
  RUN_TEST(test_parse_connectors_reads_past_eight);
  RUN_TEST(test_parse_connectors_caps_and_reports_drop);
  RUN_TEST(test_parse_connectors_skips_nameless_but_counts_it);
  RUN_TEST(test_parse_connectors_defaults_and_flags);
  RUN_TEST(test_parse_connectors_malformed_and_empty);
  RUN_TEST(test_catalog_full_golden);
  RUN_TEST(test_mistral_60s_cap_is_disclosed_and_routes_elsewhere);
  RUN_TEST(test_pdf_recipe_is_stated_and_routed_to_openai);
  RUN_TEST(test_pdf_recipe_requires_an_openai_reachable_code_interpreter);
  RUN_TEST(test_openai_first_party_connector_id);
  RUN_TEST(test_openai_first_party_without_bearer_is_skipped);
  RUN_TEST(test_mistral_ids_map_to_the_mistral_namespace);
  RUN_TEST(test_mistral_document_library_bare_is_skipped);
  RUN_TEST(test_openai_remote_mcp_with_bearer);
  RUN_TEST(test_openai_prov_filtering);
  RUN_TEST(test_cum255_device_dialed_lan_not_forwarded_to_head);
  RUN_TEST(test_cum255_connector_kind_empty_cid_lan_blocked);
  RUN_TEST(test_url_routable_predicate);
  RUN_TEST(test_prov_routing_table_is_fail_closed);
  RUN_TEST(test_connector_config_error);
  RUN_TEST(test_mistral_builtin_and_studio_connector);
  RUN_TEST(test_anthropic_mcp_servers_with_token);
  RUN_TEST(test_anthropic_no_tools_key_polluted);
  RUN_TEST(test_catalog_marks_current_provider);
  RUN_TEST(test_catalog_no_connectors);
  RUN_TEST(test_catalog_mistral_studio_points_at_studio_not_device);
  RUN_TEST(test_known_catalog_json_shape);
  RUN_TEST(test_catalog_surfaces_connector_caps);
  RUN_TEST(test_github_caps_state_repo_creation);
  RUN_TEST(test_catalog_names_unconfigured_connectors);
  RUN_TEST(test_catalog_marks_verified_state);
  RUN_TEST(test_catalog_validation_off_makes_no_claim);
  RUN_TEST(test_openai_code_interpreter_builtin_attaches);
  RUN_TEST(test_code_sandbox_availability_key_gated);
  RUN_TEST(test_catalog_marks_connector_auth_problems);
  RUN_TEST(test_catalog_custom_mcp_gets_a_line);
  RUN_TEST(test_subagent_capabilities_block_is_generated);
  RUN_TEST(test_capscope_slugs_are_frozen);
  RUN_TEST(test_capscope_on_host_is_orchestrator_direct);
  RUN_TEST(test_capscope_off_host_is_subsessions_only);
  RUN_TEST(test_capscope_unkeyed_provider_is_unavailable);
  RUN_TEST(test_capscope_disabled_is_unavailable);
  RUN_TEST(test_capscope_auth_failed_is_unavailable);
  RUN_TEST(test_capscope_any_provider_rides_the_host);
  RUN_TEST(test_cumulo_route_advertises_builtins_only_openai);
  RUN_TEST(test_cumulo_route_advertises_builtins_only_mistral);
  RUN_TEST(test_cumulo_route_anthropic_drops_private_mcp);
  return UNITY_END();
}
