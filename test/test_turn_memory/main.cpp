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
#include "nimbus/harness/turn_contract.h"   // kCutNothingSalvaged: the device's plain cut line
#include "nimbus/harness/turn_memory.h"
#include "nimbus/heap_watch.h"   // kSmallAllocInternalLimit
#include "nimbus/orch/loops.h"   // CUM-826: the scheduler's own rules over a fire's outcome

// L24 (no panic under combined memory starvation): a turn never spends the device's
// last internal SRAM. On unit A (26 Oct) a turn admitted at ~17 KB internal free
// spent it all on its own setup (its small heap blocks preferred internal SRAM) and
// the Wi-Fi task's own allocation failed: an IDF abort, a panic. The rules pinned
// here: the turn keeps its small blocks in PSRAM for its whole length, so its setup
// costs no internal SRAM and the floor it was admitted on still holds at its provider
// request, where it is checked again; under it the turn ends with one honest reply
// recorded in the chat history and no provider request; a failed allocation at any
// hook ends the turn the same way instead of aborting; Lumi at rest is still served.

using agent::JobEngine;
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
    std::string inputs;        // what the turn sent
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
  // F2: the head loop's round 1 bills (and, with roundReplies, its reply.telegram tells
  // the owner) inside the host call, then round 2's request throws std::bad_alloc.
  bool billThenThrow = false;
  bool roundReplies = false;
  std::string hostOut = kGoodTurn;   // what a served attempt answers
  bool hostFails = false;            // every attempt fails with hostErr (hostOut is its partial turn)
  std::string hostErr;
  bool replyMidLoop = false;         // the turn's reply.telegram answers mid-loop (noteToolReplied)
  bool webRowOnly = false;           // a delivery to "web" queues nothing: its row is the copy
  bool speakHeard = false;           // the speech sink reports that the owner heard it
  std::vector<std::string> spoken;   // what the speech sink was asked to say
  bool histV2 = false;               // the turn-structured history (a scheduled fire's trigger row)
  void maybeThrow(const char* at) {
    if (throwAt != at) return;
    throwAt.clear();
    throw std::bad_alloc();
  }
  std::string memNote;   // a queued memory write's confirmation, drained once by the turn
  std::unique_ptr<JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    JobEngine::Deps jd;   // fresh sub-agent results and spawn notes (no fabric: nothing dispatches)
    jd.platform = plat.contract();
    jd.deliver = [this](const std::string& c, const std::string& t) { delivered.push_back({c, t}); };
    jobs.reset(new JobEngine(std::move(jd)));
    TurnEngine::Deps d;
    d.jobs = jobs.get();
    d.memNotes = [this](const std::string&) {
      std::string n;
      n.swap(memNote);
      return n;
    };
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
    d.deliver = [this](const std::string& c, const std::string& t) {
      maybeThrow("deliver");   // the device's filters allocate before anything is queued
      delivered.push_back({c, t});
    };
    d.apply.deliver = d.deliver;
    d.deliveryNeedsRow = [this](const std::string& c) { return webRowOnly && c == "web"; };
    // RBAC (round 1): this suite's chats are the owner's, as the device's tenant
    // table answers for them - the deny-all default would refuse the owner's own
    // device actions (the CUM-826 speech test's tts) that ran before the rule.
    d.apply.principalFor = [](const std::string& chat) {
      return orch::principalForRole(chat, orch::Role::Admin);
    };
    d.apply.speak = [this](const std::string& t) {   // the device's sink notes what was heard
      spoken.push_back(t);
      if (speakHeard) eng->noteSpoke();
    };
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
    d.histV2 = [this] { return histV2; };
    d.historyRows = [](const std::string&, size_t, const std::string&) {
      return std::vector<orch::EpisodicMessage>{};
    };
    d.captureUserRow = [](const std::string&, const std::string&, const std::string&) { return std::string("m1"); };
    d.captureTrigger = [this](const std::string&, const std::string&, const std::string&) {
      maybeThrow("trigger");   // a quiet routine's silent row, written after its turn ended
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
      d.hosts.add(h, [this, h](std::string& conv, const std::string&, const std::string& inputs, std::string& out,
                               std::string& err, const agent::HeadTools*, orch::TokenUsage* usage) {
        return hostAnswers(h, conv, inputs, out, err, usage);
      });
    eng.reset(new TurnEngine(std::move(d)));
  }
  // One provider attempt (every host answers the same way).
  bool hostAnswers(const char* h, std::string& conv, const std::string& inputs, std::string& out, std::string& err,
                   orch::TokenUsage* usage) {
    attempts.push_back({h, switchOn(), inputs});
    maybeThrow("provider");
    if (billThenThrow) {
      billThenThrow = false;
      if (usage) usage->add(100, 20);   // round 1 billed
      if (roundReplies) {               // its reply.telegram went out
        delivered.push_back({"1001", "round one told the owner"});
        eng->noteToolReplied();
      }
      throw std::bad_alloc();           // round 2's request is being built
    }
    if (replyMidLoop) {
      delivered.push_back({"1001", "told the owner mid-loop"});
      eng->noteToolReplied();
    }
    conv = "c1";
    out = hostOut;
    if (usage) usage->add(100, 20);
    if (hostFails) err = hostErr;
    return !hostFails;
  }
  bool anyDelivered(const char* needle) const {
    for (const auto& p : delivered)
      if (p.second.find(needle) != std::string::npos) return true;
    return false;
  }
  // The three inputs a turn takes before it knows it can run (merge review, major 2).
  void queueInputs() {
    jobs->addFreshResult("job0001", "modelX", "sub finding about the dentist");
    memNote = "- stored: the boiler service is on Friday\n";
    jobs->noteSpawnIntentRefused("1001", "alpha", "look up trains", "no provider has a key");
  }
  static bool sawAllInputs(const Attempt& a) {
    return a.inputs.find("sub finding about the dentist") != std::string::npos &&
           a.inputs.find("the boiler service is on Friday") != std::string::npos &&
           a.inputs.find("'alpha' did not start") != std::string::npos;
  }
  static bool sawAnyInput(const Attempt& a) {
    return a.inputs.find("sub finding about the dentist") != std::string::npos ||
           a.inputs.find("the boiler service is on Friday") != std::string::npos ||
           a.inputs.find("'alpha' did not start") != std::string::npos;
  }
  // The next owner turn, served, and the one after it: the put-back inputs reach the
  // first exactly once (never dropped, never repeated).
  void assertNextTurnGetsThemOnce(const char* why) {
    duringSetup = nullptr;
    plat.heap = 100000;
    plat.largest = 100000;
    const size_t before = attempts.size();
    eng->handleMessage("and now?", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(before + 1, attempts.size(), why);
    TEST_ASSERT_TRUE_MESSAGE(sawAllInputs(attempts.back()), why);
    eng->handleMessage("thanks", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(before + 2, attempts.size(), why);
    TEST_ASSERT_FALSE_MESSAGE(sawAnyInput(attempts.back()), why);
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

// ---- nothing a refused turn took is lost (merge review, major 2) --------------------

// A turn refused at its provider request had already taken the chat's fresh sub-agent
// results, its parked memory results and its spawn-refusal notes into its input. They
// go back: the next turn sees each of them once. Counter-test: a served turn consumes
// them (the turn after it sees none).
static void test_a_memory_refused_turn_puts_back_what_it_took() {
  Rig r;
  r.queueInputs();
  r.duringSetup = [](Rig& rr) { rr.plat.heap = 700; };
  r.eng->handleMessage("hi", "Roy", "1001");
  assertShortOfMemoryEnd(r, "refused at dispatch");
  TEST_ASSERT_TRUE(r.jobs->hasFreshResults());
  r.assertNextTurnGetsThemOnce("refused at dispatch");
}

// The class: a failed allocation at every hook before the provider request puts the
// inputs back too (from the turn's own handler, or the entry's before the turn began).
static void test_a_failed_allocation_before_the_request_puts_back_what_it_took() {
  for (const char* at : {"row", "context", "recall", "specs", "compose"}) {
    Rig r;
    r.queueInputs();
    r.throwAt = at;
    r.eng->handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_TRUE_MESSAGE(r.throwAt.empty(), at);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.attempts.size(), at);
    TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered(kShortOfMemoryReply), at);
    r.assertNextTurnGetsThemOnce(at);
  }
}

// A synthesis turn refused at its provider request defers: its results are parked
// again (no raw dump on the owner), and the next pump synthesizes them. The same for a
// failed allocation while its input is built. Counter-test: a synthesis that fails at
// the provider (not for memory) still delivers them raw (never lost).
static void test_a_memory_refused_synthesis_defers_instead_of_dumping() {
  for (int viaThrow = 0; viaThrow < 2; ++viaThrow) {
    Rig r;
    r.jobs->addFreshResult("job0001", "modelX", "sub finding about the dentist");
    if (viaThrow) r.throwAt = "context";
    else r.duringSetup = [](Rig& rr) { rr.plat.heap = 700; };
    r.eng->maybeConsolidate("1001");
    TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("raw findings"), "a memory defer dumped the raw results");
    TEST_ASSERT_TRUE(r.jobs->hasFreshResults());
    TEST_ASSERT_TRUE(LogCapture::contains("synthesis turn deferred for memory - results kept for the next try"));
    r.duringSetup = nullptr;
    r.plat.heap = 100000;
    r.eng->maybeConsolidate("1001");
    TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
    TEST_ASSERT_TRUE(r.attempts[0].inputs.find("sub finding about the dentist") != std::string::npos);
    TEST_ASSERT_FALSE(r.jobs->hasFreshResults());
  }
  Rig f;
  f.jobs->addFreshResult("job0001", "modelX", "sub finding about the dentist");
  f.throwAt = "provider";   // asked, then failed: not a memory refusal before the request
  f.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(1, (int)f.attempts.size());
  TEST_ASSERT_TRUE_MESSAGE(f.anyDelivered("raw findings"), "a failed synthesis lost its results");
}

