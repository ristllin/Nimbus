#include <unity.h>

#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/seam_meter.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/spend_guard.h"

// Lane QW4: a helper is never dropped while it can still bill, and a spend freeze stops
// the helpers it pauses.
//   CUM-774: a cancel the provider did not confirm (no answer, a refusal) used to drop the
//   job from the journal at once, so a job still running on the provider billed with
//   nothing counting it. Now it stays tracked and polled (its spend is counted when it
//   ends), the cancel is asked again with a growing wait, and it ends as stopped (its
//   result not delivered) once the provider confirms or the job stops.
//   CUM-778 (owner ruling, defensive defaults): when the monthly spend ceiling or a
//   provider's monthly limit pauses paid calls, every helper still running on a paused
//   provider's side is stopped, and its chat is told once which helpers stopped; raising
//   the limit restarts none of them. A helper that ran inside its one request keeps its
//   result (nothing of it is still running).

using agent::JobEngine;
using harness_test::FakeAdapter;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;

namespace {

struct Store : orch::JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
};

// A helper start whose answer never came and whose stop also failed: the adapter hands
// the engine the run it could not stop (antDispatch's side channel) and reports Timeout.
struct UnseenAdapter : FakeAdapter {
  std::string unseen;   // the run id prefix ("" = dispatch normally)
  int attempts = 0;
  agent::FabricErr dispatch(const agent::Directive& d, char outJobId[72]) override {
    ++attempts;
    if (unseen.empty()) return FakeAdapter::dispatch(d, outJobId);
    agent::providers::noteUnseenRun(unseen + std::to_string(attempts));
    return agent::FabricErr::Timeout;
  }
};

struct Rig {
  FakePlatform plat;
  UnseenAdapter openai;
  FakeAdapter mistral;
  std::string saved;          // the stop list as the engine last saved it
  std::string toLoad;         // what the engine reads at its first pump
  bool saveOk = true;         // the flash write succeeds
  agent::HeavyFabric fabric;
  Store store;
  orch::Journal journal;
  std::vector<std::pair<std::string, std::string>> delivered;
  struct Usage { std::string backend; uint32_t in; };
  std::vector<Usage> usage;
  std::unique_ptr<JobEngine> eng;

  Rig() {
    openai.backend = "openai";   // runs remotely: polled, cancelled on its provider
    mistral.backend = "mistral";   // runs inside its one request
    openai.pollScript = {agent::JobState::Running};
    mistral.pollScript = {agent::JobState::Running};
    fabric.registerAdapter(&openai);
    fabric.registerAdapter(&mistral);
    journal.begin(&store);
    JobEngine::Deps d;
    d.platform = plat.contract();
    d.fabric = &fabric;
    d.journal = &journal;
    d.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
      return true;
    };
    d.subPriority = [] { return std::string("openai,mistral"); };
    d.providerHasKey = [](const std::string&) { return true; };
    d.subModel = [](const std::string& p) { return "sub-" + p; };
    d.modelIsValid = [](const std::string&, const std::string&) { return true; };
    d.recordSpawnUsage = [this](const std::string& b, const std::string&, const orch::TokenUsage& u) {
      usage.push_back({b, u.promptTokens});
    };
    d.loadStopping = [this] { return toLoad; };
    d.saveStopping = [this](const std::string& v) {
      saved = v;
      return saveOk;
    };
    eng.reset(new JobEngine(std::move(d)));
  }

  // Spawn on `provider` and dispatch it (one pump).
  void spawn(const char* provider, const char* chat = "chat1") {
    orch::Spawn s;
    s.task = "research tea";
    s.provider = provider;
    s.model = "m";
    s.category = "research";
    eng->enqueueSpawn(s, chat);
    plat.ms += 3000;
    eng->pump();
  }
  void at(uint32_t ms) {
    plat.ms = ms;
    eng->pump();
  }
};

