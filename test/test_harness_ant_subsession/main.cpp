#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"

// Anthropic Managed Agents sub-session chain - CUM-481 + CUM-474.
//
// CUM-481: on a unit whose NVS was full, every spawn re-created an environment
// and an agent, then sent the session with "environment_id":"" and got HTTP 400
// ("environment_id: value is required"), logged as a bare status. The dispatch
// re-read the env id through the cache closure it had just written, and the
// write had been dropped. These cases pin the CLASS: no session request ever
// goes out without both ids, whatever the storage does, and nothing is
// re-created once this boot knows an id.
//
// CUM-474: every call of the spawn chain classifies its failure (429 -> rate
// limited with its window, 4xx -> bad request, 5xx -> provider fault) and logs
// the provider's reason, bounded, with no secret.
//
// The responses are RECORDED from a live run on 2026-10-10 (nimbus-lab sub
// --record, claude-sonnet-5, a research task with one web search), byte-frozen
// under test/support/fixtures/anthropic_managed_agents_2026-10/. Re-record with
// tools/harness-lab `nimbus-lab sub --record=<dir>` when the API moves.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::headerOf;
using harness_test::reqBody;
namespace providers = agent::providers;
using agent::FabricErr;

void setUp() {
  LogCapture::install();
  providers::antForgetIds();   // each case starts from a fresh boot
}
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- recorded fixtures -------------------------------------------------------

