#include <unity.h>

#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/provider_slots.h"

// Storm leg c on the host: a provider out of credit is parked, and the owner's next
// messages make no request to it.
//
// On 22 Oct the storm suite drove a unit whose only keyed provider refused for credit
// (Anthropic's 400 "credit balance is too low", a 429 insufficient_quota, a 402). The
// first message made one request and parked the provider for every seam, but the next
// message asked the parked provider again (the head was "asked anyway" with every host
// parked) and the owner was told twice. The acceptance (PRD addendum, 21 Oct): zero
// requests to a parked provider; the owner is told once.
//
// The rig is the turn engine over the REAL fabric loop (providers::runFabricLoop on a
// fake HTTP transport) and the real park, bound like the device, plus the single-shot
// path (a tool loop without the fabric) over scripted adapters. Every recorded refusal
// shape, on every provider as the only keyed one, on both paths:
//   - the first message: one request, the provider parks, one notice;
//   - the next message: no request, one short answer that names no cause again;
//   - every later message of the episode: no request, nothing sent;
//   - the timed re-check is the only request after that; a key change is a new start;
//     an answer ends the episode.
// Counter-tests: a refused key (401) is still asked again, a paused host never is, and
// another open provider still answers.

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;

const char* const kHosts[] = {"anthropic", "openai", "mistral"};

// The words the storm suite reads as an owner notice (tests/hil/storm leg c): the
// short answer carries none of them, or the episode reached the owner twice.
const char* const kNoticeWords[] = {"credit", "billing", "quota", "payment", "paused", "plan"};

bool hasNoticeWords(const std::string& text) {
  std::string low;
  for (char c : text) low += (char)std::tolower((unsigned char)c);
  for (const char* w : kNoticeWords)
    if (low.find(w) != std::string::npos) return true;
  return false;
}

// One recorded refusal: the provider's answer (the fabric path) and the error line its
// single-shot adapter reports for it (the single-shot path).
struct Shape {
  const char* name;
  const char* host;
  const char* apiHost;
  int status;
  const char* body;
  const char* errLine;
};

const Shape kShapes[] = {
    {"anthropic credit 400", "anthropic", "api.anthropic.com", 400,
     "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"Your credit balance is too "
     "low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits.\"}}",
     "messages HTTP 400: Your credit balance is too low to access the Anthropic API."},
    {"anthropic quota 429", "anthropic", "api.anthropic.com", 429,
     "{\"error\":{\"message\":\"You exceeded your current quota, please check your plan and billing details.\","
     "\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}",
     "messages HTTP 429: You exceeded your current quota, please check your plan and billing details."},
    {"anthropic 402", "anthropic", "api.anthropic.com", 402,
     "{\"error\":{\"message\":\"Payment required: add credit to continue.\",\"code\":402}}",
     "messages HTTP 402: Payment required: add credit to continue."},
    {"openai quota 429", "openai", "api.openai.com", 429,
     "{\"error\":{\"message\":\"You exceeded your current quota, please check your plan and billing details.\","
     "\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}",
     "resp HTTP 429: You exceeded your current quota, please check your plan and billing details."},
    {"openai no credits 429", "openai", "api.openai.com", 429,
     "{\"error\":{\"message\":\"You have no credits remaining. Add credits to continue using the API.\"}}",
     "resp HTTP 429: You have no credits remaining. Add credits to continue using the API."},
    {"openai 402", "openai", "api.openai.com", 402, "{\"error\":{\"message\":\"Payment Required\"}}",
     "resp HTTP 402: Payment Required"},
    {"mistral 402", "mistral", "api.mistral.ai", 402, "{\"detail\":\"Payment Required\"}",
     "chat HTTP 402: Payment Required"},
    {"mistral quota 429", "mistral", "api.mistral.ai", 429,
     "{\"message\":\"You exceeded your current quota\",\"code\":\"insufficient_quota\"}",
     "chat HTTP 429: You exceeded your current quota (insufficient_quota)"},
};

