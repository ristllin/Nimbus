#include <unity.h>

#include <string>

#include <ArduinoJson.h>

#include "nimbus/orch/mcp_client.h"
#include "real_fixtures.h"   // byte-exact captures from a real MCP server

using namespace nimbus::orch::mcp;

void setUp() {}
void tearDown() {}

static const char* kJson = "application/json";
static const char* kSse = "text/event-stream";

// A JSON-RPC response wrapped as a single SSE event, the way a Streamable HTTP
// server streams it.
static std::string sse(const std::string& jsonBody) {
  return "event: message\ndata: " + jsonBody + "\n\n";
}

// ---- request builders -------------------------------------------------------

static void test_build_initialize_shape() {
  std::string s = buildInitialize("nimbus", "4.3.0");
  JsonDocument d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, s).code());
  TEST_ASSERT_EQUAL_STRING("2.0", d["jsonrpc"]);
  TEST_ASSERT_EQUAL(kIdInitialize, d["id"].as<int>());
  TEST_ASSERT_EQUAL_STRING("initialize", d["method"]);
  TEST_ASSERT_EQUAL_STRING(kProtocolVersion, d["params"]["protocolVersion"]);
  TEST_ASSERT_EQUAL_STRING("nimbus", d["params"]["clientInfo"]["name"]);
  TEST_ASSERT_TRUE(d["params"]["capabilities"].is<JsonObjectConst>());
}

static void test_build_initialized_is_a_notification() {
  std::string s = buildInitializedNotification();
  JsonDocument d;
  TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(d, s).code());
  TEST_ASSERT_EQUAL_STRING("notifications/initialized", d["method"]);
  TEST_ASSERT_TRUE(d["id"].isNull());  // a notification carries no id
}

static void test_build_tools_list_cursor() {
  JsonDocument d0;
  deserializeJson(d0, buildToolsList());
  TEST_ASSERT_TRUE(d0["params"].isNull() || d0["params"]["cursor"].isNull());
  JsonDocument d1;
  deserializeJson(d1, buildToolsList("PAGE2"));
  TEST_ASSERT_EQUAL_STRING("PAGE2", d1["params"]["cursor"]);
  TEST_ASSERT_EQUAL_STRING("tools/list", d1["method"]);
}

static void test_build_tools_call_args_object() {
  JsonDocument d;
  deserializeJson(d, buildToolsCall("search", "{\"q\":\"hi\",\"n\":3}"));
  TEST_ASSERT_EQUAL_STRING("tools/call", d["method"]);
  TEST_ASSERT_EQUAL_STRING("search", d["params"]["name"]);
  TEST_ASSERT_EQUAL_STRING("hi", d["params"]["arguments"]["q"]);
  TEST_ASSERT_EQUAL(3, d["params"]["arguments"]["n"].as<int>());
}

static void test_build_tools_call_bad_args_degrade_to_empty_object() {
  JsonDocument d;
  deserializeJson(d, buildToolsCall("t", "not json"));
  TEST_ASSERT_TRUE(d["params"]["arguments"].is<JsonObjectConst>());
  TEST_ASSERT_EQUAL(0, d["params"]["arguments"].as<JsonObjectConst>().size());
  JsonDocument d2;  // a JSON array is not a valid arguments object -> {}
  deserializeJson(d2, buildToolsCall("t", "[1,2]"));
  TEST_ASSERT_TRUE(d2["params"]["arguments"].is<JsonObjectConst>());
}

// ---- initialize parsing (both body shapes) ----------------------------------

static const char* kInitOk =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\","
    "\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fs\",\"version\":\"1.2\"}}}";

