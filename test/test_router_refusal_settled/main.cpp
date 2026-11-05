#include <unity.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "../test_router_refusal_contract/contract_rows.h"   // GENERATED from the cloud repo's contract
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/router_refusal.h"
#include "nimbus/orch/budget_command.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/router_refusals.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/voice_route.h"
#include "nimbus/stt_result.h"

// What happens after the router answered: a refusal the wire recovered is over, the park's
// reason reaches the owner's words on every path, a re-check waits its turn, and a seam
// that sent nothing keeps the re-check due. The findings of the independent review of the
// router refusal lane, each pinned where a mutation of the code can turn it red.

namespace orch = nimbus::orch;
namespace providers = agent::providers;
namespace rf = agent::router_refusal;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using orch::PaidSeam;
using orch::ParkCause;
using orch::ParkKind;
using orch::providerPark;
using orch::RouterAction;

namespace {

std::string refusalBody(const char* reason) {
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason +
         "\",\"message\":\"refused\",\"charged\":false}";
}
std::string contractBody(const char* reason) {
  for (const ContractBody& b : kContractBodies)
    if (std::strcmp(b.reason, reason) == 0) return b.json;
  return refusalBody(reason);
}
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
int linesWith(const char* needle) {
  int n = 0;
  for (const auto& l : LogCapture::lines()) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}
bool parkedCumulo(uint32_t now) {
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, now);
  for (size_t i = 0; i < n; i++)
    if (std::strcmp(e[i].provider, "cumulo") == 0) return e[i].kind != ParkKind::None;
  return false;
}
bool cumuloNoticed(uint32_t now) {
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, now);
  for (size_t i = 0; i < n; i++)
    if (std::strcmp(e[i].provider, "cumulo") == 0) return e[i].noticed;
  return false;
}

uint32_t g_tapNow = 1000;
uint32_t tapNow() { return g_tapNow; }

std::string finalResponses(const char* reply) {
  return std::string("{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
                     "\"output_text\",\"text\":\"{\\\"reply\\\":\\\"") + reply +
         "\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":3,\"output_tokens\":1}}";
}

// A head over the router, through the tap, as both heads hold it.
struct TapHead {
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  rf::RouterTap tap;
  std::vector<uint32_t> delays;
  TapHead() : tap(&wire.http, "router.test") {
    pd = wire.contract();
    pd.http = &tap;
    pd.routerBase = [] { return std::string("router.test"); };
    pd.routerKey = [] { return std::string("cumulo_fake"); };
    pd.nowMs = [this] { return wire.now; };
    pd.delayMs = [this](uint32_t ms) { delays.push_back(ms); };
    tap.setClock(tapNow);
    tap.begin();
  }
  void script(int status, const std::string& body) { wire.http.script.push_back({"", "", status, body}); }
  bool turn() {
    std::string conv, out, err;
    return providers::orchTurnOpenAI(pd, conv, "system", "hello", out, err, nullptr, nullptr);
  }
};

const char* const kClampWait =
    "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\",\"retryable\":true,"
    "\"retryAfterMs\":5000,\"maxAffordableOutputTokens\":0,\"retryHint\":\"wait\"}";

}  // namespace

void setUp() {
  LogCapture::install();
  providers::clearHostParks();
  providers::ProviderDeps none;
  providers::hostParked(none, "none");   // wires the park's outcome lines to the log
  orch::installSpendFreeze(nullptr);
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  providers::clearHostParks();
  orch::installSpendFreeze(nullptr);
}

// ---- the tap: what one attempt heard --------------------------------------------------

static void test_a_recovered_clamp_leaves_no_refusal() {
  TapHead h;
  h.script(402, contractBody("insufficient_credits_for_call"));
  h.script(200, finalResponses("short"));
  TEST_ASSERT_TRUE(h.turn());
  TEST_ASSERT_EQUAL_INT(0, h.tap.refusal().status);
  TEST_ASSERT_TRUE(h.tap.servedCapped());
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::None, (int)rf::classifyRefusal(h.tap.refusal()));
}

