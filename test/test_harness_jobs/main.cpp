#include <unity.h>

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/provider_park.h"   // lane DEF2: the provider-limit pause
#include "nimbus/orch/model_choices.h"   // staticModelChoices / modelInChoices
#include "nimbus/orch/model_price_defaults.h"   // builtinModelPrice - the cheapest fallback
#include "nimbus/orch/models_tool.h"   // routerIdOnDirectProvider (L19-39)
#include "nimbus/harness/providers.h"
#include "nimbus/harness/rate_limit.h"
#include "nimbus/orch/compact.h"   // kFoldMaxChats - the spawn-note chat bound
#include "nimbus/orch/journal.h"
#include "nimbus/orch/provider_slots.h"   // kProviderSlots - the one-key spawn class test
#include "../../lib/harness/src/providers/wire.h"   // ReplyWriter - the writer every adapter uses (L19-36)

// Stage F suite - the sub-agent job machinery, host-tested for the first time:
// the one-dispatch-per-cycle gate (+2500/+1500 timers), the job-limit and
// keyless-provider refusals, a 429 refusal naming its quota window (CUM-465),
// model coercion, the round-robin poll lifecycle
// (Running -> Done fresh-parking, Error/NotFound delivery, transient backoff
// doubling + reset), the reap scheduler (Done grace vs Error attn-hold), the
// synthesis clock (coalesce + the CUM-480 drain-timed stuck fallback and its
// owner-facing body), reboot re-attach
// over a shared JournalStore, awaitTag aiming, and cancel.

using agent::JobEngine;
using harness_test::FakeAdapter;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;
namespace attn = nimbus::attn;
using solide::ring::Status;

// In-memory JournalStore standing in for the device NVS impl (same pattern as
// test_orch_journal): `slots` is shared so a test can rebuild a Journal over it
// to simulate a reboot and assert re-attach.
struct FakeStore : orch::JournalStore {
  std::map<int, std::string> slots;
  bool refusePuts = false;   // a full file system (SPAWN-JOURNAL-DURABILITY legs)
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    if (refusePuts) return false;
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override { slots.erase(slot); return true; }
  void clearNs() override { slots.clear(); }
};

// ---- rig: engine + recorders ------------------------------------------------
struct Rig {
  FakePlatform plat;
  FakeAdapter adapter;
  agent::HeavyFabric fabric;
  FakeStore store;
  std::unique_ptr<orch::Journal> journal;

  std::vector<std::pair<std::string, std::string>> delivered;  // chat, text
  // CUM-482: the device wires Deps.deliver through agent::capturingDelivery, so
  // the rig does too - every delivery is also recorded to `captured` (the chat's
  // episodic history on the device) unless `screenOut` suppresses it.
  std::vector<std::pair<std::string, std::string>> captured;
  std::function<bool(const std::string&)> screenOut;   // text -> suppressed by the outbound screen
  std::vector<attn::Event> events;                             // FakeEventSink recorder
  std::vector<std::string> cues;                               // fire()
  std::vector<std::string> synthChats;
  bool turnInFlight = false;
  bool synthConsumes = true;   // synthesize consumes the fresh results (turn ran)
  std::set<std::string> keyed{"anthropic", "openai"};
  std::function<std::string()> fileRoute;   // the device PDF route provider (nullable)
  std::map<std::string, std::string> connectorProv;  // skill -> hosting provider
  std::vector<agent::SpawnEv> hookSpawns;                  // HookRecorder
  std::vector<agent::ResultEv> hookResults;
  struct SpawnUsage { std::string backend; std::string model; uint32_t in, out; };
  std::vector<SpawnUsage> spawnUsage;                      // recordSpawnUsage recorder
  std::vector<std::string> persisted;                      // persistResult texts (project runs)
  std::vector<std::string> persistedTo;                    // persistResult project/name pairs (L19-36)
  std::string persistNote;                                 // persistResult's outcome line ("" = none)
  std::function<std::string(const std::string&)> chatContext;  // B4 (nullable)
  // CUM-541: the models each provider accepts (modelIsValid). Unset = the good-*
  // rule below; the Cumulo router's list is "<upstream>/<model>" selectors.
  std::function<bool(const std::string&, const std::string&)> modelValid;
  // Lane L: the provider choice list (validated coercion); null = the old rule.
  std::function<std::string(const std::string&)> modelChoices;
  // Context Fabric: recent-results spill recorder (nullable - legacy shape when unset).
  std::function<std::string(const char*, const char*, const std::string&, const std::string&)> spill;

  bool withClock = false;   // wire nowString/nowEpoch (the finished-job record)

  std::unique_ptr<JobEngine> eng;

  Rig() {
    adapter.backend = "anthropic";
    fabric.registerAdapter(&adapter);
    rebuildJournal();
    build();
  }

  void rebuildJournal() {   // "reboot": a fresh Journal over the SAME store
    journal.reset(new orch::Journal());
    journal->begin(&store);
  }

  void build() {   // (re)construct the engine over the current journal
    JobEngine::Deps d;
    d.platform = plat.contract();
    d.fabric = &fabric;
    d.journal = journal.get();
    d.deliver = agent::capturingDelivery(
        [this](const std::string& c, const std::string& t) {
          if (screenOut && screenOut(t)) return false;
          delivered.push_back({c, t});
          return true;
        },
        [this](const std::string& c, const std::string& t) { captured.push_back({c, t}); });
    d.event = [this](const attn::Event& e) { events.push_back(e); };
    d.fire = [this](const char* c) { cues.push_back(c); };
    d.backendHue = [](const char*) { return (uint8_t)42; };
    d.subPriority = [] { return std::string("anthropic,openai"); };
    d.providerHasKey = [this](const std::string& p) { return keyed.count(p) > 0; };
    d.subModel = [](const std::string& p) { return "sub-" + p; };
    d.modelIsValid = [](const std::string&, const std::string& m) {
      return m.rfind("good-", 0) == 0;   // models named good-* validate
    };
    if (modelValid) d.modelIsValid = modelValid;
    if (modelChoices) d.modelChoices = modelChoices;
    if (chatContext) d.chatContext = chatContext;   // B4: sub-brief [CONTEXT]
    if (spill) d.spillResult = spill;               // Context Fabric: results ring
    d.connectorProvider = [this](const std::string& skill) -> std::string {
      auto it = connectorProv.find(skill);
      return it == connectorProv.end() ? std::string() : it->second;
    };
    if (fileRoute) d.fileSpawnProvider = fileRoute;
    d.synthesize = [this](const std::string& chat) {
      synthChats.push_back(chat);
      if (synthConsumes) (void)eng->takeFreshResults();
    };
    d.turnInFlight = [this] { return turnInFlight; };
    d.hooks.onSpawn = [this](const agent::SpawnEv& ev) { hookSpawns.push_back(ev); };
    d.hooks.onResult = [this](const agent::ResultEv& ev) { hookResults.push_back(ev); };
    d.recordSpawnUsage = [this](const std::string& b, const std::string& m,
                                const orch::TokenUsage& u) {
      spawnUsage.push_back({b, m, u.promptTokens, u.completionTokens});
    };
    d.persistResult = [this](const std::string& prj, const std::string& name, const std::string&,
                             const std::string& text, const std::string&) {
      persisted.push_back(text);
      persistedTo.push_back(prj + "/" + name);
      return persistNote;   // default: no outcome line, keeps the parked text byte-exact
    };
    if (withClock) {
      d.nowString = [] { return std::string("2026-10-19 Sun 08:14 BST"); };
      d.nowEpoch = [] { return 1792394040u; };
    }
    eng.reset(new JobEngine(std::move(d)));
  }

  // Count JobState events with a given ring status for a tag's key.
  int eventsWith(Status st, const char* tag) const {
    const uint32_t key = agent::keyFromTag(tag);
    int n = 0;
    for (const auto& e : events)
      if (e.type == attn::Event::Type::JobState && e.key == key &&
          e.status == (uint8_t)st)
        n++;
    return n;
  }
};

static orch::Spawn sp(const char* task = "do a thing") {
  orch::Spawn s;
  s.task = task;
  s.provider = "anthropic";
  s.model = "good-m";
  s.category = "research";
  s.note = "On it.";
  return s;
}

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- spawn queue + dispatch gate --------------------------------------------

static void test_dispatch_one_per_cycle_with_2500ms_gate() {
  Rig r;
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  TEST_ASSERT_EQUAL(2, (int)r.cues.size());   // sfx "spawn" fired at each enqueue
  TEST_ASSERT_EQUAL_STRING("spawn", r.cues[0].c_str());

  r.eng->pump();                                       // t=1000: dispatch ONE
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  r.eng->pump();                                       // same cycle: gated
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  r.plat.ms = 3499;                                    // gate is now+2500
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  r.plat.ms = 3500;
  r.eng->pump();
  TEST_ASSERT_EQUAL(2, (int)r.adapter.dispatched.size());
  // Tag generation + Queued ring events for both.
  TEST_ASSERT_EQUAL_STRING("job0000", r.adapter.dispatched[0].tag.c_str());
  TEST_ASSERT_EQUAL_STRING("job0001", r.adapter.dispatched[1].tag.c_str());
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Idle, "job0000"));  // Queued -> Idle
}

// 2026-08-12 field: "orange breathing ring for hours". Orchestrator arcs have no
// ambient expiry (deliberate, W6), Running is exempt from the attention watchdog
// (deliberate), and a job whose polls only ever error retried forever - so its
// accent-colored Running arc breathed indefinitely. Rule 1: after 15 min of
// consecutive poll failures the job terminates HONESTLY (owner told, arc reaped).
static void test_stuck_job_gives_up_after_15min_of_poll_failures() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                                    // dispatch (t=1000)
  r.adapter.pollErr = agent::FabricErr::Network;    // provider stops answering
  for (int i = 0; i < 20; i++) { r.plat.ms += 120000; r.eng->pump(); }
  // Terminal + honest: the job left the journal, the owner got the loss report
  // (alone => direct deliver), and the ring got an Error CTA (reaped later).
  TEST_ASSERT_EQUAL(0, r.eng->activeCount());
  bool told = false;
  for (auto& d : r.delivered)
    if (d.second.find("no response from anthropic") != std::string::npos &&
        d.second.find("giving up") != std::string::npos)
      told = true;
  TEST_ASSERT_TRUE_MESSAGE(told, "the owner must hear the job was LOST, honestly");
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Error, "job0000"));
  // A healthy answer must RESET the clock: this is mutation-sensitive - remove
  // the firstErrMs reset and short outages accumulate into false give-ups.
  Rig r2;
  r2.adapter.pollScript = {agent::JobState::Running};   // healthy polls stay Running
  r2.eng->enqueueSpawn(sp(), "chat1");
  r2.eng->pump();
  for (int i = 0; i < 20; i++) {                    // alternating fail/ok forever
    r2.adapter.pollErr = agent::FabricErr::Network;
    r2.plat.ms += 8 * 60000; r2.eng->pump();        // 8 min failed (< 15)
    r2.adapter.pollErr = agent::FabricErr::Ok;      // then a healthy Running poll
    r2.plat.ms += 60000; r2.eng->pump();
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, r2.eng->activeCount(),
      "intermittent outages under the ceiling must never kill a live job");
}

// Rule 2: a job the provider still reports RUNNING past 30 min keeps polling but
// its arc parks to a dim static Idle segment - and the every-poll Running
// re-emit must NOT un-park it (that re-emit is exactly what kept the breathe
// alive for hours). Completion still fires the normal Done cue.
static void test_long_running_job_arc_parks_dim_then_completes() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};   // running until told otherwise
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                                       // dispatch
  r.plat.ms += 60000; r.eng->pump();                   // healthy Running poll
  TEST_ASSERT_TRUE(r.eventsWith(Status::Running, "job0000") >= 1);
  // Cross the 30-min park ceiling. Idle events: 1 at dispatch (Queued -> Idle)
  // + exactly 1 park downgrade.
  r.plat.ms += 31 * 60000; r.eng->pump();
  TEST_ASSERT_EQUAL_MESSAGE(2, r.eventsWith(Status::Idle, "job0000"),
      "crossing the ceiling parks the arc to dim static exactly once");
  const int runningAtPark = r.eventsWith(Status::Running, "job0000");
  for (int i = 0; i < 5; i++) { r.plat.ms += 120000; r.eng->pump(); }
  TEST_ASSERT_EQUAL_MESSAGE(runningAtPark, r.eventsWith(Status::Running, "job0000"),
      "post-park polls must not re-emit Running (that re-lights the breathe)");
  // Completion still lands normally.
  r.adapter.pollScript = {agent::JobState::Done};
  r.adapter.pollCount = 0;
  r.plat.ms += 120000; r.eng->pump();
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Done, "job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeCount());
}

// 2026-08-12 board panic (StoreProhibited): enqueueSpawn memset the non-POD
// PendingSpawn after emplace, nulling the task string's internals - a SHORT
// (SSO, <=15 B) task then wrote through the null pointer on assign, while long
// tasks survived by stealing the heap buffer (why cloud-model spawns never
// tripped it; a terse 3B local model did, with task "web_search"). Pin that a
// short task round-trips intact through enqueue -> dispatch.
static void test_short_sso_task_spawn_round_trips() {
  Rig r;
  r.eng->enqueueSpawn(sp("web_search"), "chat1");   // 10 B - inside SSO
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("web_search", r.adapter.dispatched[0].task.c_str());
  // The brief leads with it (the reply-cap sentence follows, L19-36).
  TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched[0].instruction.rfind("web_search\n\n[Reply size:", 0));
}

static void test_spawn_ack_message_bytes() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("chat1", r.delivered[0].first.c_str());
  // note + "  [" + tag (unnamed) + " · " + backend/model + "]", byte-identical.
  TEST_ASSERT_EQUAL_STRING("On it.  [job0000 \xC2\xB7 anthropic/good-m]",
                           r.delivered[0].second.c_str());
  // Accent hue rides the Queued event.
  TEST_ASSERT_TRUE(r.events.back().hasAccent);
  TEST_ASSERT_EQUAL(42, (int)r.events.back().accentHue);
}

static void test_job_limit_refusal() {
  // W5: the QUEUE depth (kMaxPendingSpawns) is what a turn can enqueue at once -
  // decoupled from kAgentMaxJobs (concurrency). A deep wave fills the queue; the
  // next spawn is refused (queue full), NOT dropped silently.
  Rig r;
  for (int i = 0; i < orch::kMaxPendingSpawns; i++)
    r.eng->enqueueSpawn(sp(), "chat1");
  TEST_ASSERT_EQUAL(orch::kMaxPendingSpawns, (int)r.cues.size());
  TEST_ASSERT_EQUAL(orch::kMaxPendingSpawns, r.eng->pendingCount());
  r.eng->enqueueSpawn(sp("one too many"), "chat1");
  TEST_ASSERT_EQUAL(orch::kMaxPendingSpawns, (int)r.cues.size());   // no cue for the refusal
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("My spawn queue is full - I couldn't take that one. "
                           "Ask again once some agents finish.",
                           r.delivered[0].second.c_str());
  // The deeper queue is bigger than the concurrency cap (that is the whole point).
  TEST_ASSERT_TRUE(orch::kMaxPendingSpawns > orch::kMaxActiveInflight);
}