std::string slurp(const std::string& path) {   // a source file, for the wiring checks
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

bool g_ceilingFrozen = false;
bool ceilingFrozen() { return g_ceilingFrozen; }

void unpauseAll() {
  orch::providerPark().resume(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  for (const char* p : {"openai", "mistral", "anthropic"})
    orch::providerPark().resume(p, orch::kPauseProviderLimit);
}

}  // namespace

void setUp() {
  LogCapture::install();
  unpauseAll();
  g_ceilingFrozen = false;
  orch::installSpendFreeze(&ceilingFrozen);
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  unpauseAll();
  orch::installSpendFreeze(nullptr);
}

// ---- CUM-774 ------------------------------------------------------------------------

static void test_a_confirmed_cancel_drops_the_job() {
  Rig r;
  r.spawn("openai");
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
}

static void test_an_unconfirmed_cancel_keeps_the_job_and_asks_again_with_a_growing_wait() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail, agent::FabricErr::Network, agent::FabricErr::Ok};
  r.spawn("openai");
  const uint32_t t0 = r.plat.ms;
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));   // being stopped
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());    // kept: it may still be billing
  TEST_ASSERT_TRUE(r.eng->stopping("job0000"));    // the owner is told it is not confirmed
  const int polls = r.openai.pollCount;
  r.at(t0 + 16000);                               // before the 30 s retry: polled, not cancelled
  r.at(t0 + 29000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_TRUE(r.openai.pollCount > polls);
  r.at(t0 + 30500);                               // second try fails: the wait doubles to 60 s
  TEST_ASSERT_EQUAL(2, r.openai.cancels);
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());
  r.at(t0 + 30500 + 59000);
  TEST_ASSERT_EQUAL(2, r.openai.cancels);
  r.at(t0 + 30500 + 61000);                       // third try confirmed: stopped
  TEST_ASSERT_EQUAL(3, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_FALSE(r.eng->stopping("job0000"));
}

// A cancel the provider answers with "no such job" (404): nothing is left running there,
// so the job ends as stopped at once.
static void test_a_cancel_of_a_job_the_provider_no_longer_has_ends_it() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::NotFound};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
}

// The job stopped on its own (or finished) before a cancel was confirmed: its spend is
// counted once at its end, and it ends as stopped, its result not delivered.
static void test_a_kept_job_is_counted_when_it_ends_and_its_result_is_not_delivered() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.openai.pollScript = {agent::JobState::Running, agent::JobState::Done};
  r.openai.usageIn = 1234;
  r.spawn("openai");
  const size_t before = r.delivered.size();
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  for (int i = 0; i < 10 && r.eng->stoppingCount(); ++i) r.at(r.plat.ms + 16000);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());      // counted once
  TEST_ASSERT_EQUAL_UINT32(1234, r.usage[0].in);
  TEST_ASSERT_EQUAL(before, r.delivered.size());  // the owner asked it to stop
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());
}

// A kept job whose provider answered its final poll is counted as that poll reports, 0
// included: each provider's poll counts the session estimate itself when its usage read
// fails or a run lists work with no tokens (round 3; live 27 Oct: a session stopped 8 s
// in reports 0 tokens and a 0 list cost).
static void test_a_kept_job_ending_with_no_usage_counts_what_its_provider_reports() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.openai.pollScript = {agent::JobState::Running, agent::JobState::Cancelled};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  for (int i = 0; i < 10 && r.eng->stoppingCount(); ++i) r.at(r.plat.ms + 16000);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(0, (int)r.usage.size());
}

// A kept job whose provider stops answering is never dropped at the usual 15 minutes (it
// may still be billing): it is polled and its cancel asked again for 6 hours, then counted
// at the session estimate and dropped, its chat told; its result never delivered.
static void test_a_kept_job_on_an_unreachable_provider_is_held_then_counted() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::Network};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  r.openai.pollErr = agent::FabricErr::Network;
  const size_t before = r.delivered.size();
  const uint32_t t0 = r.plat.ms;
  while (r.plat.ms - t0 < 10u * 60u * 1000u) r.at(r.plat.ms + 30000);
  TEST_ASSERT_EQUAL(0, (int)r.usage.size());       // not yet: 10 minutes of failures
  while (r.plat.ms - t0 < 60u * 60u * 1000u) r.at(r.plat.ms + 30000);   // one hour
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());    // still held
  TEST_ASSERT_TRUE(r.openai.cancels > 5);          // and still asked
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());       // counted at the estimate after 15 minutes
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, r.usage[0].in);
  while (r.eng->stoppingCount() && r.plat.ms - t0 < 7u * 3600u * 1000u) r.at(r.plat.ms + 60000);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_TRUE(r.plat.ms - t0 >= 6u * 3600u * 1000u);
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());       // never twice
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered.back().second.find("could not confirm") != std::string::npos);
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());
}

// ---- CUM-778 --------------------------------------------------------------------------

