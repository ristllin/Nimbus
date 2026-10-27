#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/seam_meter.h"
#include "nimbus/orch/spend_ceiling.h"
#include "../../lib/harness/src/providers/wire.h"   // cancelRequestOf - the rule under test

// Lane QW4: stopping a running paid sub-agent is never refused, and what the job spent
// until then counts toward the monthly ceiling.
//
// The Wire gate refuses every request that starts paid work while the ceiling freezes
// paid calls. Both real cancels are POSTs (OpenAI POST /v1/responses/<id>/cancel,
// Anthropic POST /v1/sessions/<id>/events with user.interrupt), so the gate used to
// refuse them and the provider-side job kept billing; and the job engine forgets a
// cancelled job at once, so its usage was never read. Now a cancel always goes out and
// a free GET of the job reads the usage it reached, counted as the sub-agent's spend.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
namespace providers = agent::providers;
namespace orch = nimbus::orch;
namespace wire = agent::providers::wire;

namespace {

bool g_frozen = false;
bool frozenNow() { return g_frozen; }

struct Metered {
  orch::SpendSeam seam;
  std::string provider, model;
  uint32_t in = 0, out = 0, cacheR = 0;
  uint64_t estimate = 0;
};
std::vector<Metered> g_meter;

void sink(orch::SpendSeam seam, const char* provider, const orch::TurnSpend& s, uint64_t est) {
  Metered m;
  m.seam = seam;
  m.provider = provider ? provider : "";
  m.estimate = est;
  if (s.n) {
    m.model = s.slot[0].model;
    m.in = s.slot[0].in;
    m.out = s.slot[0].out;
    m.cacheR = s.slot[0].cacheR;
  }
  g_meter.push_back(m);
}

const char* kOaiJob =
    "{\"id\":\"resp_1\",\"status\":\"cancelled\",\"model\":\"gpt-6-luna\","
    "\"usage\":{\"input_tokens\":1200,\"output_tokens\":340,"
    "\"input_tokens_details\":{\"cached_tokens\":0}}}";
const char* kAntSession =
    "{\"id\":\"sesn_1\",\"status\":\"idle\",\"agent\":{\"model\":{\"id\":\"claude-haiku-5-5\"}},"
    "\"usage\":{\"input_tokens\":5000,\"output_tokens\":700,\"cache_read_input_tokens\":2000,"
    "\"server_tool_use\":{\"web_search_requests\":2,\"web_fetch_requests\":1}}}";

}  // namespace

void setUp() {
  LogCapture::install();
  g_frozen = false;
  g_meter.clear();
  orch::installSpendFreeze(&frozenNow);
  orch::installSeamMeter(&sink);
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  orch::installSpendFreeze(nullptr);
  orch::installSeamMeter(nullptr);
}

// ---- the rule ---------------------------------------------------------------------------

static void test_only_requests_that_stop_paid_work_are_cancels() {
  // (isJobPath and onlyInterrupts are the rule; the table is its contract.)
  const char* interrupt = "{\"events\":[{\"type\":\"user.interrupt\"}]}";
  struct Row {
    const char* method;
    const char* path;
    const char* body;
    wire::CancelKind want;
    const char* jobPath;
  } rows[] = {
      {"POST", "/v1/responses/resp_1/cancel", "", wire::CancelKind::Response, "/v1/responses/resp_1"},
      {"POST", "/router/openai/v1/responses/resp_9/cancel", "", wire::CancelKind::Response,
       "/router/openai/v1/responses/resp_9"},
      {"POST", "/v1/sessions/sesn_1/events", interrupt, wire::CancelKind::Session, "/v1/sessions/sesn_1"},
      {"POST", "/router/anthropic/v1/sessions/s2/events", interrupt, wire::CancelKind::Session,
       "/router/anthropic/v1/sessions/s2"},
      {"POST", "/v1/responses/resp_2/cancel?trace=1", "", wire::CancelKind::Response, "/v1/responses/resp_2"},
      {"POST", "/v1/responses/resp_2/cancel/more", "", wire::CancelKind::None, ""},
      {"POST", "/v1/responses?x=/v1/responses/r/cancel", "", wire::CancelKind::None, ""},
      // Everything that starts or continues paid work is not a cancel.
      {"POST", "/v1/responses", "{\"model\":\"m\"}", wire::CancelKind::None, ""},
      {"POST", "/v1/sessions/sesn_1/events",
       "{\"events\":[{\"type\":\"user.message\",\"content\":[]}]}", wire::CancelKind::None, ""},
      {"POST", "/v1/sessions/sesn_1/events",
       "{\"events\":[{\"type\":\"user.interrupt\"},{\"type\":\"user.message\"}]}", wire::CancelKind::None, ""},
      {"POST", "/v1/sessions/sesn_1/events", "{\"events\":[]}", wire::CancelKind::None, ""},
      {"POST", "/v1/sessions", interrupt, wire::CancelKind::None, ""},
      {"POST", "/v1/responses//cancel", "", wire::CancelKind::None, ""},
      {"POST", "/v1/responses/a/b/cancel", "", wire::CancelKind::None, ""},
      {"POST", "/v1/messages", interrupt, wire::CancelKind::None, ""},
      {"GET", "/v1/responses/resp_1/cancel", "", wire::CancelKind::None, ""},
  };
  for (const Row& r : rows) {
    const wire::CancelOf c = wire::cancelRequestOf(r.method, r.path, r.body);
    TEST_ASSERT_EQUAL_MESSAGE((int)r.want, (int)c.kind, r.path);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.jobPath, c.jobPath.c_str(), r.path);
  }
}

