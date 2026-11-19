// test_hosted_stamp - RB-REC5 on a hosted instance (device parity).
//
// The hosted head's per-turn clock line is the device's one stamp (orch::day::stamp: year,
// weekday, and between midnight and the day start the day still running), byte for byte
// what the shared function gives for the instance's clock, zone and day start. The day
// start is a setting like the time zone: POST /api/config day_starts=HH:MM applies it on
// the next turn, refuses anything else with the device's plain sentence and stores
// nothing; GET /api/config and /api/state show it; a value stored invalid reads as 04:00.
// Offline: an injected FakeHttpTransport; no network, no real keys.
#include <cstdio>
#include <fstream>
#include <string>

#include "../../test/support/fake_http.h"
#include "engine_thread.h"
#include "nimbus/orch/day_start.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
namespace day = nimbus::orch::day;

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY",
                        "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options opts(const std::string& tag, bool wipe = true) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  if (wipe) ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  return o;
}

static std::string shared(uint64_t epoch, int32_t off, uint16_t start) {
  char buf[day::kFullCap];
  day::stamp(epoch, off, start, buf, sizeof buf);
  return buf;
}

// The text of "Current time: <stamp>." in a request body (JSON-escaped newline after it).
static std::string clockLine(const std::string& body) {
  const std::string key = "Current time: ";
  const size_t at = body.find(key);
  if (at == std::string::npos) return std::string();
  const size_t end = body.find(".\\n", at);
  return body.substr(at + key.size(), end == std::string::npos ? std::string::npos : end - at - key.size());
}

static ApiResp call(NimbusdRig& rig, const char* method, const char* path, const std::string& body) {
  EngineThread eng(&rig);
  eng.start();
  ReplyBuffer replies;
  WebApi api(&rig, &eng, &replies);
  ApiResp out;
  api.handle(method, path, body, out);
  eng.stop();
  return out;
}

// One turn; the stamp it carried and whether it equals the shared function's at either
// edge of the turn (a minute may tick while the turn is composed).
static bool turnStampIs(NimbusdRig& rig, FakeHttpTransport& tx, int32_t off, uint16_t start, std::string& got) {
  tx.seen.clear();
  for (int i = 0; i < 4; i++) tx.script.push_back(Exchange{"", "", 500, "{}"});
  const uint64_t before = vclock::epoch();
  rig.say("owner", "what day is it?");
  const uint64_t after = vclock::epoch();
  got = tx.seen.empty() ? std::string() : clockLine(tx.seen[0].body);
  return got == shared(before, off, start) || got == shared(after, off, start);
}

static void testTheHostedStampIsTheSharedOne(ndtest::Ctx& c) {
  std::printf("  -- the hosted clock line is the shared stamp, at every day start --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_STAMP", 1);
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("hs-stamp"), &tx);
  c.eqi(call(rig, "POST", "/api/config", "devTz=QQT-4").status, 200, "a fixed zone 4 h east saves");
  std::string got;
  c.ok(turnStampIs(rig, tx, 240, day::kDefaultStartMin, got), "no setting: the stamp at 04:00 (" + got + ")");
  c.ok(got.size() >= day::kPlainBytes && got[4] == '-' && got[10] == ' ', "the stamp starts with the year");
  c.ok(has(tx.seen.empty() ? std::string() : tx.seen[0].body, ", time zone QQT ("), "the date line names the zone");
  // 00:00 never has a clause; 23:59 has one all day but its last minute; odd starts apply.
  for (const char* v : {"00:00", "23:59", "12:00", "04:30", "00:01"}) {
    c.eqi(call(rig, "POST", "/api/config", std::string("day_starts=") + v).status, 200, std::string("day_starts=") + v + " saves");
    uint16_t m = 0;
    (void)day::parseStart(v, m);
    c.ok(turnStampIs(rig, tx, 240, m, got), std::string("the next turn's stamp at ") + v + " (" + got + ")");
    if (m == 0) c.ok(!has(got, "(day "), "00:00 never names another day");
  }
  // A blank value clears back to 04:00.
  c.eqi(call(rig, "POST", "/api/config", "day_starts=").status, 200, "a blank day start saves");
  c.ok(turnStampIs(rig, tx, 240, day::kDefaultStartMin, got), "blank reads as 04:00 (" + got + ")");
  call(rig, "POST", "/api/config", "devTz=");   // the process zone back to UTC for the rest of the run
  clearEnv();
}

static void testTheSettingIsValidatedAndShown(ndtest::Ctx& c) {
  std::printf("  -- day_starts: validated with a plain sentence, shown in config and state --\n");
  clearEnv();
  FakeHttpTransport tx;
  Config cfg;
  NimbusdRig rig(cfg, opts("hs-set"), &tx);
  c.eqi(call(rig, "POST", "/api/config", "day_starts=05:15").status, 200, "05:15 saves");
  for (const char* bad : {"25:00", "4:00", "04:60", "0400", "late", "04:00:00"}) {
    const ApiResp r = call(rig, "POST", "/api/config", std::string("day_starts=") + bad);
    c.eqi(r.status, 400, std::string("day_starts=") + bad + " is refused");
    c.ok(has(r.body, day::kStartError), std::string("with the plain sentence for ") + bad);
  }
  c.eq(rig.settings().dayStarts, "05:15", "a refused value stores nothing");
  // A refused day start refuses the whole form: a valid name beside it is not stored either.
  call(rig, "POST", "/api/config", "devName=before");
  c.eqi(call(rig, "POST", "/api/config", "devName=after&day_starts=99:99").status, 400, "a form with a bad day start");
  c.eq(rig.settings().devName, "before", "nothing of a refused form is stored");
  const ApiResp g = call(rig, "GET", "/api/config", "");
  c.ok(has(g.body, "\"day_starts\":\"05:15\""), "GET /api/config shows the setting");
  const ApiResp st = call(rig, "GET", "/api/state", "");
  c.ok(has(st.body, "\"day_starts\":\"05:15\""), "/api/state's clock shows the day start in force");
  clearEnv();
}

static void testAStoredInvalidValueReadsAs0400(ndtest::Ctx& c) {
  std::printf("  -- a stored value that is not HH:MM reads as 04:00, and survives a restart --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", "cumulo_sk_test_STAMP2", 1);
  {
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, opts("hs-bad"), &tx);
    c.eqi(call(rig, "POST", "/api/config", "day_starts=06:00").status, 200, "06:00 saves");
  }
  {
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, opts("hs-bad", false), &tx);
    c.eq(rig.settings().dayStarts, "06:00", "the day start survives a restart");
    std::string got;
    c.ok(turnStampIs(rig, tx, 0, 360, got), "and applies after it (" + got + ")");
  }
  {
    const std::string path = ndtest::scratchDir("hs-bad") + "/data/mem/settings.json";
    std::ofstream(path) << "{\"devTz\":\"\",\"day_starts\":\"7 am\"}";
    FakeHttpTransport tx;
    Config cfg;
    NimbusdRig rig(cfg, opts("hs-bad", false), &tx);
    std::string got;
    c.ok(turnStampIs(rig, tx, 0, day::kDefaultStartMin, got), "an invalid stored value reads as 04:00 (" + got + ")");
    const ApiResp st = call(rig, "GET", "/api/state", "");
    c.ok(has(st.body, "\"day_starts\":\"04:00\""), "and the state says 04:00");
  }
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted stamp and day start (RB-REC5, offline) ===\n");
  testTheHostedStampIsTheSharedOne(c);
  testTheSettingIsValidatedAndShown(c);
  testAStoredInvalidValueReadsAs0400(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
