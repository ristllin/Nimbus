#include <unity.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/mcp_client.h"
#include "nimbus/orch/mcp_resilience.h"

// test_mcp_robustness - the CLASS tests behind docs/mcp.md "Limits and safety"
// (lane TF-M1, owner ask 2026-09-01: "did you test connecting multiple mcps... one
// at a time... big payloads... tool budget?"). Each test below fails against an
// absent or broken guard, not one hand-picked case:
//   (a) MANY connectors configured -> no silent drop; one flapping server's
//       cooldown is isolated to that server (per-server breaker, not a global).
//   (b) SERIALIZATION - the client issues exactly one JSON-RPC request per HTTP
//       exchange (never a batch array), so the transport never carries two calls
//       in flight. (The device-side memory-Lock + tls_arbiter that serialize
//       MCP-vs-MCP and MCP-vs-provider are FreeRTOS primitives, not host-
//       compilable; the native build is lib/core only. That leg is pinned by the
//       HIL suite - tests/hil/test_l15_mcp_robustness.py - not here.)
//   (c) BIG PAYLOADS - a body capped mid-read is refused with the honest size
//       line, never a mislabeled parse error; the class of transport failures the
//       device detects itself (and must surface over a partial body) is pinned.
//   (d) TOOL BUDGET - a server advertising far more tools than the budget surfaces
//       exactly the budget, deterministically (first N in server order), visibly.

using namespace nimbus::orch::mcp;
using nimbus::orch::ConnectorInfo;
using nimbus::orch::parseConnectorsJson;

void setUp() {}
void tearDown() {}

// kMaxConnectors mirrors src/agent/connectors.h (device constant, not in lib/core).
// If the device raises it, this local copy is what these tests exercise - the
// invariant under test is "no SILENT drop at the cap", not the cap's value.
static const int kMaxConnectors = 24;
static const char* kJson = "application/json";

// ---------------------------------------------------------------------------
// (a) MANY connectors configured
// ---------------------------------------------------------------------------

// Build a connectors blob with `n` device-dialed, approved MCP entries.
static std::string manyMcpBlob(int n) {
  std::string s = "[";
  for (int i = 0; i < n; i++) {
    if (i) s += ",";
    s += "{\"name\":\"srv" + std::to_string(i) +
         "\",\"kind\":\"mcp\",\"url\":\"https://h" + std::to_string(i) +
         ".example.com/mcp\",\"en\":1,\"dev\":1,\"appr\":1}";
  }
  s += "]";
  return s;
}

// Three real device-MCP connectors parse with their per-entry dev/appr/url intact
// - the multi-connector case the owner asked about, at the config seam.
static void test_multiple_device_mcp_connectors_parsed() {
  const char* blob =
      "[{\"name\":\"files\",\"kind\":\"mcp\",\"url\":\"http://192.168.1.20:3111/mcp\",\"en\":1,\"dev\":1,\"appr\":1},"
      "{\"name\":\"linear\",\"kind\":\"mcp\",\"url\":\"https://mcp.linear.app/mcp\",\"en\":1,\"dev\":1,\"appr\":0},"
      "{\"name\":\"notes\",\"kind\":\"mcp\",\"url\":\"http://10.0.0.5/mcp\",\"en\":1,\"dev\":1,\"appr\":1}]";
  std::vector<ConnectorInfo> out;
  int total = -1;
  int n = parseConnectorsJson(blob, out, kMaxConnectors, &total);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_INT(3, total);
  TEST_ASSERT_EQUAL_STRING("files", out[0].name.c_str());
  TEST_ASSERT_TRUE(out[0].deviceDialed);
  TEST_ASSERT_TRUE(out[0].approved);
  // the middle one is enabled + device-dialed but NOT approved -> fail-closed set
  TEST_ASSERT_TRUE(out[1].deviceDialed);
  TEST_ASSERT_FALSE(out[1].approved);
  TEST_ASSERT_EQUAL_STRING("https://mcp.linear.app/mcp", out[1].url.c_str());
  TEST_ASSERT_TRUE(out[2].approved);
}

