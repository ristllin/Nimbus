#include <unity.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/orch/journal.h"

// Stage F suite - the sub-agent job machinery, host-tested for the first time:
// the one-dispatch-per-cycle gate (+2500/+1500 timers), the job-limit and
// keyless-provider refusals, model coercion, the round-robin poll lifecycle
// (Running -> Done fresh-parking, Error/NotFound delivery, transient backoff
// doubling + reset), the reap scheduler (Done grace vs Error attn-hold), the
// per-completion synthesis clock (coalesce + stuck fallback), reboot re-attach
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
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  void put(int slot, const std::string& v) override { slots[slot] = v; }
  void remove(int slot) override { slots.erase(slot); }
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
  std::vector<attn::Event> events;                             // FakeEventSink recorder
  std::vector<std::string> cues;                               // fire()
  std::vector<std::string> synthChats;
  bool turnInFlight = false;
  bool synthConsumes = true;   // synthesize consumes the fresh results (turn ran)
  std::set<std::string> keyed{"anthropic", "openai"};
  std::map<std::string, std::string> connectorProv;  // skill -> hosting provider
  std::vector<agent::SpawnEv> hookSpawns;                  // HookRecorder
  std::vector<agent::ResultEv> hookResults;
  struct SpawnUsage { std::string backend; uint32_t in, out; };
  std::vector<SpawnUsage> spawnUsage;                      // recordSpawnUsage recorder
  std::function<std::string(const std::string&)> chatContext;  // B4 (nullable)
  // Context Fabric: recent-results spill recorder (nullable - legacy shape when unset).
  std::function<std::string(const char*, const char*, const std::string&, const std::string&)> spill;

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
    d.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
    };
    d.event = [this](const attn::Event& e) { events.push_back(e); };
    d.fire = [this](const char* c) { cues.push_back(c); };
    d.backendHue = [](const char*) { return (uint8_t)42; };
    d.subPriority = [] { return std::string("anthropic,openai"); };
    d.providerHasKey = [this](const std::string& p) { return keyed.count(p) > 0; };
    d.subModel = [](const std::string& p) { return "sub-" + p; };
    d.modelIsValid = [](const std::string&, const std::string& m) {
      return m.rfind("good-", 0) == 0;   // models named good-* validate
    };
    if (chatContext) d.chatContext = chatContext;   // B4: sub-brief [CONTEXT]
    if (spill) d.spillResult = spill;               // Context Fabric: results ring
    d.connectorProvider = [this](const std::string& skill) -> std::string {
      auto it = connectorProv.find(skill);
      return it == connectorProv.end() ? std::string() : it->second;
    };
    d.synthesize = [this](const std::string& chat) {
      synthChats.push_back(chat);
      if (synthConsumes) (void)eng->takeFreshResults();
    };
    d.turnInFlight = [this] { return turnInFlight; };
    d.hooks.onSpawn = [this](const agent::SpawnEv& ev) { hookSpawns.push_back(ev); };
    d.hooks.onResult = [this](const agent::ResultEv& ev) { hookResults.push_back(ev); };
    d.recordSpawnUsage = [this](const std::string& b, uint32_t in, uint32_t out) {
      spawnUsage.push_back({b, in, out});
    };
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
  TEST_ASSERT_EQUAL_STRING("web_search", r.adapter.dispatched[0].instruction.c_str());
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
      "- job0000 (good-m (research)): fake result\n",
      block.c_str());
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());     // consumed + clock reset
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