// CUM-538 (item 5): a spawn that never starts used to reach only the owner; the
// head learned nothing and invented a reason ("spawn capacity is 0"). Every
// refusal path now leaves one model-facing line with the REAL reason, consumed by
// the next turn, and bounded however many pile up.
static void test_spawn_refusals_reach_the_head_with_the_real_reason() {
  Rig r;
  TEST_ASSERT_EQUAL_STRING("", r.eng->takeSpawnNotes("chat1").c_str());
  // 1. queue full
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) r.eng->enqueueSpawn(sp(), "chat1");
  orch::Spawn extra = sp("fetch the calendar");
  extra.name = "gcal-fetch";
  r.eng->enqueueSpawn(extra, "chat1");
  TEST_ASSERT_EQUAL_STRING("", r.eng->takeSpawnNotes("other-chat").c_str());   // never another chat's
  std::string notes = r.eng->takeSpawnNotes("chat1");
  TEST_ASSERT_EQUAL_STRING(
      ("- 'gcal-fetch' did not start: the spawn queue is full (" +
       std::to_string(orch::kMaxPendingSpawns) + " waiting); start it again after some finish\n")
          .c_str(),
      notes.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.eng->takeSpawnNotes("chat1").c_str());   // consumed

  // 2. the provider refused the dispatch (the owner hears the same sentence)
  Rig f;
  f.adapter.dispatchErr = agent::FabricErr::RemoteFail;
  f.eng->enqueueSpawn(sp("summarize the thread"), "chat1");
  f.eng->pump();
  notes = f.eng->takeSpawnNotes("chat1");
  TEST_ASSERT_TRUE(notes.find("'summarize the thread' did not start: Couldn't start that agent "
                              "on anthropic: anthropic had an error.") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("Couldn't start that agent on anthropic: anthropic had an error. Try "
                           "again, or use another provider.", f.delivered[0].second.c_str());

  // 3. no keyed provider
  Rig k;
  k.keyed.clear();
  orch::Spawn s = sp();
  s.provider = "";
  k.eng->enqueueSpawn(s, "chat1");
  k.eng->pump();
  TEST_ASSERT_TRUE(k.eng->takeSpawnNotes("chat1").find("did not start: no provider with a key can run it") !=
                   std::string::npos);

  // Bounded: a flood folds into one tail line.
  Rig b;
  for (int i = 0; i < orch::kMaxPendingSpawns + 40; i++) b.eng->enqueueSpawn(sp(), "chat1");
  notes = b.eng->takeSpawnNotes("chat1");
  TEST_ASSERT_TRUE(notes.size() <= 700);
  const size_t fold = notes.find("(more refusals not listed)\n");
  TEST_ASSERT_TRUE(fold != std::string::npos);
  TEST_ASSERT_EQUAL(notes.size(), fold + strlen("(more refusals not listed)\n"));   // nothing after it
}

// CUM-538 gate: a dispatch that TIMED OUT most likely ran (a Mistral sub runs
// inside the POST that outlived the 60 s wait) and no job record exists. The head
// used to read "did NOT start ... spawn again" and pay for the same work twice; the
// note now says it may have started and must not be spawned again, and the job
// counts as possibly running for kMaybeRunningMs.
static void test_timed_out_spawn_may_have_started_and_is_not_respawned() {
  Rig r;
  r.adapter.dispatchErr = agent::FabricErr::Timeout;
  orch::Spawn s = sp("fetch the calendar");
  s.name = "gcal-fetch";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL_STRING("The agent on anthropic didn't respond in time - it may have run "
                           "without me getting its result. Try a smaller task or split it into "
                           "steps.", r.delivered[0].second.c_str());   // the owner's honest line
  bool maybe = false;
  const std::string notes = r.eng->takeSpawnNotes("chat1", &maybe);
  TEST_ASSERT_TRUE(maybe);
  TEST_ASSERT_TRUE_MESSAGE(notes.find("did not start") == std::string::npos, notes.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "- 'gcal-fetch' may have started: the agent on anthropic did not answer in time, so "
      "it may have run, and its result will not arrive. Do not spawn it again unless the "
      "owner asks\n", notes.c_str());
  TEST_ASSERT_EQUAL(1, r.eng->maybeRunningCount());
  r.plat.ms += JobEngine::kMaybeRunningMs - 1;
  TEST_ASSERT_EQUAL(1, r.eng->maybeRunningCount());
  r.plat.ms += 1;
  TEST_ASSERT_EQUAL(0, r.eng->maybeRunningCount());   // the window closes
  // ...and stays closed when millis() wraps: 2^32 ms after the timeout, plus one
  // second, the stale time would read as one second old if it were kept.
  r.plat.ms += 1000u - JobEngine::kMaybeRunningMs;   // wraps: timeout + 2^32 + 1 s
  TEST_ASSERT_EQUAL(0, r.eng->maybeRunningCount());

  // A timeout after the chat's budget is spent is still listed, and the flag is set:
  // a folded-away "may have started" line would read as "spawn again".
  Rig full;
  for (int i = 0; i < orch::kMaxPendingSpawns + 20; i++) full.eng->enqueueSpawn(sp(), "chat1");
  full.adapter.dispatchErr = agent::FabricErr::Timeout;
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) full.eng->pump(), full.plat.ms += 10000;
  const std::string fn = full.eng->takeSpawnNotes("chat1", &maybe);
  TEST_ASSERT_TRUE(maybe);
  TEST_ASSERT_TRUE_MESSAGE(fn.find("(more refusals not listed)\n") != std::string::npos, fn.c_str());
  TEST_ASSERT_TRUE_MESSAGE(fn.find("may have started: the agent on anthropic did not answer") !=
                               std::string::npos, fn.c_str());
  TEST_ASSERT_TRUE(fn.size() <= 1400);

  // Every other dispatch failure stays a plain refusal and is never counted.
  for (agent::FabricErr e : {agent::FabricErr::RemoteFail, agent::FabricErr::Network,
                             agent::FabricErr::Auth, agent::FabricErr::RateLimited}) {
    Rig f;
    f.adapter.dispatchErr = e;
    f.eng->enqueueSpawn(sp("summarize the thread"), "chat1");
    f.eng->pump();
    const std::string n = f.eng->takeSpawnNotes("chat1", &maybe);
    TEST_ASSERT_FALSE(maybe);
    TEST_ASSERT_TRUE(n.find("did not start: Couldn't start that agent on anthropic") != std::string::npos);
    TEST_ASSERT_EQUAL(0, f.eng->maybeRunningCount());
  }
}

// A real synchronous sub adapter over the compat wire, as the device's ZaiAdapter
// and CumuloAdapter and the hosted zai/cumulo fabric adapters wrap it, on a
// scripted transport whose clock moves inside the exchange.
struct CompatAdapter : agent::ManagedAgentAdapter {
  harness_test::FakeProviderDeps deps;
  agent::providers::ProviderDeps pd = deps.contract();
  const char* backendId() const override { return "zai"; }
  agent::Capabilities capabilities() const override { return {}; }
  agent::FabricErr dispatch(const agent::Directive& d, char outJobId[72]) override {
    agent::providers::CompatEndpoint ep;
    ep.host = "api.z.ai";
    ep.basePath = "/api/paas/v4";
    ep.key = "tok";
    ep.model = "glm-5.3";
    ep.backendTag = "zai";
    return agent::providers::openaiCompatDispatch(pd, ep, d, outJobId);
  }
  agent::FabricErr poll(const char* jobId, agent::ResultEnvelope& env) override {
    return agent::providers::openaiCompatPoll(pd, "zai", jobId, env);
  }
  agent::FabricErr cancel(const char* jobId) override {
    return agent::providers::openaiCompatCancel(pd, jobId);
  }
};

// CUM-538 gate: the may-have-started path held for Mistral only. A Z.ai
// sub whose completion outlives the 30 s POST got "did not start", so the head was
// told to spawn the same paid work again. Wire to note, both sides of the line.
static void test_a_zai_sub_that_outruns_its_post_may_have_started() {
  for (const bool slow : {true, false}) {
    Rig r;
    CompatAdapter zai;
    r.fabric.registerAdapter(&zai);
    r.keyed.insert("zai");
    // Slow: the request went out and nothing came back for 29 s. Fast: the connect
    // failed at once, so nothing was sent.
    zai.deps.http.script.push_back(slow ? harness_test::transportCut(29000)
                                        : harness_test::transportCut(1000, "connect failed"));
    orch::Spawn s = sp("research the GLM release notes");
    s.provider = "zai";
    s.name = "glm-notes";
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL(1, (int)zai.deps.http.seen.size());
    bool maybe = false;
    const std::string notes = r.eng->takeSpawnNotes("chat1", &maybe);
    if (slow) {
      TEST_ASSERT_TRUE(maybe);
      TEST_ASSERT_EQUAL_STRING(
          "- 'glm-notes' may have started: the agent on zai did not answer in time, so it "
          "may have run, and its result will not arrive. Do not spawn it again unless the "
          "owner asks\n", notes.c_str());
      TEST_ASSERT_EQUAL(1, r.eng->maybeRunningCount());
      TEST_ASSERT_EQUAL_STRING("The agent on zai didn't respond in time - it may have run "
                               "without me getting its result. Try a smaller task or split "
                               "it into steps.", r.delivered[0].second.c_str());
    } else {
      TEST_ASSERT_FALSE(maybe);
      TEST_ASSERT_EQUAL_STRING("- 'glm-notes' did not start: Couldn't start that agent on zai: zai "
                               "could not be reached. Check the connection and try again.\n",
                               notes.c_str());
      TEST_ASSERT_EQUAL(0, r.eng->maybeRunningCount());
    }
  }
}

// Lane DEF: at the monthly spend ceiling a sub-agent dispatch is refused before any
// request leaves the device (the wire gate), whatever provider it would run on; the
// queued work does not start and nothing is billed. Counter-test: the same spawn
// with the ceiling open reaches the provider.
static bool g_subFrozen = false;
static bool subFrozen() { return g_subFrozen; }
static void test_spend_ceiling_refuses_a_sub_dispatch_before_any_request() {
  for (const bool frozen : {true, false}) {
    Rig r;
    CompatAdapter zai;
    r.fabric.registerAdapter(&zai);
    r.keyed.insert("zai");
    zai.deps.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"});
    g_subFrozen = frozen;
    orch::installSpendFreeze(&subFrozen);
    orch::Spawn s = sp("summarize the GLM paper");
    s.provider = "zai";
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    orch::installSpendFreeze(nullptr);
    TEST_ASSERT_EQUAL(frozen ? 0 : 1, (int)zai.deps.http.seen.size());
    if (frozen) {
      bool maybe = true;
      const std::string notes = r.eng->takeSpawnNotes("chat1", &maybe);
      TEST_ASSERT_FALSE(maybe);   // nothing went out: it did not start, nothing was billed
      TEST_ASSERT_TRUE_MESSAGE(notes.find("did not start") != std::string::npos, notes.c_str());
      TEST_ASSERT_EQUAL(0, r.eng->maybeRunningCount());
    }
  }
}

// Lane DEF2: a provider whose own monthly limit is reached (its park pause) runs no
// helper: a spawn naming it moves to the next open keyed provider, and the limit's words
// ride the swap; Mistral, Z.ai or any other provider with room keeps working.
static void test_a_provider_at_its_limit_runs_no_helper() {
  orch::providerPark().clear();
  orch::providerPark().pause("anthropic", orch::kPauseProviderLimit);
  Rig r;
  CompatAdapter zai;
  r.fabric.registerAdapter(&zai);
  r.keyed = {"anthropic", "zai"};
  zai.deps.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"});
  r.eng->enqueueSpawn(sp("summarize the GLM paper"), "chat1");   // names anthropic
  r.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());   // nothing to anthropic
  TEST_ASSERT_EQUAL(1, (int)zai.deps.http.seen.size());     // it ran on Z.ai instead
  TEST_ASSERT_TRUE(LogCapture::contains("spawn moved anthropic -> zai"));
  TEST_ASSERT_TRUE(LogCapture::contains("its monthly limit is reached"));
  orch::providerPark().clear();
}

// CUM-538 gate: a 200 whose result could not be read (empty content, or a body the
// device could not parse) means the provider RAN the sub and charged for it. It was
// told "couldn't start" and the head "did not start", so it could pay again.
static void test_an_unreadable_answer_may_have_run() {
  Rig r;
  CompatAdapter zai;
  r.fabric.registerAdapter(&zai);
  r.keyed.insert("zai");
  zai.deps.http.script.push_back({"", "", 200, "{\"choices\":[{\"message\":{\"content\":\"\"}}]}"});
  orch::Spawn s = sp("summarize the GLM paper");
  s.provider = "zai";
  s.name = "glm-sum";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL_STRING("The agent on zai answered, but I couldn't read its result - it may have run. "
                           "Try a smaller task or split it into steps.", r.delivered[0].second.c_str());
  bool maybe = false;
  const std::string notes = r.eng->takeSpawnNotes("chat1", &maybe);
  TEST_ASSERT_TRUE(maybe);
  TEST_ASSERT_EQUAL_STRING(
      "- 'glm-sum' may have started: the agent on zai answered but its result could not be read, "
      "so it may have run, and its result will not arrive. Do not spawn it again unless the owner "
      "asks\n", notes.c_str());
  TEST_ASSERT_EQUAL(1, r.eng->maybeRunningCount());
}

// CUM-538 gate: a "may have started" note survives the chat bound. With 16 other
// chats holding unread refusals, the 17th chat's note evicted the oldest entry
// whole, so the spawning chat's head never heard the do-not-respawn order.
static void test_may_have_started_note_survives_the_chat_bound() {
  Rig r;
  r.adapter.dispatchErr = agent::FabricErr::Timeout;
  orch::Spawn s = sp("fetch the calendar");
  s.name = "gcal-fetch";
  r.eng->enqueueSpawn(s, "chatA");
  r.eng->pump();   // times out: chatA holds the oldest entry, a "may have started" line
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) r.eng->enqueueSpawn(sp(), "fill");
  for (int i = 0; i < (int)orch::kFoldMaxChats; i++)   // every further spawn is refused: queue full
    r.eng->enqueueSpawn(sp(), "chat" + std::to_string(i));
  bool maybe = false;
  const std::string a = r.eng->takeSpawnNotes("chatA", &maybe);
  TEST_ASSERT_TRUE(maybe);
  TEST_ASSERT_TRUE_MESSAGE(a.find("'gcal-fetch' may have started") != std::string::npos, a.c_str());
  // The entry that made room was the oldest plain refusal; the newest is kept.
  TEST_ASSERT_EQUAL_STRING("", r.eng->takeSpawnNotes("chat0").c_str());
  TEST_ASSERT_TRUE(r.eng->takeSpawnNotes("chat15").find("did not start: the spawn queue is full") !=
                   std::string::npos);

  // Every entry holds a "may have started" line: a plain refusal is not kept (its
  // owner was told it at once), and no timeout note is dropped for it.
  Rig t;
  t.adapter.dispatchErr = agent::FabricErr::Timeout;
  for (int i = 0; i < (int)orch::kFoldMaxChats; i++) {
    t.eng->enqueueSpawn(sp(), "t" + std::to_string(i));
    t.eng->pump();
    t.plat.ms += 10000;
  }
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) t.eng->enqueueSpawn(sp(), "fill");
  t.eng->enqueueSpawn(sp(), "late");   // refused: queue full
  TEST_ASSERT_EQUAL_STRING("", t.eng->takeSpawnNotes("late").c_str());
  for (int i = 0; i < (int)orch::kFoldMaxChats; i++) {
    (void)t.eng->takeSpawnNotes("t" + std::to_string(i), &maybe);
    TEST_ASSERT_TRUE_MESSAGE(maybe, ("t" + std::to_string(i)).c_str());
  }
}

// CUM-538 gate: the note budget is per chat. One chat's unread flood used to spend
// the single engine-wide budget, so another chat's first refusal became a bare
// "(more refusals not listed)" with no name and no reason.
static void test_spawn_note_budget_is_per_chat() {
  Rig b;
  for (int i = 0; i < orch::kMaxPendingSpawns + 40; i++) b.eng->enqueueSpawn(sp(), "chat1");
  orch::Spawn extra = sp("summarize the thread");
  extra.name = "b-task";
  b.eng->enqueueSpawn(extra, "chat2");   // the queue is still full
  TEST_ASSERT_EQUAL_STRING(("- 'b-task' did not start: the spawn queue is full (" +
                            std::to_string(orch::kMaxPendingSpawns) +
                            " waiting); start it again after some finish\n")
                               .c_str(),
                           b.eng->takeSpawnNotes("chat2").c_str());
  const std::string n1 = b.eng->takeSpawnNotes("chat1");   // chat1 folded in its own budget
  TEST_ASSERT_TRUE(n1.size() <= 700);
  TEST_ASSERT_EQUAL(n1.size() - strlen("(more refusals not listed)\n"),
                    n1.find("(more refusals not listed)\n"));

  // Bounded overall: at most kFoldMaxChats chats hold notes; one more drops the
  // notes that waited longest, and every other chat keeps its reason.
  Rig c;
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) c.eng->enqueueSpawn(sp(), "fill");
  (void)c.eng->takeSpawnNotes("fill");
  const int chats = (int)orch::kFoldMaxChats + 1;
  for (int k = 0; k < chats; k++) {
    orch::Spawn t = sp("x");
    t.name = "t" + std::to_string(k);
    c.eng->enqueueSpawn(t, "chat" + std::to_string(k));
  }
  TEST_ASSERT_EQUAL_STRING("", c.eng->takeSpawnNotes("chat0").c_str());
  for (int k = 1; k < chats; k++) {
    const std::string n = c.eng->takeSpawnNotes("chat" + std::to_string(k));
    TEST_ASSERT_TRUE_MESSAGE(n.find("- 't" + std::to_string(k) + "' did not start: the spawn queue is full") == 0,
                             n.c_str());
  }
}