// A one-shot reminder or wakeup deferred for memory asks to fire again soon (its next
// slot is its whole delay again); a routine keeps its next slot. Both on the floor
// refusal and on a failed allocation while it is set up.
static void test_a_memory_deferred_one_shot_retries_soon() {
  for (int viaThrow = 0; viaThrow < 2; ++viaThrow) {
    for (int kind = 0; kind < 3; ++kind) {   // routine, one-shot wakeup, owner reminder
      Rig r;
      if (viaThrow) r.throwAt = "context";
      else r.duringSetup = [](Rig& rr) { rr.plat.heap = 700; };
      const orch::FireOutcome o =
          r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1", false, kind == 1, kind == 2);
      TEST_ASSERT_TRUE(o.deferred);
      TEST_ASSERT_TRUE(o.error.empty());
      TEST_ASSERT_EQUAL_UINT32(kind == 0 ? 0u : 120u, o.retryAfterSec);
      TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
    }
  }
}

// F2 (v4.6.1 review): a failed allocation in the head loop after round 1 billed (and
// its reply.telegram told the owner) is no memory refusal. The fire is not deferred
// with an empty error (drainSystemTurn would restage the post-update turn, a routine
// would fire again, a one-shot would retry: the owner messaged twice, round 1 billed
// twice), and round 1's usage is metered once. Counter-test: a throw before any
// provider request still defers, with nothing billed.
static orch::FireOutcome fireScheduledOrBounded(Rig& r, bool bounded) {
  if (bounded) return r.eng->injectBoundedTurn("1001", "[FIRMWARE UPDATED] v4.6.1", "firmware-update",
                                               TurnEngine::TurnBounds{});
  return r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1");
}

