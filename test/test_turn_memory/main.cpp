#include <unity.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/turn_memory.h"
#include "nimbus/heap_watch.h"   // kSmallAllocInternalLimit

// L24 (no panic under combined memory starvation): a turn never spends the device's
// last internal SRAM. On unit A (26 Oct) a turn admitted at ~17 KB internal free
// spent it all on its own setup (its small heap blocks preferred internal SRAM) and
// the Wi-Fi task's own allocation failed: an IDF abort, a panic. The rules pinned
// here: the turn keeps its small blocks in PSRAM for its whole length, so its setup
// costs no internal SRAM and the floor it was admitted on still holds at its provider
// request, where it is checked again; under it the turn ends with one honest reply
// recorded in the chat history and no provider request; a failed allocation at any
// hook ends the turn the same way instead of aborting; Lumi at rest is still served.

using agent::kShortOfMemoryReply;
using agent::PsramFirstScope;
using agent::TurnEngine;
using agent::TurnMemoryGate;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;

static const char* kGoodTurn = "{\"reply\":\"hello there\",\"memory\":\"\",\"ask\":\"\"}";
// The turn floor (ORCH_TURN_HARD_FLOOR / ORCH_TURN_MIN_LARGEST_BLOCK, the engine's defaults).
static const uint32_t kFloorFree = TurnEngine::Tuning{}.turnHardFloor;
static const uint32_t kFloorLargest = TurnEngine::Tuning{}.turnMinLargestBlock;

// ---- the PSRAM-first switch recorder ---------------------------------------------
static std::vector<bool>& switchCalls() {
  static std::vector<bool> v;
  return v;
}
static bool switchOn() { return !switchCalls().empty() && switchCalls().back(); }

// ---- a small engine rig ------------------------------------------------------------
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  std::vector<std::pair<std::string, std::string>> delivered;   // (chat, text)
  std::vector<std::pair<std::string, std::string>> rows;        // assistant history rows
  std::vector<std::string> ownerRows;                           // owner rows captured
  std::vector<bool> ownerRowSwitch;                             // PSRAM-first while the row was captured
  struct Attempt {
    std::string host;
    bool psramFirst = false;   // the switch state while the provider was asked
  };
  std::vector<Attempt> attempts;
  std::vector<agent::TurnStartEv> starts;
  std::vector<agent::TurnEndEv> ends;
  std::vector<std::string> endCauses;                           // first cause of each failed end
  std::vector<bool> contextSwitch;                              // PSRAM-first while the turn's context was built
  std::function<void(Rig&)> onDelay;                            // runs on every platform delay (a dip recovering)
  // What the turn's setup does to internal SRAM (the device's small blocks used to):
  // composeInputs runs this, so a test can drain or fragment the heap mid-setup.
  std::function<void(Rig&)> duringSetup;
  bool throwInSetup = false;      // composeInputs throws std::bad_alloc
  bool throwInOwnerRow = false;   // the owner-row capture throws (before the turn)
  bool throwInReplyRow = false;   // the reply's history row throws (after the provider billed)
  std::string throwAt;            // the class table: the one hook that throws std::bad_alloc next
  void maybeThrow(const char* at) {
    if (throwAt != at) return;
    throwAt.clear();
    throw std::bad_alloc();
  }
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.platform.delayMs = [this](uint32_t ms) {
      plat.delays.push_back(ms);
      plat.ms += ms;
      if (onDelay) onDelay(*this);
    };
    d.skillsPicks = [this](const std::string&) {   // read while the turn's context is built
      maybeThrow("context");
      contextSwitch.push_back(switchOn());
      return std::vector<agent::AttachPick>{};
    };
    d.deliver = [this](const std::string& c, const std::string& t) { delivered.push_back({c, t}); };
    d.apply.deliver = d.deliver;
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      maybeThrow("reply");
      if (throwInReplyRow) {
        throwInReplyRow = false;
        throw std::bad_alloc();
      }
      rows.push_back({c, t});
    };
    d.episodicCaptureUser = [this](const std::string&, const std::string& t, const std::string&) {
      maybeThrow("row");
      if (throwInOwnerRow) throw std::bad_alloc();
      ownerRows.push_back(t);
      ownerRowSwitch.push_back(switchOn());
    };
    d.composeInputs = [this](const std::string&) {
      maybeThrow("compose");
      if (throwInSetup) throw std::bad_alloc();
      if (duringSetup) duringSetup(*this);
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.hostLabel = "anthropic / model-anthropic";
      return in;
    };
    d.hooks.onTurnStart = [this](const agent::TurnStartEv& ev) { starts.push_back(ev); };
    d.recall = [this](const std::string&, const orch::Principal&) {
      maybeThrow("recall");
      return orch::RecallOut{};
    };
    d.toolSpecs = [this](const orch::Principal&) {
      maybeThrow("specs");
      return std::vector<orch::ToolRegistry::Spec>{};
    };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      ends.push_back(ev);
      endCauses.push_back(ev.chain && !ev.chain->empty() ? ev.chain->first()->text : std::string());
    };
    for (const char* h : {"openai", "anthropic", "mistral"})
      d.hosts.add(h, [this, h](std::string& conv, const std::string&, const std::string&, std::string& out,
                               std::string&, const agent::HeadTools*, orch::TokenUsage* usage) {
        attempts.push_back({h, switchOn()});
        maybeThrow("provider");
        conv = "c1";
        out = kGoodTurn;
        if (usage) usage->add(100, 20);
        return true;
      });
    eng.reset(new TurnEngine(std::move(d)));
  }
  bool anyDelivered(const char* needle) const {
    for (const auto& p : delivered)
      if (p.second.find(needle) != std::string::npos) return true;
    return false;
  }
};