static void test_parse_initialize_json() {
  InitializeResult r = parseInitialize(200, kJson, kInitOk, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("2025-06-18", r.protocolVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("fs", r.serverName.c_str());
  TEST_ASSERT_EQUAL_STRING("1.2", r.serverVersion.c_str());
  TEST_ASSERT_TRUE(r.hasTools);
}

static void test_parse_initialize_sse() {
  InitializeResult r = parseInitialize(200, kSse, sse(kInitOk), "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.hasTools);
  TEST_ASSERT_EQUAL_STRING("fs", r.serverName.c_str());
}

static void test_parse_initialize_no_tools_capability() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\","
      "\"capabilities\":{},\"serverInfo\":{\"name\":\"x\"}}}";
  InitializeResult r = parseInitialize(200, kJson, body, "x");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.hasTools);
}

// ---- tools/list parsing -----------------------------------------------------

static const char* kListOk =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
    "{\"name\":\"read_file\",\"description\":\"Read a file\",\"inputSchema\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"list_dir\",\"description\":\"List a directory\"}"
    "],\"nextCursor\":\"c2\"}}";

static void test_parse_tools_list_json() {
  ToolsListResult r = parseToolsList(200, kJson, kListOk, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(2, (int)r.tools.size());
  TEST_ASSERT_EQUAL_STRING("read_file", r.tools[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("Read a file", r.tools[0].description.c_str());
  TEST_ASSERT_TRUE(r.tools[0].inputSchemaJson.find("\"path\"") != std::string::npos);
  // missing inputSchema -> "{}" not a broken/quoted value
  TEST_ASSERT_EQUAL_STRING("{}", r.tools[1].inputSchemaJson.c_str());
  TEST_ASSERT_EQUAL_STRING("c2", r.nextCursor.c_str());
}

static void test_parse_tools_list_sse_and_empty() {
  ToolsListResult r = parseToolsList(200, kSse, sse(kListOk), "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(2, (int)r.tools.size());
  const char* empty = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[]}}";
  ToolsListResult e = parseToolsList(200, kJson, empty, "files");
  TEST_ASSERT_TRUE(e.ok);
  TEST_ASSERT_EQUAL(0, (int)e.tools.size());
  TEST_ASSERT_TRUE(e.nextCursor.empty());
}

static void test_parse_tools_list_skips_nameless() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
      "{\"description\":\"no name\"},{\"name\":\"ok\"}]}}";
  ToolsListResult r = parseToolsList(200, kJson, body, "s");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(1, (int)r.tools.size());
  TEST_ASSERT_EQUAL_STRING("ok", r.tools[0].name.c_str());
}

// ---- tools/call parsing -----------------------------------------------------

static void test_parse_call_text_content() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
      "{\"type\":\"text\",\"text\":\"line one\"},{\"type\":\"text\",\"text\":\"line two\"}],"
      "\"isError\":false}}";
  CallToolResult r = parseCallTool(200, kJson, body, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.isError);
  TEST_ASSERT_EQUAL_STRING("line one\nline two", r.text.c_str());
}

static void test_parse_call_tool_level_error() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
      "{\"type\":\"text\",\"text\":\"no such path\"}],\"isError\":true}}";
  CallToolResult r = parseCallTool(200, kJson, body, "files");
  TEST_ASSERT_TRUE(r.ok);          // transport succeeded
  TEST_ASSERT_TRUE(r.isError);     // the tool itself failed
  TEST_ASSERT_EQUAL_STRING("no such path", r.text.c_str());
}

static void test_parse_call_non_text_block_named() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
      "{\"type\":\"image\",\"data\":\"...\",\"mimeType\":\"image/png\"}]}}";
  CallToolResult r = parseCallTool(200, kJson, body, "s");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("[image content]", r.text.c_str());
}

static void test_parse_call_structured_content_fallback() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[],"
      "\"structuredContent\":{\"count\":5}}}";
  CallToolResult r = parseCallTool(200, kJson, body, "s");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.text.find("\"count\":5") != std::string::npos);
}

// ---- error paths (the fake server sends garbage / failures) -----------------