static int meteredPromptTokens(const Rig& r) {
  int billed = 0;
  for (const auto& rec : r.cfg.recorded) billed += (int)rec.in;
  return billed;
}

static void test_a_failed_allocation_after_a_billed_round_is_not_a_defer() {
  for (int bounded = 0; bounded < 2; ++bounded) {
    for (int replies = 0; replies < 2; ++replies) {
      char why[48];
      std::snprintf(why, sizeof why, "bounded=%d replies=%d", bounded, replies);
      Rig r;
      r.billThenThrow = true;
      r.roundReplies = replies == 1;
      const orch::FireOutcome o = fireScheduledOrBounded(r, bounded == 1);
      TEST_ASSERT_FALSE_MESSAGE(r.billThenThrow, why);   // the host was asked and threw
      TEST_ASSERT_FALSE_MESSAGE(o.ok, why);
      TEST_ASSERT_FALSE_MESSAGE(o.deferred, why);
      TEST_ASSERT_FALSE_MESSAGE(o.error.empty(), why);   // a failure the scheduler records, never a restage
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, o.retryAfterSec, why);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.attempts.size(), why);
      TEST_ASSERT_EQUAL_MESSAGE(100, meteredPromptTokens(r), why);   // round 1, metered once
      TEST_ASSERT_EQUAL_MESSAGE(100, (int)o.tokens.promptTokens, why);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.ends.size(), why);
      TEST_ASSERT_FALSE_MESSAGE(r.ends[0].ok, why);
      TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered(kShortOfMemoryReply), why);
      TEST_ASSERT_EQUAL_MESSAGE(replies, (int)r.delivered.size(), why);   // round 1's reply only
      TEST_ASSERT_FALSE_MESSAGE(r.eng->turnInFlight(), why);
      TEST_ASSERT_EQUAL_MESSAGE(0, PsramFirstScope::depth(), why);
    }
    Rig early;   // before any provider request: still a defer, nothing billed
    early.throwAt = "context";
    const orch::FireOutcome d = fireScheduledOrBounded(early, bounded == 1);
    TEST_ASSERT_TRUE(early.throwAt.empty());
    TEST_ASSERT_TRUE(d.deferred);
    TEST_ASSERT_TRUE(d.error.empty());
    TEST_ASSERT_EQUAL(0, (int)early.attempts.size());
    TEST_ASSERT_EQUAL(0, meteredPromptTokens(early));
  }
}