// ---- OpenAI ----------------------------------------------------------------------------

static void test_openai_cancel_goes_out_while_frozen_and_counts_the_job() {
  g_frozen = true;
  FakeProviderDeps d;
  d.http.script.push_back({"api.openai.com", "/v1/responses/resp_1/cancel", 200,
                           "{\"id\":\"resp_1\",\"status\":\"cancelled\",\"usage\":null}"});
  d.http.script.push_back({"api.openai.com", "/v1/responses/resp_1", 200, kOaiJob});
  auto pd = d.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_1"));
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("POST", d.http.seen[0].method.c_str());
  TEST_ASSERT_EQUAL_STRING("GET", d.http.seen[1].method.c_str());
  TEST_ASSERT_EQUAL_STRING("/v1/responses/resp_1", d.http.seen[1].path.c_str());
  TEST_ASSERT_EQUAL_STRING("Bearer sk-fake-oai", harness_test::headerOf(d.http.seen[1], "Authorization").c_str());
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_EQUAL((int)orch::SpendSeam::Sub, (int)g_meter[0].seam);
  TEST_ASSERT_EQUAL_STRING("openai", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-6-luna", g_meter[0].model.c_str());
  TEST_ASSERT_EQUAL_UINT32(1200, g_meter[0].in);
  TEST_ASSERT_EQUAL_UINT32(340, g_meter[0].out);
  // The cancel's caller reads the cancel's own outcome, not the GET's.
  TEST_ASSERT_FALSE(wire::lastExchange().spendFrozen);
}

// The live answer (26 Oct 2026, gpt-6-luna cancelled 6 s into its run): the cancel
// carries usage:null and the job reports 0/0 afterwards. Nothing billed, nothing counted.
static void test_a_job_that_reports_no_tokens_counts_nothing() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 200, "{\"status\":\"cancelled\",\"usage\":null}"});
  d.http.script.push_back({"", "/v1/responses/resp_2", 200,
                           "{\"status\":\"cancelled\",\"model\":\"gpt-6-luna\",\"usage\":"
                           "{\"input_tokens\":0,\"output_tokens\":0,\"total_tokens\":0}}"});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_2");
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
}

// A job whose usage cannot be read counts the session estimate: never zero.
static void test_an_unread_job_counts_the_estimate() {
  g_frozen = true;
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 200, "{\"status\":\"cancelled\"}"});
  d.http.script.push_back({"", "/v1/responses/resp_3", 500, "{}"});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_3");
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_EQUAL_STRING("openai", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, g_meter[0].in);
}

// A run that lists output (it worked) but reports no tokens counts the estimate; the
// web searches and the code interpreter it ran bill per call beside the tokens.
static void test_work_without_reported_tokens_and_per_call_runs_are_counted() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 200, "{\"status\":\"cancelled\"}"});
  d.http.script.push_back({"", "/v1/responses/resp_4", 200,
                           "{\"status\":\"cancelled\",\"model\":\"gpt-6-luna\",\"usage\":"
                           "{\"input_tokens\":0,\"output_tokens\":0},\"output\":["
                           "{\"type\":\"reasoning\"},{\"type\":\"web_search_call\"},"
                           "{\"type\":\"web_search_call\"},{\"type\":\"code_interpreter_call\"}]}"});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_4");
  TEST_ASSERT_EQUAL(3, (int)g_meter.size());   // tokens (estimated), searches, container
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, g_meter[0].in);
  TEST_ASSERT_EQUAL_STRING("gpt-6-luna", g_meter[0].model.c_str());
  TEST_ASSERT_EQUAL_UINT64(2ULL * orch::kProviderSearchRunMicro, g_meter[1].estimate);
  TEST_ASSERT_EQUAL_UINT64(orch::kProviderContainerMicro, g_meter[2].estimate);
}

