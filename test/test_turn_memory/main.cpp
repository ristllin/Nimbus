#include <unity.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <regex>
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
// here: the turn keeps its small blocks in PSRAM for its whole length; the setup
// re-checks the dispatch floor and, under it, ends with one honest reply recorded in
// the chat history and no provider request; a failed allocation ends the turn the
// same way instead of aborting; the admission floor covers the dispatch floor plus
// the measured setup cost, and still admits Lumi at rest.

using agent::kShortOfMemoryReply;
using agent::PsramFirstScope;
using agent::TurnEngine;
using agent::TurnMemoryGate;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;

static const char* kGoodTurn = "{\"reply\":\"hello there\",\"memory\":\"\",\"ask\":\"\"}";

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
  // What the turn's setup does to internal SRAM (the device's small blocks used to):
  // composeInputs runs this, so a test can drain or fragment the heap mid-setup.
  std::function<void(Rig&)> duringSetup;
  bool throwInSetup = false;      // composeInputs throws std::bad_alloc
  bool throwInOwnerRow = false;   // the owner-row capture throws (before the turn)
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string& c, const std::string& t) { delivered.push_back({c, t}); };
    d.apply.deliver = d.deliver;
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) { rows.push_back({c, t}); };
    d.episodicCaptureUser = [this](const std::string&, const std::string& t, const std::string&) {
      if (throwInOwnerRow) throw std::bad_alloc();
      ownerRows.push_back(t);
      ownerRowSwitch.push_back(switchOn());
    };
    d.composeInputs = [this](const std::string&) {
      if (throwInSetup) throw std::bad_alloc();
      if (duringSetup) duringSetup(*this);
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.hostLabel = "anthropic / model-anthropic";
      return in;
    };
    d.hooks.onTurnStart = [this](const agent::TurnStartEv& ev) { starts.push_back(ev); };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      ends.push_back(ev);
      endCauses.push_back(ev.chain && !ev.chain->empty() ? ev.chain->first()->text : std::string());
    };
    for (const char* h : {"openai", "anthropic", "mistral"})
      d.hosts.add(h, [this, h](std::string& conv, const std::string&, const std::string&, std::string& out,
                               std::string&, const agent::HeadTools*, orch::TokenUsage* usage) {
        attempts.push_back({h, switchOn()});
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

// The unit A run phase by phase: the first sample under the floor names the phase,
// and the gate stays refused for the rest of the turn.
static void test_gate_names_the_first_phase_under_the_floor() {
  TurnMemoryGate g;
  g.begin(16936, 7156);
  g.phase("recall", 15296, 7156);
  g.phase("specs", 11500, 7156);
  TEST_ASSERT_TRUE(g.ok());
  g.phase("tools", 744, 20);
  TEST_ASSERT_FALSE(g.ok());
  TEST_ASSERT_EQUAL_STRING("tools", g.refusedAt());
  g.phase("dispatch", 30000, 14000);   // a later recovery does not clear it
  TEST_ASSERT_FALSE(g.ok());
  TEST_ASSERT_EQUAL_STRING("tools", g.refusedAt());
  TEST_ASSERT_EQUAL_UINT32(744, g.lowFree());
  TEST_ASSERT_EQUAL_UINT32(16936 - 744, g.setupCost());
}

// The class: one sample, every free x largest pair. The gate holds exactly when both
// the dispatch floors hold (fragmentation alone refuses, like the admission gate).
static void test_gate_property_over_every_sample() {
  for (uint32_t freeB = 0; freeB <= 20000; freeB += 250) {
    for (uint32_t largest = 0; largest <= 10000; largest += 250) {
      TurnMemoryGate g;
      g.begin(freeB, largest);
      const bool want = freeB >= agent::kDispatchFloorFree && largest >= agent::kDispatchFloorLargest;
      TEST_ASSERT_EQUAL_MESSAGE(want, g.ok(), "gate verdict differs from the dispatch floors");
    }
  }
  // Counter-test at the boundary: one byte under either floor refuses.
  TurnMemoryGate a, b, c;
  a.begin(agent::kDispatchFloorFree, agent::kDispatchFloorLargest);
  b.begin(agent::kDispatchFloorFree - 1, 100000);
  c.begin(100000, agent::kDispatchFloorLargest - 1);
  TEST_ASSERT_TRUE(a.ok());
  TEST_ASSERT_FALSE(b.ok());
  TEST_ASSERT_FALSE(c.ok());
}

// The evidence lines: the compact form names the low point, the full form every phase.
static void test_gate_lines_name_the_low_point_and_every_phase() {
  TurnMemoryGate g;
  g.begin(16936, 7156);
  g.phase("tools", 744, 20);
  g.phase("dispatch", 772, 20);
  const std::string compact = g.line();
  TEST_ASSERT_TRUE(compact.find("turn memory: admit=16936/7156 low=744/20 at tools") != std::string::npos);
  TEST_ASSERT_TRUE(compact.find("cost=16192 floor=8000/5000 UNDER at tools") != std::string::npos);
  const std::string full = g.line(true);
  TEST_ASSERT_TRUE(full.find("admit=16936/7156 tools=744/20 dispatch=772/20") != std::string::npos);
  TurnMemoryGate ok;
  ok.begin(30000, 14000);
  TEST_ASSERT_TRUE(ok.line().find(" ok") != std::string::npos);
  // More phases than marks: the extra ones are counted, never written past the end.
  TurnMemoryGate many;
  many.begin(30000, 14000);
  for (int i = 0; i < 40; ++i) many.phase("p", 30000, 14000);
  TEST_ASSERT_TRUE(many.line(true).find("(+27 more)") != std::string::npos);
}

// The admission floor leaves the dispatch floor after the setup's measured worst
// case, on the default floors and on the device's (the static_assert is in place).
static void test_admission_floor_covers_dispatch_plus_setup() {
  TEST_ASSERT_TRUE(agent::admissionCoversSetup(TurnEngine::Tuning{}.turnHardFloor));
  TEST_ASSERT_TRUE(agent::admissionCoversSetup(agent::kDispatchFloorFree + agent::kTurnSetupCostBytes));
  TEST_ASSERT_FALSE(agent::admissionCoversSetup(agent::kDispatchFloorFree + agent::kTurnSetupCostBytes - 1));
  const std::string cfg = readSource("src/agent/agent_config.h");
  std::smatch m;
  TEST_ASSERT_TRUE(std::regex_search(cfg, m, std::regex("#define ORCH_TURN_HARD_FLOOR\\s+(\\d+)U")));
  TEST_ASSERT_TRUE(agent::admissionCoversSetup((uint32_t)std::stoul(m[1].str())));
  const std::string orch = readSource("src/agent/orchestrator.cpp");
  TEST_ASSERT_TRUE(orch.find("static_assert(admissionCoversSetup(ORCH_TURN_HARD_FLOOR)") != std::string::npos);
  TEST_ASSERT_TRUE(orch.find("setPsramFirstSwitch(") != std::string::npos);
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
static void test_setup_under_the_dispatch_floor_ends_honestly() {
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
  TEST_ASSERT_TRUE(r.endCauses[0].find("dispatch floor") != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("turn ended for memory before the provider request"));
  TEST_ASSERT_TRUE(LogCapture::contains("turn memory: admit=16936/7156"));
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

// At the CUM-404 admission floor itself (12000 / 5000) a setup within its measured
// cost still dispatches; one byte more than the cost under the floor refuses.
static void test_admission_floor_turn_dispatches_within_the_setup_cost() {
  Rig within;
  within.plat.heap = 12000;
  within.plat.largest = 5000;
  within.duringSetup = [](Rig& rr) { rr.plat.heap = 12000 - agent::kTurnSetupCostBytes; };
  within.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)within.attempts.size());
  Rig over;
  over.plat.heap = 12000;
  over.plat.largest = 5000;
  over.duringSetup = [](Rig& rr) { rr.plat.heap = agent::kDispatchFloorFree - 1; };
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
  RUN_TEST(test_gate_names_the_first_phase_under_the_floor);
  RUN_TEST(test_gate_property_over_every_sample);
  RUN_TEST(test_gate_lines_name_the_low_point_and_every_phase);
  RUN_TEST(test_admission_floor_covers_dispatch_plus_setup);
  RUN_TEST(test_psram_first_scope_nests_and_restores);
  RUN_TEST(test_owner_turn_runs_psram_first_end_to_end);
  RUN_TEST(test_scheduled_turn_runs_psram_first);
  RUN_TEST(test_setup_under_the_dispatch_floor_ends_honestly);
  RUN_TEST(test_fragmented_setup_ends_honestly);
  RUN_TEST(test_failed_allocation_in_the_turn_ends_honestly);
  RUN_TEST(test_failed_allocation_before_the_turn_ends_honestly);
  RUN_TEST(test_scheduled_fire_under_the_floor_is_a_defer);
  RUN_TEST(test_lumi_at_rest_is_admitted_and_dispatched);
  RUN_TEST(test_admission_floor_turn_dispatches_within_the_setup_cost);
  RUN_TEST(test_psram_first_restores_the_boot_spill_limit);
  return UNITY_END();
}