// The device tracks a bounded set; exceeding it must be VISIBLE, never silent.
// parseConnectorsJson reports totalEntries so the caller (connectors.cpp) can log
// the drop. A blob past the cap returns exactly maxN AND a larger total.
static void test_many_connectors_no_silent_drop() {
  std::string blob = manyMcpBlob(kMaxConnectors + 6);  // 30 configured, cap 24
  std::vector<ConnectorInfo> out;
  int total = -1;
  int n = parseConnectorsJson(blob.c_str(), out, kMaxConnectors, &total);
  TEST_ASSERT_EQUAL_INT(kMaxConnectors, n);       // only the cap is materialized
  TEST_ASSERT_EQUAL_INT(kMaxConnectors + 6, total);  // ...but the drop is seen
  TEST_ASSERT_TRUE(total > n);                     // the caller's log condition
}

// Right at the cap, nothing is dropped and total == n (the boundary).
static void test_connectors_exactly_at_cap() {
  std::string blob = manyMcpBlob(kMaxConnectors);
  std::vector<ConnectorInfo> out;
  int total = -1;
  int n = parseConnectorsJson(blob.c_str(), out, kMaxConnectors, &total);
  TEST_ASSERT_EQUAL_INT(kMaxConnectors, n);
  TEST_ASSERT_EQUAL_INT(kMaxConnectors, total);
}

// One flapping server must NOT wedge the others: the breaker is PER-SERVER. Trip
// server A to Open; server B (its own breaker) still allows. A shared/global
// breaker - the bug this guards - would trip B too and fail this test.
static void test_one_flapping_server_does_not_block_others() {
  BreakerConfig cfg{3, 30000};
  CircuitBreaker a(cfg), b(cfg), c(cfg);
  // A fails repeatedly -> Open (fails fast).
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_TRUE(a.allow((uint32_t)i));
    a.onFailure((uint32_t)i);
  }
  TEST_ASSERT_EQUAL(BreakerState::Open, a.state());
  TEST_ASSERT_FALSE(a.allow(10));               // A is cooling down
  // B and C are untouched: healthy servers keep serving through A's outage.
  TEST_ASSERT_TRUE(b.allow(10));
  TEST_ASSERT_EQUAL(BreakerState::Closed, b.state());
  TEST_ASSERT_TRUE(c.allow(10));
  TEST_ASSERT_EQUAL(BreakerState::Closed, c.state());
  // and A recovers on its own after the cooldown, without B/C involvement.
  TEST_ASSERT_TRUE(a.allow(10 + 30000));
  TEST_ASSERT_EQUAL(BreakerState::HalfOpen, a.state());
}

// ---------------------------------------------------------------------------
// (b) SERIALIZATION - one JSON-RPC request per exchange (no batch)
// ---------------------------------------------------------------------------

// Assert a serialized request is a single JSON-RPC object (never a batch array),
// carries exactly one method, and one id (or none for a notification). A builder
// that emitted a JSON-RPC batch `[{...},{...}]` would put two calls in one POST -
// two calls in flight in one exchange - which this rejects.
static void assertSingleRequest(const std::string& body, const char* method, bool isNotification) {
  JsonDocument d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, body).code());
  TEST_ASSERT_FALSE(d.is<JsonArrayConst>());     // NOT a JSON-RPC batch
  TEST_ASSERT_TRUE(d.is<JsonObjectConst>());
  TEST_ASSERT_EQUAL_STRING("2.0", d["jsonrpc"]);
  TEST_ASSERT_EQUAL_STRING(method, d["method"]);
  if (isNotification)
    TEST_ASSERT_TRUE(d["id"].isNull());          // a notification awaits no reply
  else
    TEST_ASSERT_FALSE(d["id"].isNull());         // exactly one in-flight id
}

static void test_every_builder_is_a_single_request() {
  assertSingleRequest(buildInitialize("nimbus", "1.0"), "initialize", false);
  assertSingleRequest(buildInitializedNotification(), "notifications/initialized", true);
  assertSingleRequest(buildToolsList("cur"), "tools/list", false);
  assertSingleRequest(buildToolsCall("t", "{}"), "tools/call", false);
  assertSingleRequest(buildResourcesList(), "resources/list", false);
  assertSingleRequest(buildResourceTemplatesList(), "resources/templates/list", false);
  assertSingleRequest(buildResourcesRead("file:///a"), "resources/read", false);
  assertSingleRequest(buildPromptsList(), "prompts/list", false);
  assertSingleRequest(buildPromptsGet("p", "{}"), "prompts/get", false);
}