const char* apiHostOf(const std::string& host) {
  if (host == "anthropic") return "api.anthropic.com";
  if (host == "openai") return "api.openai.com";
  return "api.mistral.ai";
}

// A served final round in each provider's wire shape (the orch_turn tool call).
std::string finalBody(const std::string& host) {
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
           "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
           "\"call_id\":\"c\",\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}]}";
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\","
         "\"tool_calls\":[{\"id\":\"abcdefghi\",\"function\":{\"name\":\"orch_turn\","
         "\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}}]}";
}

// The three head paths: the plain single shot (tool loop off), the tool loop without
// the fabric (mid-turn failover off, or a head the fabric cannot drive), the fabric.
enum class Path { SingleShot, LoopNoFabric, Fabric };
const Path kPaths[] = {Path::SingleShot, Path::LoopNoFabric, Path::Fabric};
const char* pathName(Path p) {
  return p == Path::Fabric ? "fabric" : p == Path::LoopNoFabric ? "tool loop without the fabric" : "single shot";
}

// The engine bound like the device: the real fabric loop and park over one fake
// transport whose clock and keys are the rig's, and scripted single-shot adapters.
struct ParkRig {
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  std::vector<std::string> delivered;
  std::map<std::string, int> singleShotAttempts;
  std::map<std::string, std::string> singleShotErr;   // per host: "" = served
  std::unique_ptr<TurnEngine> eng;
  bool fabric;

  ParkRig(const std::vector<std::string>& keyed, Path path) : fabric(path == Path::Fabric) {
    providers::clearHostParks();
    for (const char* h : kHosts) orch::billingEpisodeEnd(h);
    cfg.keyed = std::set<std::string>(keyed.begin(), keyed.end());
    cfg.midFail = fabric;
    cfg.toolLoop = path != Path::SingleShot;
    pd = wire.contract();
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [this](const char* h) {
      auto it = cfg.keys.find(h);
      return it == cfg.keys.end() ? std::string() : it->second;
    };
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    d.apply.deliver = d.deliver;
    d.toolSpecs = [](const orch::Principal&) {
      return std::vector<orch::ToolRegistry::Spec>{
          {"memory_search", "search the memory", "{\"type\":\"object\",\"properties\":{}}"}};
    };
    d.mcpDispatch = [](const std::string&, const orch::Principal&) {
      return std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}");
    };
    for (const char* h : kHosts) d.hosts.add(h, singleShot(h));
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string& ins, const std::string& inp,
                            std::string& out, std::string& err, const agent::HeadTools& ht,
                            orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return providers::runFabricLoop(pd, hl, ins, inp, out, err, ht, spend, notify);
    };
    d.hosts.fabricSupports = [](const std::string& h) { return providers::fabricSupports(h); };
    d.hosts.parked = [this](const std::string& h, providers::HostPark* why) {
      return providers::hostParked(pd, h, why);
    };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) {
      providers::noteHostFailed(pd, h, k, status);
    };
    d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
    eng.reset(new TurnEngine(std::move(d)));
  }

  agent::ProviderTurnFn singleShot(const std::string& host) {
    return [this, host](std::string&, const std::string&, const std::string&, std::string& out, std::string& err,
                        const agent::HeadTools*, orch::TokenUsage*) {
      singleShotAttempts[host]++;
      const std::string e = singleShotErr[host];
      if (!e.empty()) {
        err = e;
        return false;
      }
      out = "{\"reply\":\"done\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    };
  }

  // What the provider answers next, on whichever path the turn takes.
  void refuseNext(const Shape& sh) {
    if (fabric) wire.http.script.push_back({sh.apiHost, "", sh.status, sh.body});
    singleShotErr[sh.host] = sh.errLine;
  }
  void serveNext(const std::string& host) {
    if (fabric) wire.http.script.push_back({apiHostOf(host), "", 200, finalBody(host)});
    singleShotErr[host].clear();
  }

  // Requests the provider received: the fabric's wire and the single-shot adapter.
  int requestsTo(const std::string& host) const {
    int n = 0;
    for (const auto& req : wire.http.seen) n += req.host == apiHostOf(host) ? 1 : 0;
    auto it = singleShotAttempts.find(host);
    return n + (it == singleShotAttempts.end() ? 0 : it->second);
  }
  // Scripted answers nobody asked for (a request that should have gone did not).
  size_t unusedScript() const { return wire.http.script.size() - wire.http.idx; }

  void turn(const char* text = "hi") { eng->handleMessage(text, "Roy", "1001"); }
  const std::string& last() const {
    static const std::string none;
    return delivered.empty() ? none : delivered.back();
  }
};

