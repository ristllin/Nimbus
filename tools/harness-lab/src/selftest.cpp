// nimbus-lab selftest - offline checks of the lab's own plumbing (no keys, no
// network): the run pins, the wire rewrite, the recorder's scrub and the fault
// chain. `make check` runs it, so the battery covers the lab's seams too.
#include "selftest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lab_files.h"
#include "lab_options.h"
#include "lab_text.h"
#include "lab_wire.h"
#include "lane_mg_live.h"
#include "nimbus/harness/fault_transport.h"
#include "nimbus/orch/turn_effects.h"

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

  // A pin that changes the body's length must move the splices with it: on 31 Oct the
  // effort pin left an inline photo's base64 3 bytes late, outside its JSON string, and
  // 14 of 62 bench turns died with HTTP 400 (reported then as a progressive-JPEG defect).
  {
    const uint8_t a[4] = {1, 2, 3, 4}, b[5] = {5, 6, 7, 8, 9};
    agent::HttpRequest r;
    r.method = "POST";
    r.host = "api.anthropic.com";
    r.path = "/v1/messages";
    r.body = R"({"model":"m","output_config":{"effort":"medium"},"messages":[{"data":""},{"data":""}]})";
    const size_t at1 = r.body.find(R"("data":")") + 8;
    const size_t at2 = r.body.find(R"("data":")", at1) + 8;
    r.splices.push_back(agent::BodySplice{at1, a, sizeof(a), true});
    r.splices.push_back(agent::BodySplice{at2, b, sizeof(b), true});
    WirePins lowOnly{"low", 0, 0};
    WireOverrideTransport pinned(&echo, &lowOnly);
    pinned.exec(r, out, err);
    const std::string wire = agent::materializeBody(echo.seen.back());
    JsonDocument dd;
    check(!deserializeJson(dd, wire), "a pinned body with photos spliced in is still valid JSON");
    check(std::string(dd["output_config"]["effort"] | "") == "low", "the pin landed");
    check(std::string(dd["messages"][0]["data"] | "") == "AQIDBA==", "the first photo is in its own string");
    check(std::string(dd["messages"][1]["data"] | "") == "BQYHCAk=", "the second photo is in its own string");
    check(echo.seen.back().splices.size() == 2, "the splices survive the rewrite");
  }

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

// The lab's keyed files.append reads as a replacement to the device's claim check, so a
// lab run judges a keyed write the way the firmware does. Lane UPSERT: only a correction
// of the SAME item replaces; a different item under a reused key is appended under a new
// key and records no replacement.
void testKeyedReply() {
  std::puts("keyed reply");
  namespace orch = nimbus::orch;
  LabFiles files;
  orch::ToolRegistry reg;
  files.registerTools(reg);
  auto keyed = [&](const std::string& row) {
    JsonDocument rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["id"] = 1;
    rpc["method"] = "tools/call";
    rpc["params"]["name"] = "files.append";
    JsonObject a = rpc["params"]["arguments"].to<JsonObject>();
    a["project"] = "food";
    a["name"] = "food-log.csv";
    a["text"] = "date,meal,items,kcal,protein_g\n" + row + "\n";
    a["key"] = "meal";
    a["sum"] = "kcal";
    std::string in;
    serializeJson(rpc, in);
    JsonDocument out;
    deserializeJson(out, reg.handleRpc(in, orch::principalForRole("owner", orch::Role::Admin)));
    return std::string(out["result"]["content"][0]["text"] | "");
  };
  orch::TurnEffects added, replaced, rekeyed, same;
  orch::recordToolEffect(added, orch::FxKind::FileWrite, false, keyed("2026-10-30,lunch,black beans,300,10"), orch::kFxLogRow);
  check(!added.anyOk(orch::FxKind::FileReplace), "a new row replaced nothing");
  check(added.anyOkWith(orch::FxKind::FileWrite, orch::kFxLogRow), "a new row is a landed log row");
  const std::string fix = keyed("2026-10-30,lunch,black beans,320,11");
  orch::recordToolEffect(replaced, orch::FxKind::FileWrite, false, fix, orch::kFxLogRow);
  check(replaced.anyOk(orch::FxKind::FileReplace), "a keyed correction of the same item records a replacement");
  check(replaced.anyOkWith(orch::FxKind::FileWrite, orch::kFxLogRow), "a keyed correction is a landed log row");
  check(fix.find("was 300 kcal") != std::string::npos && fix.find("the replaced row's items:") != std::string::npos,
        ("the replace receipt names what it replaced: " + fix).c_str());
  const std::string other = keyed("2026-10-30,lunch,rice,250,6");
  orch::recordToolEffect(rekeyed, orch::FxKind::FileWrite, false, other, orch::kFxLogRow);
  check(!rekeyed.anyOk(orch::FxKind::FileReplace), "a different item under a reused key replaced nothing");
  check(rekeyed.anyOkWith(orch::FxKind::FileWrite, orch::kFxLogRow), "a different item under a reused key still lands a row");
  check(other.find("reused a stored key") != std::string::npos, "the receipt says the stored key was reused");
  orch::recordToolEffect(same, orch::FxKind::FileWrite, false, keyed("2026-10-30,lunch,black beans,320,11"), orch::kFxLogRow);
  check(!same.anyOk(orch::FxKind::FileReplace), "a re-send that changed nothing replaced nothing");
}