static void test_keyless_provider_refusal() {
  Rig r;
  r.keyed.clear();   // nothing key'd -> firstSubProvider() == ""
  orch::Spawn s = sp();
  s.provider = "";   // "" => device resolves by priority
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("No provider is configured to run that agent.",
                           r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
}

// Connector-aware routing: a spawn with NO explicit provider whose skill names an
// enabled connector routes to the provider that hosts it (heavy connector work
// then runs on lab compute instead of the sub-priority default).
static void test_connector_routing_no_hint() {
  Rig r;
  FakeAdapter mistral; mistral.backend = "mistral";
  r.fabric.registerAdapter(&mistral);
  r.keyed.insert("mistral");
  r.connectorProv["notion"] = "mistral";   // the Notion connector lives on Mistral
  orch::Spawn s = sp();
  s.provider = "";                          // no explicit hint
  s.skill = "notion";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());   // NOT the anthropic default
  TEST_ASSERT_EQUAL(1, (int)mistral.dispatched.size());     // routed to the connector's provider
}

// An explicit provider hint wins over connector routing.
static void test_connector_routing_explicit_hint_wins() {
  Rig r;
  FakeAdapter mistral; mistral.backend = "mistral";
  r.fabric.registerAdapter(&mistral);
  r.keyed.insert("mistral");
  r.connectorProv["notion"] = "mistral";
  orch::Spawn s = sp();
  s.provider = "anthropic";   // explicit
  s.skill = "notion";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());   // honoured the explicit hint
  TEST_ASSERT_EQUAL(0, (int)mistral.dispatched.size());
}

// No connector match -> falls through to the sub-priority default.
static void test_connector_routing_falls_through() {
  Rig r;
  orch::Spawn s = sp();
  s.provider = "";
  s.skill = "unknownskill";   // connectorProvider returns "" -> firstSubProvider()
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());   // anthropic (first key'd)
}

// A connector whose hosting provider has NO key must NOT route there - it falls
// through to the sub-priority default (preserves the keyless refusal, no doomed
// keyless dispatch).
static void test_connector_routing_unkeyed_falls_through() {
  Rig r;
  r.connectorProv["notion"] = "mistral";   // hosts on mistral...
  // ...but mistral is NOT in `keyed` and no mistral adapter is registered.
  orch::Spawn s = sp();
  s.provider = "";
  s.skill = "notion";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());   // anthropic default, not mistral
}

static void test_model_coercion_to_sub_default() {
  Rig r;
  orch::Spawn s = sp();
  s.model = "bad-x";   // not in the choice list
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("sub-anthropic", r.adapter.dispatched[0].model.c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("spawn model coerced (bad-x -> sub-anthropic on anthropic)"));
}

// ---- CUM-541: one-key spawns (a provider with no key runs on the router) -------

// A device or instance whose only key is the Cumulo router, with an adapter for
// every backend so a test sees which one a spawn really reached.
struct OneKeyRig {
  Rig r;
  FakeAdapter oai, mis, cumulo, zai;
  std::set<std::string> routerOffers{"openai/gpt-5.6", "anthropic/claude-opus-5-5",
                                     "mistral/mistral-large-4-0", "zai/glm-5.3"};
  OneKeyRig() {
    oai.backend = "openai";
    mis.backend = "mistral";
    cumulo.backend = "cumulo";
    zai.backend = "zai";
    r.fabric.registerAdapter(&oai);
    r.fabric.registerAdapter(&mis);
    r.fabric.registerAdapter(&cumulo);
    r.fabric.registerAdapter(&zai);
    r.keyed = {"cumulo"};
    r.modelValid = [this](const std::string& p, const std::string& m) {
      if (p == "cumulo") return routerOffers.count(m) > 0;
      return p == "openai" && m == "gpt-5.6";
    };
    r.build();
  }
  // Every dispatch any backend received.
  int dispatches() const {
    return (int)(r.adapter.dispatched.size() + oai.dispatched.size() + mis.dispatched.size() +
                 cumulo.dispatched.size() + zai.dispatched.size());
  }
  void spawn(const char* provider, const char* model, const char* name = "fca-helper") {
    orch::Spawn s = sp("Summarise the UK FCA Consumer Duty");
    s.provider = provider;
    s.model = model;
    s.name = name;
    r.eng->enqueueSpawn(s, "chat1");
    r.plat.ms += 5000;
    r.eng->pump();
  }
};

// (a) the live Joiner-VN3 case: the head named openai/gpt-5.6 on a Cumulo-only
// instance, the coercion kept openai and the keyless openai sub failed ("spawn
// failed 2"). It runs on the router as openai/gpt-5.6.
static void test_one_key_spawn_openai_runs_on_router() {
  OneKeyRig k;
  k.spawn("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL(0, (int)k.oai.dispatched.size());
  TEST_ASSERT_EQUAL(1, (int)k.cumulo.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("openai/gpt-5.6", k.cumulo.dispatched[0].model.c_str());
  TEST_ASSERT_EQUAL(1, (int)k.r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("On it.  [fca-helper \xC2\xB7 cumulo/openai/gpt-5.6]",
                           k.r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("", k.r.eng->takeSpawnNotes("chat1").c_str());
  TEST_ASSERT_TRUE(LogCapture::contains(
      "spawn on openai has no key - runs on the Cumulo router as openai/gpt-5.6 (asked gpt-5.6)"));
  TEST_ASSERT_EQUAL(1, k.r.eng->activeJobCount());

  // A router list of bare ids (the cold-start list: the router's default upstream
  // is openai) offers openai/gpt-5.6 as gpt-5.6.
  OneKeyRig bare;
  bare.routerOffers = {"gpt-5.6", "gpt-5.6-terra"};
  bare.spawn("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL(1, (int)bare.cumulo.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", bare.cumulo.dispatched[0].model.c_str());
  TEST_ASSERT_EQUAL(0, (int)bare.oai.dispatched.size());

  // No model named: the model the coercion gives openai (its sub model) is the one
  // asked of the router.
  OneKeyRig sub;
  sub.routerOffers.insert("openai/sub-openai");
  sub.spawn("openai", "");
  TEST_ASSERT_EQUAL(1, (int)sub.cumulo.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("openai/sub-openai", sub.cumulo.dispatched[0].model.c_str());
}

// (b) any provider the router lists: anthropic with a model the router offers,
// named bare or already as the router selector.
static void test_one_key_spawn_anthropic_runs_on_router() {
  for (const char* model : {"claude-opus-5-5", "anthropic/claude-opus-5-5"}) {
    OneKeyRig k;
    k.spawn("anthropic", model);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)k.r.adapter.dispatched.size(), model);   // the anthropic adapter
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)k.cumulo.dispatched.size(), model);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("anthropic/claude-opus-5-5",
                                     k.cumulo.dispatched[0].model.c_str(), model);
  }
}

// (c) a direct key wins: with an OpenAI key the spawn stays on openai, never the
// router.
static void test_one_key_spawn_direct_key_stays_direct() {
  OneKeyRig k;
  k.r.keyed = {"cumulo", "openai"};
  k.spawn("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL(1, (int)k.oai.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6", k.oai.dispatched[0].model.c_str());
  TEST_ASSERT_EQUAL(0, (int)k.cumulo.dispatched.size());
  TEST_ASSERT_FALSE(LogCapture::contains("runs on the Cumulo router"));
}

// (d) nothing offers it: no openai key and the router does not list the model (or
// there is no router key at all) - refused with the real reason, to the owner and
// the head, and no keyless request is sent anywhere.
static void test_one_key_spawn_nothing_offers_it_is_refused() {
  OneKeyRig k;
  k.spawn("openai", "gpt-9-imaginary");
  TEST_ASSERT_EQUAL(0, k.dispatches());
  TEST_ASSERT_EQUAL(1, (int)k.r.delivered.size());
  TEST_ASSERT_EQUAL_STRING(
      "Couldn't start that agent on openai. No openai key is set, and the Cumulo router does "
      "not offer openai/gpt-9-imaginary. Add the key under Providers & models, or ask for a "
      "model the router lists.", k.r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "- 'fca-helper' did not start: openai has no key here and the Cumulo router does not "
      "offer openai/gpt-9-imaginary; spawn it on cumulo with a model its [AVAILABLE MODELS] "
      "entry lists\n", k.r.eng->takeSpawnNotes("chat1").c_str());
  TEST_ASSERT_EQUAL(0, k.r.eng->activeJobCount());

  OneKeyRig none;
  none.r.keyed.clear();
  none.spawn("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL(0, none.dispatches());
  TEST_ASSERT_EQUAL_STRING("Couldn't start that agent on openai. No openai key is set. Add the "
                           "key under Providers & models.", none.r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("- 'fca-helper' did not start: openai has no key here\n",
                           none.r.eng->takeSpawnNotes("chat1").c_str());
}

// A connector task never moves to (or runs on) the router: the router does not carry
// the owner's connectors and its subs report no connector run, so the result could
// not be marked unverified (CUM-536). Refused with the connector's real host instead,
// however the spawn got there (a keyless provider, no provider, or cumulo named).
static void connectorSpawn(OneKeyRig& k, const char* provider, const char* skill) {
  orch::Spawn s = sp("List every event on my calendar today");
  s.provider = provider;
  s.model = "mistral-large-4-0";
  s.skill = skill;
  s.name = "gcal-fetch";
  k.r.eng->enqueueSpawn(s, "chat1");
  k.r.plat.ms += 5000;
  k.r.eng->pump();
}

static void test_one_key_spawn_connector_task_stays_off_the_router() {
  for (const char* provider : {"mistral", "", "cumulo"}) {
    OneKeyRig k;
    k.r.connectorProv["gcal"] = "mistral";
    connectorSpawn(k, provider, "gcal");
    const std::string on = provider[0] ? provider : "cumulo";   // no provider: the router rung
    TEST_ASSERT_EQUAL_MESSAGE(0, k.dispatches(), provider);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(("Couldn't start that agent on " + on + ". The gcal connector runs "
                                      "only on a mistral key. Add it under Providers & models.").c_str(),
                                     k.r.delivered[0].second.c_str(), provider);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("- 'gcal-fetch' did not start: gcal is a mistral connector; it runs "
                                     "only on a mistral key, never on the Cumulo router\n",
                                     k.r.eng->takeSpawnNotes("chat1").c_str(), provider);
  }
  // The head named the wrong provider while the connector's own provider is keyed.
  OneKeyRig wrong;
  wrong.r.keyed = {"cumulo", "mistral"};
  wrong.r.connectorProv["gcal"] = "mistral";
  connectorSpawn(wrong, "openai", "gcal");
  TEST_ASSERT_EQUAL(0, wrong.dispatches());
  TEST_ASSERT_EQUAL_STRING("Couldn't start that agent on openai. The gcal connector runs on mistral, "
                           "so ask for it there.", wrong.r.delivered[0].second.c_str());
  // A connector any provider runs ("any") never runs on the router either, and still
  // runs on a keyed provider of the configured order.
  OneKeyRig any;
  any.r.connectorProv["notion"] = "any";
  connectorSpawn(any, "", "notion");
  TEST_ASSERT_EQUAL(0, any.dispatches());
  TEST_ASSERT_EQUAL_STRING("Couldn't start that agent on cumulo. The notion connector needs a provider "
                           "key of its own, not the Cumulo router. Add one under Providers & models.",
                           any.r.delivered[0].second.c_str());
  OneKeyRig keyed;
  keyed.r.keyed = {"cumulo", "openai"};
  keyed.r.connectorProv["notion"] = "any";
  connectorSpawn(keyed, "", "notion");
  TEST_ASSERT_EQUAL(1, (int)keyed.oai.dispatched.size());
  TEST_ASSERT_EQUAL(0, (int)keyed.cumulo.dispatched.size());
  // The same spawn without a connector runs on the router.
  OneKeyRig plain;
  plain.spawn("mistral", "mistral-large-4-0");
  TEST_ASSERT_EQUAL(1, (int)plain.cumulo.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("mistral/mistral-large-4-0", plain.cumulo.dispatched[0].model.c_str());
}

// A spawn that names no provider on a one-key device: the configured order has no
// keyed entry, so the router key is the next rung (the device's default order is
// openai,anthropic,mistral), never "No provider is configured".
static void test_one_key_spawn_no_provider_runs_on_router() {
  OneKeyRig k;
  k.spawn("", "");
  TEST_ASSERT_EQUAL(1, (int)k.cumulo.dispatched.size());
  TEST_ASSERT_EQUAL_STRING("sub-cumulo", k.cumulo.dispatched[0].model.c_str());

  OneKeyRig z;   // a Z.ai-only device: its key is the rung
  z.r.keyed = {"zai"};
  z.spawn("", "");
  TEST_ASSERT_EQUAL(1, (int)z.zai.dispatched.size());
  TEST_ASSERT_EQUAL(0, (int)z.cumulo.dispatched.size());
}

// The class: for EVERY provider slot, a Cumulo-only spawn naming that provider runs
// on the router when the router offers "<slug>/<model>", and is refused naming the
// real reason (never dispatched) when it does not.
static void test_one_key_spawn_every_provider_slot() {
  for (size_t i = 0; i < orch::kProviderSlotCount; i++) {
    const std::string slug = orch::kProviderSlots[i].slug;
    if (slug == "cumulo") continue;
    OneKeyRig k;
    k.routerOffers = {slug + "/m1"};
    k.spawn(slug.c_str(), "m1");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)k.cumulo.dispatched.size(), slug.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE((slug + "/m1").c_str(), k.cumulo.dispatched[0].model.c_str(),
                                     slug.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, k.dispatches(), slug.c_str());

    OneKeyRig no;
    no.routerOffers = {};
    no.spawn(slug.c_str(), "m1");
    TEST_ASSERT_EQUAL_MESSAGE(0, no.dispatches(), slug.c_str());
    TEST_ASSERT_TRUE_MESSAGE(no.r.delivered[0].second.find("Couldn't start that agent on " + slug +
                                                          ". No " + slug + " key is set") == 0,
                             slug.c_str());
  }
}

// The device registers every backend at boot, keyed or not (CUM-541 bench: a
// Cumulo key added after a factory reset left helpers "isn't configured" until a
// restart). A backend that is registered but not yet set is refused with the real
// reason, and works on the next spawn once its key (or endpoint) is added, with no
// re-registration.
static void test_backend_added_at_runtime_works_without_restart() {
  for (const char* slug : {"zai", "cumulo", "custom"}) {
    Rig r;
    FakeAdapter late;
    late.backend = slug;
    r.fabric.registerAdapter(&late);
    r.keyed = {};
    r.build();
    orch::Spawn s = sp("Summarise the UK FCA Consumer Duty");
    s.provider = slug;
    r.eng->enqueueSpawn(s, "chat1");
    r.plat.ms += 5000;
    r.eng->pump();
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)late.dispatched.size(), slug);
    const std::string what = std::string(slug) == "custom" ? " endpoint" : " key";
    const std::string want = std::string("Couldn't start that agent on ") + slug + ". No " + slug +
                             what + " is set. Add the" + what + " under Providers & models.";
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.delivered[0].second.c_str(), slug);
    TEST_ASSERT_TRUE_MESSAGE(r.delivered[0].second.find("isn't configured") == std::string::npos,
                             slug);

    if (std::string(slug) == "custom") {   // with the router keyed: no router clause
      Rig k;
      FakeAdapter c;
      c.backend = "custom";
      k.fabric.registerAdapter(&c);
      k.keyed = {"cumulo"};
      k.modelValid = [](const std::string&, const std::string&) { return false; };
      k.build();
      k.eng->enqueueSpawn(s, "chat1");
      k.plat.ms += 5000;
      k.eng->pump();
      TEST_ASSERT_EQUAL(0, (int)c.dispatched.size());
      const std::string& said = k.delivered[0].second;
      TEST_ASSERT_TRUE(said.find("No custom endpoint is set") != std::string::npos);
      TEST_ASSERT_TRUE_MESSAGE(said.find("does not offer") == std::string::npos, said.c_str());
    }

    r.keyed.insert(slug);   // the owner adds it on the web page; no restart
    r.eng->enqueueSpawn(s, "chat1");
    r.plat.ms += 5000;
    r.eng->pump();
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)late.dispatched.size(), slug);
  }
}