void assertNotice(const ParkRig& r, const std::string& host, const char* what) {
  const std::string& t = r.last();
  TEST_ASSERT_TRUE_MESSAGE(t.find("says the account is out of credit") != std::string::npos, what);
  TEST_ASSERT_TRUE_MESSAGE(t.find("Add credit on its billing page") != std::string::npos, what);
  TEST_ASSERT_TRUE_MESSAGE(t.find("Your message was not answered.") != std::string::npos, what);
  const orch::ProviderSlot* ps = orch::findProviderSlot(host.c_str());
  TEST_ASSERT_TRUE_MESSAGE(ps && t.find(ps->label) != std::string::npos, what);   // names the provider
}

void assertShortAnswer(const ParkRig& r, const char* what) {
  const std::string& t = r.last();
  TEST_ASSERT_TRUE_MESSAGE(t.rfind("Your message was not answered.", 0) == 0, what);
  TEST_ASSERT_TRUE_MESSAGE(t.find("checked again in") != std::string::npos, what);
  TEST_ASSERT_FALSE_MESSAGE(hasNoticeWords(t), (std::string(what) + ": " + t).c_str());
}

}  // namespace

void setUp() { LogCapture::install(); }
void tearDown() {
  agent::hlog::setSink(nullptr);
  providers::clearHostParks();
}

// The class, over every recorded refusal shape on both head paths: the first message
// makes one request and tells the owner once; the next makes none and gets one short
// answer; every later message of the episode makes none and gets nothing.
static void test_every_refusal_shape_asks_once_and_tells_once() {
  int cases = 0;
  for (const Shape& sh : kShapes) {
    for (Path path : kPaths) {
      char what[160];
      snprintf(what, sizeof what, "%s, %s", sh.name, pathName(path));
      ParkRig r({sh.host}, path);
      r.refuseNext(sh);
      r.turn("storm c: reply with the single word pong");
      TEST_ASSERT_EQUAL_MESSAGE(1, r.requestsTo(sh.host), what);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), what);
      assertNotice(r, sh.host, what);
      orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
      const size_t n = orch::providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, r.plat.ms);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)n, what);
      TEST_ASSERT_EQUAL_MESSAGE((int)orch::ParkKind::Billing, (int)e[0].kind, what);   // parked for billing

      r.turn("storm c probe: reply pong");
      TEST_ASSERT_EQUAL_MESSAGE(1, r.requestsTo(sh.host), what);   // zero requests to the parked provider
      TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.delivered.size(), what);
      assertShortAnswer(r, what);

      for (int t = 0; t < 4; t++) r.turn("hello?");
      TEST_ASSERT_EQUAL_MESSAGE(1, r.requestsTo(sh.host), what);
      TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.delivered.size(), what);   // told once, answered once
      TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.unusedScript(), what);
      int notices = 0;
      for (const std::string& t : r.delivered) notices += hasNoticeWords(t) ? 1 : 0;
      TEST_ASSERT_EQUAL_MESSAGE(1, notices, what);
      cases++;
    }
  }
  printf("property: %d (refusal shape, head path) cases: 1 request, 1 notice, 1 short answer, then silence\n",
         cases);
}

