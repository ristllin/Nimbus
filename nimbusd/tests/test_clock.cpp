// test_clock - offline proof of nimbusd's one clock (lane RB-VCa).
//
//   (a) the grep gate: no file in nimbusd/src reads the wall clock or the steady
//       clock directly; only posix_platform.h (vclock) does. Its counter-test plants
//       every forbidden form in a synthetic source and requires the scanner to flag it;
//   (b) vclock: the offset is 0 and cannot move without NIMBUSD_TEST_CLOCK=1; with it,
//       advance moves the wall and the monotonic reading together, and the engine's
//       Platform sees the same clock;
//   (c) the control route: without the flag POST /test/clock does not exist (404 even
//       with the token); with it the route is token-gated, advances the clock and runs
//       one idle step before it answers, so a wake-up armed two hours out fires inside
//       that one request.
#include <dirent.h>

#include <chrono>
#include <cstdio>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../../test/support/fake_http.h"
#include "daemon_http.h"
#include "engine_thread.h"
#include "http_control.h"
#include "posix_platform.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
using harness_test::Exchange;
using harness_test::FakeHttpTransport;
namespace orch = nimbus::orch;

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---- (a) the grep gate ------------------------------------------------------

// Drop // and /* */ comments so a comment that names time(nullptr) does not trip the
// gate. String literals are kept: no daemon string spells a clock call.
static std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  bool block = false;
  for (size_t i = 0; i < src.size(); ++i) {
    if (block) {
      if (src.compare(i, 2, "*/") == 0) { block = false; ++i; }
      else if (src[i] == '\n') out += '\n';
      continue;
    }
    if (src.compare(i, 2, "/*") == 0) { block = true; ++i; continue; }
    if (src.compare(i, 2, "//") == 0) {
      while (i < src.size() && src[i] != '\n') ++i;
      if (i < src.size()) out += '\n';
      continue;
    }
    out += src[i];
  }
  return out;
}

// Every direct clock read: time(nullptr) and friends (also std:: and ::), any
// clock's now() (an alias named *Clock or *clock included), clock_gettime, gettimeofday, and the Arduino millis().
// A member or another namespace's time() (x.time(), p->time(), a::time()) is not one.
static const std::regex& clockReadRe() {
  static const std::regex re(
      R"((^|[^A-Za-z0-9_.>:])(std::|::)?time\s*\()"
      R"(|(^|[^A-Za-z0-9_])(?!vclock\b)[A-Za-z_]*[Cc]lock\s*::\s*now\b|\bclock_gettime\b|\bgettimeofday\b|\bftime\s*\(|(^|[^A-Za-z0-9_.>:])millis\s*\()");
  return re;
}

// 1-based line numbers of the direct clock reads in `src`.
static std::vector<int> clockReads(const std::string& src) {
  std::vector<int> lines;
  const std::string code = stripComments(src);
  int n = 1;
  size_t at = 0;
  while (at <= code.size()) {
    const size_t eol = code.find('\n', at);
    const std::string line = code.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    if (std::regex_search(line, clockReadRe())) lines.push_back(n);
    if (eol == std::string::npos) break;
    at = eol + 1;
    ++n;
  }
  return lines;
}

static std::string srcDir() {
  std::string f = __FILE__;
  const size_t cut = f.rfind("/tests/");
  return (cut == std::string::npos ? std::string("nimbusd") : f.substr(0, cut)) + "/src";
}