// ---- lifecycle hooks + spawn spend attribution -------------------------------

static void test_hooks_spawn_and_result_fire() {
  Rig r;   // default script: Running, Done
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                       // dispatch
  TEST_ASSERT_EQUAL(1, (int)r.hookSpawns.size());
  TEST_ASSERT_EQUAL_STRING("job0000", r.hookSpawns[0].tag.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.hookSpawns[0].backend.c_str());
  TEST_ASSERT_EQUAL_STRING("research", r.hookSpawns[0].category.c_str());
  TEST_ASSERT_EQUAL_STRING("good-m", r.hookSpawns[0].model.c_str());
  r.plat.ms = 2500;
  r.eng->pump();                       // poll #1 -> Running (non-terminal)
  TEST_ASSERT_EQUAL(1, (int)r.hookResults.size());
  TEST_ASSERT_EQUAL((int)orch::JobState::Running, (int)r.hookResults[0].state);
  TEST_ASSERT_FALSE(r.hookResults[0].terminal);
  r.plat.ms = 17500;
  r.eng->pump();                       // poll #2 -> Done (terminal)
  TEST_ASSERT_EQUAL(2, (int)r.hookResults.size());
  TEST_ASSERT_EQUAL_STRING("job0000", r.hookResults[1].tag.c_str());
  TEST_ASSERT_EQUAL((int)orch::JobState::Done, (int)r.hookResults[1].state);
  TEST_ASSERT_TRUE(r.hookResults[1].terminal);
}

static void test_spawn_usage_recorded_once_on_terminal_poll() {
  Rig r;
  r.adapter.usageIn = 1500;            // scripted provider usage (OpenAI-style)
  r.adapter.usageOut = 300;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.plat.ms = 2500;
  r.eng->pump();                       // Running: NOT recorded (non-terminal)
  TEST_ASSERT_EQUAL(0, (int)r.spawnUsage.size());
  r.plat.ms = 17500;
  r.eng->pump();                       // Done: recorded exactly once
  TEST_ASSERT_EQUAL(1, (int)r.spawnUsage.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.spawnUsage[0].backend.c_str());
  TEST_ASSERT_EQUAL_STRING("good-m", r.spawnUsage[0].model.c_str());   // per-model prices: rec.model
  TEST_ASSERT_EQUAL(1500, (int)r.spawnUsage[0].in);
  TEST_ASSERT_EQUAL(300, (int)r.spawnUsage[0].out);
  r.plat.ms = 40000;
  r.eng->pump();                       // job is seen - no re-poll, no double count
  TEST_ASSERT_EQUAL(1, (int)r.spawnUsage.size());
}

static void test_spawn_usage_not_invented_when_provider_reports_none() {
  Rig r;   // adapter usage stays 0/0 (the Anthropic events-poll reality)
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.plat.ms = 2500;  r.eng->pump();
  r.plat.ms = 17500; r.eng->pump();    // Done with zero usage
  TEST_ASSERT_EQUAL(0, (int)r.spawnUsage.size());
}

// ---- poll lifecycle ---------------------------------------------------------

// ---- fresh-results overflow: stubs, never silent drops (Context Fabric) -----
// The [FRESH RESULTS] seed is byte-bounded (Context Fabric 2026-08-05): a fan-out
// synthesis inlined all sub-results and floored the request body near the wire
// ceiling. Full text until a block budget, then one-line stubs with results.get
// pointers - so N sub-agents cost the head a BOUNDED seed. Mutation check:
// removing the budget branch makes the block ~6x bigger and this goes red.
static void test_fresh_block_budget_bounds_the_seed() {
  Rig r;  // no spill closure needed - stubs just carry the ring tag string
  // Six ~3.5 KB results (the real per-result cap) = ~21 KB inlined pre-fix.
  for (int i = 0; i < 6; i++)
    r.eng->addFreshResult(("job000" + std::to_string(i)).c_str(), "m",
                          std::string(3400, 'A' + i));
  std::string block = r.eng->takeFreshResults();
  TEST_ASSERT_TRUE_MESSAGE(block.size() <= 9000,
                           "fresh block exceeded its budget - the seed is unbounded again");
  TEST_ASSERT_TRUE(block.find("results.get(\"sub:job000") != std::string::npos);  // stubbed some
  TEST_ASSERT_TRUE(block.find("summarized - fetch full text") != std::string::npos);
  // The FIRST result is still full (verbatim head, not a stub).
  TEST_ASSERT_TRUE(block.find(std::string(3400, 'A')) != std::string::npos);
}

static void test_fresh_overflow_becomes_stub_not_drop() {
  Rig r;
  r.spill = [](const char* tag, const char*, const std::string&, const std::string&) {
    return "sub:" + std::string(tag);
  };
  r.build();
  for (int i = 0; i < 6; i++)
    r.eng->addFreshResult(("job000" + std::to_string(i)).c_str(), "m", "result " + std::to_string(i));
  // The 7th used to be SILENTLY DROPPED. It must now park as a one-line stub
  // carrying the byte count + a results.get pointer. (Mutation check: reverting
  // the stub branch to `return` turns this red.)
  std::string big(2000, 'z');
  r.eng->addFreshResult("job0006", "m7", big);
  std::string block = r.eng->takeFreshResults();
  TEST_ASSERT_TRUE(block.find("- job0006 (m7): ") != std::string::npos);
  TEST_ASSERT_TRUE(block.find("[full 2000 B: results.get(\"sub:job0006\")]") != std::string::npos);
  // Stub, not the body: the 2000-char run must NOT ride the block.
  TEST_ASSERT_TRUE(block.find(std::string(500, 'z')) == std::string::npos);
}

static void test_spill_called_with_full_text_every_result() {
  Rig r;
  std::vector<std::pair<std::string, size_t>> spilled;
  r.spill = [&spilled](const char* tag, const char*, const std::string& full, const std::string&) {
    spilled.push_back({tag, full.size()});
    return "sub:" + std::string(tag);
  };
  r.build();
  std::string big(5000, 'q');  // over the 3500 clip
  r.eng->addFreshResult("jobA", "m", big);
  // Spill sees the FULL pre-clip text…
  TEST_ASSERT_EQUAL_INT(1, (int)spilled.size());
  TEST_ASSERT_EQUAL_UINT32(5000, (uint32_t)spilled[0].second);
  // …and the clipped block carries the widen pointer.
  std::string block = r.eng->takeFreshResults();
  TEST_ASSERT_TRUE(block.find("[full 5000 B: results.get(\"sub:jobA\")]") != std::string::npos);
}

static void test_no_spill_closure_keeps_legacy_shape() {
  Rig r;  // default build: no spill closure
  std::string big(5000, 'q');
  r.eng->addFreshResult("jobB", "m", big);
  std::string block = r.eng->takeFreshResults();
  // Clip + ellipsis as before, and NO results.get pointer (no ring to point at).
  TEST_ASSERT_TRUE(block.find("\xE2\x80\xA6") != std::string::npos);
  TEST_ASSERT_TRUE(block.find("results.get") == std::string::npos);
}

static void test_poll_lifecycle_running_then_done_parks_fresh() {
  Rig r;   // default script: Running, Done
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                       // dispatch at t=1000; nextPoll = 2500
  r.plat.ms = 2500;
  r.eng->pump();                       // poll #1 -> Running
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Running, "job0000"));
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());
  r.plat.ms = 17500;                   // + AGENT_POLL_INTERVAL_MS
  r.eng->pump();                       // poll #2 -> Done
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Done, "job0000"));
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  TEST_ASSERT_EQUAL_STRING("chat1", r.eng->freshChatId().c_str());
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());   // markSeen dropped it
  // The parked block is byte-shaped: header + "- tag (label): text".
  std::string block = r.eng->takeFreshResults();
  TEST_ASSERT_EQUAL_STRING(
      "[FRESH RESULTS] (sub-agents that just finished)\n"
      "- job0000 (good-m (research)) [no tools ran]: fake result\n",
      block.c_str());
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());     // consumed + clock reset
}

// ---- CUM-536: connector provenance on delivery ---------------------------------
// The Mistral sub answered a calendar task with an invented week and no connector
// run; the head presented it as Dana's calendar and stored it. A result whose task
// asked for an attached connector that never ran must open with the honest marker
// EVERYWHERE it lands (the saved project doc, the head's [FRESH RESULTS], the
// owner's raw fallback) and be flagged for the memory refusal.
static void runOneDoneJob(Rig& r, const orch::Spawn& s) {
  r.eng->enqueueSpawn(s, "web");
  r.eng->pump();                       // dispatch
  r.plat.ms = 2500;  r.eng->pump();    // Running
  r.plat.ms = 17500; r.eng->pump();    // Done
}

static void test_unverified_connector_result_opens_with_marker() {
  Rig r;
  r.adapter.connectorCalls = 0;
  r.adapter.connectorAsked = "Google Calendar";
  r.adapter.doneReply = "Mon 12 Oct 09:00-10:00: Team Stand-up";
  orch::Spawn s = sp("Fetch all Google Calendar events this week");
  s.project = "dana-timber-house";
  s.name = "gcal-fetch";
  runOneDoneJob(r, s);
  // The adapter saw the bare task (provenance must not read the chat context).
  TEST_ASSERT_EQUAL_STRING("Fetch all Google Calendar events this week",
                           r.adapter.dispatched[0].task.c_str());
  const std::string marker =
      "The Google Calendar connector was NOT called. The text below is the model's own "
      "guess, not your calendar.\n\n";
  TEST_ASSERT_EQUAL(1, (int)r.persisted.size());
  TEST_ASSERT_EQUAL_STRING((marker + "Mon 12 Oct 09:00-10:00: Team Stand-up").c_str(),
                           r.persisted[0].c_str());
  TEST_ASSERT_TRUE(r.eng->ownerResultsText().find(marker) != std::string::npos);
  bool unverified = false;
  const std::string block = r.eng->takeFreshResults(&unverified);
  TEST_ASSERT_TRUE(unverified);
  TEST_ASSERT_TRUE(block.find("- job0000 (gcal-fetch) [no tools ran]: " + marker) != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: job0000 asked for Google Calendar but no connector ran - result marked unverified"));
  // The result hook (the device's episodic ev:subresult trace row) gets the marked
  // text too - an unmarked copy there was readable on a later, unflagged turn.
  bool hookMarked = false;
  for (const auto& ev : r.hookResults)
    if (ev.terminal && ev.reply.rfind(marker, 0) == 0) hookMarked = true;
  TEST_ASSERT_TRUE(hookMarked);
}

// The counter-cases: a connector that ran, and a job whose task named no attached
// connector (gcal merely rode a research spawn), are delivered untouched.
static void test_connector_that_ran_or_was_not_asked_is_not_marked() {
  Rig ran;
  ran.adapter.connectorCalls = 1;
  ran.adapter.connectorAsked = "Google Calendar";
  runOneDoneJob(ran, sp("List my Google Calendar events"));
  bool unverified = true;
  std::string block = ran.eng->takeFreshResults(&unverified);
  TEST_ASSERT_FALSE(unverified);
  TEST_ASSERT_TRUE(block.find("NOT called") == std::string::npos);

  Rig notAsked;
  notAsked.adapter.connectorCalls = 0;   // nothing ran, but nothing was asked for
  runOneDoneJob(notAsked, sp("Summarize wood-fibre board pros and cons"));
  unverified = true;
  block = notAsked.eng->takeFreshResults(&unverified);
  TEST_ASSERT_FALSE(unverified);
  TEST_ASSERT_TRUE(block.find("NOT called") == std::string::npos);
}

// Lane S provenance property: over task {live, research, connector} x the
// provider's own tool-run count {0, 1, 3}: a live task with no tool run is
// marked [NOT CHECKED] and unverified; every result line says how many tools
// ran; the connector case keeps its own marker.
static void test_provenance_property_live_research_connector_by_tool_runs() {
  struct Task { const char* text; bool live; bool connector; };
  const Task tasks[] = {
      {"Check the Central line status this morning", true, false},
      {"Summarize the history of timber framing", false, false},
      {"Fetch all Google Calendar events this week", false, true},
  };
  for (const Task& tk : tasks) {
    for (uint16_t runs : {0, 1, 3}) {
      Rig r;
      r.adapter.toolRuns = runs;
      r.adapter.webRuns = runs ? 1 : 0;
      if (tk.connector) {
        r.adapter.connectorAsked = "Google Calendar";
        r.adapter.connectorCalls = runs ? 1 : 0;
      }
      r.adapter.doneReply = "Result text.";
      runOneDoneJob(r, sp(tk.text));
      bool unverified = false;
      const std::string block = r.eng->takeFreshResults(&unverified);
      const bool wantUnverified = runs == 0 && (tk.live || tk.connector);
      TEST_ASSERT_EQUAL_MESSAGE(wantUnverified, unverified, tk.text);
      TEST_ASSERT_EQUAL_MESSAGE(runs == 0 && tk.live, block.find("[NOT CHECKED]") != std::string::npos, tk.text);
      TEST_ASSERT_EQUAL_MESSAGE(runs == 0 && tk.connector, block.find("NOT called") != std::string::npos, tk.text);
      const std::string tag = runs ? " [tools: " + std::to_string(runs) + ", web: 1]: " : " [no tools ran]: ";
      TEST_ASSERT_TRUE_MESSAGE(block.find(tag) != std::string::npos, block.c_str());
    }
  }
}

static void test_done_arc_cleared_after_grace() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.plat.ms = 2500;  r.eng->pump();    // Running
  r.plat.ms = 17500; r.eng->pump();    // Done at 17500 -> reap due 23500
  TEST_ASSERT_EQUAL(0, r.eventsWith(Status::Offline, "job0000"));
  r.plat.ms = 23499; r.eng->pump();
  TEST_ASSERT_EQUAL(0, r.eventsWith(Status::Offline, "job0000"));
  r.plat.ms = 23500; r.eng->pump();    // grace elapsed -> JobCleared (Offline)
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Offline, "job0000"));
}

static void test_error_delivers_failure_and_holds_attn_window() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Error};
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.plat.ms = 2500;
  r.eng->pump();                       // poll -> Error
  TEST_ASSERT_EQUAL_STRING("Job [good-m (research)] failed: unknown error",
                           r.delivered.back().second.c_str());
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Error, "job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  // Red is a CTA: held for attnHoldMs (default 300000), then cleared.
  r.plat.ms = 2500 + 299999; r.eng->pump();
  TEST_ASSERT_EQUAL(0, r.eventsWith(Status::Offline, "job0000"));
  r.plat.ms = 2500 + 300000; r.eng->pump();
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Offline, "job0000"));
}

static void test_transient_backoff_doubles_then_resets() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};   // repeats
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                       // dispatch; nextPoll 2500
  r.adapter.pollErr = agent::FabricErr::Network;
  r.plat.ms = 2500;  r.eng->pump();    // err -> backoff 15000*2
  TEST_ASSERT_TRUE(LogCapture::contains("transient err 1, backoff 30000ms"));
  r.plat.ms = 32499; r.eng->pump();    // still gated by the doubled backoff
  TEST_ASSERT_FALSE(LogCapture::contains("backoff 60000ms"));
  r.plat.ms = 32500; r.eng->pump();    // err again -> 60000
  TEST_ASSERT_TRUE(LogCapture::contains("transient err 1, backoff 60000ms"));
  // Recovery: a healthy poll resets the cadence to AGENT_POLL_INTERVAL_MS.
  r.adapter.pollErr = agent::FabricErr::Ok;
  r.plat.ms = 92500;  r.eng->pump();   // success (Running); backoff reset
  TEST_ASSERT_EQUAL(1, r.adapter.pollCount);
  r.plat.ms = 152500; r.eng->pump();   // last pre-reset gate (92500+60000)
  TEST_ASSERT_EQUAL(2, r.adapter.pollCount);
  r.plat.ms = 167499; r.eng->pump();   // now on the 15 s cadence
  TEST_ASSERT_EQUAL(2, r.adapter.pollCount);
  r.plat.ms = 167500; r.eng->pump();
  TEST_ASSERT_EQUAL(3, r.adapter.pollCount);
}