// The rest of the episode: the timed re-check is the one request (refused again: no
// word, since the owner was told and answered), an answer ends the episode (the next
// refusal is a new episode, told once again), and a new key is a new start.
static void test_recheck_is_the_only_request_and_an_answer_ends_the_episode() {
  for (const char* host : kHosts) {
    for (Path path : kPaths) {
      char what[96];
      snprintf(what, sizeof what, "%s, %s", host, pathName(path));
      const Shape* sh = nullptr;
      for (const Shape& s : kShapes)
        if (std::string(s.host) == host && !sh) sh = &s;
      ParkRig r({host}, path);
      r.refuseNext(*sh);
      r.turn();
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.delivered.size(), what);

      r.plat.ms += providers::kParkProbeMs - 60000;   // a minute before the re-check
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(1, r.requestsTo(host), what);
      r.plat.ms += 60000;   // the re-check is due: one request, refused again, re-parked quietly
      r.refuseNext(*sh);
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(2, r.requestsTo(host), what);
      TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.delivered.size(), what);
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(2, r.requestsTo(host), what);   // parked for another whole wait

      r.plat.ms += providers::kParkProbeMs;   // credit was added: the re-check is answered
      r.serveNext(host);
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(3, r.requestsTo(host), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("done", r.last().c_str(), what);
      TEST_ASSERT_FALSE_MESSAGE(r.eng->headHost().empty(), what);
      r.refuseNext(*sh);   // out of credit again: a new episode, told once again
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(4, r.requestsTo(host), what);
      assertNotice(r, host, what);
      r.turn();
      assertShortAnswer(r, what);
      TEST_ASSERT_EQUAL_MESSAGE(4, r.requestsTo(host), what);

      r.cfg.keys[host] = std::string("sk-new-") + host;   // a new key: a new start
      r.refuseNext(*sh);
      r.turn();
      TEST_ASSERT_EQUAL_MESSAGE(5, r.requestsTo(host), what);
      assertNotice(r, host, what);
      TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.unusedScript(), what);
    }
  }
}

// A refusal the nightly upkeep met (a quiet turn holds its words and takes no latch)
// is told by the next attended message, with no request: the notice, not the short
// answer, because the owner has not heard of it yet.
static void test_a_refusal_met_at_night_is_told_by_the_next_message_without_a_request() {
  for (Path path : kPaths) {
    ParkRig r({"anthropic"}, path);
    r.refuseNext(kShapes[0]);
    r.eng->injectScheduledTurn("1001", "dream", "Dream", "dream", true);
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(0, (int)r.delivered.size());   // nothing at night
    r.turn("morning");
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
    assertNotice(r, "anthropic", pathName(path));
    r.turn("hello?");
    assertShortAnswer(r, pathName(path));
    r.eng->injectScheduledTurn("1001", "routine", "Routine", "r1", false);   // a routine is refused quietly too
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(2, (int)r.delivered.size());
  }
}

// Routines meet the park first: the first routine carries the notice, later ones say
// nothing and send nothing, and the owner's own next message still gets the short
// answer (a routine never takes it).
static void test_routines_carry_the_notice_never_the_short_answer() {
  for (Path path : kPaths) {
    ParkRig r({"openai"}, path);
    r.refuseNext(kShapes[3]);
    r.eng->injectScheduledTurn("1001", "check the weather", "Morning", "r1", false);
    TEST_ASSERT_EQUAL(1, r.requestsTo("openai"));
    TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
    assertNotice(r, "openai", pathName(path));
    for (int t = 0; t < 3; t++) r.eng->injectScheduledTurn("1001", "check the weather", "Morning", "r1", false);
    TEST_ASSERT_EQUAL(1, r.requestsTo("openai"));
    TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
    r.turn("anything new?");
    TEST_ASSERT_EQUAL(1, r.requestsTo("openai"));
    TEST_ASSERT_EQUAL(2, (int)r.delivered.size());
    assertShortAnswer(r, pathName(path));
    r.turn("hello?");
    TEST_ASSERT_EQUAL(2, (int)r.delivered.size());
  }
}