static std::string readSource(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

void setUp() {
  LogCapture::install();
  switchCalls().clear();
  agent::setPsramFirstSwitch([](bool on) { switchCalls().push_back(on); });
}
void tearDown() {
  agent::setPsramFirstSwitch(nullptr);
  agent::hlog::setSink(nullptr);
}

// ---- the gate --------------------------------------------------------------------

// The unit A run phase by phase: setup samples are evidence (the first one under the
// floor is named), and only the sample at the provider request decides.
static void test_gate_records_dips_and_decides_at_dispatch() {
  TurnMemoryGate g;
  g.begin(16936, 7156, kFloorFree, kFloorLargest);
  g.phase("recall", 15296, 7156);
  g.phase("specs", 12500, 7156);
  TEST_ASSERT_EQUAL_STRING("", g.firstUnder());
  g.phase("tools", 744, 20);
  TEST_ASSERT_TRUE(g.ok());                       // a setup dip alone never refuses
  TEST_ASSERT_EQUAL_STRING("tools", g.firstUnder());
  TEST_ASSERT_FALSE(g.dispatch("dispatch", 772, 20));
  TEST_ASSERT_EQUAL_STRING("dispatch", g.refusedAt());
  TEST_ASSERT_TRUE(g.dispatch("recheck", 30000, 14000));   // the last dispatch sample decides
  TEST_ASSERT_TRUE(g.ok());
  TEST_ASSERT_EQUAL_STRING("tools", g.firstUnder());
  TEST_ASSERT_EQUAL_UINT32(744, g.lowFree());
  TEST_ASSERT_EQUAL_UINT32(16936 - 744, g.setupCost());
}

// The class: one sample, every free x largest pair. The gate holds exactly when both
// turn floors hold (fragmentation alone refuses, like the admission gate).
static void test_gate_property_over_every_sample() {
  for (uint32_t freeB = 0; freeB <= 24000; freeB += 250) {
    for (uint32_t largest = 0; largest <= 10000; largest += 250) {
      TurnMemoryGate g;
      g.begin(100000, 100000, kFloorFree, kFloorLargest);
      const bool want = freeB >= kFloorFree && largest >= kFloorLargest;
      TEST_ASSERT_EQUAL_MESSAGE(want, g.dispatch("dispatch", freeB, largest), "gate verdict differs from the floors");
      TEST_ASSERT_EQUAL_MESSAGE(want, g.ok(), "ok() differs from the dispatch verdict");
    }
  }
  // Counter-test at the boundary: one byte under either floor refuses.
  TurnMemoryGate a, b, c;
  a.begin(100000, 100000, kFloorFree, kFloorLargest);
  b.begin(100000, 100000, kFloorFree, kFloorLargest);
  c.begin(100000, 100000, kFloorFree, kFloorLargest);
  TEST_ASSERT_TRUE(a.dispatch("dispatch", kFloorFree, kFloorLargest));
  TEST_ASSERT_FALSE(b.dispatch("dispatch", kFloorFree - 1, 100000));
  TEST_ASSERT_FALSE(c.dispatch("dispatch", 100000, kFloorLargest - 1));
}

// The evidence lines: the compact form names the low point, the full form every phase.
static void test_gate_lines_name_the_low_point_and_every_phase() {
  TurnMemoryGate g;
  g.begin(16936, 7156, kFloorFree, kFloorLargest);
  g.phase("tools", 744, 20);
  g.dispatch("dispatch", 772, 20);
  const std::string compact = g.line();
  TEST_ASSERT_TRUE(compact.find("turn memory: admit=16936/7156 low=744/20 at tools") != std::string::npos);
  TEST_ASSERT_TRUE(compact.find("cost=16192 floor=12000/5000 UNDER at dispatch") != std::string::npos);
  const std::string full = g.line(true);
  TEST_ASSERT_TRUE(full.find("admit=16936/7156 tools=744/20 dispatch=772/20") != std::string::npos);
  TurnMemoryGate ok;
  ok.begin(30000, 14000, kFloorFree, kFloorLargest);
  TEST_ASSERT_TRUE(ok.line().find(" ok") != std::string::npos);
  TurnMemoryGate dipped;
  dipped.begin(16000, 7156, kFloorFree, kFloorLargest);
  dipped.phase("recall", 6000, 7156);
  dipped.dispatch("dispatch", 15000, 7156);
  TEST_ASSERT_TRUE(dipped.line().find("ok (dipped at recall)") != std::string::npos);
  // More phases than marks: the extra ones are counted, never written past the end.
  TurnMemoryGate many;
  many.begin(30000, 14000, kFloorFree, kFloorLargest);
  for (int i = 0; i < 40; ++i) many.phase("p", 30000, 14000);
  TEST_ASSERT_TRUE(many.line(true).find("(+25 more)") != std::string::npos);
}

// The check at the provider request uses the floor the turn was admitted on, for any
// floor: a rig with a raised turn floor refuses a setup that leaves one byte less.
static void test_dispatch_checks_the_floor_the_turn_was_admitted_on() {
  for (uint32_t floor : {12000u, 15000u, 20000u}) {
    TurnEngine::Tuning t;
    t.turnHardFloor = floor;
    struct Probe {
      uint32_t heap = 0;
    } p;
    FakeConfig cfg;
    int attempts = 0;
    std::vector<std::string> said;
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform.freeHeap = [&p] { return p.heap; };
    d.platform.largestFreeBlock = [] { return 100000u; };
    d.platform.delayMs = [](uint32_t) {};
    d.deliver = [&said](const std::string&, const std::string& txt) { said.push_back(txt); };
    d.apply.deliver = d.deliver;
    d.composeInputs = [&p, floor](const std::string&) {
      p.heap = floor - 1;   // the setup leaves one byte under the floor
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      return in;
    };
    d.hosts.add("anthropic", [&attempts](std::string&, const std::string&, const std::string&, std::string& out,
                                         std::string&, const agent::HeadTools*, orch::TokenUsage*) {
      ++attempts;
      out = kGoodTurn;
      return true;
    });
    TurnEngine eng(std::move(d), t);
    p.heap = floor + 5000;
    eng.handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(0, attempts, "a setup under the admission floor reached the provider");
    TEST_ASSERT_EQUAL(1, (int)said.size());
    TEST_ASSERT_EQUAL_STRING(kShortOfMemoryReply, said[0].c_str());
  }
}

// ---- PSRAM-first ---------------------------------------------------------------------

// Scopes nest; only the outermost switches, and the switch always comes back off.
static void test_psram_first_scope_nests_and_restores() {
  TEST_ASSERT_EQUAL(0, PsramFirstScope::depth());
  {
    PsramFirstScope a;
    TEST_ASSERT_EQUAL(1, PsramFirstScope::depth());
    {
      PsramFirstScope b;
      TEST_ASSERT_EQUAL(2, PsramFirstScope::depth());
    }
    TEST_ASSERT_TRUE(switchOn());
  }
  TEST_ASSERT_EQUAL(0, PsramFirstScope::depth());
  TEST_ASSERT_EQUAL(2, (int)switchCalls().size());   // one on, one off
  TEST_ASSERT_TRUE(switchCalls()[0]);
  TEST_ASSERT_FALSE(switchCalls()[1]);
}

// A whole owner turn runs with its small blocks in PSRAM: from the owner row's
// capture through the provider request, and the switch is off again after.
static void test_owner_turn_runs_psram_first_end_to_end() {
  Rig r;
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.attempts[0].psramFirst);
  TEST_ASSERT_EQUAL(1, (int)r.ownerRowSwitch.size());
  TEST_ASSERT_TRUE(r.ownerRowSwitch[0]);
  TEST_ASSERT_FALSE(r.contextSwitch.empty());
  for (bool on : r.contextSwitch) TEST_ASSERT_TRUE(on);
  TEST_ASSERT_FALSE(switchOn());
  TEST_ASSERT_EQUAL(0, PsramFirstScope::depth());
  TEST_ASSERT_EQUAL(2, (int)switchCalls().size());   // nested scopes switch once
}

