#include <unity.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/orch/session_tools.h"

using namespace nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

// Recorded calls so tests can assert the handlers received the right args.
struct Recorder {
  std::string lastSpawnTask, lastSpawnProvider, lastTellId, lastTellMsg, lastPollId, lastTermId;
  bool spawnFail = false, tellFail = false;
  std::string pollReply = "hello from sub";
  std::vector<SessionInfo> sessions;
};
static Recorder g_rec;

static SessionHandlers handlers() {
  SessionHandlers h;
  h.spawn = [](const std::string& provider, const std::string&, const std::string& task, std::string& err) -> std::string {
    g_rec.lastSpawnProvider = provider; g_rec.lastSpawnTask = task;
    if (g_rec.spawnFail) { err = "no provider key"; return ""; }
    return "job0007";
  };
  h.tell = [](const std::string& id, const std::string& msg, std::string& err) -> bool {
    g_rec.lastTellId = id; g_rec.lastTellMsg = msg;
    if (g_rec.tellFail) { err = "session gone"; return false; }
    return true;
  };
  h.poll = [](const std::string& id, std::string&) -> std::string {
    g_rec.lastPollId = id; return g_rec.pollReply;
  };
  h.terminate = [](const std::string& id, std::string&) -> bool { g_rec.lastTermId = id; return true; };
  h.list = []() -> std::vector<SessionInfo> { return g_rec.sessions; };
  return h;
}

static ToolRegistry buildReg() {
  g_rec = Recorder();
  ToolRegistry reg;
  registerSessionTools(reg, handlers());
  return reg;
}

void setUp() {}
void tearDown() {}

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }
static ToolResult call(ToolRegistry& reg, const char* name, const char* argsJson) {
  JsonDocument d; deserializeJson(d, argsJson);
  return reg.dispatch(name, d.as<JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
}

static void test_all_registered() {
  ToolRegistry reg = buildReg();
  TEST_ASSERT_EQUAL_INT(5, reg.size());
  for (const char* n : {"session.spawn", "session.tell", "session.poll", "session.terminate", "session.list"})
    TEST_ASSERT_TRUE(reg.has(n));
}

static void test_spawn_passes_args_and_reports_id() {
  ToolRegistry reg = buildReg();
  ToolResult r = call(reg, "session.spawn", R"({"task":"summarize the logs","provider":"openai"})");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_TRUE(has(r.output, "job0007"));
  TEST_ASSERT_EQUAL_STRING("summarize the logs", g_rec.lastSpawnTask.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", g_rec.lastSpawnProvider.c_str());
}

static void test_spawn_missing_task_and_failure() {
  ToolRegistry reg = buildReg();
  TEST_ASSERT_FALSE(call(reg, "session.spawn", R"({})").success);  // missing task
  g_rec.spawnFail = true;
  ToolResult r = call(reg, "session.spawn", R"({"task":"x"})");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(has(r.error, "no provider key"));
}

static void test_tell_and_poll_as_user() {
  ToolRegistry reg = buildReg();
  TEST_ASSERT_TRUE(call(reg, "session.tell", R"({"id":"job0007","message":"add unit tests"})").success);
  TEST_ASSERT_EQUAL_STRING("job0007", g_rec.lastTellId.c_str());
  TEST_ASSERT_EQUAL_STRING("add unit tests", g_rec.lastTellMsg.c_str());
  ToolResult p = call(reg, "session.poll", R"({"id":"job0007"})");
  TEST_ASSERT_TRUE(p.success);
  TEST_ASSERT_TRUE(has(p.output, "hello from sub"));
}

static void test_poll_no_reply_yet() {
  ToolRegistry reg = buildReg();
  g_rec.pollReply = "";
  ToolResult p = call(reg, "session.poll", R"({"id":"job0007"})");
  TEST_ASSERT_TRUE(p.success);
  TEST_ASSERT_TRUE(has(p.output, "no reply yet"));
}

static void test_tell_needs_both_args() {
  ToolRegistry reg = buildReg();
  TEST_ASSERT_FALSE(call(reg, "session.tell", R"({"id":"job0007"})").success);   // no message
  TEST_ASSERT_FALSE(call(reg, "session.tell", R"({"message":"hi"})").success);    // no id
}

static void test_terminate_and_list() {
  ToolRegistry reg = buildReg();
  TEST_ASSERT_TRUE(call(reg, "session.terminate", R"({"id":"job0007"})").success);
  TEST_ASSERT_EQUAL_STRING("job0007", g_rec.lastTermId.c_str());

  ToolResult empty = call(reg, "session.list", R"({})");
  TEST_ASSERT_TRUE(has(empty.output, "No running sessions"));
  g_rec.sessions = {{"job0001", "openai", "gpt-5.5", "review diff", "running", 2, true}};
  ToolResult r = call(reg, "session.list", R"({})");
  TEST_ASSERT_TRUE(has(r.output, "job0001"));
  TEST_ASSERT_TRUE(has(r.output, "REPLY WAITING"));
}

static void test_missing_handler_reports_unsupported() {
  ToolRegistry reg;
  SessionHandlers h;  // all handlers null
  registerSessionTools(reg, h);
  ToolResult r = call(reg, "session.spawn", R"({"task":"x"})");
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(has(r.error, "not supported"));
}

static void test_over_mcp_rpc() {
  ToolRegistry reg = buildReg();
  std::string resp = reg.handleRpc(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
      R"("params":{"name":"session.spawn","arguments":{"task":"go"}}})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(resp, "job0007"));
  TEST_ASSERT_TRUE(has(resp, "\"isError\":false"));
  std::string list = reg.handleRpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(has(list, "session.terminate"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_all_registered);
  RUN_TEST(test_spawn_passes_args_and_reports_id);
  RUN_TEST(test_spawn_missing_task_and_failure);
  RUN_TEST(test_tell_and_poll_as_user);
  RUN_TEST(test_poll_no_reply_yet);
  RUN_TEST(test_tell_needs_both_args);
  RUN_TEST(test_terminate_and_list);
  RUN_TEST(test_missing_handler_reports_unsupported);
  RUN_TEST(test_over_mcp_rpc);
  return UNITY_END();
}
