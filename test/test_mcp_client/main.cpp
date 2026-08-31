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

// Content-type is a hint, not gospel: a JSON body mislabeled as SSE (and an SSE
// body with no content-type) must still parse.
static void test_content_type_mismatch_falls_back() {
  ToolsListResult a = parseToolsList(200, kSse, kListOk, "s");  // JSON body, SSE label
  TEST_ASSERT_TRUE(a.ok);
  TEST_ASSERT_EQUAL(2, (int)a.tools.size());
  ToolsListResult b = parseToolsList(200, "", sse(kListOk), "s");  // SSE body, no label
  TEST_ASSERT_TRUE(b.ok);
  TEST_ASSERT_EQUAL(2, (int)b.tools.size());
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

// Real Linear MCP (the DoD's named remote target): stateless SSE, real serverInfo.
static void test_real_linear_initialize() {
  InitializeResult r = parseInitialize(200, kSse, mcpfix::kLinearInitBody, "linear");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("2025-06-18", r.protocolVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("Linear MCP", r.serverName.c_str());
  TEST_ASSERT_TRUE(r.hasTools);
}

static void test_real_linear_tools_slice() {
  ToolsListResult r = parseToolsList(200, kSse, mcpfix::kLinearToolsSliceBody, "linear");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(mcpfix::kLinearToolsSliceCount, (int)r.tools.size());
  bool sawAttachment = false;
  for (const auto& t : r.tools) {
    if (t.name == "get_attachment") sawAttachment = true;
    // every real Linear tool name is already wire-safe (underscores, no colon)
    std::string ns = namespacedTool("linear", t.name);
    TEST_ASSERT_TRUE(ns.rfind("mcp.linear.", 0) == 0);
  }
  TEST_ASSERT_TRUE(sawAttachment);
}

// ---- rich surfaces: capability flags, resources, prompts, notifications -----

// The everything-server initialize (real fixture) advertises tools/resources/
// prompts with listChanged and resources.subscribe. parseInitialize must surface
// each flag so the device knows which discovery calls to make.
static void test_rich_capability_flags_everything() {
  InitializeResult r = parseInitialize(200, kSse, mcpfix::kInitBody, "everything");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.hasTools);
  TEST_ASSERT_TRUE(r.hasResources);
  TEST_ASSERT_TRUE(r.hasPrompts);
  TEST_ASSERT_TRUE(r.resourcesSubscribe);
  TEST_ASSERT_TRUE(r.toolsListChanged);
  TEST_ASSERT_TRUE(r.resourcesListChanged);
  TEST_ASSERT_TRUE(r.promptsListChanged);
}

// The real Linear initialize advertises ONLY tools.listChanged - so the resource
// and prompt flags must be false (the device skips those discovery calls).
static void test_rich_capability_flags_linear() {
  InitializeResult r = parseInitialize(200, kSse, mcpfix::kLinearInitBody, "Linear");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.hasTools);
  TEST_ASSERT_TRUE(r.toolsListChanged);
  TEST_ASSERT_FALSE(r.hasResources);
  TEST_ASSERT_FALSE(r.hasPrompts);
  TEST_ASSERT_FALSE(r.resourcesSubscribe);
}

static void test_build_paginated_requests() {
  JsonDocument d;
  deserializeJson(d, buildResourcesList("cur1"));
  TEST_ASSERT_EQUAL_STRING("resources/list", d["method"]);
  TEST_ASSERT_EQUAL(kIdResourcesList, d["id"].as<int>());
  TEST_ASSERT_EQUAL_STRING("cur1", d["params"]["cursor"]);
  deserializeJson(d, buildResourceTemplatesList());
  TEST_ASSERT_EQUAL_STRING("resources/templates/list", d["method"]);
  TEST_ASSERT_TRUE(d["params"].isNull());  // no cursor -> no params
  deserializeJson(d, buildPromptsList("p2"));
  TEST_ASSERT_EQUAL_STRING("prompts/list", d["method"]);
  TEST_ASSERT_EQUAL_STRING("p2", d["params"]["cursor"]);
}

static void test_build_resources_read_and_prompts_get() {
  JsonDocument d;
  deserializeJson(d, buildResourcesRead("file:///a.txt"));
  TEST_ASSERT_EQUAL_STRING("resources/read", d["method"]);
  TEST_ASSERT_EQUAL_STRING("file:///a.txt", d["params"]["uri"]);
  deserializeJson(d, buildPromptsGet("summarize", "{\"topic\":\"x\"}"));
  TEST_ASSERT_EQUAL_STRING("prompts/get", d["method"]);
  TEST_ASSERT_EQUAL_STRING("summarize", d["params"]["name"]);
  TEST_ASSERT_EQUAL_STRING("x", d["params"]["arguments"]["topic"]);
  // malformed args degrade to an empty object, never a broken request
  deserializeJson(d, buildPromptsGet("p", "not json"));
  TEST_ASSERT_TRUE(d["params"]["arguments"].is<JsonObjectConst>());
  TEST_ASSERT_EQUAL(0u, d["params"]["arguments"].as<JsonObjectConst>().size());
}

static void test_parse_resources_list_and_pagination() {
  const char* body =
      R"({"jsonrpc":"2.0","id":4,"result":{"resources":[)"
      R"({"uri":"file:///readme.md","name":"README","description":"the readme","mimeType":"text/markdown"},)"
      R"({"name":"no uri, skipped"},)"
      R"({"uri":"file:///data.bin","name":"data"}],"nextCursor":"page2"}})";
  ResourcesListResult r = parseResourcesList(200, kJson, body, "srv");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(2u, r.resources.size());  // nameless-URI entry skipped
  TEST_ASSERT_EQUAL_STRING("file:///readme.md", r.resources[0].uri.c_str());
  TEST_ASSERT_EQUAL_STRING("text/markdown", r.resources[0].mimeType.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.resources[1].mimeType.c_str());
  TEST_ASSERT_EQUAL_STRING("page2", r.nextCursor.c_str());
}