// A routine runs PSRAM-first too (it reaches runTurn without handleMessage).
static void test_scheduled_turn_runs_psram_first() {
  Rig r;
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1");
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.attempts[0].psramFirst);
  TEST_ASSERT_FALSE(r.contextSwitch.empty());     // its context is built inside the scope too
  for (bool on : r.contextSwitch) TEST_ASSERT_TRUE(on);
  TEST_ASSERT_FALSE(switchOn());
}

// ---- the honest end ------------------------------------------------------------------

static void assertShortOfMemoryEnd(const Rig& r, const char* why) {
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.attempts.size(), why);   // no provider request
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), why);  // exactly one reply
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kShortOfMemoryReply, r.delivered[0].second.c_str(), why);
  bool row = false;
  for (const auto& p : r.rows) row = row || p.second == kShortOfMemoryReply;
  TEST_ASSERT_TRUE_MESSAGE(row, why);   // the history row the owner reads on the web page
  TEST_ASSERT_FALSE_MESSAGE(r.eng->turnInFlight(), why);
  TEST_ASSERT_EQUAL_MESSAGE(0, PsramFirstScope::depth(), why);
  TEST_ASSERT_FALSE_MESSAGE(switchOn(), why);
}

// The L24 shape: admitted at ~17 KB, the setup drains internal under the dispatch
// floor. One honest reply, recorded as the chat's row, no provider request, the
// turn's end hook paired with its start, and the cause names the floor.
static void test_setup_under_the_turn_floor_ends_honestly() {
  Rig r;
  r.plat.heap = 16936;
  r.plat.largest = 7156;
  r.duringSetup = [](Rig& rr) {
    rr.plat.heap = 744;
    rr.plat.largest = 20;
  };
  r.eng->handleMessage("CALL the device.log tool now", "Roy", "serial");
  assertShortOfMemoryEnd(r, "drained setup");
  TEST_ASSERT_EQUAL(1, (int)r.ownerRows.size());   // the owner's message is in history
  TEST_ASSERT_EQUAL(1, (int)r.starts.size());
  TEST_ASSERT_EQUAL(1, (int)r.ends.size());
  TEST_ASSERT_FALSE(r.ends[0].ok);
  TEST_ASSERT_TRUE(r.endCauses[0].find("under the turn floor") != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("turn ended for memory before the provider request"));
  TEST_ASSERT_TRUE(LogCapture::contains("turn memory: admit=16936/7156"));
}