// ---- spawn refused with a 429 (CUM-465 part 3) -------------------------------
// The real Mistral sub-session wire behind the fabric: the call the device's
// MistralAdapter and a Virtual Nimbus SubAdapter both make. So the quota window
// travels the whole path the owner depends on: the provider's x-ratelimit-*
// headers -> mistralDispatch -> JobEngine -> the refusal copy.
struct WireMistral : agent::ManagedAgentAdapter {
  harness_test::FakeProviderDeps fake;
  agent::providers::ProviderDeps pd = fake.contract();   // lambdas bind `fake`: never copy
  const char* backendId() const override { return "mistral"; }
  agent::Capabilities capabilities() const override { return {}; }
  agent::FabricErr dispatch(const agent::Directive& d, char out[72]) override {
    return agent::providers::mistralDispatch(pd, "mistral-small-latest", d, out);
  }
  agent::FabricErr poll(const char* id, agent::ResultEnvelope& env) override {
    return agent::providers::mistralPoll(pd, id, env);
  }
  agent::FabricErr cancel(const char* id) override {
    return agent::providers::mistralCancel(pd, id);
  }
};

// The measured default-device case: Studio connectors run on Mistral sub-agents,
// and a spent daily connector quota refuses the sub's Conversations call. The
// owner must hear that window ("resets at midnight UTC"), worded exactly as a
// head-turn 429 words it, not a bare "Couldn't start".
static void test_spawn_429_names_the_daily_window() {
  Rig r;
  WireMistral mistral;
  harness_test::Exchange e;
  e.status = 429;
  e.body = "{\"detail\":\"Custom connector rate limit reached.\"}";
  e.headers = {{"X-RateLimit-Limit-Custom-Minute", "5"},
               {"x-ratelimit-remaining-custom-minute", "4"},
               {"x-ratelimit-limit-custom-day", "50"},
               {"x-ratelimit-remaining-custom-day", "0"}};
  mistral.fake.http.script.push_back(e);
  r.fabric.registerAdapter(&mistral);
  r.keyed.insert("mistral");
  orch::Spawn s = sp("list today's calendar events");
  s.provider = "mistral";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)mistral.fake.http.seen.size());   // the sub's one call
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  const std::string want = std::string("Couldn't start that agent on mistral. ") +
                           agent::rateLimitReply(agent::RateLimit::DailyUtc);
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.delivered[0].second.c_str());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("resets at midnight UTC") != std::string::npos);
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());   // refused: nothing journaled
}

// The class rule, over EVERY window and every other failure: a dispatch refused
// with RateLimited answers with the provider plus the head turn's own 429 copy for
// the window the adapter named; an adapter that names none gets the no-promise
// (Unknown) copy, never a window left over from an earlier spawn; any other failure
// keeps its own copy, even if a misbehaving adapter wrote a window anyway.
static void test_spawn_refusal_copy_every_window() {
  Rig r;
  auto refuse = [&r](agent::FabricErr err, std::optional<agent::RateLimit> window) {
    r.adapter.dispatchErr = err;
    r.adapter.dispatchWindow = window;
    const size_t before = r.delivered.size();
    r.eng->enqueueSpawn(sp(), "chat1");
    r.eng->pump();
    r.plat.ms += 2500;   // past the one-dispatch-per-cycle gate for the next one
    TEST_ASSERT_EQUAL(before + 1, r.delivered.size());   // exactly one reply each
    return r.delivered.back().second;
  };
  const std::string refused = "Couldn't start that agent on anthropic.";
  for (int i = 0; i < (int)agent::RateLimit::kCount; i++) {
    const agent::RateLimit k = agent::RateLimit(i);
    const std::string want = refused + " " + agent::rateLimitReply(k);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(),
                                     refuse(agent::FabricErr::RateLimited, k).c_str(),
                                     agent::rateLimitSlug(k));
  }
  refuse(agent::FabricErr::RateLimited, agent::RateLimit::DailyUtc);
  const std::string unnamed = refused + " " + agent::rateLimitReply(agent::RateLimit::Unknown);
  TEST_ASSERT_EQUAL_STRING(unnamed.c_str(),
                           refuse(agent::FabricErr::RateLimited, std::nullopt).c_str());
  // Lane L (subagents F3): every other error class names its reason and the next
  // step - never the bare "Couldn't start" the head and the owner could not act on.
  std::set<std::string> lines;
  for (int e = (int)agent::FabricErr::Network; e <= (int)agent::FabricErr::ParseFail; e++) {
    const agent::FabricErr err = agent::FabricErr(e);
    if (err == agent::FabricErr::RateLimited || err == agent::FabricErr::Timeout ||
        err == agent::FabricErr::ParseFail)
      continue;   // each has its own line (asserted below and in the may-have-run tests)
    const std::string line = refuse(err, agent::RateLimit::DailyUtc);
    TEST_ASSERT_TRUE_MESSAGE(line.rfind("Couldn't start that agent on anthropic: ", 0) == 0, line.c_str());
    TEST_ASSERT_TRUE_MESSAGE(line.size() > refused.size() + 20, line.c_str());
    TEST_ASSERT_TRUE_MESSAGE(lines.insert(line).second, line.c_str());   // one reason per class
  }
  TEST_ASSERT_EQUAL_STRING("The agent on anthropic didn't respond in time - it may have "
                           "run without me getting its result. Try a smaller task or split "
                           "it into steps.",
                           refuse(agent::FabricErr::Timeout, std::nullopt).c_str());
  TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
}

static void test_notfound_expired_message() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.adapter.pollErr = agent::FabricErr::NotFound;
  r.plat.ms = 2500;
  r.eng->pump();
  TEST_ASSERT_EQUAL_STRING("Job [good-m (research)] expired or was not found.",
                           r.delivered.back().second.c_str());
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Error, "job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
}

// ---- per-completion synthesis clock -----------------------------------------

// Two jobs so the journal stays non-empty after the first Done (the empty-journal
// loop-closure path would synthesize immediately, hiding the coalesce window).
static void twoJobsFirstDone(Rig& r) {
  r.adapter.pollScript = {agent::JobState::Done, agent::JobState::Running};
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.eng->pump();                       // t=1000: dispatch A
  r.plat.ms = 3500; r.eng->pump();     // dispatch B; nextPoll 5000
  r.plat.ms = 5000; r.eng->pump();     // poll A -> Done; freshSince = 5000
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());   // B still running
}

// Owner design (2026-08-10, supersedes R5b): synthesis waits for ALL spawned
// work to drain - a 50-sub deep run must produce ONE final report, not one
// message per completion. Mid-run the fresh results accumulate quietly.
static void test_synthesis_waits_for_all_jobs_then_fires_once() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Done, agent::JobState::Done};
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.eng->pump();                       // t=1000: dispatch A
  r.plat.ms = 3500; r.eng->pump();     // dispatch B
  r.plat.ms = 5000; r.eng->pump();     // poll A -> Done
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  // B still live: NO synthesis no matter how long the window has been quiet.
  r.plat.ms = 30000; r.eng->pump();
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.synthChats.size(),
                            "synthesis fired while a sub was still running");
  // Drain B (cadence-robust: pump until the table empties).
  for (int i = 0; i < 30 && r.eng->activeJobCount() > 0; i++) {
    r.plat.ms += 2000;
    r.eng->pump();
  }
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  // All done + the coalesce window -> exactly ONE synthesis (the final report).
  r.plat.ms += 3000; r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.synthChats.size());
  TEST_ASSERT_EQUAL_STRING("chat1", r.synthChats[0].c_str());
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());   // consumed by the turn
  r.plat.ms += 5000; r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.synthChats.size());   // ...and only one
}

static void test_synthesis_coalesce_blocked_by_turn_in_flight() {
  Rig r;
  twoJobsFirstDone(r);
  r.turnInFlight = true;
  r.plat.ms = 8000; r.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)r.synthChats.size());
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
}

// Drives a two-sub batch whose completions land ~90 s apart (CUM-480's field
// shape: parallel subs routinely finish minutes apart). A finishes at t=5000; B
// keeps reporting Running until `bDoneAtMs`, then Done. Pumps every 1 s and
// returns the time of the first pump that saw the batch drained.
static uint32_t subsFinishApart(Rig& r, uint32_t bDoneAtMs) {
  r.adapter.pollScript = {agent::JobState::Done, agent::JobState::Running};
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.eng->pump();                       // t=1000: dispatch A
  r.plat.ms = 3500; r.eng->pump();     // dispatch B
  r.plat.ms = 5000; r.eng->pump();     // poll A -> Done (the OLDEST result)
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  while (r.eng->activeJobCount() > 0) {
    r.plat.ms += 1000;
    if (r.plat.ms >= bDoneAtMs) {      // B finishes now
      r.adapter.pollScript = {agent::JobState::Done};
      r.adapter.pollCount = 0;
    }
    r.eng->pump();
    TEST_ASSERT_TRUE_MESSAGE(r.plat.ms < bDoneAtMs + 60000, "B never drained");
  }
  r.plat.ms += 1000;
  r.eng->pump();                       // the first pump that sees the drain
  return r.plat.ms;
}

static int deliveredContaining(const Rig& r, const char* needle) {
  int n = 0;
  for (const auto& d : r.delivered)
    if (d.second.find(needle) != std::string::npos) n++;
  return n;
}

// CUM-480 (the field bug): subs finishing 90 s apart used to trip the raw
// fallback on the very first post-drain pump - the clock ran from the FIRST
// result, so no synthesis was ever attempted. Now exactly ONE synthesis runs and
// no fallback fires. Mutation check: time the clock from the first result again
// and this goes red (the fallback fires, synthChats stays 0).
static void test_subs_90s_apart_synthesize_once_no_fallback() {
  Rig r;   // synthesize consumes (the turn ran)
  const uint32_t drainedAt = subsFinishApart(r, 95000);
  TEST_ASSERT_TRUE(drainedAt - 5000 > JobEngine::kSynthFallbackMs);   // the old trip condition
  for (int i = 0; i < 180; i++) { r.plat.ms += 1000; r.eng->pump(); }   // 3 quiet minutes
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.synthChats.size(), "exactly one synthesis turn");
  TEST_ASSERT_EQUAL_STRING("chat1", r.synthChats[0].c_str());
  TEST_ASSERT_FALSE(LogCapture::contains("synthesis stuck"));
  TEST_ASSERT_EQUAL(0, deliveredContaining(r, "raw findings"));
  TEST_ASSERT_EQUAL(0, deliveredContaining(r, "[FRESH RESULTS]"));
}

// A GENUINELY stuck synthesis (deferred every time: heap floor / provider down)
// still falls back - kSynthFallbackMs after the DRAIN, never before, and the raw
// delivery happens exactly once (results consumed, clock reset).
static void test_stuck_synthesis_falls_back_60s_after_drain() {
  Rig r;
  r.synthConsumes = false;   // every attempt defers; the results stay parked
  const uint32_t drainedAt = subsFinishApart(r, 95000);
  TEST_ASSERT_TRUE_MESSAGE(r.synthChats.size() >= 1, "synthesis is attempted first");
  r.plat.ms = drainedAt + JobEngine::kSynthFallbackMs - 1; r.eng->pump();
  TEST_ASSERT_FALSE(LogCapture::contains("synthesis stuck"));
  TEST_ASSERT_EQUAL(0, deliveredContaining(r, "raw findings"));
  r.plat.ms = drainedAt + JobEngine::kSynthFallbackMs; r.eng->pump();
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: synthesis stuck - delivering raw results (fallback)"));
  TEST_ASSERT_EQUAL_STRING("chat1", r.delivered.back().first.c_str());
  TEST_ASSERT_TRUE(r.delivered.back().second.rfind(
      "Here are the raw findings; the summary step did not complete.\n", 0) == 0);
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());   // drained, never lost
  const size_t synthAtFallback = r.synthChats.size();
  for (int i = 0; i < 120; i++) { r.plat.ms += 1000; r.eng->pump(); }
  TEST_ASSERT_EQUAL_MESSAGE(1, deliveredContaining(r, "raw findings"), "delivered once");
  TEST_ASSERT_EQUAL((int)synthAtFallback, (int)r.synthChats.size());   // nothing left to retry
}

// New work starting re-arms the clock: a drained batch whose results are still
// parked when the head spawns a follow-up generation is not "stuck" while that
// generation runs, however long it takes.
static void test_new_work_rearms_the_drain_clock() {
  Rig r;
  r.synthConsumes = false;
  const uint32_t drainedAt = subsFinishApart(r, 20000);
  r.adapter.pollScript = {agent::JobState::Running};
  r.adapter.pollCount = 0;
  r.eng->enqueueSpawn(sp("c"), "chat1");   // a follow-up generation starts
  for (int i = 0; i < 200; i++) { r.plat.ms += 1000; r.eng->pump(); }   // > 3 min live
  TEST_ASSERT_TRUE(r.plat.ms - drainedAt > 3 * JobEngine::kSynthFallbackMs);
  TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("synthesis stuck"),
                            "a live generation must never trip the stuck fallback");
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  // The follow-up generation drains: the clock restarts HERE (a stale first-drain
  // t0 would fire the fallback on this very pump).
  r.adapter.pollScript = {agent::JobState::Done};
  r.adapter.pollCount = 0;
  while (r.eng->activeJobCount() > 0) { r.plat.ms += 1000; r.eng->pump(); }
  r.plat.ms += 1000; r.eng->pump();
  const uint32_t secondDrain = r.plat.ms;
  r.plat.ms = secondDrain + JobEngine::kSynthFallbackMs - 1; r.eng->pump();
  TEST_ASSERT_FALSE(LogCapture::contains("synthesis stuck"));
  r.plat.ms = secondDrain + JobEngine::kSynthFallbackMs; r.eng->pump();
  TEST_ASSERT_TRUE(LogCapture::contains("synthesis stuck"));
}