// A tools/call and a tools/list are DISTINCT single requests - the client cannot
// fold two operations into one exchange. (Belt-and-suspenders against a "combine
// the discovery calls" optimization that would break the one-at-a-time contract.)
static void test_call_and_list_are_separate_single_requests() {
  std::string a = buildToolsList();
  std::string b = buildToolsCall("x", "{}");
  JsonDocument da, db;
  deserializeJson(da, a);
  deserializeJson(db, b);
  TEST_ASSERT_FALSE(da.is<JsonArrayConst>());
  TEST_ASSERT_FALSE(db.is<JsonArrayConst>());
  TEST_ASSERT_EQUAL_STRING("tools/list", da["method"]);
  TEST_ASSERT_EQUAL_STRING("tools/call", db["method"]);
}

// ---------------------------------------------------------------------------
// (c) BIG PAYLOADS - capped and refused with the honest line
// ---------------------------------------------------------------------------

// isTransportError enumerates EXACTLY the kinds the device sets from the socket
// read (a capped/absent body). Iterate EVERY ErrorKind and assert membership, so a
// new transport-detected kind added without teaching isTransportError about it
// FAILS here (the "test the class, not the instance" rule): the seam would then
// parse a partial body for that kind and mislabel it.
static void test_transport_error_class_is_exhaustive() {
  struct Row { ErrorKind k; bool transport; };
  const Row rows[] = {
      {ErrorKind::None, false},
      {ErrorKind::Timeout, true},
      {ErrorKind::Connect, true},
      {ErrorKind::Http, false},
      {ErrorKind::Unauthorized, false},
      {ErrorKind::Malformed, false},
      {ErrorKind::Rpc, false},
      {ErrorKind::TooLarge, true},
      {ErrorKind::Empty, false},
  };
  // The table must cover every enum value (Empty is the last). If someone adds a
  // kind after Empty, this count guard forces them to classify it here.
  TEST_ASSERT_EQUAL_INT((int)ErrorKind::Empty + 1, (int)(sizeof(rows) / sizeof(rows[0])));
  for (const Row& r : rows)
    TEST_ASSERT_EQUAL_MESSAGE(r.transport, isTransportError(r.k), "isTransportError misclassified a kind");
}

// A body capped mid-read (TooLarge) is refused with the honest size line - and
// crucially NOT the parse-failure line the truncated prefix would otherwise get.
// This is the exact contrast the device seam relies on (connectors.cpp callTool):
// prefer isTransportError(kind) over parsing resp.body.
static void test_oversize_refused_with_honest_copy_not_parse_error() {
  // What the device would surface if it (wrongly) PARSED the capped, truncated
  // prefix: a "could not be read" Malformed line - honest about JSON, wrong about
  // the real cause (the payload was too big).
  std::string truncated = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"aaaa";
  CallToolResult parsed = parseCallTool(200, kJson, truncated, "files");
  TEST_ASSERT_FALSE(parsed.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Malformed, parsed.error);   // the WRONG message

  // What the seam actually surfaces, because it prefers the transport kind:
  TEST_ASSERT_TRUE(isTransportError(ErrorKind::TooLarge));
  std::string honest = nextStepError(ErrorKind::TooLarge, "files");
  TEST_ASSERT_TRUE(honest.find("more data than the device can hold") != std::string::npos);
  TEST_ASSERT_TRUE(honest.find("files") != std::string::npos);
  TEST_ASSERT_TRUE(honest.find("narrow the request") != std::string::npos);  // the next step
  TEST_ASSERT_TRUE(honest.find('!') == std::string::npos);                    // house style
  // and the two lines are genuinely different (the fix changes what the user sees)
  TEST_ASSERT_TRUE(honest != parsed.errorMsg);
}

// An over-cap body is a DEFINITIVE failure, not a transient one: retrying just
// re-fetches the same too-big payload and burns the turn budget. Pin that so a
// future retry-policy change cannot start hammering an oversize server.
static void test_oversize_is_not_retryable() {
  TEST_ASSERT_FALSE(isRetryable(ErrorKind::TooLarge, 200));
}

// A response comfortably under the cap parses normally - the "just under succeeds"
// half of the boundary. (The byte cap itself, kMaxBodyBytes, lives in the device
// read loop; the free-heap-under-load half is HIL - test_l15_mcp_robustness.py.)
static void test_under_cap_body_parses_normally() {
  std::string body =
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
      "{\"type\":\"text\",\"text\":\"ok\"}],\"isError\":false}}";
  CallToolResult r = parseCallTool(200, kJson, body, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.isError);
  TEST_ASSERT_EQUAL_STRING("ok", r.text.c_str());
}