static std::string fixture(const char* name) {
  std::ifstream f(std::string("test/support/fixtures/anthropic_managed_agents_2026-10/") + name,
                  std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  TEST_ASSERT_FALSE_MESSAGE(ss.str().empty(), name);
  return ss.str();
}

static std::string jsonId(const std::string& body) {
  JsonDocument d;
  deserializeJson(d, body);
  return std::string(d["id"] | "");
}

struct Recorded {
  std::string env = fixture("environment_create_200.json");
  std::string agent = fixture("agent_create_200.json");
  std::string session = fixture("session_create_200.json");
  std::string send = fixture("session_events_send_200.json");
  std::string envId = jsonId(env), agentId = jsonId(agent), sessionId = jsonId(session);
};

static void scriptCreates(FakeProviderDeps& d, const Recorded& r, bool env, bool agent) {
  if (env) d.http.script.push_back({"api.anthropic.com", "/v1/environments", 200, r.env});
  if (agent) d.http.script.push_back({"api.anthropic.com", "/v1/agents", 200, r.agent});
}
static void scriptSessionAndTurn(FakeProviderDeps& d, const Recorded& r) {
  d.http.script.push_back({"api.anthropic.com", "/v1/sessions", 200, r.session});
  d.http.script.push_back({"api.anthropic.com", "/events", 200, r.send});
}

static agent::Directive directive(const char* model = "claude-sonnet-5") {
  agent::Directive dir;
  dir.category = "research";
  dir.instruction = "find the sources";
  dir.model = model;
  return dir;
}

static FabricErr dispatch(FakeProviderDeps& d, const agent::Directive& dir, char jobId[72]) {
  auto pd = d.contract();
  return providers::antDispatch(pd, "claude-sonnet-5", dir, jobId);
}

static int countPath(const FakeProviderDeps& d, const char* path) {
  int n = 0;
  for (const auto& r : d.http.seen) n += r.path == path ? 1 : 0;
  return n;
}

static int countLog(const char* needle) {
  int n = 0;
  for (const auto& l : LogCapture::lines()) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// ---- the request contract (golden bodies + headers) --------------------------

static const char* kGoldenEnvBody =
    "{\"name\":\"nimbus-env\",\"config\":{\"type\":\"cloud\","
    "\"networking\":{\"type\":\"unrestricted\"}}}";
static const char* kGoldenAgentBody =
    "{\"name\":\"Nimbus Agent (claude-sonnet-5)\",\"model\":\"claude-sonnet-5\","
    "\"system\":\"You are an autonomous assistant agent running on a managed sandbox. "
    "Use your tools (bash, web search/fetch, files) to complete the task fully, then "
    "give the final result concisely.\",\"tools\":[{\"type\":\"agent_toolset_20260401\"}]}";

static void assertManagedAgentsHeaders(const agent::HttpRequest& r) {
  TEST_ASSERT_EQUAL_STRING("POST", r.method.c_str());
  TEST_ASSERT_EQUAL_STRING("api.anthropic.com", r.host.c_str());
  TEST_ASSERT_EQUAL_STRING("sk-fake-ant", headerOf(r, "x-api-key").c_str());
  TEST_ASSERT_EQUAL_STRING("2023-06-01", headerOf(r, "anthropic-version").c_str());
  TEST_ASSERT_EQUAL_STRING("managed-agents-2026-04-01", headerOf(r, "anthropic-beta").c_str());
  TEST_ASSERT_EQUAL_STRING("application/json", headerOf(r, "Content-Type").c_str());
}

// The four requests of a cold spawn, byte for byte, against the
// managed-agents-2026-04-01 contract (verified live 2026-10-10). The env and
// agent bodies are the exact 84 / 303 bytes the bench unit's heaptrace showed.
static void test_cold_dispatch_golden_requests() {
  Recorded r;
  FakeProviderDeps d;
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  auto pd = d.contract();
  pd.attachAnthropic = nullptr;   // no owner connectors, as on the bench unit
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-5", directive(), jobId));
  TEST_ASSERT_EQUAL(4, (int)d.http.seen.size());
  for (const auto& req : d.http.seen) assertManagedAgentsHeaders(req);
  TEST_ASSERT_EQUAL_STRING("/v1/environments", d.http.seen[0].path.c_str());
  TEST_ASSERT_EQUAL_STRING(kGoldenEnvBody, reqBody(d, 0).c_str());
  TEST_ASSERT_EQUAL(84, (int)reqBody(d, 0).size());
  TEST_ASSERT_EQUAL_STRING("/v1/agents", d.http.seen[1].path.c_str());
  TEST_ASSERT_EQUAL_STRING(kGoldenAgentBody, reqBody(d, 1).c_str());
  TEST_ASSERT_EQUAL(303, (int)reqBody(d, 1).size());
  TEST_ASSERT_EQUAL_STRING("/v1/sessions", d.http.seen[2].path.c_str());
  const std::string session =
      "{\"agent\":\"" + r.agentId + "\",\"environment_id\":\"" + r.envId + "\"}";
  TEST_ASSERT_EQUAL_STRING(session.c_str(), reqBody(d, 2).c_str());
  TEST_ASSERT_EQUAL_STRING(("/v1/sessions/" + r.sessionId + "/events").c_str(),
                           d.http.seen[3].path.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "{\"events\":[{\"type\":\"user.message\",\"content\":"
      "[{\"type\":\"text\",\"text\":\"find the sources\"}]}]}",
      reqBody(d, 3).c_str());
  TEST_ASSERT_EQUAL_STRING(("anthropic:" + r.sessionId).c_str(), jobId);
}

// Owner MCP connectors are fitted to the managed-agents shape (live-verified
// 2026-10-10, CUM-519): every attached server gets an mcp_toolset that names it
// with always_allow (the device cannot confirm tool calls), and a server that
// carries a token - which the API refuses inline - is left off and logged
// instead of failing every spawn at agent creation.
static void test_mcp_connectors_fitted_to_managed_agents_shape() {
  Recorded r;
  FakeProviderDeps d;
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  auto pd = d.contract();
  pd.attachAnthropic = [](JsonDocument& doc) {
    JsonArray servers = doc["mcp_servers"].to<JsonArray>();
    JsonObject open = servers.add<JsonObject>();
    open["type"] = "url"; open["url"] = "https://mcp.example.com/a"; open["name"] = "open-mcp";
    JsonObject authed = servers.add<JsonObject>();
    authed["type"] = "url"; authed["url"] = "https://mcp.example.com/b"; authed["name"] = "authed-mcp";
    authed["authorization_token"] = "tok-fake";
  };
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-5", directive(), jobId));
  JsonDocument agent;
  deserializeJson(agent, reqBody(d, 1));
  TEST_ASSERT_EQUAL(1, (int)agent["mcp_servers"].size());
  TEST_ASSERT_EQUAL_STRING("open-mcp", agent["mcp_servers"][0]["name"] | "");
  TEST_ASSERT_TRUE(agent["mcp_servers"][0]["authorization_token"].isNull());
  TEST_ASSERT_EQUAL(2, (int)agent["tools"].size());
  TEST_ASSERT_EQUAL_STRING("agent_toolset_20260401", agent["tools"][0]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("mcp_toolset", agent["tools"][1]["type"] | "");
  TEST_ASSERT_EQUAL_STRING("open-mcp", agent["tools"][1]["mcp_server_name"] | "");
  TEST_ASSERT_EQUAL_STRING("always_allow",
                           agent["tools"][1]["default_config"]["permission_policy"]["type"] | "");
  TEST_ASSERT_TRUE(reqBody(d, 1).find("tok-fake") == std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains(
      "anthropic: connector authed-mcp needs a vault credential - left off the sub-agent"));
  TEST_ASSERT_FALSE(LogCapture::contains("tok-fake"));
}

// Only token-carrying servers: nothing usable is left, so the agent body is the
// plain no-connector body byte for byte (no empty mcp_servers array).
static void test_only_token_connectors_leave_the_plain_agent_body() {
  Recorded r;
  FakeProviderDeps d;
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  auto pd = d.contract();
  pd.attachAnthropic = [](JsonDocument& doc) {
    JsonObject s = doc["mcp_servers"].add<JsonObject>();
    s["type"] = "url"; s["url"] = "https://mcp.example.com/b"; s["name"] = "authed-mcp";
    s["authorization_token"] = "tok-fake";
  };
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok,
                    (int)providers::antDispatch(pd, "claude-sonnet-5", directive(), jobId));
  TEST_ASSERT_EQUAL_STRING(kGoldenAgentBody, reqBody(d, 1).c_str());
}

// The recorded polls: an empty typed event list is Running, then the agent's
// chunked message + session.status_idle(end_turn) is Done with the chunks joined.
static void test_recorded_polls_running_then_done() {
  Recorded r;
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/events?types[]=agent.message", 200,
                           fixture("session_events_list_running_200.json")});
  d.http.script.push_back({"api.anthropic.com", "/events?types[]=agent.message", 200,
                           fixture("session_events_list_done_200.json")});
  auto pd = d.contract();
  const std::string job = "anthropic:" + r.sessionId;
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)providers::antPoll(pd, job.c_str(), env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Running, (int)env.state);
  env = agent::ResultEnvelope{};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)providers::antPoll(pd, job.c_str(), env));
  TEST_ASSERT_EQUAL((int)agent::JobState::Done, (int)env.state);
  const std::string reply = env.reply;
  TEST_ASSERT_EQUAL(0, (int)reply.find("The latest stable release of the curl project"));
  TEST_ASSERT_TRUE(reply.find("Source: https://curl.se/docs/releases.html") != std::string::npos);
}