// A job the provider no longer has (404) counts nothing: no phantom estimate. A cancel
// the provider did not confirm is not read back at all (CUM-774: the job engine keeps
// polling the job and counts it when it ends; a read-back here would count it twice).
static void test_an_unconfirmed_cancel_is_not_read_back() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 404, "{\"error\":{\"message\":\"not found\"}}"});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_5");
  TEST_ASSERT_EQUAL(1, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
  FakeProviderDeps e;   // a cancel with no answer: kept by the engine, nothing read here
  e.http.script.push_back(harness_test::transportCut(0, "no response"));
  auto pe = e.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail,
                    (int)providers::oaiCancel(pe, "api.openai.com", "sk-fake-oai", "openai:resp_6"));
  TEST_ASSERT_TRUE(wire::lastExchange().noResponse);   // the caller reads the cancel's own outcome
  TEST_ASSERT_EQUAL(1, (int)e.http.seen.size());
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());   // a cancel is free: no estimate either
}

// A job the provider no longer has when it is read back (404) counts nothing.
static void test_a_job_that_is_gone_counts_nothing() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 200, "{\"status\":\"cancelled\"}"});
  d.http.script.push_back({"", "/v1/responses/resp_7", 404, "{}"});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_7");
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
}

// Not frozen, the same: the job engine forgets a cancelled job either way.
static void test_a_cancel_is_counted_when_nothing_is_frozen_too() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/cancel", 200, "{\"status\":\"cancelled\"}"});
  d.http.script.push_back({"", "/v1/responses/resp_1", 200, kOaiJob});
  auto pd = d.contract();
  providers::oaiCancel(pd, "api.openai.com", "sk-fake-oai", "openai:resp_1");
  TEST_ASSERT_EQUAL(1, (int)g_meter.size());
  TEST_ASSERT_EQUAL_UINT32(1200, g_meter[0].in);
}

// ---- Anthropic -------------------------------------------------------------------------

static void test_anthropic_interrupt_goes_out_while_frozen_and_counts_the_session() {
  g_frozen = true;
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/sessions/sesn_1/events", 200, "{\"data\":[]}"});
  d.http.script.push_back({"", "/v1/sessions/sesn_1", 200, kAntSession});
  auto pd = d.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antCancel(pd, "anthropic:sesn_1"));
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL_STRING("GET", d.http.seen[1].method.c_str());
  TEST_ASSERT_EQUAL(2, (int)g_meter.size());   // the session's tokens, its server tool runs
  TEST_ASSERT_EQUAL((int)orch::SpendSeam::Sub, (int)g_meter[0].seam);
  TEST_ASSERT_EQUAL_STRING("anthropic", g_meter[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", g_meter[0].model.c_str());
  TEST_ASSERT_EQUAL_UINT32(5000, g_meter[0].in);
  TEST_ASSERT_EQUAL_UINT32(700, g_meter[0].out);
  TEST_ASSERT_EQUAL_UINT32(2000, g_meter[0].cacheR);
  TEST_ASSERT_EQUAL_UINT64(3ULL * orch::kProviderSearchRunMicro, g_meter[1].estimate);
}

// Round 3 (merge review blocker 2): the interrupt is processed asynchronously, and a
// session reports its tokens once it is idle. A read-back that finds it still running
// (or cannot read it) is not final: nothing is counted there and the stop reads as not
// yet confirmed, so the job engine keeps the helper on its stop list and counts the
// session's final usage when it settles (or the estimate if it never does).
static void test_an_interrupted_session_still_running_is_not_counted_yet() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/sessions/sesn_7/events", 200, "{\"data\":[]}"});
  d.http.script.push_back({"", "/v1/sessions/sesn_7", 200,
                           "{\"id\":\"sesn_7\",\"status\":\"running\",\"agent\":{\"model\":\"claude-haiku-5-5\"},"
                           "\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}"});
  auto pd = d.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail, (int)providers::antCancel(pd, "anthropic:sesn_7"));
  TEST_ASSERT_EQUAL(2, (int)d.http.seen.size());
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
  FakeProviderDeps u;   // the read itself failed: not final either
  u.http.script.push_back({"", "/v1/sessions/sesn_8/events", 200, "{\"data\":[]}"});
  u.http.script.push_back({"", "/v1/sessions/sesn_8", 503, "{}"});
  auto pu = u.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail, (int)providers::antCancel(pu, "anthropic:sesn_8"));
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
  FakeProviderDeps i;   // idle with no tokens: final, and 0 is what it billed (live, 27 Oct)
  i.http.script.push_back({"", "/v1/sessions/sesn_9/events", 200, "{\"data\":[]}"});
  i.http.script.push_back({"", "/v1/sessions/sesn_9", 200,
                           "{\"id\":\"sesn_9\",\"status\":\"idle\",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}"});
  auto pi = i.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok, (int)providers::antCancel(pi, "anthropic:sesn_9"));
}