// A short wait resends the same request: served, but nothing was kept short.
static void test_a_recovered_wait_is_not_a_capped_reply() {
  for (const std::string& body : {std::string(kClampWait), contractBody("credits_held_in_flight")}) {
    TapHead h;
    h.script(body == kClampWait ? 402 : 429, body);
    h.script(200, finalResponses("whole"));
    TEST_ASSERT_TRUE(h.turn());
    TEST_ASSERT_EQUAL_INT(0, h.tap.refusal().status);
    TEST_ASSERT_FALSE_MESSAGE(h.tap.servedCapped(), body.c_str());
    TEST_ASSERT_EQUAL_UINT(1, h.delays.size());
    TEST_ASSERT_EQUAL_STRING(h.wire.http.seen[0].body.c_str(), h.wire.http.seen[1].body.c_str());
  }
}

// The class: whatever refuses the resend is what the attempt heard, read exactly as it would
// be with no clamp in front of it, and still the attempt's first request (no tool ran). A
// fail_over reason cools its upstream. Every row of the contract.
static void test_the_resends_refusal_is_the_attempts_over_the_whole_table() {
  for (const ContractRow& c : kContractRows) {
    TapHead h;
    g_tapNow = 1000;
    h.script(402, contractBody("insufficient_credits_for_call"));
    h.script(c.http, contractBody(c.reason));
    TEST_ASSERT_FALSE_MESSAGE(h.turn(), c.reason);
    const rf::RouterRefusal& got = h.tap.refusal();
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, got.reason.c_str(), c.reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.http, got.status, c.reason);
    TEST_ASSERT_TRUE_MESSAGE(got.firstRequest, c.reason);
    TEST_ASSERT_FALSE_MESSAGE(h.tap.servedCapped(), c.reason);
    rf::RouterRefusal alone;
    alone.status = c.http;
    alone.reason = c.reason;
    alone.firstRequest = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)rf::classifyRefusal(alone), (int)rf::classifyRefusal(got), c.reason);
    if (orch::routerReasonRow(c.reason)->action == RouterAction::FailOver && contractBody(c.reason).find("retryAfterMs") != std::string::npos)
      TEST_ASSERT_TRUE_MESSAGE(h.tap.cooling("openai/gpt-6"), c.reason);
    providers::clearHostParks();
  }
}

// A resend that got no answer at all is not a refusal, and not the clamp either.
static void test_no_answer_on_the_resend_is_not_the_clamp() {
  TapHead h;
  h.script(402, contractBody("insufficient_credits_for_call"));
  h.wire.http.script.push_back(harness_test::transportCut(0));
  TEST_ASSERT_FALSE(h.turn());
  TEST_ASSERT_EQUAL_INT(0, h.tap.refusal().status);
  TEST_ASSERT_FALSE(h.tap.servedCapped());
}

// A later round of the attempt: its refusal counts as itself, after tools ran, and cools.
static void test_a_later_rounds_refusal_is_not_the_first_request() {
  FakeProviderDeps wire;
  rf::RouterTap tap(&wire.http, "router.test");
  tap.setClock(tapNow);
  tap.begin();
  wire.http.script.push_back({"", "", 402, contractBody("insufficient_credits_for_call")});
  wire.http.script.push_back({"", "", 200, "{}"});
  wire.http.script.push_back({"", "", 429, contractBody("upstream_quota_exhausted")});
  agent::HttpRequest req;
  req.host = "router.test";
  req.path = "/router/openai/v1/responses";
  agent::HttpResponse resp;
  std::string err;
  tap.exec(req, resp, err);
  TEST_ASSERT_TRUE(tap.refusal().firstRequest);
  req.cappedResend = true;
  tap.exec(req, resp, err);
  TEST_ASSERT_EQUAL_INT(0, tap.refusal().status);
  TEST_ASSERT_TRUE(tap.servedCapped());
  req.cappedResend = false;
  tap.exec(req, resp, err);
  TEST_ASSERT_EQUAL_STRING("upstream_quota_exhausted", tap.refusal().reason.c_str());
  TEST_ASSERT_FALSE(tap.refusal().firstRequest);
  TEST_ASSERT_TRUE(tap.cooling("openai/gpt-6"));
  TEST_ASSERT_TRUE(tap.servedCapped());   // this attempt was capped; begin() starts a new one
  tap.begin();
  TEST_ASSERT_FALSE(tap.servedCapped());
}