// ---- CUM-481: the cache class ------------------------------------------------

// One cache state the dispatch can start from.
struct CacheCase {
  const char* name;
  bool nvsFull;      // writes dropped (CUM-479)
  bool envInNvs;     // an env id already persisted
  bool agentInNvs;   // an agent for the model already persisted
};

static void checkSessionBodies(const FakeProviderDeps& d, const Recorded& r, const char* name) {
  for (const auto& req : d.http.seen) {
    if (req.path != "/v1/sessions") continue;
    JsonDocument b;
    deserializeJson(b, req.body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.agentId.c_str(), b["agent"] | "", name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.envId.c_str(), b["environment_id"] | "", name);
  }
}

// EVERY cache state x storage health, two spawns each: the session never goes
// out with an empty or wrong id, and neither id is created more than once (the
// second spawn is session + turn only - no provider-side re-create thrash). A
// new cache path that re-reads a dropped write fails here.
static void test_session_never_sent_without_ids_any_cache_state() {
  const CacheCase cases[] = {
      {"cold, healthy NVS", false, false, false},
      {"cold, full NVS", true, false, false},
      {"env cached, healthy NVS", false, true, false},
      {"env cached, full NVS", true, true, false},
      {"both cached, healthy NVS", false, true, true},
      {"both cached, full NVS", true, true, true},
  };
  for (const CacheCase& c : cases) {
    Recorded r;
    FakeProviderDeps d;
    providers::antForgetIds();
    d.nvsFull = c.nvsFull;
    if (c.envInNvs) d.antEnv = r.envId;
    if (c.agentInNvs) d.antAgents = "claude-sonnet-5=" + r.agentId + ";";
    scriptCreates(d, r, !c.envInNvs, !c.agentInNvs);
    scriptSessionAndTurn(d, r);
    scriptSessionAndTurn(d, r);
    char jobId[72] = {};
    TEST_ASSERT_EQUAL_MESSAGE((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId), c.name);
    TEST_ASSERT_EQUAL_MESSAGE((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId), c.name);
    checkSessionBodies(d, r, c.name);
    TEST_ASSERT_EQUAL_MESSAGE(c.envInNvs ? 0 : 1, countPath(d, "/v1/environments"), c.name);
    TEST_ASSERT_EQUAL_MESSAGE(c.agentInNvs ? 0 : 1, countPath(d, "/v1/agents"), c.name);
    TEST_ASSERT_EQUAL_MESSAGE(2, countPath(d, "/v1/sessions"), c.name);
  }
}