// CUM-826 (F2 follow-through): a one-shot wakeup or owner reminder whose turn reached
// the owner and then failed is retired, never given its short retry (the owner would
// get the message twice). One that failed before it said anything keeps its one retry;
// a memory defer stays a defer. A recurring routine that reached the owner and failed
// counts the failure and keeps its next slot (no early re-fire). The engine's
// FireOutcome is run through the scheduler's own pure rules (nimbus/orch/loops.h).
static void test_a_one_shot_that_answered_then_failed_is_retired_not_retried() {
  for (int kind = 1; kind <= 2; ++kind) {   // a wakeup, an owner reminder
    const bool once = kind == 1, reminder = kind == 2;
    Rig a;
    a.billThenThrow = true;
    a.roundReplies = true;
    const orch::FireOutcome o = a.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1", false, once, reminder);
    TEST_ASSERT_TRUE(orch::fireWasAttempted(o));
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_TRUE(o.answered);
    orch::LoopRecord l;
    l.sched.kind = orch::SchedKind::Once;
    orch::onFireResult(l, o.ok, o.tokens, orch::fnv64(o.detail), 1000);
    TEST_ASSERT_EQUAL(1, (int)l.consecFails);   // the failure is on the record
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::Retire, (int)orch::onceAfterFire(l, o));
    TEST_ASSERT_FALSE(orch::retriesSoon(o));

    Rig q;   // billed, failed, said nothing: one short retry, then retired
    q.billThenThrow = true;
    const orch::FireOutcome f = q.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1", false, once, reminder);
    TEST_ASSERT_TRUE(orch::fireWasAttempted(f));
    TEST_ASSERT_FALSE(f.answered);
    orch::LoopRecord m;
    m.sched.kind = orch::SchedKind::Once;
    orch::onFireResult(m, f.ok, f.tokens, orch::fnv64(f.detail), 1000);
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::RetryShort, (int)orch::onceAfterFire(m, f));
    orch::onFireResult(m, f.ok, f.tokens, orch::fnv64(f.detail), 1300);
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::Retire, (int)orch::onceAfterFire(m, f));

    Rig d;   // a memory defer before any request: deferred, retried soon, nothing counted
    d.throwAt = "context";
    const orch::FireOutcome g = d.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1", false, once, reminder);
    TEST_ASSERT_FALSE(orch::fireWasAttempted(g));
    TEST_ASSERT_FALSE(g.answered);
    TEST_ASSERT_EQUAL_UINT32(120u, g.retryAfterSec);
  }
  Rig r;   // a recurring routine
  r.billThenThrow = true;
  r.roundReplies = true;
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1");
  TEST_ASSERT_TRUE(o.answered);
  orch::LoopRecord l;
  l.sched.kind = orch::SchedKind::Interval;
  orch::onFireResult(l, o.ok, o.tokens, orch::fnv64(o.detail), 1000);
  TEST_ASSERT_EQUAL(1, (int)l.consecFails);
  TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::NotOnce, (int)orch::onceAfterFire(l, o));
  TEST_ASSERT_FALSE(orch::retriesSoon(o));
  Rig s;   // a served routine answered too, and is no failure
  const orch::FireOutcome ok = s.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1");
  TEST_ASSERT_TRUE(ok.ok);
  TEST_ASSERT_TRUE(ok.answered);
}

