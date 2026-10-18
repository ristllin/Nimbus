// nimbus-lab selftest - offline checks of the lab's own plumbing (no keys, no
// network): the run pins, the wire rewrite, the recorder's scrub and the fault
// chain. `make check` runs it, so the battery covers the lab's seams too.
#include "selftest.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lab_options.h"
#include "lab_wire.h"
#include "nimbus/harness/fault_transport.h"

namespace lab {
namespace {

int g_fail = 0;
void check(bool cond, const char* what) {
  std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) g_fail++;
}

// A transport that records what reached it and answers 200 with a usage object.
struct EchoTransport : agent::HttpTransport {
  std::vector<agent::HttpRequest> seen;
  std::string body = R"({"model":"test-model","usage":{"input_tokens":3,"output_tokens":1}})";
  int status = 200;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string&) override {
    seen.push_back(req);
    out.status = status;
    out.body = body;
    return true;
  }
};

agent::HttpRequest antReq() {
  agent::HttpRequest r;
  r.method = "POST";
  r.host = "api.anthropic.com";
  r.path = "/v1/messages";
  r.timeoutMs = 120000;
  // Deeper than ArduinoJson's default nesting limit of 10, like a real tool schema.
  r.body = R"({"model":"test-model","max_tokens":3000,"tools":[{"input_schema":)"
           R"({"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":{"i":{"j":1}}}}}}}}}}}],)"
           R"("output_config":{"format":{"type":"json"}}})";
  return r;
}

void testWirePins() {
  std::puts("wire pins");
  WirePins pins{"low", 512, 30000};
  EchoTransport echo;
  WireOverrideTransport w(&echo, &pins);
  agent::HttpResponse out;
  std::string err;
  w.exec(antReq(), out, err);
  JsonDocument d;
  deserializeJson(d, echo.seen.back().body, DeserializationOption::NestingLimit(64));
  check(std::string(d["output_config"]["effort"] | "") == "low", "effort reaches a deeply nested Anthropic body");
  check(std::string(d["output_config"]["format"]["type"] | "") == "json", "an existing output_config is kept");
  check((d["max_tokens"] | 0) == 512, "max tokens is pinned");
  check(echo.seen.back().timeoutMs == 30000, "the timeout is clamped to the ceiling");

  WirePins none{"", 0, 0};
  WireOverrideTransport raw(&echo, &none);
  const agent::HttpRequest orig = antReq();
  raw.exec(orig, out, err);
  check(echo.seen.back().body == orig.body, "with no pins the body is byte-identical");

  agent::HttpRequest tg;
  tg.method = "POST";
  tg.host = "api.telegram.org";
  tg.path = "/botX/sendMessage";
  tg.body = R"({"max_tokens":1})";
  w.exec(tg, out, err);
  check(echo.seen.back().body == tg.body, "a non-provider host is never rewritten");
}

void testOptions() {
  std::puts("run pins from the environment");
  LabRig::Options o;
  std::string err;
  check(applyModelPins("claude-opus-5,mistral/mistral-small-latest,glm-5.3-flash", o, err),
        "a model list parses");
  check(o.models["anthropic"] == "claude-opus-5" && o.models["mistral"] == "mistral-small-latest" &&
            o.models["zai"] == "glm-5.3-flash",
        "each model lands on its inferred host");
  check(!applyModelPins("bogus/x", o, err), "an unknown host is refused");
  check(!applyModelPins("anthropic/bad model", o, err), "a bad model token is refused");
  check(applyEffort("adapter", o, err) && o.pins.effort.empty(), "effort adapter sends the adapter's own");
  check(!applyEffort("extreme", o, err), "an unknown effort is refused");
  check(!applyMaxTokens("0", o, err) && !applyMaxTokens("x", o, err), "a bad max tokens is refused");
  check(!applyRounds("33", o, err) && applyRounds("6", o, err) && o.rounds == 6, "rounds keep the device clamp");
  check(!applyFault("anthropic:0:500", o, err), "a bad fault spec is refused");
  check(promoteHost("anthropic,mistral,zai,openai", "zai") == "zai,anthropic,mistral,openai",
        "a pinned host moves to the head of the priority");
}

void testScrub() {
  std::puts("recorder scrub");
  const std::string s = scrubSecrets(
      R"({"error":"Incorrect API key provided: sk-proj-abc123****wxyz","auth":"Bearer abcdefgh12345678","u":"https://x/?key=SECRET1&a=1"})");
  check(s.find("abc123") == std::string::npos, "a partly masked sk- key is scrubbed");
  check(s.find("abcdefgh12345678") == std::string::npos, "a bearer token is scrubbed");
  check(s.find("SECRET1") == std::string::npos, "a key query parameter is scrubbed");
}

void testFaultChain() {
  std::puts("fault chain (pins -> faults -> network)");
  EchoTransport echo;
  agent::FaultPlan plan;
  agent::FaultRule r;
  std::string why;
  agent::parseFaultRule("anthropic:2:529:overloaded", r, why);
  plan.add(r);
  agent::FaultInjectingTransport f(&echo, &plan);
  WirePins pins{"low", 0, 0};
  WireOverrideTransport w(&f, &pins);
  agent::HttpResponse a, b, c;
  std::string err;
  w.exec(antReq(), a, err);
  w.exec(antReq(), b, err);
  w.exec(antReq(), c, err);
  check(a.status == 200 && b.status == 529 && c.status == 200, "request 2 fails, 1 and 3 pass");
  check(echo.seen.size() == 2, "the injected request never reached the network");
}

}  // namespace

int cmdSelftest() {
  testWirePins();
  testOptions();
  testScrub();
  testFaultChain();
  std::printf("%s\n", g_fail ? "LAB SELFTEST FAILED" : "lab selftest passed");
  return g_fail ? 1 : 0;
}

}  // namespace lab