// The bench unit's exact state: full NVS, cold caches. The dropped writes are
// detected and logged ONCE (when the ids are created), then RAM serves them.
static void test_full_nvs_logged_once_then_served_from_ram() {
  Recorded r;
  FakeProviderDeps d;
  d.nvsFull = true;
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  scriptSessionAndTurn(d, r);
  scriptSessionAndTurn(d, r);
  char jobId[72] = {};
  for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL(1, countLog("anthropic: env id not saved"));
  TEST_ASSERT_EQUAL(1, countLog("anthropic: agent map not saved"));
  TEST_ASSERT_EQUAL(1, d.antEnvSets);     // tried once, never retried per spawn
  TEST_ASSERT_EQUAL(1, d.antAgentSets);
  TEST_ASSERT_EQUAL(8, (int)d.http.seen.size());   // 2 creates + 3 x (session + turn)
  TEST_ASSERT_EQUAL(0, countLog("create session HTTP"));
}

// A healthy NVS logs nothing about storage and persists both ids.
static void test_healthy_nvs_persists_without_warning() {
  Recorded r;
  FakeProviderDeps d;
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL_STRING(r.envId.c_str(), d.antEnv.c_str());
  TEST_ASSERT_EQUAL_STRING(("claude-sonnet-5=" + r.agentId + ";").c_str(), d.antAgents.c_str());
  TEST_ASSERT_EQUAL(0, countLog("not saved"));
}

// A stale session 404 forgets both ids even when the NVS clear is dropped too:
// the next spawn re-creates instead of looping on the stale persisted ids.
static void test_stale_404_forgets_ids_even_when_clear_is_dropped() {
  Recorded r;
  FakeProviderDeps d;
  d.nvsFull = true;
  d.antEnv = "env_stale";
  d.antAgents = "claude-sonnet-5=ag_stale;";
  d.http.script.push_back({"", "/v1/sessions", 404,
      "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\",\"message\":\"agent not found\"}}"});
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::NotFound, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_TRUE(LogCapture::contains("create session HTTP 404: not_found_error: agent not found"));
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL(1, countPath(d, "/v1/environments"));   // re-created, not the stale id
  const std::string retry =
      "{\"agent\":\"" + r.agentId + "\",\"environment_id\":\"" + r.envId + "\"}";
  TEST_ASSERT_EQUAL_STRING(retry.c_str(), reqBody(d, 3).c_str());
}

// EVERY recorded answer that means "a cached id is dead" (deleted, unknown, or
// archived env or agent - live bodies 2026-10-10) forgets both ids, so the next
// spawn re-creates them; a 400 about anything else (the CUM-481 empty field)
// keeps them. An archived env used to fail every spawn until NVS was erased.
static void test_every_dead_id_answer_forgets_the_cache() {
  struct DeadCase { const char* fixture; int code; bool dead; };
  const DeadCase cases[] = {
      {"session_create_404_environment_not_found.json", 404, true},
      {"session_create_404_agent_not_found.json", 404, true},
      {"session_create_400_archived_environment.json", 400, true},
      {"session_create_400_archived_agent.json", 400, true},
      {"session_create_400_empty_environment_id.json", 400, false},
  };
  for (const DeadCase& c : cases) {
    Recorded r;
    FakeProviderDeps d;
    providers::antForgetIds();
    d.antEnv = "env_cached";
    d.antAgents = "claude-sonnet-5=ag_cached;";
    d.http.script.push_back({"", "/v1/sessions", c.code, fixture(c.fixture)});
    char jobId[72] = {};
    TEST_ASSERT_NOT_EQUAL_MESSAGE((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId), c.fixture);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.dead ? "" : "env_cached", d.antEnv.c_str(), c.fixture);
    if (c.dead) scriptCreates(d, r, true, true);
    scriptSessionAndTurn(d, r);
    TEST_ASSERT_EQUAL_MESSAGE((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId), c.fixture);
    TEST_ASSERT_EQUAL_MESSAGE(c.dead ? 1 : 0, countPath(d, "/v1/environments"), c.fixture);
  }
}