// The fire's own handler (an allocation that fails after its turn ended, here the quiet
// routine's silent row) reads the turn's result recorded before that bookkeeping: a
// served routine is reported ok with its spend, so its consecFails stays put (a working
// routine is never breaker-disabled by its bookkeeping). A turn that failed and then
// hit the same failure is reported failed, never deferred. A failure in the fire's
// setup, before the turn, is still a defer.
static void test_a_failed_allocation_after_the_fire_ran_is_not_a_defer() {
  Rig r;
  r.histV2 = true;
  r.hostOut = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";   // the quiet routine stays silent
  r.throwAt = "trigger";
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "tidy up", "Upkeep", "lp1", /*quietOk=*/true);
  TEST_ASSERT_TRUE(r.throwAt.empty());   // the silent row was written after the turn and threw
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_FALSE(o.deferred);
  TEST_ASSERT_FALSE(o.answered);   // it said nothing
  TEST_ASSERT_EQUAL(100, meteredPromptTokens(r));
  TEST_ASSERT_EQUAL(100, (int)o.tokens.promptTokens);
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
  orch::LoopRecord l;
  l.sched.kind = orch::SchedKind::Interval;
  orch::onFireResult(l, o.ok, o.tokens, orch::fnv64(o.detail), 1000);
  TEST_ASSERT_EQUAL(0, (int)l.consecFails);

  Rig f;   // the turn failed, then the same bookkeeping failed: a failure, never a defer
  f.histV2 = true;
  f.hostOut = "";
  f.hostFails = true;
  f.hostErr = "HTTP 500 server error";
  f.throwAt = "trigger";
  const orch::FireOutcome g = f.eng->injectScheduledTurn("1001", "tidy up", "Upkeep", "lp1", /*quietOk=*/true);
  TEST_ASSERT_TRUE(f.throwAt.empty());
  TEST_ASSERT_FALSE(g.ok);
  TEST_ASSERT_FALSE(g.deferred);
  TEST_ASSERT_FALSE(g.error.empty());

  Rig e;
  e.histV2 = true;
  e.throwAt = "context";
  const orch::FireOutcome d = e.eng->injectScheduledTurn("1001", "tidy up", "Upkeep", "lp1", /*quietOk=*/true);
  TEST_ASSERT_TRUE(d.deferred);
  TEST_ASSERT_TRUE(d.error.empty());
}

static orch::FireOutcome fireOneShot(Rig& r, int kind) {
  return r.eng->injectScheduledTurn("1001", "say hi", "Morning", "lp1", false, kind == 1, kind == 2);
}

static int onceVerdict(const orch::FireOutcome& o) {
  orch::LoopRecord l;
  l.sched.kind = orch::SchedKind::Once;
  orch::onFireResult(l, o.ok, o.tokens, orch::fnv64(o.detail), 1000);
  return (int)orch::onceAfterFire(l, o);
}