// The mg-live spawn leg's verdict, as the class (independent-review finding on
// the H18 leg): "the sub keeps its tools" is proven only by the provider's own
// run record (webRuns), never by reply text - a from-memory answer can cite a
// URL or name web_search with no tool run at all, and a stepdown that ran no
// tool proves nothing about the tools being kept.
void testMgSpawnVerdict() {
  std::puts("mg spawn verdict");
  check(lab::spawnKeepsTools(true, "Python 3.15.2 per python.org", 1),
        "a provider-recorded web run passes");
  check(lab::spawnKeepsTools(true, "Python 3.15.2 per python.org", 3),
        "several provider-recorded web runs pass");
  check(!lab::spawnKeepsTools(true, "I used the web_search tool, see https://www.python.org", 0),
        "reply text alone never proves the tool ran");
  check(!lab::spawnKeepsTools(
            true, "answer\n[note: this ran on mistral-medium-latest because mistral-large-4-0 cannot use Mistral's built-in tools]", 0),
        "a stepdown that ran no tool does not prove tools kept");
  check(!lab::spawnKeepsTools(false, "Python 3.15.2 per python.org", 2),
        "a dispatch error fails even with web runs recorded");
}


// Structurally valid UTF-8, the way Python's strict decoder reads a record: every lead
// byte is followed by exactly its continuation bytes, and nothing else is a stray byte.
bool wellFormedUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    const size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3
                       : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (len == 0 || i + len > s.size()) return false;
    for (size_t k = 1; k < len; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    i += len;
  }
  return true;
}

// REGR3E, 8 Nov 2026: a docs tool result cut at 300 bytes fell inside the 3-byte arrow
// (U+2192), run_eval.py read the transcript as UTF-8 and the whole leg stopped. The rule
// is a class, so the test sweeps every cap over every character width, not that one cut.
void testRecordCut() {
  std::puts("record cuts never split a UTF-8 character");
  const std::string arrow = "\xE2\x86\x92";      // U+2192, 3 bytes
  const std::string eacute = "\xC3\xA9";         // U+00E9, 2 bytes
  const std::string rocket = "\xF0\x9F\x9A\x80";  // U+1F680, 4 bytes
  check(wellFormedUtf8(arrow + eacute + rocket) && !wellFormedUtf8(arrow.substr(0, 2) + "..."),
        "the validator accepts whole characters and refuses a cut one");

  const std::string docs = std::string(298, 'a') + arrow + std::string(40, 'b');
  check(cutRecord(docs, 300) == std::string(298, 'a') + "...",
        "a 300 B cap inside the arrow drops the arrow whole, then adds the marker");
  check(cutRecord(std::string(297, 'a') + arrow + "bb", 300) == std::string(297, 'a') + arrow + "...",
        "a character that ends exactly on the cap is kept");
  check(cutRecord("short", 300) == "short" && capUtf8("short", 300) == "short",
        "a text under the cap is returned untouched, with no marker");
  check(capUtf8(docs, 300) == std::string(298, 'a'), "capUtf8 cuts the same way without the marker");

  bool allValid = true, allPrefix = true, allBounded = true;
  size_t points = 0;
  for (const std::string& ch : {eacute, arrow, rocket}) {
    const std::string text = "ab" + ch + "cd" + ch + ch + "e" + ch + std::string(7, 'z') + ch;
    for (size_t n = 0; n < text.size(); n++) {
      const std::string capped = capUtf8(text, n);
      const std::string cut = cutRecord(text, n);
      points++;
      allValid = allValid && wellFormedUtf8(capped) && wellFormedUtf8(cut);
      allPrefix = allPrefix && text.compare(0, capped.size(), capped) == 0 && cut == capped + "...";
      allBounded = allBounded && capped.size() <= n;
    }
  }
  std::printf("  (%zu cut points swept)\n", points);
  check(allValid, "no cap over any 2, 3 or 4 byte character leaves invalid UTF-8");
  check(allPrefix, "the kept text is always a prefix of the original");
  check(allBounded, "the kept text never exceeds the cap");
}

// The recorder writes a provider error body to errors.jsonl, cut at 4096 bytes, and
// record_to_fixtures.py reads that file as UTF-8: the same class as the transcript.
void testRecorderBodyCut() {
  std::puts("recorder error body cut (errors.jsonl)");
  namespace fs = std::filesystem;
  std::string dir = (fs::temp_directory_path() / "nimbus-lab-selftest-XXXXXX").string();
  if (!mkdtemp(dir.data())) {
    check(false, "a temp dir for the recorder");
    return;
  }
  EchoTransport echo;
  echo.status = 400;
  echo.body = std::string(4094, 'a') + "\xE2\x86\x92" + "tail";   // the arrow straddles byte 4096
  RecordingTransport rec(&echo, dir);
  agent::HttpResponse out;
  std::string err;
  rec.exec(antReq(), out, err);
  check(rec.errorRows() == 1, "a 400 from a provider host is recorded as an error row");
  std::ifstream in(dir + "/errors.jsonl", std::ios::binary);
  std::stringstream all;
  all << in.rdbuf();
  const std::string file = all.str();
  check(!file.empty() && wellFormedUtf8(file), "errors.jsonl is valid UTF-8 when the body is cut inside a character");
  JsonDocument row;
  deserializeJson(row, file);
  const std::string body = row["body"] | "";
  check(body == std::string(4094, 'a'), "the row keeps the body up to the character it could not keep whole");
  fs::remove_all(dir);
}

}  // namespace

int cmdSelftest() {
  testWirePins();
  testOptions();
  testScrub();
  testFaultChain();
  testKeyedReply();
  testMgSpawnVerdict();
  testRecordCut();
  testRecorderBodyCut();
  std::printf("%s\n", g_fail ? "LAB SELFTEST FAILED" : "lab selftest passed");
  return g_fail ? 1 : 0;
}

}  // namespace lab