static void test_http_error_status() {
  CallToolResult r = parseCallTool(500, kJson, "internal error page", "files");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Http, (int)r.error);
  TEST_ASSERT_TRUE(r.errorMsg.find("HTTP 500") != std::string::npos);
  TEST_ASSERT_TRUE(r.errorMsg.find("try again") != std::string::npos);
}

static void test_unauthorized() {
  ToolsListResult r = parseToolsList(401, kJson, "", "linear");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Unauthorized, (int)r.error);
  TEST_ASSERT_TRUE(r.errorMsg.find("token") != std::string::npos);
  TEST_ASSERT_TRUE(r.errorMsg.find("linear") != std::string::npos);
}

static void test_malformed_body() {
  ToolsListResult r = parseToolsList(200, kJson, "{not valid json", "files");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Malformed, (int)r.error);
}

static void test_valid_json_but_not_rpc_envelope() {
  // A 200 with a plain object that is not a JSON-RPC response (no result/error).
  ToolsListResult r = parseToolsList(200, kJson, "{\"hello\":\"world\"}", "files");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Malformed, (int)r.error);
}

static void test_rpc_error_object() {
  const char* body =
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
  ToolsListResult r = parseToolsList(200, kJson, body, "files");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Rpc, (int)r.error);
  TEST_ASSERT_TRUE(r.errorMsg.find("Method not found") != std::string::npos);
}

static void test_empty_2xx_body() {
  CallToolResult r = parseCallTool(200, kJson, "", "files");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Empty, (int)r.error);
}