// The counter-test: each forbidden form must be flagged, each allowed form must not.
static void checkScannerCatchesEveryForm(ndtest::Ctx& c) {
  const char* const bad[] = {
      "  const auto t = time(nullptr);",
      "  x = (uint32_t)time(NULL);",
      "  y = std::time(nullptr);",
      "  z = ::time(0);",
      "  time(&t);",
      "  auto a = std::chrono::system_clock::now();",
      "  auto b = std::chrono::steady_clock::now();",
      "  auto h = std::chrono::high_resolution_clock::now();",
      "  auto s = steady_clock :: now();",
      "  using Clock = std::chrono::steady_clock; auto t = Clock::now();",
      "  auto u = myvclock::now();",
      "  clock_gettime(CLOCK_MONOTONIC, &ts);",
      "  gettimeofday(&tv, nullptr);",
      "  uint32_t m = millis();",
  };
  for (const char* b : bad) c.ok(clockReads(b).size() == 1, std::string("the gate flags: ") + b);
  const char* const fine[] = {
      "  const auto t = vclock::epoch();",
      "  const time_t t = vclock::now();",
      "  f(nimbusd::vclock::now());",
      "  const uint32_t ms = vclock::steadyMs();",
      "  std::string s = hosted::localTime(now, \"%H:%M\");",
      "  localtime_r(&t, &lt);",
      "  mktime(&lt);",
      "  rec.time(3);",
      "  rec->time(3);",
      "  other::time(3);",
      "  // time(nullptr) in a comment",
      "  /* std::chrono::steady_clock::now() */ int x = 0;",
      "  uint64_t uptime(int);",
      "  std::this_thread::sleep_for(std::chrono::seconds(1));",
  };
  for (const char* f : fine) c.ok(clockReads(f).empty(), std::string("the gate leaves alone: ") + f);
  c.eqi((long)clockReads("int a;\n\n  b = time(nullptr);\n").size(), 1, "a planted read is found once");
  const std::vector<int> planted = clockReads("int a;\n\n  b = time(nullptr);\n");
  c.eqi(planted.empty() ? 0 : planted[0], 3, "and reported on its own line");
  c.eqi((long)clockReads("/* a\n time(nullptr)\n */ x = time(nullptr);\n").size(), 1,
        "a block comment hides only what it covers");
}

// Every .h and .cpp under `dir`, sub-folders included, as paths relative to it.
static void listSources(const std::string& dir, const std::string& rel, std::vector<std::string>& out) {
  DIR* d = opendir((dir + rel).c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = rel + "/" + name;
    if (e->d_type == DT_DIR) { listSources(dir, path, out); continue; }
    const size_t dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    if (ext == ".h" || ext == ".cpp") out.push_back(path);
  }
  closedir(d);
}

static void checkNoDirectClockRead(ndtest::Ctx& c) {
  const std::string dir = srcDir();
  std::vector<std::string> files;
  listSources(dir, "", files);
  int scanned = 0, offenders = 0;
  bool sawPlatform = false;
  for (const std::string& f : files) {
    std::string text;
    if (!fsutil::readFile(dir + f, text)) continue;
    ++scanned;
    const std::vector<int> hits = clockReads(text);
    if (f == "/posix_platform.h") {
      sawPlatform = !hits.empty();   // the one clock itself reads the host clocks
      continue;
    }
    for (int line : hits) {
      ++offenders;
      std::printf("    clock read outside posix_platform.h: %s:%d\n", f.c_str() + 1, line);
    }
  }
  c.ok(scanned >= 20, "the gate scanned the whole tree under " + dir + " (" + std::to_string(scanned) + " files)");
  c.ok(sawPlatform, "the gate sees posix_platform.h's own host reads (the scanner is live on real code)");
  c.eqi(offenders, 0, "no direct wall-clock or steady-clock read outside posix_platform.h");
}

// ---- (b) vclock ---------------------------------------------------------------