// A refusal the wire does not resend still stands while the attempt runs on: the first one
// names the attempt, as before. A different path is not its resend.
static void test_a_refusal_with_no_resend_stands() {
  FakeProviderDeps wire;
  rf::RouterTap tap(&wire.http, "router.test");
  tap.begin();
  wire.http.script.push_back({"", "", 403, contractBody("model_not_priced")});
  wire.http.script.push_back({"", "", 200, "{}"});
  wire.http.script.push_back({"", "", 402, contractBody("insufficient_credits_for_call")});
  wire.http.script.push_back({"", "", 200, "{}"});
  agent::HttpRequest req;
  req.host = "router.test";
  req.path = "/router/openai/v1/responses";
  agent::HttpResponse resp;
  std::string err;
  tap.exec(req, resp, err);
  tap.exec(req, resp, err);
  TEST_ASSERT_EQUAL_STRING("model_not_priced", tap.refusal().reason.c_str());
  tap.begin();
  tap.exec(req, resp, err);
  req.path = "/router/openai/v1/other";
  tap.exec(req, resp, err);
  TEST_ASSERT_EQUAL_STRING("insufficient_credits_for_call", tap.refusal().reason.c_str());
}

// ---- a body with no reason ------------------------------------------------------------

static void test_a_status_with_no_reason() {
  rf::RouterRefusal r;
  for (int s : {402, 403}) {
    r.status = s;
    TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::Refused, (int)rf::classifyRefusal(r));
  }
  r.status = 401;
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::Auth, (int)rf::classifyRefusal(r));
  r.status = 429;
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::RateLimit, (int)rf::classifyRefusal(r));
  TEST_ASSERT_EQUAL_INT((int)orch::StepErr::Rejected,
                        (int)orch::classifyStepError(rf::refusalCause(rf::RefusalKind::Refused, "x")));
}

// The engine's failed hook types a step from a status alone, so it cannot park Cumulo: the
// wire heard the body. Any other provider's typed refusal still parks.
static void test_a_typed_step_never_parks_cumulo() {
  providers::ProviderDeps pd;
  uint32_t now = 1000;
  pd.nowMs = [&now] { return now; };
  for (orch::StepErr k : {orch::StepErr::Auth, orch::StepErr::Billing}) {
    providers::noteHostFailed(pd, "cumulo", k, k == orch::StepErr::Auth ? 403 : 402);
    TEST_ASSERT_FALSE(parkedCumulo(now));
  }
  providers::noteHostFailed(pd, "openai", orch::StepErr::Auth, 403);
  TEST_ASSERT_TRUE(providerPark().isParked("openai"));
  // A due re-check of Cumulo that failed with a typed kind still re-arms.
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), now);
  now += orch::ProviderPark::kRecheckMs + 1;
  TEST_ASSERT_TRUE(providerPark().claimProbe("cumulo", PaidSeam::Chat, now));
  providers::noteHostFailed(pd, "cumulo", orch::StepErr::Auth, 403);
  TEST_ASSERT_EQUAL_INT(1, linesWith("cumulo re-check failed without a verdict"));
  TEST_ASSERT_TRUE(providerPark().recheckInMs("cumulo") > 0);
}

// ---- what ends a park -----------------------------------------------------------------