static void test_synthesis_fallback_delivers_raw_after_60s() {
  // The raw fallback obeys the same all-done gate (a raw dump of mid-run
  // results is the same spam the gate exists to stop).
  Rig r;
  r.turnInFlight = true;    // keeps the coalesce branch from consuming first
  r.synthConsumes = false;
  r.adapter.pollScript = {agent::JobState::Done, agent::JobState::Done};
  r.eng->enqueueSpawn(sp("a"), "chat1");
  r.eng->enqueueSpawn(sp("b"), "chat1");
  r.eng->pump();
  r.plat.ms = 3500; r.eng->pump();
  r.plat.ms = 5000; r.eng->pump();     // A Done
  for (int i = 0; i < 30 && r.eng->activeJobCount() > 0; i++) {
    r.plat.ms += 2000; r.eng->pump();  // drain B
  }
  TEST_ASSERT_EQUAL(0, r.eng->activeJobCount());
  // The fallback clock started at the FIRST unconsumed result (A, t=5000) and
  // is deliberately NOT reset by later completions - deadline 65000.
  r.plat.ms = 64999; r.eng->pump();
  TEST_ASSERT_FALSE(LogCapture::contains("synthesis stuck"));
  r.plat.ms = 65000; r.eng->pump();
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: synthesis stuck - delivering raw results (fallback)"));
  const std::string& raw = r.delivered.back().second;
  TEST_ASSERT_EQUAL_STRING("chat1", r.delivered.back().first.c_str());
  TEST_ASSERT_TRUE(raw.rfind("[FRESH RESULTS] (sub-agents that just finished)\n", 0) == 0);
  TEST_ASSERT_TRUE(raw.find("- job0000 (good-m (research)): fake result\n") != std::string::npos);
  TEST_ASSERT_FALSE(r.eng->hasFreshResults());   // drained, never lost
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_spawn_brief_carries_chat_context);
  RUN_TEST(test_spawn_brief_no_context_when_empty);
  RUN_TEST(test_dispatch_one_per_cycle_with_2500ms_gate);
  RUN_TEST(test_short_sso_task_spawn_round_trips);
  RUN_TEST(test_stuck_job_gives_up_after_15min_of_poll_failures);
  RUN_TEST(test_long_running_job_arc_parks_dim_then_completes);
  RUN_TEST(test_spawn_ack_message_bytes);
  RUN_TEST(test_job_limit_refusal);
  RUN_TEST(test_keyless_provider_refusal);
  RUN_TEST(test_connector_routing_no_hint);
  RUN_TEST(test_connector_routing_explicit_hint_wins);
  RUN_TEST(test_connector_routing_falls_through);
  RUN_TEST(test_connector_routing_unkeyed_falls_through);
  RUN_TEST(test_model_coercion_to_sub_default);
  RUN_TEST(test_fresh_overflow_becomes_stub_not_drop);
  RUN_TEST(test_fresh_block_budget_bounds_the_seed);
  RUN_TEST(test_spill_called_with_full_text_every_result);
  RUN_TEST(test_no_spill_closure_keeps_legacy_shape);
  RUN_TEST(test_poll_lifecycle_running_then_done_parks_fresh);
  RUN_TEST(test_done_arc_cleared_after_grace);
  RUN_TEST(test_error_delivers_failure_and_holds_attn_window);
  RUN_TEST(test_transient_backoff_doubles_then_resets);
  RUN_TEST(test_notfound_expired_message);
  RUN_TEST(test_synthesis_waits_for_all_jobs_then_fires_once);
  RUN_TEST(test_synthesis_coalesce_blocked_by_turn_in_flight);
  RUN_TEST(test_synthesis_fallback_delivers_raw_after_60s);
  RUN_TEST(test_loop_closure_synthesizes_when_all_done);
  RUN_TEST(test_reboot_reattach_polls_from_journal);
  RUN_TEST(test_await_tag_aims_next_poll);
  RUN_TEST(test_cancel_marks_cancelled_and_frees_ring);
  RUN_TEST(test_session_infos_shape);
  RUN_TEST(test_hooks_spawn_and_result_fire);
  RUN_TEST(test_spawn_usage_recorded_once_on_terminal_poll);
  RUN_TEST(test_spawn_usage_not_invented_when_provider_reports_none);
  UNITY_END();
  return 0;
}