// A transient dip (the recall embed's network buffers, freed a few ms after its
// connection closes) recovers before the provider request: the turn is served, not
// refused, and the memory line says where it dipped.
static void test_a_dip_that_recovers_before_dispatch_is_served() {
  Rig r;
  r.plat.heap = 16000;
  r.plat.largest = 7156;
  r.duringSetup = [](Rig& rr) { rr.plat.heap = 3000; };
  r.onDelay = [](Rig& rr) { rr.plat.heap = 15800; };
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.anyDelivered("hello there"));
  TEST_ASSERT_FALSE(r.anyDelivered(kShortOfMemoryReply));
  TEST_ASSERT_EQUAL(1, (int)r.plat.delays.size());   // one short wait was enough
  TEST_ASSERT_TRUE(LogCapture::contains("ok (dipped at context)"));
}

// A dip that does not recover is re-checked a bounded number of times, then refused.
static void test_a_dip_that_stays_is_rechecked_then_refused() {
  Rig r;
  r.duringSetup = [](Rig& rr) { rr.plat.heap = 3000; };
  r.eng->handleMessage("hi there", "Roy", "1001");
  assertShortOfMemoryEnd(r, "persistent dip");
  TEST_ASSERT_EQUAL(agent::kDispatchRechecks, (int)r.plat.delays.size());
  for (uint32_t d : r.plat.delays) TEST_ASSERT_EQUAL_UINT32(agent::kDispatchRecheckMs, d);
}