static void test_a_provider_limit_stops_its_running_helpers_and_tells_once() {
  Rig r;
  r.spawn("openai");
  r.spawn("mistral");
  TEST_ASSERT_EQUAL(2, r.eng->activeJobCount());
  const size_t before = r.delivered.size();   // the spawn notes
  orch::providerPark().pause("openai", orch::kPauseProviderLimit);
  r.at(r.plat.ms + 6000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);           // the OpenAI helper stopped
  TEST_ASSERT_EQUAL(0, r.mistral.cancels);          // the in-request one keeps its result
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("chat1", r.delivered.back().first.c_str());
  const std::string t = r.delivered.back().second;
  TEST_ASSERT_TRUE(t.find("OpenAI's monthly limit is reached") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("is stopping a running helper") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("does not restart") != std::string::npos);
  r.at(r.plat.ms + 6000);                           // told once
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());
  // Raising the limit restarts nothing.
  orch::providerPark().resume("openai", orch::kPauseProviderLimit);
  const size_t dispatched = r.openai.dispatched.size();
  r.at(r.plat.ms + 6000);
  TEST_ASSERT_EQUAL(dispatched, r.openai.dispatched.size());
}

static void test_the_ceiling_stops_every_remote_helper_in_one_message_per_chat() {
  Rig r;
  r.spawn("openai", "chat1");
  r.spawn("openai", "chat1");
  r.spawn("openai", "chat2");
  const size_t before = r.delivered.size();
  orch::providerPark().pause(orch::kPauseAllProviders, orch::kPauseSpendCeiling);
  r.at(r.plat.ms + 6000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);               // one cancel a pump (each may take a deadline)
  TEST_ASSERT_EQUAL(before + 2, r.delivered.size());    // every chat told at once, one message each
  r.at(r.plat.ms + 100);
  r.at(r.plat.ms + 100);
  TEST_ASSERT_EQUAL(3, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  TEST_ASSERT_EQUAL(before + 2, r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("chat1", r.delivered[before].first.c_str());
  TEST_ASSERT_TRUE(r.delivered[before].second.find("2 running helpers") != std::string::npos);
  TEST_ASSERT_TRUE(r.delivered[before].second.find("spend ceiling") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("chat2", r.delivered[before + 1].first.c_str());
}

// A stop the provider does not confirm keeps the job (CUM-774) and is not announced
// twice; the owner's own pause stops nothing (only the spend limits do).
static void test_an_unconfirmed_stop_is_retried_not_repeated_and_an_owner_pause_stops_nothing() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::Network, agent::FabricErr::Ok};
  r.spawn("openai");
  const size_t before = r.delivered.size();
  orch::providerPark().pause("openai", orch::kPauseProviderLimit);
  r.at(r.plat.ms + 6000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());   // out of the slots at once
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());
  r.at(r.plat.ms + 31000);                          // the retry confirms
  TEST_ASSERT_EQUAL(2, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());

  Rig o;
  o.spawn("openai");
  orch::providerPark().pause("openai", orch::kPauseOwner);
  o.at(o.plat.ms + 6000);
  TEST_ASSERT_EQUAL(0, o.openai.cancels);
  orch::providerPark().resume("openai", orch::kPauseOwner);
}

// A hosted instance's ceiling never reaches its park (no freeze listener there): the
// ceiling itself (spendFrozen) stops the helpers all the same.
static void test_the_ceiling_stops_helpers_without_the_park() {
  Rig r;
  r.spawn("openai");
  r.spawn("mistral");
  const size_t before = r.delivered.size();
  g_ceilingFrozen = true;
  r.at(r.plat.ms + 6000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.mistral.cancels);   // in-request: nothing of it still runs
  TEST_ASSERT_EQUAL(before + 1, r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered.back().second.find("spend ceiling") != std::string::npos);
}

// ---- round 3 (merge review) ------------------------------------------------------------

// Blocker 3: a job whose polls fail for 15 minutes used to be dropped as "lost" with no
// cancel and no count while it could still bill. Now it is counted at the session
// estimate at once and its stop is asked (it runs on in the background stop list).
static void test_the_give_up_counts_and_stops_the_job() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::Network};
  r.spawn("openai");
  r.openai.pollErr = agent::FabricErr::Network;
  const uint32_t t0 = r.plat.ms;
  while (r.plat.ms - t0 < 17u * 60u * 1000u) r.at(r.plat.ms + 30000);
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  TEST_ASSERT_TRUE(r.openai.cancels >= 1);                  // its stop was asked
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());                // and it was counted
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, r.usage[0].in);
}