static void checkClock(ndtest::Ctx& c) {
  unsetenv("NIMBUSD_TEST_CLOCK");
  c.eqi((long)vclock::offsetSec(), 0, "the offset starts at 0 (production reads the host clock)");
  c.ok(!vclock::testClockEnabled(), "no flag, no test clock");
  c.ok(!vclock::advance(60), "advance is refused without NIMBUSD_TEST_CLOCK=1");
  c.eqi((long)vclock::offsetSec(), 0, "and the clock did not move");
  setenv("NIMBUSD_TEST_CLOCK", "yes", 1);
  c.ok(!vclock::advance(60), "only the exact value 1 enables it");
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  c.ok(!vclock::advance(vclock::kMaxAdvanceSec + 1), "an advance past a year and a day is refused");
  c.eqi((long)vclock::offsetSec(), 0, "and moved nothing");

  CgroupMemory mem;
  const agent::Platform p = makePosixPlatform(&mem);
  const uint64_t e0 = vclock::epoch(), pe0 = p.nowEpoch();
  const uint32_t m0 = vclock::steadyMs(), pm0 = p.nowMs();
  c.ok(vclock::advance(3600), "with the flag an hour's advance is taken");
  c.eqi((long)vclock::offsetSec(), 3600, "the offset is the advance");
  const uint64_t e1 = vclock::epoch();
  const uint32_t m1 = vclock::steadyMs();
  c.ok(e1 - e0 >= 3600 && e1 - e0 <= 3605, "the wall reading moved an hour");
  c.ok(m1 - m0 >= 3600000u && m1 - m0 <= 3605000u, "the monotonic reading moved the same hour");
  c.ok(p.nowEpoch() - pe0 >= 3600 && p.nowMs() - pm0 >= 3600000u, "the engine's Platform reads the same clock");
  c.ok((uint64_t)vclock::now() == vclock::epoch() || (uint64_t)vclock::now() + 1 == vclock::epoch(),
       "now() is epoch() as a time_t");
  c.ok(vclock::advance(0), "a zero advance is a valid no-op");
  unsetenv("NIMBUSD_TEST_CLOCK");
  c.ok(!vclock::advance(60), "the flag gone, advance is refused again");
  c.eqi((long)vclock::offsetSec(), 3600, "and the offset already taken stays (time never runs back)");
}

// ---- (c) the control route ------------------------------------------------------

// A loopback client for one control surface: the token rides every call unless `authed`
// is false.
struct HttpOut {
  int status = 0;
  std::string body;
};

struct Client {
  int port = 0;
  std::string token;
  DaemonHttpTransport tx;

  HttpOut send(const std::string& method, const std::string& path, const std::string& body = "",
               bool authed = true) {
    agent::HttpRequest req;
    req.method = method;
    req.host = "127.0.0.1";
    req.port = port;
    req.tls = false;
    req.path = path;
    req.timeoutMs = 15000;
    if (!body.empty()) req.headers.push_back({"Content-Type", "application/json"});
    if (authed) req.headers.push_back({"Authorization", "Bearer " + token});
    req.body = body;
    agent::HttpResponse resp;
    std::string err;
    HttpOut out;
    if (tx.exec(req, resp, err)) out = HttpOut{resp.status, resp.body};
    return out;
  }
};

static const char* const kToken = "clock-test-token";

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  o.toolLoop = false;
  return o;
}

static std::string esc(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\';
    o += ch;
  }
  return o;
}

static Exchange headTurn(const std::string& reply) {
  Exchange e;
  e.expectHost = kCumuloHost;
  e.expectPathContains = "/router/openai/v1/responses";
  e.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
           "\"output_text\",\"text\":\"" + esc("{\"reply\":\"" + reply + "\",\"memory\":\"\"}") +
           "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
  return e;
}