// Fragmentation alone (total free ample, the largest block cut) ends the same way.
static void test_fragmented_setup_ends_honestly() {
  Rig r;
  r.duringSetup = [](Rig& rr) { rr.plat.largest = 4000; };
  r.eng->handleMessage("hi", "Roy", "1001");
  assertShortOfMemoryEnd(r, "fragmented setup");
}

// A failed allocation in the turn's setup ends the turn honestly: never an abort,
// the guards unwound, the end hook paired, and the next turn runs normally.
static void test_failed_allocation_in_the_turn_ends_honestly() {
  Rig r;
  r.throwInSetup = true;
  r.eng->handleMessage("hi", "Roy", "1001");
  assertShortOfMemoryEnd(r, "bad_alloc in setup");
  TEST_ASSERT_EQUAL(1, (int)r.starts.size());
  TEST_ASSERT_EQUAL(1, (int)r.ends.size());
  TEST_ASSERT_FALSE(r.ends[0].ok);
  TEST_ASSERT_TRUE(r.endCauses[0].find("allocation failed") != std::string::npos);
  r.throwInSetup = false;
  r.delivered.clear();
  r.eng->handleMessage("hi again", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.anyDelivered("hello there"));
}

// A failed allocation after the provider billed and the reply went out: the spend is
// metered exactly once, the end hook fires once, and no apology rides on top of the
// answer the owner already has.
static void test_failed_allocation_after_the_answer_meters_once_and_adds_nothing() {
  Rig r;
  r.throwInReplyRow = true;
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.anyDelivered("hello there"));
  TEST_ASSERT_FALSE(r.anyDelivered(kShortOfMemoryReply));
  TEST_ASSERT_EQUAL(1, (int)r.ends.size());
  TEST_ASSERT_FALSE(r.ends[0].ok);
  int billed = 0;
  for (const auto& rec : r.cfg.recorded) billed += (int)rec.in;
  TEST_ASSERT_EQUAL(100, billed);   // the attempt's 100 prompt tokens, once
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
  TEST_ASSERT_EQUAL(0, PsramFirstScope::depth());
  TEST_ASSERT_FALSE(switchOn());
}

// The class: a failed allocation at EVERY hook a turn calls ends the turn honestly.
// Never an escape (the device would abort), the guards unwound, the PSRAM switch off,
// the end hook paired with the start hook, the owner told exactly once (the answer if
// it was already out, else the short-of-memory reply), and the next turn served.
static void test_a_failed_allocation_at_every_hook_ends_the_turn_honestly() {
  for (const char* at : {"row", "context", "recall", "specs", "compose", "provider", "reply"}) {
    Rig r;
    r.throwAt = at;
    r.eng->handleMessage("hi", "Roy", "1001");   // an escaping exception fails the test run
    TEST_ASSERT_TRUE_MESSAGE(r.throwAt.empty(), at);   // the hook was reached and threw
    TEST_ASSERT_FALSE_MESSAGE(r.eng->turnInFlight(), at);
    TEST_ASSERT_EQUAL_MESSAGE(0, PsramFirstScope::depth(), at);
    TEST_ASSERT_FALSE_MESSAGE(switchOn(), at);
    TEST_ASSERT_EQUAL_MESSAGE(r.starts.size(), r.ends.size(), at);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), at);
    const std::string& said = r.delivered[0].second;
    TEST_ASSERT_TRUE_MESSAGE(said == kShortOfMemoryReply || said == "hello there", at);
    if (std::strcmp(at, "reply") == 0) TEST_ASSERT_EQUAL_STRING_MESSAGE("hello there", said.c_str(), at);
    else TEST_ASSERT_EQUAL_STRING_MESSAGE(kShortOfMemoryReply, said.c_str(), at);
    r.delivered.clear();
    r.eng->handleMessage("hi again", "Roy", "1001");
    TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("hello there"), at);
  }
}

// A failed allocation before the turn began (the owner row's capture) ends the same
// way, with no turn hooks (the turn never started).
static void test_failed_allocation_before_the_turn_ends_honestly() {
  Rig r;
  r.throwInOwnerRow = true;
  r.eng->handleMessage("hi", "Roy", "1001");
  assertShortOfMemoryEnd(r, "bad_alloc before the turn");
  TEST_ASSERT_EQUAL(0, (int)r.starts.size());
  TEST_ASSERT_EQUAL(0, (int)r.ends.size());
}