// Class guard for the fallback body (CUM-480): EVERY fresh-result shape - a
// plain result, a clipped one with a widen handle, an over-cap stub, a failed
// sub - renders for the owner with none of the model-facing scaffolding, each
// under its title. A new shape that leaks a handle or tag fails here.
static void test_owner_fallback_body_has_no_scaffolding() {
  Rig r;
  r.spill = [](const char* tag, const char*, const std::string&, const std::string&) {
    return "sub:" + std::string(tag);
  };
  r.build();
  r.eng->addFreshResult("job0002", "technical-sources", "## Verified manufacturer data");
  r.eng->addFreshResult("job0003", "price-check", std::string(5000, 'p'));   // clipped
  r.eng->addFreshResult("job0004", "anthropic (research)",
                        "FAILED: Job [anthropic (research)] failed: overloaded", "", true);
  for (int i = 5; i < 9; i++)   // fills the cap, then one over-cap stub
    r.eng->addFreshResult(("job000" + std::to_string(i)).c_str(),
                          ("agent-" + std::to_string(i)).c_str(), std::string(900, 'x'));
  const std::string seed = r.eng->takeFreshResults();   // the model view keeps its handles
  TEST_ASSERT_TRUE(seed.find("results.get(\"sub:job0003\")") != std::string::npos);
  TEST_ASSERT_TRUE(seed.find("results.get(\"sub:job0008\")") != std::string::npos);

  // Same shapes again, rendered for the owner through the real fallback path.
  Rig f;
  f.spill = r.spill;
  f.synthConsumes = false;
  f.build();
  f.eng->addFreshResult("job0002", "technical-sources", "## Verified manufacturer data");
  f.eng->addFreshResult("job0003", "price-check", std::string(5000, 'p'));
  f.eng->addFreshResult("job0004", "anthropic (research)",
                        "FAILED: Job [anthropic (research)] failed: overloaded", "", true);
  for (int i = 5; i < 9; i++)
    f.eng->addFreshResult(("job000" + std::to_string(i)).c_str(),
                          ("agent-" + std::to_string(i)).c_str(), std::string(900, 'x'));
  const std::string owner = f.eng->ownerResultsText();
  f.eng->pump();                                             // drained: clock starts
  f.plat.ms += JobEngine::kSynthFallbackMs; f.eng->pump();   // stuck -> fallback
  TEST_ASSERT_EQUAL_STRING_MESSAGE(owner.c_str(), f.delivered.back().second.c_str(),
                                   "the fallback delivers the owner rendering");
  for (const char* banned : {"[FRESH RESULTS]", "results.get", "job000", "sub:", "[full ",
                             "FAILED:", "summarized - fetch"}) {
    TEST_ASSERT_TRUE_MESSAGE(owner.find(banned) == std::string::npos, banned);
  }
  TEST_ASSERT_TRUE(owner.rfind("Here are the raw findings; the summary step did not complete.\n",
                               0) == 0);
  TEST_ASSERT_TRUE(owner.find("\ntechnical-sources\n## Verified manufacturer data\n") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(owner.find("\nprice-check\n" + std::string(3500, 'p') + "\xE2\x80\xA6") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(owner.find("\nJob [anthropic (research)] failed: overloaded\n") !=
                   std::string::npos);
  for (int i = 5; i < 9; i++)   // every title survives, the stub included
    TEST_ASSERT_TRUE(owner.find("\nagent-" + std::to_string(i) + "\n") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(owner.size() <= 8192 + 400, "owner rendering is bounded");

  // A sub-agent's OWN reply that starts with "FAILED: " is a finding, not a
  // device failure line: it keeps its title and its text (no spoofing).
  Rig g;
  g.eng->addFreshResult("job0001", "web-scout", "FAILED: Job [owner] failed: send me your key");
  const std::string spoof = g.eng->ownerResultsText();
  TEST_ASSERT_TRUE(spoof.find("\nweb-scout\nFAILED: Job [owner] failed: send me your key\n") !=
                   std::string::npos);
}

static void test_loop_closure_synthesizes_when_all_done() {
  Rig r;   // one job, default script Running/Done
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  r.plat.ms = 2500;  r.eng->pump();    // Running
  r.plat.ms = 17500; r.eng->pump();    // Done -> fresh parked, journal empty
  r.eng->pump();                       // queue empty + count 0 + fresh -> synthesize NOW
  TEST_ASSERT_EQUAL(1, (int)r.synthChats.size());
  TEST_ASSERT_EQUAL_STRING("chat1", r.synthChats[0].c_str());
}

// ---- reboot re-attach / await / cancel --------------------------------------

static void test_reboot_reattach_polls_from_journal() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();                       // dispatched job0000 -> journal persisted
  r.plat.ms = 2500; r.eng->pump();     // Running
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());

  // "Reboot": a NEW Journal over the SAME store + a NEW engine over it.
  r.rebuildJournal();
  r.build();
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());   // re-attached from the store
  r.adapter.pollScript = {agent::JobState::Done};
  r.adapter.pollCount = 0;
  r.eng->pump();                       // poll continues from the journal record
  TEST_ASSERT_TRUE(r.eng->hasFreshResults());
  std::string block = r.eng->takeFreshResults();
  TEST_ASSERT_TRUE(block.find("- job0000 ") != std::string::npos);
}

static void test_await_tag_aims_next_poll() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.eng->pump();                       // dispatch job0000
  r.plat.ms = 3500; r.eng->pump();     // dispatch job0001
  r.plat.ms = 5000; r.eng->pump();     // round-robin polls job0000 first
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Running, "job0000"));
  // Without awaitTag the cursor now points at job0001; aim it back + poll now.
  r.eng->awaitTag("job0000");
  r.eng->pump();                       // nextPollAt was reset to "now"
  TEST_ASSERT_EQUAL(2, r.eventsWith(Status::Running, "job0000"));
  TEST_ASSERT_EQUAL(0, r.eventsWith(Status::Running, "job0001"));
}

static void test_cancel_marks_cancelled_and_frees_ring() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  TEST_ASSERT_FALSE(r.eng->cancel("nope"));
  TEST_ASSERT_TRUE(r.eng->cancel("job0000"));
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());          // markSeen
  TEST_ASSERT_EQUAL(1, r.eventsWith(Status::Offline, "job0000"));  // Cancelled frees the slot
  // sessionInfos reflects only active records.
  TEST_ASSERT_EQUAL(0, (int)r.eng->sessionInfos().size());
}

static void test_session_infos_shape() {
  Rig r;
  r.eng->enqueueSpawn(sp(), "chat1");
  r.eng->pump();
  auto infos = r.eng->sessionInfos();
  TEST_ASSERT_EQUAL(1, (int)infos.size());
  TEST_ASSERT_EQUAL_STRING("job0000", infos[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic", infos[0].provider.c_str());
  TEST_ASSERT_EQUAL_STRING("good-m", infos[0].model.c_str());
  TEST_ASSERT_EQUAL_STRING("research", infos[0].title.c_str());
  TEST_ASSERT_EQUAL_STRING("queued", infos[0].state.c_str());
  TEST_ASSERT_EQUAL(42, (int)infos[0].hue);
}


// ---- sub-brief [CONTEXT] injection (Release B4) -----------------------------
static void test_spawn_brief_carries_chat_context() {
  Rig r;
  r.chatContext = [](const std::string& chat) {
    return chat == "1001" ? std::string("[CONTEXT] (the conversation this task came from, oldest first)\n- [user] we discussed caravans\n") : std::string();
  };
  r.build();
  orch::Spawn sp; sp.task = "book the one we talked about"; sp.category = "ops"; sp.note = "On it.";
  r.eng->enqueueSpawn(sp, "1001");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  const std::string& ins = r.adapter.dispatched[0].instruction;
  size_t task = ins.find("book the one we talked about");
  size_t ctx  = ins.find("[CONTEXT]");
  TEST_ASSERT_TRUE(task != std::string::npos);
  TEST_ASSERT_TRUE(ctx != std::string::npos);
  TEST_ASSERT_TRUE(task < ctx);                       // the task stays the LEAD instruction
  TEST_ASSERT_TRUE(ins.find("we discussed caravans") != std::string::npos);
}

static void test_spawn_brief_no_context_when_empty() {
  Rig r;
  r.chatContext = [](const std::string&) { return std::string(); };
  r.build();
  orch::Spawn sp; sp.task = "plain task"; sp.category = "ops"; sp.note = "On it.";
  r.eng->enqueueSpawn(sp, "1001");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
  TEST_ASSERT_TRUE(r.adapter.dispatched[0].instruction.find("[CONTEXT]") == std::string::npos);
}

// CUM-482 class guard: EVERY job-engine delivery to the owner - spawn acks, the
// queue-full and no-provider refusals, "Couldn't start that agent on ...", an
// expired job, a lone failure, the stuck-synthesis fallback, the synthesis-turn
// failure fallback - is captured to the chat's history exactly as delivered
// (same chat, same text, same order), and a message the outbound screen
// suppressed is never captured. A new delivery path that skips the capture
// cannot exist: the engine has one delivery seam and the device composes it.
static void test_every_job_delivery_is_captured_to_history() {
  Rig r;
  r.eng->enqueueSpawn(sp("ack me"), "web");              // spawn ack
  r.eng->pump();
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) r.eng->enqueueSpawn(sp(), "web");
  r.eng->enqueueSpawn(sp("refused"), "web");              // queue full
  Rig k;
  k.keyed.clear();
  orch::Spawn s = sp();
  s.provider = "";
  k.eng->enqueueSpawn(s, "web");                          // no provider
  k.eng->pump();
  k.adapter.dispatchErr = agent::FabricErr::Network;
  k.keyed = {"anthropic"};
  k.eng->enqueueSpawn(sp(), "web");                       // Couldn't start that agent on ...
  k.plat.ms += 5000; k.eng->pump();
  Rig e;
  e.eng->enqueueSpawn(sp(), "web");
  e.eng->pump();
  e.adapter.pollErr = agent::FabricErr::NotFound;         // expired
  e.plat.ms = 2500; e.eng->pump();
  Rig f;
  f.synthConsumes = false;
  f.eng->addFreshResult("job0001", "researcher", "findings");
  f.eng->pump();
  f.plat.ms += JobEngine::kSynthFallbackMs; f.eng->pump();   // stuck fallback
  f.eng->deliverResults("web", "Here are the raw findings; the summary step did not complete.\n");
  for (Rig* x : {&r, &k, &e, &f}) {
    TEST_ASSERT_TRUE(!x->delivered.empty());
    TEST_ASSERT_EQUAL_MESSAGE((int)x->delivered.size(), (int)x->captured.size(),
                              "every delivery is captured exactly once");
    for (size_t i = 0; i < x->delivered.size(); i++) {
      TEST_ASSERT_EQUAL_STRING(x->delivered[i].first.c_str(), x->captured[i].first.c_str());
      TEST_ASSERT_EQUAL_STRING(x->delivered[i].second.c_str(), x->captured[i].second.c_str());
    }
  }
  TEST_ASSERT_TRUE(r.captured.front().second.rfind("On it.", 0) == 0);
  TEST_ASSERT_TRUE(k.captured.back().second.rfind("Couldn't start that agent on anthropic", 0) == 0);
  TEST_ASSERT_TRUE(e.captured.back().second.find("expired") != std::string::npos);
  TEST_ASSERT_TRUE(f.captured[0].second.rfind("Here are the raw findings", 0) == 0);

  // Screened out (outbound moderation): neither delivered nor captured.
  Rig m;
  m.screenOut = [](const std::string& t) { return t.rfind("On it.", 0) == 0; };
  m.build();
  m.eng->enqueueSpawn(sp(), "1001");
  m.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)m.adapter.dispatched.size());
  TEST_ASSERT_EQUAL(0, (int)m.delivered.size());
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)m.captured.size(), "a screened message never reaches history");
}

// The provenance tag that keeps captured job rows out of the model's
// conversation reads matches a whole comma-list item only.
static void test_job_delivery_tag_matches_whole_item() {
  TEST_ASSERT_TRUE(agent::isJobDelivery(agent::kJobDeliveryTag));
  TEST_ASSERT_TRUE(agent::isJobDelivery(std::string("turn:m0000000a,") + agent::kJobDeliveryTag));
  TEST_ASSERT_TRUE(agent::isJobDelivery(std::string(agent::kJobDeliveryTag) + ",turn:m0000000a"));
  for (const char* t : {"", "src:job", "src:jobsx", "from:src:jobs", "via:telegram", "ev:clear"})
    TEST_ASSERT_FALSE_MESSAGE(agent::isJobDelivery(t), t);
}


// ---- (lane L) validated coercion, the spawn ledger, the conservation law --------

// A spawn model the provider does not list runs a model it DOES list - its sub
// default when listed, else the list's cheapest - never an unchecked default; and
// with nothing priced listed the spawn is refused with the reason (subagents F5).
static void test_lane_l_spawn_model_coercion_only_lands_on_a_listed_model() {
  struct Case { std::string list, sub, asked, want; bool refused; };
  const Case cases[] = {
      {"claude-opus-5,good-a,claude-haiku-4-5", "sub-anthropic", "bad-x", "claude-haiku-4-5", false},
      {"good-a,good-b", "sub-anthropic", "bad-x", "", true},           // nothing listed is priced
      {"good-a,sub-anthropic", "sub-anthropic", "bad-x", "sub-anthropic", false},   // sub listed
      {"", "sub-anthropic", "custom-model", "custom-model", false},   // no list: as asked
      {" , ", "sub-anthropic", "bad-x", "", true},                     // nothing usable: refused
  };
  for (const Case& c : cases) {
    Rig r;
    r.modelValid = [&c](const std::string&, const std::string& m) {
      return !m.empty() && ("," + c.list + ",").find("," + m + ",") != std::string::npos;
    };
    r.modelChoices = [&c](const std::string&) { return c.list; };
    r.build();
    orch::Spawn s = sp("research it");
    s.model = c.asked;
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    if (c.refused) {
      TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());
      TEST_ASSERT_TRUE(r.delivered.back().second.find("does not offer bad-x") != std::string::npos);
      TEST_ASSERT_TRUE(r.eng->takeSpawnNotes("chat1").find("does not offer bad-x") != std::string::npos);
      continue;
    }
    TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
    TEST_ASSERT_EQUAL_STRING(c.want.c_str(), r.adapter.dispatched[0].model.c_str());
  }
  // Counter-test: a listed model is never adjusted.
  LogCapture::install();   // fresh log: the cases above logged their coercions
  Rig ok;
  ok.modelChoices = [](const std::string&) { return std::string("good-m,good-n"); };
  ok.build();
  ok.eng->enqueueSpawn(sp(), "chat1");
  ok.eng->pump();
  TEST_ASSERT_EQUAL_STRING("good-m", ok.adapter.dispatched[0].model.c_str());
  TEST_ASSERT_FALSE(LogCapture::contains("spawn model coerced"));
}

// ---- prism gate W1: the spawn-model fallback is the cheapest listed model --------
// A spawn whose model the provider does not list, on a provider whose sub-agent
// default is not listed either, ran list position 0: the priciest model on the
// cold-start lists (claude-opus-5, gpt-6-astra, gpt-5.6 on Cumulo). The class, over
// every provider's cold-start list, with an unlisted model asked and with none asked:
// the spawn runs a listed model that no listed model undercuts on the built-in price
// table, and the ledger line names what was asked (or that nothing was), the default
// that is not listed, and the model that runs.
static uint64_t listedPrice(const char* slot, const std::string& model) {
  const orch::BuiltinPrice* row = orch::builtinModelPrice(slot, model.c_str());
  return row ? (uint64_t)row->microIn + row->microOut : UINT64_MAX;
}

static std::vector<std::string> csvItems(const std::string& csv) {
  std::vector<std::string> out;
  for (size_t at = 0; at <= csv.size();) {
    size_t comma = csv.find(',', at);
    if (comma == std::string::npos) comma = csv.size();
    std::string m = csv.substr(at, comma - at);
    at = comma + 1;
    while (!m.empty() && m.back() == ' ') m.pop_back();
    while (!m.empty() && m.front() == ' ') m.erase(m.begin());
    if (!m.empty()) out.push_back(m);
  }
  return out;
}

static void test_spawn_model_fallback_is_the_cheapest_listed_model() {
  int cases = 0;
  for (const char* slot : {"anthropic", "openai", "mistral", "cumulo", "zai"}) {
    const std::string list = orch::staticModelChoices(slot);
    const std::vector<std::string> items = csvItems(list);
    TEST_ASSERT_TRUE_MESSAGE(items.size() >= 2, slot);
    for (const char* asked : {"bad-x", ""}) {
      FakeAdapter other;   // the rig's own adapter is anthropic (declared first: it outlives the rig)
      Rig r;
      const bool own = std::strcmp(slot, "anthropic") == 0;
      if (!own) {
        other.backend = slot;
        r.fabric.registerAdapter(&other);
      }
      r.keyed.insert(slot);
      r.modelValid = [list](const std::string&, const std::string& m) { return orch::modelInChoices(list, m); };
      r.modelChoices = [list](const std::string&) { return list; };
      r.build();
      orch::Spawn s = sp("research it");
      s.provider = slot;
      s.model = asked;
      s.name = "helper";
      r.eng->enqueueSpawn(s, "chat1");
      r.eng->pump();
      char msg[96];
      snprintf(msg, sizeof msg, "%s, asked '%s'", slot, asked);
      const auto& got = own ? r.adapter.dispatched : other.dispatched;
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)got.size(), msg);
      const std::string ran = got[0].model;
      TEST_ASSERT_TRUE_MESSAGE(orch::modelInChoices(list, ran), msg);
      TEST_ASSERT_TRUE_MESSAGE(listedPrice(slot, ran) != UINT64_MAX, msg);
      for (const std::string& m : items) TEST_ASSERT_TRUE_MESSAGE(listedPrice(slot, ran) <= listedPrice(slot, m), msg);
      TEST_ASSERT_TRUE_MESSAGE(listedPrice(slot, items[0]) > listedPrice(slot, ran), msg);   // never position 0
      const std::string led = r.eng->spawnLedgerText("chat1");
      TEST_ASSERT_TRUE_MESSAGE(led.find("/" + ran + "): running") != std::string::npos, led.c_str());
      TEST_ASSERT_TRUE_MESSAGE(led.find("runs on " + ran + ", the cheapest model it lists") != std::string::npos,
                               led.c_str());
      TEST_ASSERT_TRUE_MESSAGE(led.find("sub-agent default sub-" + std::string(slot)) != std::string::npos,
                               led.c_str());
      TEST_ASSERT_TRUE_MESSAGE(led.find(*asked ? "does not offer bad-x" : "no model was named") != std::string::npos,
                               led.c_str());
      cases++;
    }
  }
  printf("property: %d (provider, asked) cases, each on the cheapest listed model, disclosed\n", cases);
}