static void checkRouteAbsentWithoutFlag(ndtest::Ctx& c) {
  unsetenv("NIMBUSD_TEST_CLOCK");
  Config cfg;
  NimbusdRig rig(cfg, opts("clk-off"));
  EngineThread eng(&rig);
  eng.start();
  HttpControl http(&eng, "127.0.0.1", 0, kToken, nullptr, &rig);
  Client cl{http.start(), kToken, {}};
  const int64_t before = vclock::offsetSec();
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":7200})").status, 404,
        "without the flag POST /test/clock is 404 even with the token");
  c.eqi(cl.send("GET", "/test/clock").status, 404, "and GET /test/clock is 404");
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":7200})", false).status, 404,
        "and without the token it is the same 404 (nothing tells the route apart from a missing page)");
  c.eqi((long)(vclock::offsetSec() - before), 0, "the clock did not move");
  // The flag alone is not enough: an ungated surface (no web token) never gets the route.
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  {
    HttpControl open(&eng, "127.0.0.1", 0, "", nullptr, &rig);
    Client ol{open.start(), "", {}};
    c.eqi(ol.send("POST", "/test/clock", R"({"advance_s":7200})", false).status, 404,
          "with the flag but no web token the route does not exist");
    open.stop();
  }
  c.eqi((long)(vclock::offsetSec() - before), 0, "and the clock did not move");
  unsetenv("NIMBUSD_TEST_CLOCK");
  // The flag set after the surface was built does not grow the route: the table is fixed at start.
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":7200})").status, 404, "a flag set after start does not add the route");
  unsetenv("NIMBUSD_TEST_CLOCK");
  http.stop();
  eng.stop();
}

static void seedWakeup(const std::string& dataDir, uint64_t at) {
  fsutil::mkdirs(dataDir + "/mem");
  std::vector<orch::LoopRecord> seed(1);
  seed[0].id = "lpclock1";
  seed[0].name = "wakeup";
  seed[0].prompt = "CHECK-AFTER-TWO-HOURS";
  seed[0].chatId = "";
  seed[0].sched.kind = orch::SchedKind::Once;
  seed[0].sched.intervalSec = 7200;
  seed[0].createdBy = orch::CreatedBy::Agent;
  seed[0].nextRun = at;
  fsutil::writeFileAtomic(dataDir + "/mem/loops.json", orch::dumpLoops(seed));
}

// Bad requests move nothing.
static void checkBadAdvances(ndtest::Ctx& c, Client& cl) {
  const int64_t before = vclock::offsetSec();
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":7300})", false).status, 401,
        "with the flag the route is token-gated (401 without it)");
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":-5})").status, 400, "a negative advance is refused (time never runs back)");
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":"soon"})").status, 400, "a non-number is refused");
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":1.5})").status, 400, "a fraction is refused");
  c.eqi(cl.send("POST", "/test/clock", "{}").status, 400, "an empty object is refused");
  c.eqi(cl.send("POST", "/test/clock", "not json").status, 400, "a body that is not JSON is refused");
  c.eqi(cl.send("POST", "/test/clock", R"({"advance_s":40000000})").status, 400,
        "an advance past a year and a day is refused");
  c.eqi(cl.send("POST", "/test/clock", "{\"to_epoch\":" + std::to_string(vclock::epoch() - 3600) + "}").status, 400,
        "a to_epoch in the past is refused");
  c.eqi((long)(vclock::offsetSec() - before), 0, "none of them moved the clock");
}

// to_epoch lands on that moment, and the page's own state shows it.
static void checkToEpoch(ndtest::Ctx& c, Client& cl) {
  const uint64_t target = vclock::epoch() + 600;
  c.eqi(cl.send("POST", "/test/clock", "{\"to_epoch\":" + std::to_string(target) + "}").status, 200,
        "POST to_epoch ten minutes ahead -> 200");
  c.ok(vclock::epoch() >= target && vclock::epoch() <= target + 3, "the clock reads that moment");
  const HttpOut st = cl.send("GET", "/api/state");
  c.eqi(st.status, 200, "GET /api/state -> 200");
  const size_t at = st.body.find("\"epoch\":");
  const uint64_t shown = at == std::string::npos ? 0 : (uint64_t)std::strtod(st.body.c_str() + at + 8, nullptr);
  c.ok(shown >= target && shown <= target + 3, "the state the page reads shows the moved clock (" + std::to_string(shown) + ")");
}