// Round 3: the stop list trusts a final poll as its provider reports it, so the poll
// itself applies the wire's rule: a run that lists output (it worked) but reports no
// tokens counts the session estimate; one with no output and no tokens counts nothing.
static void test_a_final_poll_of_work_without_tokens_counts_the_estimate() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "/v1/responses/resp_9", 200,
                           "{\"status\":\"cancelled\",\"model\":\"gpt-6-luna\",\"usage\":"
                           "{\"input_tokens\":0,\"output_tokens\":0},\"output\":[{\"type\":\"reasoning\"}]}"});
  auto pd = d.contract();
  agent::ResultEnvelope env{};
  TEST_ASSERT_EQUAL((int)agent::FabricErr::Ok,
                    (int)providers::oaiPoll(pd, "api.openai.com", "sk-fake-oai", "openai", "openai:resp_9", env));
  TEST_ASSERT_TRUE(orch::isTerminal(env.state));
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, env.promptTokens);
  FakeProviderDeps n;
  n.http.script.push_back({"", "/v1/responses/resp_10", 200,
                           "{\"status\":\"cancelled\",\"model\":\"gpt-6-luna\",\"usage\":"
                           "{\"input_tokens\":0,\"output_tokens\":0},\"output\":[]}"});
  auto pn = n.contract();
  agent::ResultEnvelope e2{};
  providers::oaiPoll(pn, "api.openai.com", "sk-fake-oai", "openai", "openai:resp_10", e2);
  TEST_ASSERT_EQUAL_UINT32(0, e2.promptTokens);
}

// ---- what is still refused --------------------------------------------------------------

// The counter-test: while frozen, every request that starts or continues paid work is
// still refused before it leaves (an answer to a session, a new dispatch).
static void test_paid_work_is_still_refused_while_frozen() {
  g_frozen = true;
  FakeProviderDeps d;
  auto pd = d.contract();
  TEST_ASSERT_EQUAL((int)agent::FabricErr::RemoteFail,
                    (int)providers::antAnswer(pd, "anthropic:sesn_1", "carry on"));
  agent::Directive dir{};
  dir.instruction = "research tea";
  char job[72] = {};
  const agent::FabricErr e =
      providers::oaiDispatch(pd, "api.openai.com", "sk-fake-oai", "gpt-6-luna", "openai", dir, job);
  TEST_ASSERT_TRUE(e != agent::FabricErr::Ok);
  TEST_ASSERT_EQUAL(0, (int)d.http.seen.size());   // nothing left the device
  TEST_ASSERT_EQUAL(0, (int)g_meter.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_only_requests_that_stop_paid_work_are_cancels);
  RUN_TEST(test_openai_cancel_goes_out_while_frozen_and_counts_the_job);
  RUN_TEST(test_a_job_that_reports_no_tokens_counts_nothing);
  RUN_TEST(test_an_unread_job_counts_the_estimate);
  RUN_TEST(test_work_without_reported_tokens_and_per_call_runs_are_counted);
  RUN_TEST(test_an_unconfirmed_cancel_is_not_read_back);
  RUN_TEST(test_a_job_that_is_gone_counts_nothing);
  RUN_TEST(test_a_cancel_is_counted_when_nothing_is_frozen_too);
  RUN_TEST(test_anthropic_interrupt_goes_out_while_frozen_and_counts_the_session);
  RUN_TEST(test_an_interrupted_session_still_running_is_not_counted_yet);
  RUN_TEST(test_a_final_poll_of_work_without_tokens_counts_the_estimate);
  RUN_TEST(test_paid_work_is_still_refused_while_frozen);
  return UNITY_END();
}