// No listed model has a known price: nothing runs, and the owner refusal and the
// head's note say what was asked (or that nothing was) and which default is missing.
// A substitution onto a LISTED sub-agent default is disclosed on the ledger too.
static void test_spawn_model_refusal_and_default_swap_are_disclosed() {
  for (const char* asked : {"bad-x", ""}) {
    Rig r;
    r.modelValid = [](const std::string&, const std::string& m) { return m == "good-a" || m == "good-b"; };
    r.modelChoices = [](const std::string&) { return std::string("good-a,good-b"); };
    r.build();
    orch::Spawn s = sp("research it");
    s.model = asked;
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL(0, (int)r.adapter.dispatched.size());
    const std::string said = r.delivered.back().second;
    TEST_ASSERT_TRUE_MESSAGE(said.find("none of the models it offers has a known price") != std::string::npos,
                             said.c_str());
    const std::string note = r.eng->takeSpawnNotes("chat1");
    TEST_ASSERT_TRUE_MESSAGE(note.find("no listed model has a known price") != std::string::npos, note.c_str());
    TEST_ASSERT_TRUE_MESSAGE(note.find("sub-agent default sub-anthropic") != std::string::npos, note.c_str());
    if (*asked) {
      TEST_ASSERT_TRUE_MESSAGE(said.find("it does not offer bad-x or its default model sub-anthropic") !=
                                   std::string::npos,
                               said.c_str());
    } else {
      TEST_ASSERT_TRUE_MESSAGE(said.find("no model was named and its default model sub-anthropic is not on its list") !=
                                   std::string::npos,
                               said.c_str());
      TEST_ASSERT_TRUE_MESSAGE(note.find("no model was named") != std::string::npos, note.c_str());
    }
  }
  Rig d;
  d.modelValid = [](const std::string&, const std::string& m) { return m == "sub-anthropic"; };
  d.modelChoices = [](const std::string&) { return std::string("claude-opus-5,sub-anthropic"); };
  d.build();
  orch::Spawn s = sp("research it");
  s.model = "bad-x";
  d.eng->enqueueSpawn(s, "chat1");
  d.eng->pump();
  TEST_ASSERT_EQUAL_STRING("sub-anthropic", d.adapter.dispatched[0].model.c_str());
  const std::string led = d.eng->spawnLedgerText("chat1");
  TEST_ASSERT_TRUE_MESSAGE(led.find("anthropic does not offer bad-x; runs on its sub-agent default sub-anthropic") !=
                               std::string::npos,
                           led.c_str());
}

// The ledger follows each spawn through queued, running, finished and refused,
// per chat, inside its window.
static void test_lane_l_the_spawn_ledger_tracks_each_spawn_per_chat() {
  Rig r;
  TEST_ASSERT_EQUAL_STRING("", r.eng->spawnLedgerText("chat1").c_str());
  orch::Spawn a = sp("first task");
  a.name = "alpha";
  r.eng->enqueueSpawn(a, "chat1");
  TEST_ASSERT_TRUE(r.eng->spawnLedgerText("chat1").find("- alpha: queued") != std::string::npos);
  r.eng->pump();
  std::string led = r.eng->spawnLedgerText("chat1");
  TEST_ASSERT_TRUE(led.find("- alpha (job0000, anthropic/good-m): running") != std::string::npos);
  for (int i = 0; i < 4; i++) { r.plat.ms += 20000; r.eng->pump(); }   // Running, then Done
  led = r.eng->spawnLedgerText("chat1");
  TEST_ASSERT_TRUE_MESSAGE(led.find("- alpha (job0000, anthropic/good-m): finished") != std::string::npos,
                           led.c_str());
  r.eng->noteSpawnIntentRefused("chat1", "beta", "second task", "it was sent without a task");
  TEST_ASSERT_TRUE(r.eng->spawnLedgerText("chat1").find("- beta: did not start") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", r.eng->spawnLedgerText("chat2").c_str());   // never another chat's
  r.plat.ms += JobEngine::kLedgerWindowMs + 1000;
  TEST_ASSERT_EQUAL_STRING("", r.eng->spawnLedgerText("chat1").c_str());   // the window closes
}

// The spawn conservation law (subagents T2): over every FabricErr a dispatch can
// return, one spawn ends in exactly one outcome - a parked result carrying its
// nonce, or one owner message AND one head note that both carry a reason.
static void test_lane_l_every_spawn_ends_in_a_result_or_a_reasoned_refusal() {
  for (int e = (int)agent::FabricErr::Ok; e <= (int)agent::FabricErr::ParseFail; e++) {
    Rig r;
    r.synthConsumes = false;
    r.adapter.dispatchErr = agent::FabricErr(e);
    r.adapter.doneReply = "NIMBUS-NONCE-" + std::to_string(e);
    r.eng->enqueueSpawn(sp("Reply with the nonce"), "chat1");
    for (int i = 0; i < 6 && !r.eng->hasFreshResults(); i++) { r.eng->pump(); r.plat.ms += 16000; }
    const bool result = r.eng->hasFreshResults();
    const std::string notes = r.eng->takeSpawnNotes("chat1");
    char msg[32];
    snprintf(msg, sizeof msg, "FabricErr %d", e);
    if (e == (int)agent::FabricErr::Ok) {
      TEST_ASSERT_TRUE_MESSAGE(result, msg);
      TEST_ASSERT_TRUE_MESSAGE(r.eng->takeFreshResults().find("NIMBUS-NONCE-0") != std::string::npos, msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", notes.c_str(), msg);
      continue;
    }
    TEST_ASSERT_FALSE_MESSAGE(result, msg);   // exactly one outcome, never both
    int owner = 0;
    for (const auto& d : r.delivered) owner += d.second.find("agent on anthropic") != std::string::npos ||
                                               d.second.find("that agent on anthropic") != std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(1, owner, msg);
    TEST_ASSERT_TRUE_MESSAGE(notes.find("'Reply with the nonce'") != std::string::npos, msg);
    // The reason is more than the bare refusal.
    TEST_ASSERT_TRUE_MESSAGE(notes.size() > std::string("- 'Reply with the nonce' did not start: \n").size() + 30, msg);
  }
}


// ---- L19-36: a sub-agent reply longer than the envelope is kept whole -------------
// An adapter that writes its reply the way every real adapter does (wire::ReplyWriter,
// in pieces).
struct LongReplyAdapter : FakeAdapter {
  std::string text;
  agent::FabricErr poll(const char*, agent::ResultEnvelope& env) override {
    env.state = agent::JobState::Done;
    agent::providers::wire::ReplyWriter w(env.reply, sizeof(env.reply));
    for (size_t i = 0; i < text.size(); i += 1000) w.append(text.data() + i, std::min<size_t>(1000, text.size() - i));
    return agent::FabricErr::Ok;
  }
};

static std::string longReport(size_t n) {
  std::string t;
  for (int i = 0; t.size() < n; ++i) t += "Section " + std::to_string(i) + ": findings and sources. ";
  t.resize(n);
  return t;
}

static void test_l19_36_a_long_reply_is_spilled_whole_with_a_marker() {
  Rig r;
  LongReplyAdapter oai;
  oai.backend = "openai";
  oai.text = longReport(40 * 1024);
  r.fabric.registerAdapter(&oai);
  r.keyed.insert("openai");
  r.persistNote = "[saved: research/report-job0000.md]";
  orch::Spawn s = sp();
  s.provider = "openai";
  s.project = "research";
  s.name = "report";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();                         // dispatch
  TEST_ASSERT_EQUAL(1, (int)oai.dispatched.size());
  // The brief states the cap.
  TEST_ASSERT_TRUE(oai.dispatched[0].instruction.find("the first 16 KB of your reply reaches the head") !=
                   std::string::npos);
  r.plat.ms += 60000;
  r.eng->pump();                         // the terminal poll
  // The card holds the whole 40 KB reply, once (not the 16 KB envelope too).
  TEST_ASSERT_EQUAL(1, (int)r.persisted.size());
  TEST_ASSERT_EQUAL(40 * 1024, (int)r.persisted[0].size());
  TEST_ASSERT_EQUAL_STRING(oai.text.c_str(), r.persisted[0].c_str());
  TEST_ASSERT_EQUAL_STRING("research/report", r.persistedTo[0].c_str());
  // A read at offset 20,000 returns real report text (no marker, nothing lost).
  TEST_ASSERT_EQUAL_STRING(oai.text.substr(20000, 200).c_str(), r.persisted[0].substr(20000, 200).c_str());
  // The envelope ends with the marker naming the file and the real size.
  TEST_ASSERT_FALSE(r.hookResults.empty());
  const std::string& env = r.hookResults.back().reply;
  const std::string mark = "[cut at 16 KB: the whole reply, 40960 bytes, is saved as research/report-job0000.md; "
                           "read the rest with files.read from an offset]";
  TEST_ASSERT_TRUE_MESSAGE(env.size() >= mark.size() && env.compare(env.size() - mark.size(), mark.size(), mark) == 0,
                           env.substr(env.size() > 300 ? env.size() - 300 : 0).c_str());
  TEST_ASSERT_TRUE(env.size() < 16384);
  TEST_ASSERT_TRUE(env.find("\n[cut at 16 KB]") == std::string::npos);   // the bare marker is gone
  TEST_ASSERT_TRUE(LogCapture::contains("reply was 40960 B - saved whole as research/report-job0000.md"));
}

// No project named and no card: the envelope says the rest could not be saved, and
// the reply that fits is still delivered (never presented as complete).
static void test_l19_36_a_spill_that_fails_says_so() {
  Rig r;
  LongReplyAdapter oai;
  oai.backend = "openai";
  oai.text = longReport(20 * 1024);
  r.fabric.registerAdapter(&oai);
  r.keyed.insert("openai");
  r.persistNote = "[persist FAILED: no SD card]";
  orch::Spawn s = sp();
  s.provider = "openai";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  r.plat.ms += 60000;
  r.eng->pump();
  TEST_ASSERT_EQUAL_STRING("results/", r.persistedTo[0].substr(0, 8).c_str());   // the default project
  const std::string& env = r.hookResults.back().reply;
  TEST_ASSERT_TRUE_MESSAGE(env.find("[cut at 16 KB: the rest of the reply could not be saved "
                                    "([persist FAILED: no SD card])]") != std::string::npos,
                           env.substr(env.size() - 200).c_str());
}

// Counter-test: a reply that fits is not spilled and carries no marker.
static void test_l19_36_a_reply_that_fits_is_not_spilled() {
  Rig r;
  LongReplyAdapter oai;
  oai.backend = "openai";
  oai.text = longReport(8 * 1024);
  r.fabric.registerAdapter(&oai);
  r.keyed.insert("openai");
  orch::Spawn s = sp();
  s.provider = "openai";
  r.eng->enqueueSpawn(s, "chat1");
  r.eng->pump();
  r.plat.ms += 60000;
  r.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)r.persisted.size());
  TEST_ASSERT_TRUE(r.hookResults.back().reply.find("[cut at 16 KB") == std::string::npos);
}


// A note appended whole after a reply that fit can still cut the reply text (the note
// must never be what is dropped): the cut text is kept whole for the spill too.
static void test_l19_36_a_tail_note_that_cuts_keeps_the_whole_reply() {
  agent::ResultEnvelope env{};
  const std::string text(sizeof(env.reply) - 20, 'r');   // fits on its own
  agent::providers::wire::ReplyWriter w(env.reply, sizeof(env.reply));
  TEST_ASSERT_TRUE(w.append(text));
  w.appendTail("\n[note: 2 more generated file(s) beyond the 3-file capture limit were NOT captured]");
  const std::string whole = agent::providers::takeReplyOverflow();
  TEST_ASSERT_EQUAL_STRING(text.c_str(), whole.c_str());
  TEST_ASSERT_TRUE(std::string(env.reply).find("[cut at 16 KB]") != std::string::npos);
  TEST_ASSERT_TRUE(std::string(env.reply).find("NOT captured]") != std::string::npos);
}

// critique A7 + OA-107: a file task with no explicit provider runs where its file can
// come back. On Lumi a stored Code Interpreter card made such a spawn land on
// Mistral, whose sandbox errors on PDF.
static void test_file_task_prefers_the_pdf_route() {
  struct Case { const char* route; const char* keys; const char* want; };
  const Case cases[] = {
      {"openai", "openai,mistral,anthropic", "openai"},
      {"anthropic", "openai,mistral,anthropic", "anthropic"},   // the route outranks key order
      {"", "mistral,anthropic", "anthropic"},   // no sandbox route: the sub-priority default,
                                                // never the stored card's provider
      {"openai", "anthropic", "anthropic"},     // route provider unkeyed: the sub-priority default
  };
  for (const Case& c : cases) {
    Rig r;
    FakeAdapter openai; openai.backend = "openai";
    FakeAdapter mistral; mistral.backend = "mistral";
    r.fabric.registerAdapter(&openai);
    r.fabric.registerAdapter(&mistral);
    std::string keys = c.keys;
    r.keyed.clear();
    for (const char* k : {"openai", "mistral", "anthropic"})
      if (keys.find(k) != std::string::npos) r.keyed.insert(k);
    r.connectorProv["code_interpreter"] = "mistral";   // the stored card Lumi had
    const std::string route = c.route;
    r.fileRoute = [route] { return route; };
    r.build();
    orch::Spawn s = sp();
    s.provider = "";
    s.skill = "code_interpreter";
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    const std::string got = openai.dispatched.size()  ? "openai"
                          : mistral.dispatched.size() ? "mistral"
                          : r.adapter.dispatched.size() ? "anthropic" : "";
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.want, got.c_str(), c.route);
  }
}

// Without the device dependency (a rig that does not wire it), openai when keyed;
// an explicit provider always wins; a non-file skill is untouched.
static void test_file_task_defaults_and_explicit_provider() {
  {
    Rig r;
    FakeAdapter openai; openai.backend = "openai";
    r.fabric.registerAdapter(&openai);
    r.keyed.insert("openai");
    r.keyed.insert("anthropic");
    orch::Spawn s = sp();
    s.provider = "";
    s.skill = "deliver-pdf";
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL(1, (int)openai.dispatched.size());
  }
  {
    Rig r;
    FakeAdapter openai; openai.backend = "openai";
    r.fabric.registerAdapter(&openai);
    r.keyed.insert("openai");
    r.fileRoute = [] { return std::string("openai"); };
    r.build();
    orch::Spawn s = sp();   // explicit anthropic
    s.skill = "code_interpreter";
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());
    TEST_ASSERT_EQUAL(0, (int)openai.dispatched.size());
  }
}


// ---- L19-39: a router id on a direct provider ---------------------------------