// The agent map is matched at an entry start: a model whose name ENDS with the
// requested one must not hand back its agent.
static void test_agent_map_lookup_is_anchored() {
  Recorded r;
  FakeProviderDeps d;
  d.antEnv = r.envId;
  d.antAgents = "my-claude-sonnet-5=ag_wrong;";
  scriptCreates(d, r, false, true);
  scriptSessionAndTurn(d, r);
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  checkSessionBodies(d, r, "anchored lookup");
  TEST_ASSERT_EQUAL_STRING(("my-claude-sonnet-5=ag_wrong;claude-sonnet-5=" + r.agentId + ";").c_str(),
                           d.antAgents.c_str());
}

// The RAM-only ids belong to the key they were created under: after a key change
// they are dropped and storage is re-read, so ids that never reached NVS cannot
// ride into another account's spawn. (Persisted ids are not keyed; a stale one
// is cleared by the provider's 404 - see the stale-404 case above.)
static void test_key_change_drops_ram_only_ids() {
  Recorded r;
  FakeProviderDeps d;
  d.nvsFull = true;   // the ids live only in RAM
  d.http.script.push_back({"", "/v1/environments", 200, "{\"id\":\"env_first\"}"});
  d.http.script.push_back({"", "/v1/agents", 200, "{\"id\":\"ag_first\"}"});
  scriptSessionAndTurn(d, r);
  scriptCreates(d, r, true, true);
  scriptSessionAndTurn(d, r);
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  d.keys["anthropic"] = "sk-fake-other-account";
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL(2, countPath(d, "/v1/environments"));
  TEST_ASSERT_TRUE(reqBody(d, 6).find(r.envId) != std::string::npos);
}

// ---- CUM-474 + logging: every chain call classifies and names its reason -----

struct StatusCase {
  int code;
  FabricErr want;
};
static const StatusCase kStatuses[] = {
    {400, FabricErr::BadRequest}, {401, FabricErr::Auth},       {403, FabricErr::Auth},
    {404, FabricErr::NotFound},   {409, FabricErr::BadRequest}, {422, FabricErr::BadRequest},
    {429, FabricErr::RateLimited}, {500, FabricErr::RemoteFail}, {529, FabricErr::RemoteFail},
    {0, FabricErr::Network},
};
static const char* kSteps[] = {"create env", "create agent", "create session", "send turn"};

// Script a cold chain whose step `failAt` answers `code` with an error envelope.
static void scriptFailingChain(FakeProviderDeps& d, int failAt, int code) {
  const char* paths[] = {"/v1/environments", "/v1/agents", "/v1/sessions", "/events"};
  const char* ok[] = {"{\"id\":\"env_1\"}", "{\"id\":\"ag_1\"}", "{\"id\":\"sesn_1\"}", "{\"data\":[]}"};
  for (int i = 0; i < failAt; i++) d.http.script.push_back({"", paths[i], 200, ok[i]});
  d.http.script.push_back({"", paths[failAt], code,
      "{\"type\":\"error\",\"error\":{\"type\":\"some_error\",\"message\":\"why it failed\"}}"});
}

// Step x status: the FabricErr follows one rule at every step, and the log
// line names the step, the status and the provider's type + message.
static void test_every_chain_call_classifies_and_logs_reason() {
  for (int step = 0; step < 4; step++) {
    for (const StatusCase& sc : kStatuses) {
      FakeProviderDeps d;
      providers::antForgetIds();
      LogCapture::install();
      scriptFailingChain(d, step, sc.code);
      char jobId[72] = {};
      char msg[64];
      snprintf(msg, sizeof(msg), "%s HTTP %d", kSteps[step], sc.code);
      TEST_ASSERT_EQUAL_MESSAGE((int)sc.want, (int)dispatch(d, directive(), jobId), msg);
      char line[128];
      if (sc.code)
        snprintf(line, sizeof(line), "anthropic: %s HTTP %d: some_error: why it failed",
                 kSteps[step], sc.code);
      else
        snprintf(line, sizeof(line), "anthropic: %s: no response", kSteps[step]);
      TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains(line), line);
      TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("sk-fake-ant"), "a log line carried the key");
    }
  }
}