// The router's order is entitlement, wallet, the key's budget, upstream. Every row of the
// contract against every named park: only an answer from past what the park is about ends it.
static void test_what_ends_a_park_over_the_whole_table() {
  const struct {
    const char* parkedBy;
    int status;
    ParkCause cause;
  } parks[] = {
      {"insufficient_credits", 402, ParkCause::WalletEmpty},
      {"credits_or_subscription_required", 402, ParkCause::NeedsPlan},
      {"account_suspended", 403, ParkCause::Suspended},
      {"budget_exceeded", 402, ParkCause::KeyBudget},
  };
  const char* const wallet[] = {"insufficient_credits_for_call", "credits_held_in_flight"};
  const char* const upstream[] = {"upstream_rate_limited", "upstream_quota_exhausted", "upstream_auth_failed", "upstream_error",
                                  "fallback_exhausted", "provider_timeout", "provider_unreachable"};
  for (const auto& p : parks) {
    for (const ContractRow& c : kContractRows) {
      providers::clearHostParks();
      providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, p.status, refusalBody(p.parkedBy), 1000);
      TEST_ASSERT_TRUE(providerPark().noticeDue("cumulo"));
      providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, c.http, contractBody(c.reason), 2000);
      bool pastWallet = false, pastBudget = false;
      for (const char* w : wallet) pastWallet = pastWallet || std::strcmp(c.reason, w) == 0;
      for (const char* u : upstream) pastBudget = pastBudget || std::strcmp(c.reason, u) == 0;
      const bool ends = pastBudget || (pastWallet && p.cause != ParkCause::KeyBudget);
      std::string msg = std::string(p.parkedBy) + " then " + c.reason;
      TEST_ASSERT_EQUAL_MESSAGE(ends, !parkedCumulo(3000), msg.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(!ends, cumuloNoticed(3000), msg.c_str());   // the latch ends with the episode, not before
    }
  }
}

// The wire: a re-check of a budget park that the clamp answers first. The capped resend's
// answer is the verdict, one outcome line says so, and the owner's one notice stands.
static void test_a_budget_recheck_through_the_clamp() {
  TapHead h;
  h.wire.now = 1000;
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("budget_exceeded"), h.wire.now);
  TEST_ASSERT_TRUE(providerPark().noticeDue("cumulo"));
  h.wire.now += orch::ProviderPark::kRecheckMs + 1;
  h.script(402, contractBody("insufficient_credits_for_call"));
  h.script(402, contractBody("budget_exceeded"));
  TEST_ASSERT_FALSE(h.turn());
  TEST_ASSERT_TRUE(parkedCumulo(h.wire.now));
  TEST_ASSERT_EQUAL_INT(0, linesWith("answered - open again"));
  TEST_ASSERT_EQUAL_INT(0, linesWith("failed without a verdict"));
  TEST_ASSERT_EQUAL_INT(1, linesWith("cumulo re-check refused again (billing, HTTP 402)"));
  TEST_ASSERT_TRUE(cumuloNoticed(h.wire.now));
  TEST_ASSERT_EQUAL_INT((int)ParkCause::KeyBudget, (int)providerPark().causeOf("cumulo"));

  // No cap fits, so nothing more goes out: no verdict on the budget, a new wait, told once.
  providers::clearHostParks();
  LogCapture::install();
  TapHead n;
  n.wire.now = 1000;
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("budget_exceeded"), n.wire.now);
  TEST_ASSERT_TRUE(providerPark().noticeDue("cumulo"));
  n.wire.now += orch::ProviderPark::kRecheckMs + 1;
  n.script(402, "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\",\"maxAffordableOutputTokens\":0}");
  TEST_ASSERT_FALSE(n.turn());
  TEST_ASSERT_TRUE(parkedCumulo(n.wire.now));
  TEST_ASSERT_EQUAL_INT(1, linesWith("cumulo re-check failed without a verdict"));
  TEST_ASSERT_TRUE(cumuloNoticed(n.wire.now));
}

// The first read after a restart loads the saved wait against the real clock, whichever
// question comes first. A wait read against clock 0 would already have run out.
static void test_the_first_read_loads_against_the_real_clock() {
  orch::ProviderPark park;
  orch::ParkStorage st;
  st.load = [](std::string& blob) {
    blob = "PK1\ncumulo 1 402 255 1800 0 0 1 0 00000000 3\n";
    return 1;
  };
  st.nowMs = [] { return (uint32_t)5000000; };
  park.setStorage(st);
  TEST_ASSERT_TRUE(park.isParked("cumulo"));
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  TEST_ASSERT_EQUAL_UINT(1, park.snapshot(e, orch::ProviderPark::kMaxProviders, 5000000));
  TEST_ASSERT_EQUAL_UINT32(1800000, e[0].recheckInMs);
}