// ---------------------------------------------------------------------------
// (d) TOOL BUDGET - only the budget surfaces, deterministically
// ---------------------------------------------------------------------------

static std::vector<ToolDef> nTools(int from, int count) {
  std::vector<ToolDef> v;
  for (int i = 0; i < count; i++) {
    ToolDef t;
    char name[8];
    snprintf(name, sizeof(name), "t%03d", from + i);
    t.name = name;
    v.push_back(t);
  }
  return v;
}

// A server advertising 100 tools surfaces exactly the budget, and the SAME 100
// always yield the SAME kept set (the first `budget` in server order) with the
// exact drop count reported (never a silent subset).
static void test_tool_budget_deterministic() {
  const size_t budget = 48;
  std::vector<ToolDef> page = nTools(0, 100);
  std::vector<ToolDef> acc;
  size_t dropped = appendToolsWithinBudget(acc, page, budget);
  TEST_ASSERT_EQUAL_INT((int)budget, (int)acc.size());
  TEST_ASSERT_EQUAL_INT(100 - (int)budget, (int)dropped);
  TEST_ASSERT_EQUAL_STRING("t000", acc.front().name.c_str());   // first survives
  TEST_ASSERT_EQUAL_STRING("t047", acc.back().name.c_str());    // budgeth survives
  // a second identical run yields an identical kept set (determinism)
  std::vector<ToolDef> acc2;
  appendToolsWithinBudget(acc2, page, budget);
  TEST_ASSERT_EQUAL_INT((int)acc.size(), (int)acc2.size());
  TEST_ASSERT_EQUAL_STRING(acc.back().name.c_str(), acc2.back().name.c_str());
}

// The budget spans PAGES (tools/list is paginated): the accumulator carries across
// pages, so the cut lands at the same absolute position regardless of page splits.
static void test_tool_budget_across_pages() {
  const size_t budget = 48;
  std::vector<ToolDef> acc;
  size_t d1 = appendToolsWithinBudget(acc, nTools(0, 30), budget);   // page 1
  TEST_ASSERT_EQUAL_INT(0, (int)d1);
  TEST_ASSERT_EQUAL_INT(30, (int)acc.size());
  size_t d2 = appendToolsWithinBudget(acc, nTools(30, 30), budget);  // page 2 overflows
  TEST_ASSERT_EQUAL_INT(12, (int)d2);        // 30 + 30 = 60, 12 over the 48 budget
  TEST_ASSERT_EQUAL_INT(48, (int)acc.size());
  TEST_ASSERT_EQUAL_STRING("t047", acc.back().name.c_str());
}

// Under budget, nothing is dropped (the common case: a small server).
static void test_tool_budget_under_keeps_all() {
  std::vector<ToolDef> acc;
  size_t dropped = appendToolsWithinBudget(acc, nTools(0, 5), 48);
  TEST_ASSERT_EQUAL_INT(0, (int)dropped);
  TEST_ASSERT_EQUAL_INT(5, (int)acc.size());
}

int main() {
  UNITY_BEGIN();
  // (a) multiple connectors + flapping isolation
  RUN_TEST(test_multiple_device_mcp_connectors_parsed);
  RUN_TEST(test_many_connectors_no_silent_drop);
  RUN_TEST(test_connectors_exactly_at_cap);
  RUN_TEST(test_one_flapping_server_does_not_block_others);
  // (b) serialization
  RUN_TEST(test_every_builder_is_a_single_request);
  RUN_TEST(test_call_and_list_are_separate_single_requests);
  // (c) big payloads
  RUN_TEST(test_transport_error_class_is_exhaustive);
  RUN_TEST(test_oversize_refused_with_honest_copy_not_parse_error);
  RUN_TEST(test_oversize_is_not_retryable);
  RUN_TEST(test_under_cap_body_parses_normally);
  // (d) tool budget
  RUN_TEST(test_tool_budget_deterministic);
  RUN_TEST(test_tool_budget_across_pages);
  RUN_TEST(test_tool_budget_under_keeps_all);
  return UNITY_END();
}