// CUM-826: a cut or unreadable answer the turn salvages and delivers to the owner IS
// model text that reached them, on both salvage exits (the provider reported the cut;
// the answer parsed as a cut turn). A one-shot whose fire failed that way retires,
// never re-fires. Counter-test: nothing salvageable (the device's plain cut line) is no
// answer, and the one-shot keeps its short retry.
static void test_a_salvaged_reply_is_an_answer() {
  static const char* kCut = "{\"reply\":\"First part of a long answer is complete. Second part was cut mi";
  for (int kind = 1; kind <= 2; ++kind) {   // a wakeup, an owner reminder
    for (int reported = 0; reported < 2; ++reported) {
      char why[40];
      std::snprintf(why, sizeof why, "kind=%d reported=%d", kind, reported);
      Rig r;
      r.hostOut = kCut;
      r.hostFails = reported == 1;
      r.hostErr = "cut at the output limit (12000 tokens)";
      const orch::FireOutcome o = fireOneShot(r, kind);
      TEST_ASSERT_FALSE_MESSAGE(o.ok, why);
      TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("First part of a long answer is complete."), why);
      TEST_ASSERT_TRUE_MESSAGE(o.answered, why);
      TEST_ASSERT_EQUAL_MESSAGE((int)orch::OnceAfterFire::Retire, onceVerdict(o), why);
    }
    Rig n;   // nothing kept: the plain cut line is the device's, not an answer
    n.hostOut = "";
    n.hostFails = true;
    n.hostErr = "cut at the output limit (12000 tokens)";
    const orch::FireOutcome o = fireOneShot(n, kind);
    TEST_ASSERT_TRUE(n.anyDelivered(agent::kCutNothingSalvaged));
    TEST_ASSERT_FALSE(o.answered);
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::RetryShort, onceVerdict(o));
  }
}

// A delivery that throws before anything was queued (the device's output filters
// allocate first) reached nobody: the one-shot is not answered and keeps its one short
// retry - never silently retired.
static void test_a_delivery_that_throws_before_queueing_is_no_answer() {
  for (int kind = 1; kind <= 2; ++kind) {
    Rig r;
    r.throwAt = "deliver";
    const orch::FireOutcome o = fireOneShot(r, kind);
    TEST_ASSERT_TRUE(r.throwAt.empty());
    TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
    TEST_ASSERT_FALSE(r.anyDelivered("hello there"));
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_FALSE(o.deferred);
    TEST_ASSERT_FALSE(o.answered);
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::RetryShort, onceVerdict(o));
  }
}

// A routine's delivery to the web chat queues nothing of its own: the open page
// re-renders the chat's history, so the row IS the owner's copy (deliveryNeedsRow).
// The fire spoke only once that row is written: a row that throws reached nobody, and
// the one-shot keeps its short retry (no "was delivered" alert). The same for a
// salvaged answer delivered there. Counter-tests: the row written is an answer, and a
// Telegram chat's queued reply is an answer even when its row then throws.
static void test_a_web_fire_spoke_only_once_its_row_was_written() {
  static const char* kCut = "{\"reply\":\"First part of a long answer is complete. Second part was cut mi";
  for (int kind = 1; kind <= 2; ++kind) {
    for (int salvage = 0; salvage < 2; ++salvage) {
      char why[32];
      std::snprintf(why, sizeof why, "kind=%d salvage=%d", kind, salvage);
      Rig r;
      r.webRowOnly = true;
      if (salvage) r.hostOut = kCut;
      r.throwAt = "reply";   // the reply's history row
      const orch::FireOutcome o =
          r.eng->injectScheduledTurn("web", "say hi", "Morning", "lp1", false, kind == 1, kind == 2);
      TEST_ASSERT_TRUE_MESSAGE(r.throwAt.empty(), why);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), why);   // handed to the sink, which queued nothing
      TEST_ASSERT_FALSE_MESSAGE(o.ok, why);
      TEST_ASSERT_FALSE_MESSAGE(o.answered, why);
      TEST_ASSERT_EQUAL_MESSAGE((int)orch::OnceAfterFire::RetryShort, onceVerdict(o), why);

      Rig w;   // the row written: the owner can read it
      w.webRowOnly = true;
      if (salvage) w.hostOut = kCut;
      const orch::FireOutcome a =
          w.eng->injectScheduledTurn("web", "say hi", "Morning", "lp1", false, kind == 1, kind == 2);
      TEST_ASSERT_TRUE_MESSAGE(a.answered, why);
    }
    Rig t;   // a Telegram chat: the reply was queued before its row threw
    t.webRowOnly = true;
    t.throwAt = "reply";
    const orch::FireOutcome o = fireOneShot(t, kind);
    TEST_ASSERT_TRUE(o.answered);
    TEST_ASSERT_EQUAL((int)orch::OnceAfterFire::Retire, onceVerdict(o));
  }
}