// A 429 on ANY call of the chain reaches the owner as a rate limit naming the
// window its text states (CUM-474 - it used to collapse into a generic failure).
static void test_429_on_any_step_carries_its_window() {
  for (int step = 0; step < 4; step++) {
    FakeProviderDeps d;
    providers::antForgetIds();
    const char* paths[] = {"/v1/environments", "/v1/agents", "/v1/sessions", "/events"};
    const char* ok[] = {"{\"id\":\"env_1\"}", "{\"id\":\"ag_1\"}", "{\"id\":\"sesn_1\"}", ""};
    for (int i = 0; i < step; i++) d.http.script.push_back({"", paths[i], 200, ok[i]});
    d.http.script.push_back({"", paths[step], 429,
        "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":"
        "\"Number of requests has exceeded your per-minute rate limit\"}}"});
    agent::RateLimit window = agent::RateLimit::Unknown;
    agent::Directive dir = directive();
    dir.rateLimit = &window;
    char jobId[72] = {};
    TEST_ASSERT_EQUAL_MESSAGE((int)FabricErr::RateLimited, (int)dispatch(d, dir, jobId), kSteps[step]);
    TEST_ASSERT_EQUAL_MESSAGE((int)agent::RateLimit::PerMinute, (int)window, kSteps[step]);
  }
}

// The recorded bench failure, as the device now logs it.
static void test_recorded_400_is_logged_with_its_reason() {
  Recorded r;
  FakeProviderDeps d;
  scriptCreates(d, r, true, true);
  d.http.script.push_back({"", "/v1/sessions", 400,
                           fixture("session_create_400_empty_environment_id.json")});
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::BadRequest, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_TRUE(LogCapture::contains(
      "anthropic: create session HTTP 400: invalid_request_error: environment_id: value is required"));
}

// The excerpt is bounded (150 chars, under the DoD's 160) and printable ASCII
// whatever the body, and it fits the log buffer whole on the LONGEST line
// ("anthropic: create session HTTP 400: " + 150 = 186 < 191), so the reason is
// never cut by hlog's 192-byte buffer. Without the cap the buffer would let 155
// through here, so this length assertion is the cap's own.
static void test_error_excerpt_bounded_and_printable() {
  FakeProviderDeps d;
  d.antEnv = "env_1";
  d.antAgents = "claude-sonnet-5=ag_1;";
  std::string msg = "line one\\nline two \xC3\xA9 ";   // a JSON-escaped newline + a UTF-8 e-acute
  while (msg.size() < 600) msg += "more detail ";
  d.http.script.push_back({"", "/v1/sessions", 400,
      "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"" +
      msg + "\"}}"});
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::BadRequest, (int)dispatch(d, directive(), jobId));
  const char* tag = "anthropic: create session HTTP 400: ";
  std::string line;
  for (const auto& l : LogCapture::lines())
    if (l.rfind(tag, 0) == 0) line = l;
  TEST_ASSERT_FALSE(line.empty());
  const std::string excerpt = line.substr(strlen(tag));
  TEST_ASSERT_EQUAL(150, (int)excerpt.size());
  TEST_ASSERT_EQUAL(0, (int)excerpt.find("invalid_request_error: line one?line two ?? more"));
  for (char c : excerpt) TEST_ASSERT_TRUE(c >= 0x20 && c <= 0x7e);
}

// A 2xx create that carries no id is a provider fault, named as such.
static void test_2xx_without_id_is_named() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/environments", 200, "{}"});
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::RemoteFail, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_TRUE(LogCapture::contains("anthropic: create env HTTP 200: reply carried no id"));
}