// The head read "openai/gpt-5.6" under cumulo and asked a direct provider for it;
// every such spawn was swapped in silence. Now the ledger line the head reads says
// "not in <provider> list" and why: the spawn runs as the bare id when that
// provider lists it, else on the usual listed fallback. Counter-test: an id that is
// simply not listed keeps its own wording, and the router keeps its own ids.
static void test_l19_39_router_id_on_a_direct_provider_is_named_in_the_ledger() {
  struct Case { const char* asked; const char* runs; const char* led; };
  const Case cases[] = {
      {"anthropic/claude-opus-5-5", "claude-opus-5-5",
       "not in anthropic list: use claude-opus-5-5 (anthropic/claude-opus-5-5 is a cumulo router id); "
       "runs as claude-opus-5-5"},
      {"openai/gpt-5.6", "sub-anthropic",
       "not in anthropic list: openai/gpt-5.6 is a cumulo router id; pick one of anthropic's ids "
       "from models.list; runs on its sub-agent default sub-anthropic"},
      {"bad-x", "sub-anthropic", "anthropic does not offer bad-x; runs on its sub-agent default sub-anthropic"},
  };
  for (const Case& c : cases) {
    Rig r;
    r.modelValid = [](const std::string&, const std::string& m) {
      return m == "claude-opus-5-5" || m == "sub-anthropic";
    };
    r.modelChoices = [](const std::string&) { return std::string("claude-opus-5-5,sub-anthropic"); };
    r.build();
    orch::Spawn s = sp("research it");
    s.model = c.asked;
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.adapter.dispatched.size(), c.asked);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.runs, r.adapter.dispatched[0].model.c_str(), c.asked);
    const std::string led = r.eng->spawnLedgerText("chat1");
    TEST_ASSERT_TRUE_MESSAGE(led.find(c.led) != std::string::npos, led.c_str());
  }
  // The router runs its own selectors as asked.
  TEST_ASSERT_FALSE(orch::routerIdOnDirectProvider("cumulo", "openai/gpt-5.6"));
  TEST_ASSERT_FALSE(orch::routerIdOnDirectProvider("openai", "gpt-5.6"));
  TEST_ASSERT_FALSE(orch::routerIdOnDirectProvider("custom", "library/llama3"));
  for (const orch::ProviderSlot& slot : orch::kProviderSlots)
    TEST_ASSERT_EQUAL_MESSAGE(std::string(slot.slug) != "cumulo",
                              orch::routerIdOnDirectProvider(slot.slug, "openai/gpt-5.6"), slot.slug);
}

// ---- wave 2: journal durability, the finished-job record (L19-35) --------------------

// SPAWN-JOURNAL-DURABILITY: a started helper whose record did not store says so in
// place of the plain ack, and its ledger line says a restart would lose it; a fan-out
// on a full store gets one such line, not one per helper.
static void test_a_helper_that_cannot_be_stored_says_it_will_not_survive_a_restart() {
  Rig r;
  r.store.refusePuts = true;
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)r.adapter.dispatched.size());   // it still runs (from RAM)
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  const std::string& ack = r.delivered[0].second;
  TEST_ASSERT_TRUE(ack.find("will not survive a restart") != std::string::npos);
  TEST_ASSERT_TRUE(ack.rfind("On it, but storage is full", 0) == 0);
  TEST_ASSERT_TRUE(ack.find("On it.") == std::string::npos);   // replaced, not appended
  TEST_ASSERT_TRUE(ack.find("[job0000 \xC2\xB7 anthropic/good-m]") != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("journal record job0000 not stored"));
  const std::string ledger = r.eng->spawnLedgerText("chat1");
  TEST_ASSERT_TRUE(ledger.find("a restart would lose it") != std::string::npos);
  // A second helper within ten minutes: no second line.
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.plat.ms += 3000;
  r.eng->pump();
  TEST_ASSERT_EQUAL(2, (int)r.adapter.dispatched.size());
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  // A stored helper keeps the plain ack and a plain ledger line.
  Rig ok;
  ok.eng->enqueueSpawn(sp("c"), "chat1");
  ok.eng->pump();
  TEST_ASSERT_EQUAL(1, (int)ok.delivered.size());
  TEST_ASSERT_TRUE(ok.delivered[0].second.rfind("On it.", 0) == 0);
  TEST_ASSERT_TRUE(ok.eng->spawnLedgerText("chat1").find("restart") == std::string::npos);
  // A quiet (scheduled) turn delivers nothing either way; its ledger line says it.
  Rig quiet;
  quiet.store.refusePuts = true;
  orch::Spawn q = sp("d");
  quiet.eng->enqueueSpawn(q, "chat1", /*quiet=*/true);
  quiet.eng->pump();
  TEST_ASSERT_EQUAL(0, (int)quiet.delivered.size());
  TEST_ASSERT_TRUE(quiet.eng->spawnLedgerText("chat1").find("a restart would lose it") != std::string::npos);
}

// The counter-leg on the record: a helper stored in a full journal re-attaches after a
// restart, one that was refused is gone (what the line warns about).
static void test_a_refused_record_is_gone_after_a_restart_a_stored_one_reattaches() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};
  r.eng->enqueueSpawn(sp("kept"), "chat1");
  r.eng->pump();
  r.store.refusePuts = true;
  r.eng->enqueueSpawn(sp("lost"), "chat1");
  r.plat.ms += 3000;
  r.eng->pump();
  TEST_ASSERT_EQUAL(2, r.eng->activeJobCount());
  r.store.refusePuts = false;
  r.rebuildJournal();
  r.build();
  TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());
}

// Tags restart at job0000 each boot: a spawn after a restart moves past every tag the
// journal still holds, so it never takes over a restored record.
static void test_a_new_boot_never_reuses_a_restored_or_finished_tag() {
  Rig r;
  r.adapter.pollScript = {agent::JobState::Running};
  r.eng->enqueueSpawn(sp("first"), "chat1");
  r.eng->pump();                                   // job0000, still running
  r.rebuildJournal();
  r.build();
  r.eng->enqueueSpawn(sp("second"), "chat1");
  r.plat.ms += 3000;
  r.eng->pump();
  TEST_ASSERT_EQUAL(2, r.eng->activeJobCount());   // both live: no overwrite
  TEST_ASSERT_EQUAL_STRING("job0001", r.adapter.dispatched.back().tag.c_str());
  // Past a finished tag too.
  Rig f;
  f.adapter.pollScript = {agent::JobState::Done};
  for (int i = 0; i < 3; ++i) {
    f.eng->enqueueSpawn(sp("t"), "chat1");
    f.plat.ms += 3000; f.eng->pump();              // dispatch
    f.plat.ms += 3000; f.eng->pump();              // Done
  }
  f.rebuildJournal();
  f.build();
  f.eng->enqueueSpawn(sp("next"), "chat1");
  f.plat.ms += 3000; f.eng->pump();
  TEST_ASSERT_EQUAL_STRING("job0003", f.adapter.dispatched.back().tag.c_str());
}

// L19-35: two helpers finish with saved results; after a restart the head's context
// lists both (name, tag, project, file, time); this boot's jobs stay out of it.
static void test_finished_jobs_are_listed_after_a_restart() {
  Rig r;
  r.withClock = true;
  r.build();
  r.adapter.pollScript = {agent::JobState::Done};
  r.persistNote = "[saved: tides/tide-check-job0000.md]";
  orch::Spawn a = sp("why are there tides");
  a.name = "tide-check";
  a.project = "tides";
  r.eng->enqueueSpawn(a, "chat1");
  r.eng->pump();
  r.plat.ms += 3000; r.eng->pump();
  r.persistNote = "[saved: tides/moon-phase-job0001.md]";
  orch::Spawn b = sp("moon phase tonight");
  b.name = "moon-phase";
  b.project = "tides";
  r.eng->enqueueSpawn(b, "chat1");
  r.plat.ms += 3000; r.eng->pump();
  r.plat.ms += 3000; r.eng->pump();
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  TEST_ASSERT_EQUAL_STRING("", r.eng->finishedBeforeRestartText().c_str());   // same boot
  r.rebuildJournal();
  r.build();
  const std::string t = r.eng->finishedBeforeRestartText();
  TEST_ASSERT_TRUE(t.find("- moon-phase (job0001, project tides, saved as tides/moon-phase-job0001.md) "
                          "finished 2026-10-19 Sun 08:14 BST\n") == 0);   // newest first
  TEST_ASSERT_TRUE(t.find("- tide-check (job0000, project tides, saved as tides/tide-check-job0000.md) "
                          "finished 2026-10-19 Sun 08:14 BST\n") != std::string::npos);
  // A failed one is listed as ended without a result.
  Rig e;
  e.adapter.pollScript = {agent::JobState::Error};
  e.eng->enqueueSpawn(sp("x"), "chat1");
  e.eng->pump();
  e.plat.ms += 3000; e.eng->pump();
  e.rebuildJournal();
  e.build();
  TEST_ASSERT_TRUE(e.eng->finishedBeforeRestartText().find("(job0000) ended without a result") !=
                   std::string::npos);
}

// Honest loss (subagents F4.4): a synchronous sub's result lived in the device's RAM,
// so a job restored after a restart that its provider no longer knows is "lost when the
// device restarted"; an asynchronous one keeps "expired or was not found".
static void test_a_restored_sync_job_is_reported_lost_at_the_restart() {
  for (const char* backend : {"mistral", "openai"}) {
    Rig r;
    r.adapter.backend = backend;
    r.keyed.insert(backend);
    r.adapter.pollScript = {agent::JobState::Running};
    orch::Spawn s = sp("q");
    s.provider = backend;
    r.eng->enqueueSpawn(s, "chat1");
    r.eng->pump();
    TEST_ASSERT_EQUAL(1, r.eng->activeJobCount());
    r.delivered.clear();
    r.rebuildJournal();
    r.build();
    r.adapter.pollErr = agent::FabricErr::NotFound;
    r.plat.ms += 3000;
    r.eng->pump();
    TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
    TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
    const bool sync = std::string(backend) == "mistral";
    const std::string& m = r.delivered[0].second;
    TEST_ASSERT_EQUAL(sync, m.find("was lost when the device restarted") != std::string::npos);
    TEST_ASSERT_EQUAL(!sync, m.find("expired or was not found") != std::string::npos);
    // Delivered once: the next boot neither polls nor reports it again.
    r.delivered.clear();
    r.rebuildJournal();
    r.build();
    TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
    r.plat.ms += 3000;
    r.eng->pump();
    TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  }
  // A sync job that was never restored (same boot) and not found keeps the old words.
  Rig n;
  n.adapter.backend = "mistral";
  n.keyed.insert("mistral");
  n.adapter.pollErr = agent::FabricErr::NotFound;
  orch::Spawn s = sp("q");
  s.provider = "mistral";
  n.eng->enqueueSpawn(s, "chat1");
  n.eng->pump();
  n.delivered.clear();
  n.plat.ms += 3000; n.eng->pump();
  TEST_ASSERT_TRUE(n.delivered.at(0).second.find("expired or was not found") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lane_l_spawn_model_coercion_only_lands_on_a_listed_model);
  RUN_TEST(test_spawn_model_fallback_is_the_cheapest_listed_model);
  RUN_TEST(test_spawn_model_refusal_and_default_swap_are_disclosed);
  RUN_TEST(test_lane_l_the_spawn_ledger_tracks_each_spawn_per_chat);
  RUN_TEST(test_lane_l_every_spawn_ends_in_a_result_or_a_reasoned_refusal);
  RUN_TEST(test_spawn_brief_carries_chat_context);
  RUN_TEST(test_spawn_brief_no_context_when_empty);
  RUN_TEST(test_dispatch_one_per_cycle_with_2500ms_gate);
  RUN_TEST(test_short_sso_task_spawn_round_trips);
  RUN_TEST(test_stuck_job_gives_up_after_15min_of_poll_failures);
  RUN_TEST(test_long_running_job_arc_parks_dim_then_completes);
  RUN_TEST(test_spawn_ack_message_bytes);
  RUN_TEST(test_job_limit_refusal);
  RUN_TEST(test_spawn_refusals_reach_the_head_with_the_real_reason);
  RUN_TEST(test_timed_out_spawn_may_have_started_and_is_not_respawned);
  RUN_TEST(test_a_zai_sub_that_outruns_its_post_may_have_started);
  RUN_TEST(test_spend_ceiling_refuses_a_sub_dispatch_before_any_request);
  RUN_TEST(test_a_provider_at_its_limit_runs_no_helper);
  RUN_TEST(test_may_have_started_note_survives_the_chat_bound);
  RUN_TEST(test_an_unreadable_answer_may_have_run);
  RUN_TEST(test_spawn_note_budget_is_per_chat);
  RUN_TEST(test_keyless_provider_refusal);
  RUN_TEST(test_connector_routing_no_hint);
  RUN_TEST(test_connector_routing_explicit_hint_wins);
  RUN_TEST(test_connector_routing_falls_through);
  RUN_TEST(test_connector_routing_unkeyed_falls_through);
  RUN_TEST(test_file_task_prefers_the_pdf_route);
  RUN_TEST(test_file_task_defaults_and_explicit_provider);
  RUN_TEST(test_model_coercion_to_sub_default);
  RUN_TEST(test_l19_39_router_id_on_a_direct_provider_is_named_in_the_ledger);
  RUN_TEST(test_one_key_spawn_openai_runs_on_router);
  RUN_TEST(test_one_key_spawn_anthropic_runs_on_router);
  RUN_TEST(test_one_key_spawn_direct_key_stays_direct);
  RUN_TEST(test_one_key_spawn_nothing_offers_it_is_refused);
  RUN_TEST(test_one_key_spawn_connector_task_stays_off_the_router);
  RUN_TEST(test_one_key_spawn_no_provider_runs_on_router);
  RUN_TEST(test_one_key_spawn_every_provider_slot);
  RUN_TEST(test_backend_added_at_runtime_works_without_restart);
  RUN_TEST(test_fresh_overflow_becomes_stub_not_drop);
  RUN_TEST(test_fresh_block_budget_bounds_the_seed);
  RUN_TEST(test_spill_called_with_full_text_every_result);
  RUN_TEST(test_no_spill_closure_keeps_legacy_shape);
  RUN_TEST(test_poll_lifecycle_running_then_done_parks_fresh);
  RUN_TEST(test_unverified_connector_result_opens_with_marker);
  RUN_TEST(test_provenance_property_live_research_connector_by_tool_runs);
  RUN_TEST(test_connector_that_ran_or_was_not_asked_is_not_marked);
  RUN_TEST(test_done_arc_cleared_after_grace);
  RUN_TEST(test_error_delivers_failure_and_holds_attn_window);
  RUN_TEST(test_transient_backoff_doubles_then_resets);
  RUN_TEST(test_notfound_expired_message);
  RUN_TEST(test_spawn_429_names_the_daily_window);
  RUN_TEST(test_spawn_refusal_copy_every_window);
  RUN_TEST(test_synthesis_waits_for_all_jobs_then_fires_once);
  RUN_TEST(test_synthesis_coalesce_blocked_by_turn_in_flight);
  RUN_TEST(test_subs_90s_apart_synthesize_once_no_fallback);
  RUN_TEST(test_stuck_synthesis_falls_back_60s_after_drain);
  RUN_TEST(test_new_work_rearms_the_drain_clock);
  RUN_TEST(test_owner_fallback_body_has_no_scaffolding);
  RUN_TEST(test_every_job_delivery_is_captured_to_history);
  RUN_TEST(test_job_delivery_tag_matches_whole_item);
  RUN_TEST(test_loop_closure_synthesizes_when_all_done);
  RUN_TEST(test_reboot_reattach_polls_from_journal);
  RUN_TEST(test_await_tag_aims_next_poll);
  RUN_TEST(test_cancel_marks_cancelled_and_frees_ring);
  RUN_TEST(test_session_infos_shape);
  RUN_TEST(test_hooks_spawn_and_result_fire);
  RUN_TEST(test_spawn_usage_recorded_once_on_terminal_poll);
  RUN_TEST(test_spawn_usage_not_invented_when_provider_reports_none);
  RUN_TEST(test_l19_36_a_long_reply_is_spilled_whole_with_a_marker);
  RUN_TEST(test_l19_36_a_spill_that_fails_says_so);
  RUN_TEST(test_l19_36_a_reply_that_fits_is_not_spilled);
  RUN_TEST(test_l19_36_a_tail_note_that_cuts_keeps_the_whole_reply);
  RUN_TEST(test_a_helper_that_cannot_be_stored_says_it_will_not_survive_a_restart);
  RUN_TEST(test_a_refused_record_is_gone_after_a_restart_a_stored_one_reattaches);
  RUN_TEST(test_a_new_boot_never_reuses_a_restored_or_finished_tag);
  RUN_TEST(test_finished_jobs_are_listed_after_a_restart);
  RUN_TEST(test_a_restored_sync_job_is_reported_lost_at_the_restart);
  UNITY_END();
  return 0;
}