static void test_parse_resource_templates() {
  const char* body =
      R"({"result":{"resourceTemplates":[{"uriTemplate":"file:///{path}","name":"files","mimeType":"text/plain"},{"name":"skipme"}]}})";
  ResourceTemplatesListResult r = parseResourceTemplatesList(200, kJson, body, "srv");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(1u, r.templates.size());  // no-uriTemplate entry skipped
  TEST_ASSERT_EQUAL_STRING("file:///{path}", r.templates[0].uriTemplate.c_str());
}

static void test_parse_resources_read_text_and_blob() {
  const char* body =
      R"({"result":{"contents":[{"uri":"file:///a","mimeType":"text/plain","text":"hello"},{"uri":"file:///b","mimeType":"image/png","blob":"AAAA"}]}})";
  ResourceReadResult r = parseResourcesRead(200, kJson, body, "srv");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("hello\n[binary image/png]", r.text.c_str());
}

static void test_parse_prompts_list() {
  const char* body =
      R"({"result":{"prompts":[{"name":"review","description":"code review","arguments":[{"name":"lang","description":"language","required":true},{"name":"style"}]},{"description":"nameless, skipped"}]}})";
  PromptsListResult r = parsePromptsList(200, kJson, body, "srv");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(1u, r.prompts.size());
  TEST_ASSERT_EQUAL_STRING("review", r.prompts[0].name.c_str());
  TEST_ASSERT_EQUAL(2u, r.prompts[0].arguments.size());
  TEST_ASSERT_TRUE(r.prompts[0].arguments[0].required);
  TEST_ASSERT_FALSE(r.prompts[0].arguments[1].required);
}

static void test_parse_prompts_get_flattens_roles() {
  // messages carry content as a single block OR an array of blocks.
  const char* body =
      R"({"result":{"description":"d","messages":[{"role":"user","content":{"type":"text","text":"hi"}},{"role":"assistant","content":[{"type":"text","text":"hello"},{"type":"image","data":"x"}]}]}})";
  PromptGetResult r = parsePromptsGet(200, kJson, body, "srv");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("d", r.description.c_str());
  TEST_ASSERT_EQUAL_STRING("user: hi\nassistant: hello [image content]", r.text.c_str());
}

// A tools/list error still routes through the shared front-end (Unauthorized etc).
static void test_rich_parsers_share_error_frontend() {
  ResourcesListResult r = parseResourcesList(401, kJson, "", "srv");
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Unauthorized, r.error);
  PromptsListResult p = parsePromptsList(500, kJson, "oops", "srv");
  TEST_ASSERT_FALSE(p.ok);
  TEST_ASSERT_EQUAL(ErrorKind::Http, p.error);
}

static void test_parse_notification_kinds() {
  // list_changed over SSE
  ServerNotification n1 =
      parseServerNotification(kSse, sse(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})"));
  TEST_ASSERT_EQUAL((int)NotifyKind::ToolsListChanged, (int)n1.kind);
  TEST_ASSERT_TRUE(isListChanged(n1.kind));
  // progress over JSON, numeric token
  ServerNotification n2 = parseServerNotification(
      kJson,
      R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":7,"progress":3,"total":10}})");
  TEST_ASSERT_EQUAL((int)NotifyKind::Progress, (int)n2.kind);
  TEST_ASSERT_EQUAL_STRING("7", n2.progressToken.c_str());
  TEST_ASSERT_EQUAL(3.0, n2.progress);
  TEST_ASSERT_EQUAL(10.0, n2.total);
  TEST_ASSERT_FALSE(isListChanged(n2.kind));
  // resources/updated carries the uri
  ServerNotification n3 = parseServerNotification(
      kJson, R"({"jsonrpc":"2.0","method":"notifications/resources/updated","params":{"uri":"file:///a"}})");
  TEST_ASSERT_EQUAL((int)NotifyKind::ResourceUpdated, (int)n3.kind);
  TEST_ASSERT_EQUAL_STRING("file:///a", n3.uri.c_str());
  // a plain RESULT is not a notification
  ServerNotification n4 = parseServerNotification(kJson, R"({"jsonrpc":"2.0","id":2,"result":{"tools":[]}})");
  TEST_ASSERT_EQUAL((int)NotifyKind::None, (int)n4.kind);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_real_server_initialize);
  RUN_TEST(test_real_server_tools_list);
  RUN_TEST(test_real_server_call_echo);
  RUN_TEST(test_real_server_tool_namespacing);
  RUN_TEST(test_real_linear_initialize);
  RUN_TEST(test_real_linear_tools_slice);
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
  RUN_TEST(test_content_type_mismatch_falls_back);
  RUN_TEST(test_timeout_error_copy);
  RUN_TEST(test_slugify_server);
  RUN_TEST(test_namespaced_tool_is_wire_safe);
  RUN_TEST(test_namespace_helpers);
  RUN_TEST(test_rich_capability_flags_everything);
  RUN_TEST(test_rich_capability_flags_linear);
  RUN_TEST(test_build_paginated_requests);
  RUN_TEST(test_build_resources_read_and_prompts_get);
  RUN_TEST(test_parse_resources_list_and_pagination);
  RUN_TEST(test_parse_resource_templates);
  RUN_TEST(test_parse_resources_read_text_and_blob);
  RUN_TEST(test_parse_prompts_list);
  RUN_TEST(test_parse_prompts_get_flattens_roles);
  RUN_TEST(test_rich_parsers_share_error_frontend);
  RUN_TEST(test_parse_notification_kinds);
  return UNITY_END();
}