// ---- the idle re-check's gates --------------------------------------------------------

namespace {
// Cumulo parked and due, Mistral answering every turn: a paid request nobody asked for
// only goes out when nothing stands against it.
struct GateRig {
  FakePlatform plat;
  FakeConfig cfg;
  providers::ProviderDeps pd;
  int cumuloCalls = 0;
  std::function<void()> duringMistral;
  std::vector<std::string> delivered;
  std::string cumuloErr, mistralErr;
  std::unique_ptr<agent::TurnEngine> eng;
  explicit GateRig(const char* priority = "mistral,cumulo", std::set<std::string> keyed = {"mistral", "cumulo"}) {
    cfg.keyed = std::move(keyed);
    cfg.keys["cumulo"] = "cumulo_fake";
    cfg.priority = priority;
    cfg.toolLoop = false;
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [this](const char* h) { return cfg.keys[h]; };
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    d.apply.deliver = d.deliver;
    d.hosts.add("mistral", [this](std::string&, const std::string&, const std::string&, std::string& out, std::string& err,
                                  const agent::HeadTools*, orch::TokenUsage*) {
      if (duringMistral) duringMistral();
      if (!mistralErr.empty()) {
        err = mistralErr;
        return false;
      }
      out = "{\"reply\":\"on mistral\"}";
      return true;
    });
    d.hosts.add("cumulo", [this](std::string&, const std::string&, const std::string&, std::string& out, std::string& err,
                                 const agent::HeadTools*, orch::TokenUsage*) {
      ++cumuloCalls;
      if (!cumuloErr.empty()) {
        err = cumuloErr;
        return false;
      }
      out = "{\"reply\":\"ok\"}";
      return true;
    });
    d.hosts.parked = [this](const std::string& h, providers::HostPark* why) { return providers::hostParked(pd, h, why); };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) { providers::noteHostFailed(pd, h, k, status); };
    d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
  void parkDue() {
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), plat.ms);
    plat.ms += orch::ProviderPark::kRecheckMs + 1;
  }
  void parkNow(const char* reason, int status) {
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, status, refusalBody(reason), plat.ms);
  }
  std::string allDelivered() const {
    std::string all;
    for (const std::string& t : delivered) all += t + "\n";
    return all;
  }
};
bool g_frozen = false;
bool frozenNow() { return g_frozen; }
}  // namespace

static void test_the_spend_ceiling_stops_the_idle_recheck() {
  GateRig r;
  r.parkDue();
  g_frozen = true;
  orch::installSpendFreeze(frozenNow);
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());
  TEST_ASSERT_EQUAL_INT(0, r.cumuloCalls);
  g_frozen = false;
  TEST_ASSERT_TRUE(r.eng->recheckParkedHost());   // the same state with the gate lifted
  TEST_ASSERT_EQUAL_INT(1, r.cumuloCalls);
}

static void test_the_provider_budget_stops_the_idle_recheck() {
  GateRig r;
  r.parkDue();
  r.cfg.overBudgetHosts = {"cumulo"};
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());
  TEST_ASSERT_EQUAL_INT(0, r.cumuloCalls);
  r.cfg.overBudgetHosts.clear();
  TEST_ASSERT_TRUE(r.eng->recheckParkedHost());
}

static void test_low_memory_stops_the_idle_recheck() {
  GateRig r;
  r.parkDue();
  r.plat.heap = 100;
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());
  r.plat.heap = 100000;
  r.plat.largest = 100;
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());
  TEST_ASSERT_EQUAL_INT(0, r.cumuloCalls);
  r.plat.largest = 100000;
  TEST_ASSERT_TRUE(r.eng->recheckParkedHost());
}

static void test_a_running_turn_stops_the_idle_recheck() {
  GateRig r;
  r.parkDue();
  bool asked = false, sent = true;
  r.duringMistral = [&] {
    asked = true;
    sent = r.eng->recheckParkedHost();
  };
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_TRUE(asked);
  TEST_ASSERT_FALSE(sent);
  TEST_ASSERT_EQUAL_INT(0, r.cumuloCalls);
}

// ---- the owner's words on the engine's paths ------------------------------------------

