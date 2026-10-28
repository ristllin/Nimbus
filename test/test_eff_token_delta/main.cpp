// test_eff_token_delta - the EFF lane's per-shape wire measurement (the 26 Oct
// owner analysis: every tool round re-sends the whole prompt; 13 replies cost
// 572,191 input tokens). It measures, on the REAL tool schemas over the REAL
// Anthropic loop-body renderer, what one turn's tool surface and the
// conversational round budget save:
//
//   - the trim: the session tools that need a RUNNING sub-agent leave the wire
//     on a turn with no live session (session.list + session.terminate;
//     spawn/poll/tell are loopToolHidden and never rode it). The delta is their
//     exact wire bytes - name + description + input_schema as serialized.
//   - the budget: a plain conversational turn runs at most 3 model calls (2
//     tool rounds + the forced final, + at most 1 claim correction) instead of
//     the configured 8 + final. The avoided request bytes are the measured
//     body size times the rounds not sent.
//
// The printed numbers are the evidence; the assertions pin the direction and
// the exactness so a rendering change cannot silently shrink the delta.
#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../support/fake_provider_deps.h"
#include "nimbus/harness/head_tools.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/session_tools.h"
#include "nimbus/orch/tool_registry.h"

using harness_test::FakeProviderDeps;
using agent::HeadTools;
using nimbus::orch::HeadToolCall;
using nimbus::orch::HeadToolResult;
using nimbus::orch::ToolRegistry;

void setUp() {}
void tearDown() {}

// The real session family, registered exactly as the device registers it.
static ToolRegistry realSessionRegistry() {
  ToolRegistry reg;
  nimbus::orch::SessionHandlers h;   // no handlers: "not supported" at dispatch, specs are real
  nimbus::orch::registerSessionTools(reg, h);
  return reg;
}

// One scripted exchange: round 0 answers a tool call, round 1 the final turn.
// Both shapes run the SAME exchange so only the tools[] segment differs.
static const char* kToolRound =
    "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"session_list\","
    "\"input\":{}}],\"stop_reason\":\"tool_use\","
    "\"usage\":{\"input_tokens\":100,\"output_tokens\":20}}";
static const char* kFinalRound =
    "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_2\",\"name\":\"orch_turn\","
    "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\","
    "\"usage\":{\"input_tokens\":100,\"output_tokens\":20}}";

struct Measured {
  size_t bodyBytes = 0;
  size_t toolsBytes = 0;
  size_t toolsCount = 0;
};

// Render one loop exchange over `specs` and measure the request the provider
// would have been sent (request 0: the first, largest round).
static Measured measureWire(const std::vector<ToolRegistry::Spec>& specs) {
  FakeProviderDeps d;
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, kToolRound});
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, kFinalRound});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  rig.ht.specs = specs;
  rig.ht.dispatch = [&rig](const HeadToolCall& c) {
    HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = "ok";
    return r;
  };
  rig.ht.cfg.maxRounds = 8;
  rig.ht.cfg.deadlineMs = 600000;
  rig.ht.cfg.maxToolResultBytes = 4096;
  rig.ht.cfg.maxTotalToolBytes = 24576;
  std::string conv, out, err;
  const bool ok = agent::providers::orchTurnAnthropic(pd, conv, "SYS", "USER", out, err, &rig.ht,
                                                      nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  Measured m;
  m.bodyBytes = d.http.seen[0].body.size();
  ArduinoJson::JsonDocument body;
  TEST_ASSERT_EQUAL(
      ArduinoJson::DeserializationError::Ok,
      deserializeJson(body, d.http.seen[0].body, ArduinoJson::DeserializationOption::NestingLimit(16))
          .code());
  std::string tools;
  serializeJson(body["tools"], tools);
  m.toolsBytes = tools.size();
  m.toolsCount = body["tools"].as<ArduinoJson::JsonArrayConst>().size();
  return m;
}

// The wire's advertised set from the registry, exactly as the engine builds it:
// the owner's specs minus loopToolHidden (spawn/poll/tell work through the
// session_ops field and never rode the wire), minus - on the conversational
// shape - the trim's session tools (a turn with no live session).
static std::vector<ToolRegistry::Spec> wireSpecs(const ToolRegistry& reg, bool liveSession) {
  std::vector<ToolRegistry::Spec> out;
  for (const auto& s : reg.toolSpecs()) {   // the owner (admin) sees every registration
    if (s.name == "session.tell" || s.name == "session.poll" || s.name == "session.spawn") continue;
    if (!liveSession && (s.name == "session.list" || s.name == "session.terminate")) continue;
    out.push_back(s);
  }
  return out;
}

static void test_the_trim_saves_the_session_tools_wire_bytes() {
  const ToolRegistry reg = realSessionRegistry();
  const Measured work = measureWire(wireSpecs(reg, /*liveSession=*/true));
  const Measured chat = measureWire(wireSpecs(reg, /*liveSession=*/false));
  std::printf("[eff] work turn:      body=%zuB tools=%zuB (%zu tools)\n", work.bodyBytes,
              work.toolsBytes, work.toolsCount);
  std::printf("[eff] conversational: body=%zuB tools=%zuB (%zu tools)\n", chat.bodyBytes,
              chat.toolsBytes, chat.toolsCount);
  std::printf("[eff] trim delta:    %zuB of tools[] per request, every round\n",
              work.toolsBytes - chat.toolsBytes);
  // The conversational turn runs at most 3 model calls (2 tool rounds + the
  // forced final, + at most 1 claim correction) where the unbounded loop could
  // run 9; the avoided bytes are the measured body times the rounds not sent.
  std::printf("[eff] budget:        at most 3 model calls vs up to 9; up to 6 x %zuB not sent\n",
              chat.bodyBytes);
  TEST_ASSERT_TRUE(work.toolsBytes > chat.toolsBytes);   // the trim removes real bytes
  TEST_ASSERT_EQUAL(work.toolsCount - 2, chat.toolsCount);
  TEST_ASSERT_EQUAL(work.bodyBytes - chat.bodyBytes, work.toolsBytes - chat.toolsBytes);
  TEST_ASSERT_TRUE(chat.toolsBytes > 0);   // orch_turn still rides every round (CUM-641)
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_trim_saves_the_session_tools_wire_bytes);
  return UNITY_END();
}