static void checkRouteAdvancesAndIdles(ndtest::Ctx& c) {
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  setenv("CUMULO_API_KEY", "cumulo_sk_test_CLOCK", 1);   // the scripted head answers through the router
  const auto o = opts("clk-on");
  seedWakeup(o.dataDir, vclock::epoch() + 7200);
  FakeHttpTransport tx;
  tx.script.push_back(headTurn("Two hours on, the report is ready."));
  Config cfg;
  NimbusdRig rig(cfg, o, &tx);
  std::string spoken;
  std::mutex spokenMu;
  rig.setDeliver([&](const std::string&, const std::string& t) {
    std::lock_guard<std::mutex> lk(spokenMu);
    spoken += t;
  });
  EngineThread eng(&rig);
  eng.start();
  HttpControl http(&eng, "127.0.0.1", 0, kToken, nullptr, &rig);
  Client cl{http.start(), kToken, {}};

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // a real idle pump or two
  c.ok(has(rig.routines().json(), "\"lpclock1\"") && tx.seen.empty(), "the wake-up is armed two hours out and has not fired");
  checkBadAdvances(c, cl);

  const int64_t before = vclock::offsetSec();
  const uint64_t idleBefore = eng.idleSteps();
  const HttpOut g = cl.send("GET", "/test/clock");
  c.ok(g.status == 200 && has(g.body, "\"offset_s\":" + std::to_string(before)), "GET /test/clock reads the offset");
  const HttpOut r = cl.send("POST", "/test/clock", R"({"advance_s":7300})");
  c.eqi(r.status, 200, "POST /test/clock advance_s=7300 -> 200");
  c.eqi((long)(vclock::offsetSec() - before), 7300, "the clock moved by the advance");
  c.ok(has(r.body, "\"offset_s\":" + std::to_string(vclock::offsetSec())) && has(r.body, "\"epoch\":"),
       "the answer carries the new offset and epoch");
  c.ok(eng.idleSteps() > idleBefore, "an idle step ran before the route answered");
  c.eqi((long)tx.seen.size(), 1, "the wake-up two hours out fired inside that one request");
  c.ok(!tx.seen.empty() && has(tx.seen[0].body, "CHECK-AFTER-TWO-HOURS"), "with its note");
  c.ok(!has(rig.routines().json(), "\"lpclock1\""), "and the one-shot is gone");
  {
    std::lock_guard<std::mutex> lk(spokenMu);
    c.ok(has(spoken, "the report is ready"), "its message reached the owner");
  }
  checkToEpoch(c, cl);
  http.stop();
  eng.stop();
  rig.setDeliver(nullptr);
  unsetenv("NIMBUSD_TEST_CLOCK");
  unsetenv("CUMULO_API_KEY");
}

// The total offset never passes kMaxOffsetSec, however many advances ask for more.
static void checkOffsetCeiling(ndtest::Ctx& c) {
  setenv("NIMBUSD_TEST_CLOCK", "1", 1);
  int refused = 0;
  for (int i = 0; i < 12; ++i) refused += vclock::advance(vclock::kMaxAdvanceSec) ? 0 : 1;
  c.ok(refused > 0, "advances past the ten-year ceiling are refused");
  c.ok(vclock::offsetSec() <= vclock::kMaxOffsetSec, "and the offset stays under it");
  unsetenv("NIMBUSD_TEST_CLOCK");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== one clock (RB-VCa, offline) ===\n");
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY",
                        "Z_AI_TOKEN", "TELEGRAM_BOT_TOKEN"})
    unsetenv(k);
  checkScannerCatchesEveryForm(c);
  checkNoDirectClockRead(c);
  checkClock(c);
  checkRouteAbsentWithoutFlag(c);
  checkRouteAdvancesAndIdles(c);
  checkOffsetCeiling(c);   // last: it leaves the clock years ahead
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  return c.failures == 0 ? 0 : 1;
}