static void test_sse_with_leading_notifications_then_response() {
  // A stream that first carries a progress notification (a server->client
  // message, no result/error) and then the real answer. We must pick the answer.
  std::string body =
      "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\","
      "\"params\":{\"progress\":50}}\n\n" +
      sse(kListOk);
  ToolsListResult r = parseToolsList(200, kSse, body, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(2, (int)r.tools.size());
}

static void test_sse_multiline_data_field() {
  // The data field spans two `data:` lines; per SSE they join with '\n'. The
  // JSON here is split across the two lines.
  std::string body =
      "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":2,\n"
      "data: \"result\":{\"tools\":[{\"name\":\"t\"}]}}\n\n";
  ToolsListResult r = parseToolsList(200, kSse, body, "files");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(1, (int)r.tools.size());
  TEST_ASSERT_EQUAL_STRING("t", r.tools[0].name.c_str());
}

static void test_timeout_error_copy() {
  std::string m = nextStepError(ErrorKind::Timeout, "files", "20s");
  TEST_ASSERT_TRUE(m.find("files") != std::string::npos);
  TEST_ASSERT_TRUE(m.find("20s") != std::string::npos);
  TEST_ASSERT_TRUE(m.find("try again") != std::string::npos);
  // house style: no exclamation marks in error copy
  TEST_ASSERT_TRUE(m.find('!') == std::string::npos);
}

// ---- namespacing ------------------------------------------------------------

static void test_slugify_server() {
  TEST_ASSERT_EQUAL_STRING("linear", slugifyServer("Linear").c_str());
  TEST_ASSERT_EQUAL_STRING("my_files", slugifyServer("My Files!").c_str());
  TEST_ASSERT_EQUAL_STRING("mcp_linear_app", slugifyServer("mcp.linear.app").c_str());
  TEST_ASSERT_EQUAL_STRING("server", slugifyServer("***").c_str());  // fallback
  TEST_ASSERT_EQUAL_STRING("a_b", slugifyServer("a   b").c_str());   // collapse runs
}

static void test_namespaced_tool_is_wire_safe() {
  std::string n = namespacedTool("linear", "create_issue");
  TEST_ASSERT_EQUAL_STRING("mcp.linear.create_issue", n.c_str());
  // a hostile tool name is slugified, never smuggling '/' or ':' into the wire
  std::string h = namespacedTool("fs", "search/all");
  TEST_ASSERT_EQUAL_STRING("mcp.fs.search_all", h.c_str());
  for (char c : h)
    TEST_ASSERT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_');
}

static void test_namespace_helpers() {
  TEST_ASSERT_TRUE(isNamespaced("mcp.linear.create_issue"));
  TEST_ASSERT_FALSE(isNamespaced("memory.search"));
  TEST_ASSERT_EQUAL_STRING("linear", serverOf("mcp.linear.create_issue").c_str());
  TEST_ASSERT_EQUAL_STRING("", serverOf("memory.search").c_str());
}

// ---- real server (CUM-61 local-server validation, no hardware) --------------
// These feed byte-exact responses from @modelcontextprotocol/server-everything
// (streamableHttp) through the parser, proving it handles a real server's SSE.

static void test_real_server_initialize() {
  InitializeResult r = parseInitialize(200, kSse, mcpfix::kInitBody, "everything");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("2025-06-18", r.protocolVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("mcp-servers/everything", r.serverName.c_str());
  TEST_ASSERT_TRUE(r.hasTools);
}

static void test_real_server_tools_list() {
  ToolsListResult r = parseToolsList(200, kSse, mcpfix::kToolsListBody, "everything");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(mcpfix::kToolsListCount, (int)r.tools.size());
  bool sawEcho = false;
  for (const auto& t : r.tools)
    if (t.name == "echo") {
      sawEcho = true;
      TEST_ASSERT_TRUE(t.inputSchemaJson.find("message") != std::string::npos);
    }
  TEST_ASSERT_TRUE(sawEcho);
}

static void test_real_server_call_echo() {
  CallToolResult r = parseCallTool(200, kSse, mcpfix::kCallEchoBody, "everything");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.isError);
  TEST_ASSERT_EQUAL_STRING("Echo: nimbus-mcp-ok", r.text.c_str());
}

static void test_real_server_tool_namespacing() {
  // a real hyphenated server tool name slugifies to a wire-safe registry name
  TEST_ASSERT_EQUAL_STRING("mcp.everything.get_annotated_message",
                           namespacedTool("everything", "get-annotated-message").c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_real_server_initialize);
  RUN_TEST(test_real_server_tools_list);
  RUN_TEST(test_real_server_call_echo);
  RUN_TEST(test_real_server_tool_namespacing);
  RUN_TEST(test_build_initialize_shape);
  RUN_TEST(test_build_initialized_is_a_notification);
  RUN_TEST(test_build_tools_list_cursor);
  RUN_TEST(test_build_tools_call_args_object);
  RUN_TEST(test_build_tools_call_bad_args_degrade_to_empty_object);
  RUN_TEST(test_parse_initialize_json);
  RUN_TEST(test_parse_initialize_sse);
  RUN_TEST(test_parse_initialize_no_tools_capability);
  RUN_TEST(test_parse_tools_list_json);
  RUN_TEST(test_parse_tools_list_sse_and_empty);
  RUN_TEST(test_parse_tools_list_skips_nameless);
  RUN_TEST(test_parse_call_text_content);
  RUN_TEST(test_parse_call_tool_level_error);
  RUN_TEST(test_parse_call_non_text_block_named);
  RUN_TEST(test_parse_call_structured_content_fallback);
  RUN_TEST(test_http_error_status);
  RUN_TEST(test_unauthorized);
  RUN_TEST(test_malformed_body);
  RUN_TEST(test_valid_json_but_not_rpc_envelope);
  RUN_TEST(test_rpc_error_object);
  RUN_TEST(test_empty_2xx_body);
  RUN_TEST(test_sse_with_leading_notifications_then_response);
  RUN_TEST(test_sse_multiline_data_field);
  RUN_TEST(test_timeout_error_copy);
  RUN_TEST(test_slugify_server);
  RUN_TEST(test_namespaced_tool_is_wire_safe);
  RUN_TEST(test_namespace_helpers);
  return UNITY_END();
}