// Every keyed host out of credit (two of them): no request to either; the owner
// already heard of both. One out of credit and one paused: no request to either.
static void test_every_keyed_host_closed_sends_nothing() {
  {
    ParkRig r({"anthropic", "openai"}, Path::Fabric);
    r.refuseNext(kShapes[0]);   // anthropic refuses, the round moves to openai, which refuses too
    r.refuseNext(kShapes[3]);
    r.turn();
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(1, r.requestsTo("openai"));
    for (int t = 0; t < 3; t++) r.turn();
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(1, r.requestsTo("openai"));
    TEST_ASSERT_TRUE(LogCapture::contains("fabric: every host is out of credit or paused - no request"));
    // Both providers were told, in the one message of the turn that met them: the
    // notice names OpenAI (the turn ended there) and carries Anthropic's credit line.
    TEST_ASSERT_TRUE_MESSAGE(r.delivered[0].find("OpenAI says the account is out of credit") != std::string::npos,
                             r.delivered[0].c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.delivered[0].find("Add credit on anthropic's billing page.") != std::string::npos,
                             r.delivered[0].c_str());
    TEST_ASSERT_EQUAL(2, (int)r.delivered.size());   // then one short answer, then nothing
    TEST_ASSERT_FALSE_MESSAGE(hasNoticeWords(r.delivered[1]), r.delivered[1].c_str());
  }
  {
    ParkRig r({"anthropic", "openai"}, Path::Fabric);
    orch::providerPark().pause("openai", orch::kPauseOwner);
    r.refuseNext(kShapes[0]);
    r.turn();
    r.turn();
    r.turn();
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(0, r.requestsTo("openai"));
    TEST_ASSERT_EQUAL(2, (int)r.delivered.size());
    orch::providerPark().resume("openai", orch::kPauseOwner);
  }
}

// Counter-tests. Another open provider still answers every message (the parked one is
// skipped, not the turn). A refused key (401) is still asked again: a fixed key or
// model shows at once and nothing is billed for a refused key.
static void test_an_open_provider_answers_and_a_refused_key_is_asked_again() {
  {
    ParkRig r({"anthropic", "mistral"}, Path::Fabric);
    r.refuseNext(kShapes[0]);
    r.serveNext("mistral");
    r.turn();
    for (int t = 0; t < 3; t++) {
      r.serveNext("mistral");
      r.turn();
    }
    TEST_ASSERT_EQUAL(1, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(4, r.requestsTo("mistral"));
    TEST_ASSERT_EQUAL_STRING("done", r.last().c_str());
  }
  {
    ParkRig r({"anthropic"}, Path::Fabric);
    for (int t = 0; t < 2; t++) {
      r.wire.http.script.push_back(
          {"api.anthropic.com", "", 401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
                                         "\"message\":\"invalid x-api-key\"}}"});
      r.turn();
    }
    TEST_ASSERT_EQUAL(2, r.requestsTo("anthropic"));
    TEST_ASSERT_EQUAL(0, (int)r.unusedScript());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_refusal_shape_asks_once_and_tells_once);
  RUN_TEST(test_recheck_is_the_only_request_and_an_answer_ends_the_episode);
  RUN_TEST(test_a_refusal_met_at_night_is_told_by_the_next_message_without_a_request);
  RUN_TEST(test_routines_carry_the_notice_never_the_short_answer);
  RUN_TEST(test_every_keyed_host_closed_sends_nothing);
  RUN_TEST(test_an_open_provider_answers_and_a_refused_key_is_asked_again);
  return UNITY_END();
}