// Major 4: a stop the provider has not confirmed holds no job slot (no synthesis waits
// on it, a new helper can start).
static void test_an_unconfirmed_stop_holds_no_slot() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  TEST_ASSERT_TRUE(r.eng->stopping("job0000"));
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());
}

// Blocker 3: the stop list survives a restart: it is saved on every change, and an engine
// that starts with it asks the stop again (and counts the run when it ends).
static void test_the_stop_list_survives_a_restart() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_TRUE(r.saved.find("openai:job-1") != std::string::npos);
  Rig after;   // a restart: a new engine, the saved list
  after.toLoad = r.saved;
  after.openai.cancelScript = {agent::FabricErr::Ok};
  after.at(after.plat.ms + 1000);
  TEST_ASSERT_EQUAL(1, after.openai.cancels);       // asked again after the restart
  TEST_ASSERT_EQUAL(0, after.eng->stoppingCount());
  TEST_ASSERT_TRUE(after.saved.find("openai:job-1") == std::string::npos);
  Rig junk;    // a damaged saved list loads nothing and asks nothing
  junk.toLoad = "\x01\x02garbage";
  junk.at(junk.plat.ms + 1000);
  TEST_ASSERT_EQUAL(0, junk.openai.cancels);
  TEST_ASSERT_EQUAL(0, junk.eng->stoppingCount());
}

// Blocker 1: a helper start that got no answer, whose stop also failed, may be running
// with no job record. The engine keeps it on the stop list (the head is told it may have
// started), asks its stop again and counts it.
static void test_a_run_started_with_no_answer_is_stopped_in_the_background() {
  Rig r;
  r.openai.unseen = "openai:resp_unseen_";
  r.openai.cancelScript = {agent::FabricErr::Ok};
  r.spawn("openai");
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());
  bool maybe = false;
  TEST_ASSERT_TRUE(r.eng->takeSpawnNotes("chat1", &maybe).find("may have started") != std::string::npos);
  TEST_ASSERT_TRUE(maybe);
  r.at(r.plat.ms + 31000);
  TEST_ASSERT_EQUAL(1, r.openai.cancels);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(1, r.openai.attempts);   // never dispatched again
}

// ...and when the stop list is full the newest is still followed: the oldest entry gives
// its place, counted at the session estimate first (round 3 review: never drop a new
// stop, and nothing leaves the list uncounted).
static void test_a_full_stop_list_evicts_the_oldest_counted() {
  Rig r;
  r.openai.unseen = "openai:resp_unseen_";
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  for (int i = 0; i < JobEngine::kMaxStopping; ++i) r.spawn("openai");
  TEST_ASSERT_EQUAL(JobEngine::kMaxStopping, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(0, (int)r.usage.size());
  r.spawn("openai");
  TEST_ASSERT_EQUAL(JobEngine::kMaxStopping, r.eng->stoppingCount());
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());                 // the evicted oldest, counted
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, r.usage[0].in);
  TEST_ASSERT_TRUE(r.eng->stopping(("openai:resp_unseen_" + std::to_string(JobEngine::kMaxStopping + 1)).c_str()));
  TEST_ASSERT_FALSE(r.eng->stopping("openai:resp_unseen_1"));
  TEST_ASSERT_TRUE(JobEngine::kMaxStopping >= 3 * orch::kAgentMaxJobs);   // room for every helper, twice over
}

// Round 3 review (blocker): the pump drivers must keep pumping while only the stop list
// has work (the hosted instance pumps only while jobs are busy; the device closes its
// Telegram socket only while jobs are active), and a list saved before a restart counts
// as work before any pump.
static void test_the_stop_list_keeps_the_pump_running() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeCount());
  TEST_ASSERT_TRUE(r.eng->hasBackgroundWork());
  Rig after;
  after.toLoad = r.saved;
  TEST_ASSERT_TRUE(after.eng->hasBackgroundWork());   // read before the first pump
  Rig idle;
  TEST_ASSERT_FALSE(idle.eng->hasBackgroundWork());
  TEST_ASSERT_TRUE(has(slurp("nimbusd/src/rig.h"), "jobs_->hasBackgroundWork()"));
  TEST_ASSERT_TRUE(has(slurp("src/agent/orchestrator.cpp"), "g_jobs->stoppingCount()"));
}