// A session id that cannot fit the job id whole is refused before any turn is
// sent: a truncated job id would poll and cancel a different session.
static void test_oversized_session_id_refused_before_turn() {
  FakeProviderDeps d;
  d.antEnv = "env_1";
  d.antAgents = "claude-sonnet-5=ag_1;";
  const std::string longId = "sesn_" + std::string(57, 'x');   // 62 chars > 61
  d.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"" + longId + "\"}"});
  char jobId[72] = {};
  TEST_ASSERT_EQUAL((int)FabricErr::RemoteFail, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());   // no user.message went out
  TEST_ASSERT_TRUE(LogCapture::contains("anthropic: create session: id too long (62 chars)"));
  // 61 chars still fits: "anthropic:" + 61 + NUL = 72.
  providers::antForgetIds();
  const std::string maxId = "sesn_" + std::string(56, 'y');
  d.http.script.push_back({"", "/v1/sessions", 200, "{\"id\":\"" + maxId + "\"}"});
  d.http.script.push_back({"", "/events", 200, "{\"data\":[]}"});
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)dispatch(d, directive(), jobId));
  TEST_ASSERT_EQUAL_STRING(("anthropic:" + maxId).c_str(), jobId);
}

// ---- poll terminal states ----------------------------------------------------

// Every idle stop_reason follows the documented rule: end_turn is Done,
// requires_action (or a missing reason) keeps the job Running, and anything else
// - known or not yet known - ends it as an error instead of Running forever.
static agent::ResultEnvelope pollIdle(const char* stopReason) {
  FakeProviderDeps d;
  std::string ev = "{\"data\":[{\"type\":\"session.status_idle\"";
  if (stopReason) ev += std::string(",\"stop_reason\":{\"type\":\"") + stopReason + "\"}";
  ev += "}]}";
  d.http.script.push_back({"", "/events", 200, ev});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)FabricErr::Ok, (int)providers::antPoll(pd, "anthropic:sesn_1", env));
  return env;
}

static void test_poll_idle_stop_reasons() {
  struct IdleCase { const char* reason; agent::JobState want; const char* error; };
  const IdleCase cases[] = {
      {"end_turn", agent::JobState::Done, ""},
      {"requires_action", agent::JobState::Running, ""},
      {nullptr, agent::JobState::Running, ""},
      {"retries_exhausted", agent::JobState::Error, "the agent stopped after repeated errors"},
      {"budget_reached", agent::JobState::Error, "the agent stopped early (budget_reached)"},
      {"some_future_reason", agent::JobState::Error, "the agent stopped early (some_future_reason)"},
  };
  for (const IdleCase& c : cases) {
    agent::ResultEnvelope env = pollIdle(c.reason);
    const char* label = c.reason ? c.reason : "(no stop_reason)";
    TEST_ASSERT_EQUAL_MESSAGE((int)c.want, (int)env.state, label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.error, env.error, label);
  }
}

// A refused poll logs its reason (and keeps the poll's own error mapping).
static void test_refused_poll_logs_reason() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/events", 500,
      "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\"Internal server error\"}}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)FabricErr::RemoteFail, (int)providers::antPoll(pd, "anthropic:sesn_1", env));
  TEST_ASSERT_TRUE(LogCapture::contains("anthropic: poll HTTP 500: api_error: Internal server error"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cold_dispatch_golden_requests);
  RUN_TEST(test_mcp_connectors_fitted_to_managed_agents_shape);
  RUN_TEST(test_only_token_connectors_leave_the_plain_agent_body);
  RUN_TEST(test_recorded_polls_running_then_done);
  RUN_TEST(test_session_never_sent_without_ids_any_cache_state);
  RUN_TEST(test_full_nvs_logged_once_then_served_from_ram);
  RUN_TEST(test_healthy_nvs_persists_without_warning);
  RUN_TEST(test_stale_404_forgets_ids_even_when_clear_is_dropped);
  RUN_TEST(test_every_dead_id_answer_forgets_the_cache);
  RUN_TEST(test_agent_map_lookup_is_anchored);
  RUN_TEST(test_key_change_drops_ram_only_ids);
  RUN_TEST(test_every_chain_call_classifies_and_logs_reason);
  RUN_TEST(test_429_on_any_step_carries_its_window);
  RUN_TEST(test_recorded_400_is_logged_with_its_reason);
  RUN_TEST(test_error_excerpt_bounded_and_printable);
  RUN_TEST(test_2xx_without_id_is_named);
  RUN_TEST(test_oversized_session_id_refused_before_turn);
  RUN_TEST(test_poll_idle_stop_reasons);
  RUN_TEST(test_refused_poll_logs_reason);
  UNITY_END();
  return 0;
}