namespace {
const struct NamedPark {
  const char* reason;
  int status;
  const char* says;
  const char* fix;
} kNamedParks[] = {
    {"insufficient_credits", 402, "says the credit balance is empty", "Add credits in the portal"},
    {"credits_or_subscription_required", 402, "needs credits or a plan", "Add credits in the portal"},
    {"budget_exceeded", 402, "says this device reached its monthly limit", "Raise the limit in the portal"},
    {"account_suspended", 403, "says this account is suspended", "Contact support"},
};
void assertNotTheOldWords(const std::string& t) {
  TEST_ASSERT_FALSE_MESSAGE(has(t, "out of credit"), t.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(t, "billing page"), t.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(t, "rejected its key"), t.c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(t, "Assistant > Models"), t.c_str());
}
}  // namespace

// Cumulo is the only host and the park holds: the owner's one notice is the router's reason.
static void test_the_parked_head_notice_names_the_reason() {
  for (const NamedPark& p : kNamedParks) {
    providers::clearHostParks();
    GateRig r("cumulo", {"cumulo"});
    r.parkNow(p.reason, p.status);
    r.eng->handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.cumuloCalls, p.reason);
    const std::string all = r.allDelivered();
    TEST_ASSERT_TRUE_MESSAGE(has(all, p.says), all.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(all, p.fix), all.c_str());
    assertNotTheOldWords(all);
  }
}

// Cumulo first in the list and parked, another head open: the turn starts there and says
// why, in the router's words.
static void test_the_alt_start_notice_names_the_reason() {
  for (const NamedPark& p : kNamedParks) {
    providers::clearHostParks();
    GateRig r("cumulo,mistral");
    r.parkNow(p.reason, p.status);
    r.eng->handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.cumuloCalls, p.reason);
    const std::string all = r.allDelivered();
    TEST_ASSERT_TRUE_MESSAGE(has(all, "on mistral"), all.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(all, p.says), all.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(all, "until then, mistral answers."), all.c_str());
    assertNotTheOldWords(all);
  }
}

// That turn then fails too: the notice folds into the failure's one message, and its fix line
// is the router's, not the billing page.
static void test_the_folded_notice_keeps_the_fix() {
  for (const NamedPark& p : kNamedParks) {
    providers::clearHostParks();
    GateRig r("cumulo,mistral");
    r.parkNow(p.reason, p.status);
    r.mistralErr = "chat HTTP 429: slow down";
    r.eng->handleMessage("hi", "Roy", "1001");
    const std::string all = r.allDelivered();
    TEST_ASSERT_TRUE_MESSAGE(has(all, p.fix), all.c_str());
    assertNotTheOldWords(all);
  }
}

// /budget says why Cumulo is paused: a device limit or a suspension is not out of credit.
static void test_the_budget_text_names_the_reason() {
  const struct {
    const char* reason;
    int status;
    const char* says;
  } rows[] = {
      {"budget_exceeded", 402, "its device limit was reached"},
      {"account_suspended", 403, "the account is suspended"},
      {"insufficient_credits", 402, "out of credit"},
  };
  for (const auto& row : rows) {
    providers::clearHostParks();
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, row.status, refusalBody(row.reason), 1000);
    orch::LimitsView v;
    v.loaded = true;
    v.resetsOn = "1 December";
    const ParkKind kind = providerPark().peek("cumulo", PaidSeam::Chat, 0, 2000).kind;
    v.pauses = {{"cumulo", 0, (uint8_t)kind}};
    const std::string text = orch::usageText(v);
    TEST_ASSERT_TRUE_MESSAGE(has(text, row.says), text.c_str());
    if (std::strcmp(row.reason, "insufficient_credits") != 0) {
      TEST_ASSERT_FALSE_MESSAGE(has(text, "out of credit"), text.c_str());
      TEST_ASSERT_FALSE_MESSAGE(has(text, "its key was refused"), text.c_str());
    }
  }
}

// ---- the voice seams ------------------------------------------------------------------