// A routine whose setup goes under the floor is a defer (CUM-403: retried, never a
// failure), tells nobody, and asks no provider.
static void test_scheduled_fire_under_the_floor_is_a_defer() {
  Rig r;
  r.duringSetup = [](Rig& rr) { rr.plat.heap = 700; };
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1");
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_TRUE(o.deferred);
  TEST_ASSERT_EQUAL_STRING("deferred: low memory", o.detail.c_str());
  TEST_ASSERT_TRUE(o.error.empty());
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  TEST_ASSERT_EQUAL(1, (int)r.ends.size());
}

// ---- the floors still admit what runs today -----------------------------------------

// Lumi 10 minutes after the v4.6.0 update: 32,308 B free, 7,156 B largest. Its turns
// run today; the floors must not refuse them (a setup that keeps its blocks in PSRAM
// costs it nothing internal here).
static void test_lumi_at_rest_is_admitted_and_dispatched() {
  Rig r;
  r.plat.heap = 32308;
  r.plat.largest = 7156;
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.anyDelivered("hello there"));
  TEST_ASSERT_FALSE(r.anyDelivered(kShortOfMemoryReply));
}

// At the CUM-404 admission floor itself (12000 / 5000) a setup that costs no internal
// SRAM (PSRAM-first) dispatches; one that leaves one byte under it is refused.
static void test_admission_floor_turn_dispatches_when_the_setup_costs_nothing() {
  Rig within;
  within.plat.heap = kFloorFree;
  within.plat.largest = kFloorLargest;
  within.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)within.attempts.size());
  Rig over;
  over.plat.heap = kFloorFree;
  over.plat.largest = kFloorLargest;
  over.duringSetup = [](Rig& rr) { rr.plat.heap = kFloorFree - 1; };
  over.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(0, (int)over.attempts.size());
  TEST_ASSERT_TRUE(over.anyDelivered(kShortOfMemoryReply));
}

// ---- the device seams (source guards) ------------------------------------------------

// The switch restores exactly the boot's spill limit, and the boot still sets it.
static void test_psram_first_restores_the_boot_spill_limit() {
  const std::string hw = readSource("src/sys/heap_watch.cpp");
  TEST_ASSERT_TRUE(hw.find("heap_caps_malloc_extmem_enable(on ? 0 : kSmallAllocInternalLimit)") !=
                   std::string::npos);
  const std::string main = readSource("src/main.cpp");
  char want[64];
  std::snprintf(want, sizeof want, "heap_caps_malloc_extmem_enable(%u)",
                (unsigned)nimbus::heapwatch::kSmallAllocInternalLimit);
  TEST_ASSERT_TRUE_MESSAGE(main.find(want) != std::string::npos,
                           "main.cpp's boot spill limit and kSmallAllocInternalLimit differ");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gate_records_dips_and_decides_at_dispatch);
  RUN_TEST(test_gate_property_over_every_sample);
  RUN_TEST(test_gate_lines_name_the_low_point_and_every_phase);
  RUN_TEST(test_dispatch_checks_the_floor_the_turn_was_admitted_on);
  RUN_TEST(test_psram_first_scope_nests_and_restores);
  RUN_TEST(test_owner_turn_runs_psram_first_end_to_end);
  RUN_TEST(test_scheduled_turn_runs_psram_first);
  RUN_TEST(test_setup_under_the_turn_floor_ends_honestly);
  RUN_TEST(test_a_dip_that_recovers_before_dispatch_is_served);
  RUN_TEST(test_a_dip_that_stays_is_rechecked_then_refused);
  RUN_TEST(test_fragmented_setup_ends_honestly);
  RUN_TEST(test_failed_allocation_in_the_turn_ends_honestly);
  RUN_TEST(test_failed_allocation_after_the_answer_meters_once_and_adds_nothing);
  RUN_TEST(test_failed_allocation_before_the_turn_ends_honestly);
  RUN_TEST(test_a_failed_allocation_at_every_hook_ends_the_turn_honestly);
  RUN_TEST(test_scheduled_fire_under_the_floor_is_a_defer);
  RUN_TEST(test_lumi_at_rest_is_admitted_and_dispatched);
  RUN_TEST(test_admission_floor_turn_dispatches_when_the_setup_costs_nothing);
  RUN_TEST(test_psram_first_restores_the_boot_spill_limit);
  return UNITY_END();
}