// Speech marks the fire only through the device's sink, when something was heard or a
// voice note was sent: a sink that sent nothing leaves it unanswered (the one-shot
// keeps its short retry); one that reports it heard is an answer.
static void test_speech_is_an_answer_only_when_heard() {
  static const char* kSpeakOnly =
      "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\",\"device\":[{\"type\":\"tts\",\"text\":\"Time for tea.\"}]}";
  for (int heard = 0; heard < 2; ++heard) {
    Rig r;
    r.cfg.tts = true;   // the owner's "Voice replies" toggle
    r.hostOut = kSpeakOnly;
    r.speakHeard = heard == 1;
    r.throwAt = "reply";   // the spoken line's history row, written after the speech
    const orch::FireOutcome o = fireOneShot(r, 2);
    TEST_ASSERT_TRUE(r.throwAt.empty());
    TEST_ASSERT_EQUAL(1, (int)r.spoken.size());
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_EQUAL(heard == 1, o.answered);
    TEST_ASSERT_EQUAL((int)(heard ? orch::OnceAfterFire::Retire : orch::OnceAfterFire::RetryShort), onceVerdict(o));
  }
}

// The device seams that hand the model's text over outside apply note the spoke once
// the hand-over succeeded, before any history row (source guards: device code).
static void test_device_hand_overs_note_the_spoke() {
  const std::string files = readSource("src/agent/files_subsystem.cpp");
  const size_t send = files.find("agent::telegram::sendMediaSd(");
  const size_t note = files.find("agent::orchestrator::noteTurnSpoke();", send);
  TEST_ASSERT_TRUE_MESSAGE(send != std::string::npos && note != std::string::npos, "files.send notes the spoke");
  TEST_ASSERT_TRUE(note < files.find("ToolResult::ok(\"queued \"", send));
  const std::string main = readSource("src/main.cpp");
  TEST_ASSERT_TRUE_MESSAGE(main.find("if (agent::voicereply::deliver(chat, text).notesSent > 0) "
                                     "agent::orchestrator::noteTurnSpoke();") != std::string::npos,
                           "the speak sink's Telegram fallback notes a note that arrived");
  const std::string orch = readSource("src/agent/orchestrator.cpp");
  const size_t played = orch.find("if (!::sfx::awaitReplyStart(");
  const size_t heard = orch.find("noteTurnSpoke();", played);
  TEST_ASSERT_TRUE(played != std::string::npos && heard < orch.find("if (capture) memory::captureMessage", played));
  TEST_ASSERT_TRUE(orch.find("s_lastDeliveryNeedsRow = !queued || (chatId == \"web\" && loops::firing());") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(orch.find("d.deliveryNeedsRow = [](const std::string&) { return s_lastDeliveryNeedsRow; };") !=
                   std::string::npos);
}

// The fire's flags are its own: a previous turn on the SAME engine (an owner turn whose
// reply was delivered, one whose reply.telegram answered mid-loop, a served fire) never
// decides a fire that then fails in its setup. It is a memory defer, not answered, not
// served.
static void test_a_previous_turns_flags_never_decide_a_fire() {
  for (int variant = 0; variant < 3; ++variant) {
    char why[24];
    std::snprintf(why, sizeof why, "variant=%d", variant);
    Rig r;
    if (variant == 0) {   // applied (and spoke): an owner turn's reply was delivered
      r.eng->handleMessage("hi", "Roy", "1001");
      TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("hello there"), why);
    } else if (variant == 1) {   // toolReplied_ (and spoke): reply.telegram answered, no reply
      r.replyMidLoop = true;
      r.hostOut = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";
      r.eng->handleMessage("hi", "Roy", "1001");
      TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("told the owner mid-loop"), why);
      TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("hello there"), why);
      r.replyMidLoop = false;
    } else {   // a served fire
      TEST_ASSERT_TRUE_MESSAGE(fireOneShot(r, 1).ok, why);
    }
    r.throwAt = "context";
    const orch::FireOutcome o = fireOneShot(r, 2);
    TEST_ASSERT_TRUE_MESSAGE(r.throwAt.empty(), why);
    TEST_ASSERT_TRUE_MESSAGE(o.deferred, why);
    TEST_ASSERT_FALSE_MESSAGE(o.ok, why);
    TEST_ASSERT_FALSE_MESSAGE(o.answered, why);
    TEST_ASSERT_TRUE_MESSAGE(o.error.empty(), why);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(120u, o.retryAfterSec, why);
  }
}