// The TTS seam reads 160 bytes of a refusal. Every body of the contract, cut there: the
// reason still reaches the words, and "add credits" stays with the wallet.
static void test_tts_words_from_a_cut_body() {
  for (const ContractRow& c : kContractRows) {
    const std::string body = contractBody(c.reason).substr(0, 159);
    const std::string why = orch::voiceRefusalWhy(c.http, body, "");
    const bool wallet = std::strcmp(c.reason, "insufficient_credits") == 0 ||
                        std::strcmp(c.reason, "credits_or_subscription_required") == 0;
    TEST_ASSERT_EQUAL_MESSAGE(wallet, has(why, "dd credits"), why.c_str());
    if (std::strcmp(c.reason, "insufficient_credits_for_call") == 0)
      TEST_ASSERT_EQUAL_STRING("This reply is too long for the remaining credits.", why.c_str());
    else if (orch::voiceRefusalKnown(c.reason))
      TEST_ASSERT_EQUAL_STRING_MESSAGE(orch::voiceRefusalStatus(c.reason).c_str(), why.c_str(), c.reason);
    else
      TEST_ASSERT_TRUE_MESSAGE(has(why, c.reason), why.c_str());   // named, with the status
  }
  // Really cut: the contract's clamp body is longer than the seam's buffer.
  TEST_ASSERT_TRUE(contractBody("insufficient_credits").size() > 159);
  // A direct provider, or nothing readable: the status, and its code when it has one.
  TEST_ASSERT_EQUAL_STRING("the voice provider refused (HTTP 429, insufficient_quota)",
                           orch::voiceRefusalWhy(429, "{\"error\":{\"message\":\"quota\"}}", "insufficient_quota").c_str());
  TEST_ASSERT_EQUAL_STRING("the voice provider refused (HTTP 500)", orch::voiceRefusalWhy(500, "<html>", "").c_str());
}

// A speech upload that never left leaves a due re-check due. One that may have left does not.
static void test_stt_errors_that_sent_nothing() {
  TEST_ASSERT_TRUE(nimbus::voice::sttSentNothing(nimbus::voice::kErrSlotBusy));
  TEST_ASSERT_TRUE(nimbus::voice::sttSentNothing(nimbus::voice::kErrFileOpen));
  TEST_ASSERT_FALSE(nimbus::voice::sttSentNothing(nimbus::voice::kErrConnectFailed));
  TEST_ASSERT_FALSE(nimbus::voice::sttSentNothing("HTTP 0"));
  TEST_ASSERT_FALSE(nimbus::voice::sttSentNothing("HTTP 402"));
  TEST_ASSERT_FALSE(nimbus::voice::sttSentNothing(""));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_recovered_clamp_leaves_no_refusal);
  RUN_TEST(test_a_recovered_wait_is_not_a_capped_reply);
  RUN_TEST(test_the_resends_refusal_is_the_attempts_over_the_whole_table);
  RUN_TEST(test_no_answer_on_the_resend_is_not_the_clamp);
  RUN_TEST(test_a_later_rounds_refusal_is_not_the_first_request);
  RUN_TEST(test_a_refusal_with_no_resend_stands);
  RUN_TEST(test_a_status_with_no_reason);
  RUN_TEST(test_a_typed_step_never_parks_cumulo);
  RUN_TEST(test_what_ends_a_park_over_the_whole_table);
  RUN_TEST(test_a_budget_recheck_through_the_clamp);
  RUN_TEST(test_the_first_read_loads_against_the_real_clock);
  RUN_TEST(test_the_spend_ceiling_stops_the_idle_recheck);
  RUN_TEST(test_the_provider_budget_stops_the_idle_recheck);
  RUN_TEST(test_low_memory_stops_the_idle_recheck);
  RUN_TEST(test_a_running_turn_stops_the_idle_recheck);
  RUN_TEST(test_the_parked_head_notice_names_the_reason);
  RUN_TEST(test_the_alt_start_notice_names_the_reason);
  RUN_TEST(test_the_folded_notice_keeps_the_fix);
  RUN_TEST(test_the_budget_text_names_the_reason);
  RUN_TEST(test_tts_words_from_a_cut_body);
  RUN_TEST(test_stt_errors_that_sent_nothing);
  return UNITY_END();
}