// Round 3 review: a run that keeps answering "running" while its stop keeps failing is
// still counted at the estimate 15 minutes after it joined the list (not only after 15
// minutes of failed polls), and leaves after 24 hours (a bounded exit).
static void test_a_run_that_never_stops_is_counted_and_bounded() {
  Rig r;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.openai.pollScript = {agent::JobState::Running};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  const uint32_t t0 = r.plat.ms;
  while (r.plat.ms - t0 < 16u * 60u * 1000u) r.at(r.plat.ms + 30000);
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, r.usage[0].in);
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());
  while (r.eng->stoppingCount() && r.plat.ms - t0 < 25u * 3600u * 1000u) r.at(r.plat.ms + 300000);
  TEST_ASSERT_EQUAL(0, r.eng->stoppingCount());
  TEST_ASSERT_TRUE(r.plat.ms - t0 >= 24u * 3600u * 1000u);
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());
}

// Round 3 review: a stop list that cannot be saved (a full flash) would be forgotten at a
// restart, so the run is counted at the session estimate at once.
static void test_a_stop_that_cannot_be_saved_is_counted_at_once() {
  Rig r;
  r.saveOk = false;
  r.openai.cancelScript = {agent::FabricErr::RemoteFail};
  r.spawn("openai");
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(1, (int)r.usage.size());
  TEST_ASSERT_EQUAL_UINT32(orch::kSessionInputEstimateTokens, r.usage[0].in);
  TEST_ASSERT_EQUAL(1, r.eng->stoppingCount());   // still followed this run
}

static void test_the_stopped_message_names_what_paused_them() {
  const std::string one = orch::helpersStoppedText({"research (gpt-6-luna)"}, false, {"OpenAI"});
  TEST_ASSERT_TRUE(one.find("Paid AI calls are paused (OpenAI's monthly limit is reached)") != std::string::npos);
  TEST_ASSERT_TRUE(one.find("a running helper: research (gpt-6-luna)") != std::string::npos);
  TEST_ASSERT_TRUE(one.find("its result is not delivered") != std::string::npos);
  const std::string two = orch::helpersStoppedText({"a", "b", "c"}, false, {"OpenAI", "Anthropic"});
  TEST_ASSERT_TRUE(two.find("the monthly limits of OpenAI and Anthropic are reached") != std::string::npos);
  TEST_ASSERT_TRUE(two.find("3 running helpers: a, b and c") != std::string::npos);
  TEST_ASSERT_TRUE(orch::helpersStoppedText({"a"}, true, {}).find("spend ceiling") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", orch::helpersStoppedText({}, true, {}).c_str());
  for (unsigned char ch : two) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7f);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_confirmed_cancel_drops_the_job);
  RUN_TEST(test_an_unconfirmed_cancel_keeps_the_job_and_asks_again_with_a_growing_wait);
  RUN_TEST(test_a_cancel_of_a_job_the_provider_no_longer_has_ends_it);
  RUN_TEST(test_a_kept_job_is_counted_when_it_ends_and_its_result_is_not_delivered);
  RUN_TEST(test_a_kept_job_ending_with_no_usage_counts_what_its_provider_reports);
  RUN_TEST(test_a_kept_job_on_an_unreachable_provider_is_held_then_counted);
  RUN_TEST(test_a_provider_limit_stops_its_running_helpers_and_tells_once);
  RUN_TEST(test_the_ceiling_stops_every_remote_helper_in_one_message_per_chat);
  RUN_TEST(test_an_unconfirmed_stop_is_retried_not_repeated_and_an_owner_pause_stops_nothing);
  RUN_TEST(test_the_ceiling_stops_helpers_without_the_park);
  RUN_TEST(test_the_give_up_counts_and_stops_the_job);
  RUN_TEST(test_an_unconfirmed_stop_holds_no_slot);
  RUN_TEST(test_the_stop_list_survives_a_restart);
  RUN_TEST(test_a_run_started_with_no_answer_is_stopped_in_the_background);
  RUN_TEST(test_a_full_stop_list_evicts_the_oldest_counted);
  RUN_TEST(test_the_stop_list_keeps_the_pump_running);
  RUN_TEST(test_a_run_that_never_stops_is_counted_and_bounded);
  RUN_TEST(test_a_stop_that_cannot_be_saved_is_counted_at_once);
  RUN_TEST(test_the_stopped_message_names_what_paused_them);
  return UNITY_END();
}