// The same rule for an owner turn: round 1's reply already reached the owner, so no
// short-of-memory reply rides on top of it, and round 1 is metered once.
static void test_an_owner_turn_that_replied_then_failed_adds_nothing_and_meters_once() {
  Rig r;
  r.billThenThrow = true;
  r.roundReplies = true;
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_FALSE(r.anyDelivered(kShortOfMemoryReply));
  TEST_ASSERT_EQUAL(100, meteredPromptTokens(r));
  TEST_ASSERT_EQUAL(r.starts.size(), r.ends.size());
}

// The short-of-memory reply goes out once even when its own history row fails to
// allocate (the turn's handler must not say it again).
static void test_a_failed_reply_row_does_not_send_the_short_reply_twice() {
  Rig r;
  r.duringSetup = [](Rig& rr) { rr.plat.heap = 700; };
  r.throwAt = "reply";   // the refusal's history capture
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_TRUE(r.throwAt.empty());
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING(kShortOfMemoryReply, r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL(r.starts.size(), r.ends.size());
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
  TEST_ASSERT_EQUAL(0, PsramFirstScope::depth());
}

// The post-update system turn deferred for memory is staged again, never dropped,
// and a newer staged turn is not overwritten by it (device seam, source guard).
static void test_a_memory_deferred_system_turn_is_staged_again() {
  const std::string orch = readSource("src/agent/orchestrator.cpp");
  TEST_ASSERT_TRUE(orch.find("if (o.deferred && o.error.empty()) {") != std::string::npos);
  TEST_ASSERT_TRUE(orch.find("restageSystemTurn(prompt, name);") != std::string::npos);
  TEST_ASSERT_TRUE(orch.find("if (!s_sysTurnPending) {") != std::string::npos);
  TEST_ASSERT_TRUE(orch.find("millis() - s_sysTurnDeferredAt < kSysTurnRetryMs") != std::string::npos);
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
  RUN_TEST(test_a_memory_refused_turn_puts_back_what_it_took);
  RUN_TEST(test_a_failed_allocation_before_the_request_puts_back_what_it_took);
  RUN_TEST(test_a_memory_refused_synthesis_defers_instead_of_dumping);
  RUN_TEST(test_a_memory_deferred_one_shot_retries_soon);
  RUN_TEST(test_a_failed_allocation_after_a_billed_round_is_not_a_defer);
  RUN_TEST(test_an_owner_turn_that_replied_then_failed_adds_nothing_and_meters_once);
  RUN_TEST(test_a_one_shot_that_answered_then_failed_is_retired_not_retried);
  RUN_TEST(test_a_failed_allocation_after_the_fire_ran_is_not_a_defer);
  RUN_TEST(test_a_salvaged_reply_is_an_answer);
  RUN_TEST(test_a_delivery_that_throws_before_queueing_is_no_answer);
  RUN_TEST(test_a_previous_turns_flags_never_decide_a_fire);
  RUN_TEST(test_a_web_fire_spoke_only_once_its_row_was_written);
  RUN_TEST(test_speech_is_an_answer_only_when_heard);
  RUN_TEST(test_device_hand_overs_note_the_spoke);
  RUN_TEST(test_a_failed_reply_row_does_not_send_the_short_reply_twice);
  RUN_TEST(test_a_memory_deferred_system_turn_is_staged_again);
  RUN_TEST(test_psram_first_restores_the_boot_spill_limit);
  return UNITY_END();
}
