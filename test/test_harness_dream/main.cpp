#include <unity.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/dream.h"
#include "nimbus/harness/engine.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"

// DREAMING suite - the pure dream core (lib/harness/dream.{h,cpp}) + the loops
// defer seam + the quiet-turn rail: reserved-loop identity/shape + cancel
// refusal (mirroring the device loop.cancel handler), the idle gate
// (quiet/jobs/heap + the boot window), defer-not-fire semantics
// (orch::deferLoop leaves every ceiling counter alone), the episodic digest
// (order + byte budget), the [DREAM] prompt assembly, the maintenance phase
// against the REAL VectorMemory with a fake clock, and a day-in-the-life run
// through the TurnEngine (Rig pattern from test_harness_turn): the dream turn
// rides the ordinary scheduled path - rails armed, "loop:dream" attribution,
// mem_write landing, and an empty reply delivering NOTHING (quietOk).

using agent::JobEngine;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace dream = agent::dream;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- (1) reserved identity --------------------------------------------------

static void test_reserved_record_shape() {
  orch::LoopRecord l = dream::reservedLoopRecord();
  TEST_ASSERT_EQUAL_STRING("dream", l.id.c_str());
  TEST_ASSERT_EQUAL_STRING("Dream", l.name.c_str());
  TEST_ASSERT_EQUAL((int)orch::SchedKind::Daily, (int)l.sched.kind);
  TEST_ASSERT_EQUAL(210, (int)l.sched.minuteOfDay);   // 03:30 local
  TEST_ASSERT_EQUAL(0x7F, (int)l.sched.weekMask);
  TEST_ASSERT_EQUAL((int)orch::CreatedBy::Owner, (int)l.createdBy);
  TEST_ASSERT_TRUE(l.enabled);    // created-enabled: owner-shipped behavior
  TEST_ASSERT_TRUE(l.approved);   // never PENDING (not an agent creation)
  TEST_ASSERT_TRUE(l.prompt.length() > 0);
  TEST_ASSERT_TRUE((int)l.prompt.length() <= orch::kLoopPromptMax);
  TEST_ASSERT_TRUE((int)l.name.length() <= orch::kLoopNameMax);
  TEST_ASSERT_EQUAL(0, (int)l.nextRun);   // computed by the subsystem at ensure
}

static void test_reserved_id_and_cancel_refusal() {
  TEST_ASSERT_TRUE(dream::isReserved("dream"));
  TEST_ASSERT_FALSE(dream::isReserved("lp000001"));
  TEST_ASSERT_FALSE(dream::isReserved(""));
  std::string r = dream::cancelRefusal("dream");
  TEST_ASSERT_TRUE(r.find("reserved") != std::string::npos);
  TEST_ASSERT_TRUE(r.find("pause") != std::string::npos);   // points at the alternative
  TEST_ASSERT_EQUAL_STRING("", dream::cancelRefusal("lp000001").c_str());
}

// The device loop.cancel handler shape (memory_subsystem.cpp): refusal BEFORE
// the cancel, with the reason - never a lying "no loop with that id".
static void test_loop_cancel_tool_refuses_reserved() {
  orch::ToolRegistry reg;
  reg.add("loop.cancel", "Cancel (delete) a scheduled loop by its id.",
          [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> orch::ToolResult {
            std::string id = a["id"] | "";
            if (id.empty()) return orch::ToolResult::fail("missing 'id'");
            std::string refusal = dream::cancelRefusal(id);
            if (!refusal.empty()) return orch::ToolResult::fail(refusal);
            return orch::ToolResult::ok("loop cancelled");
          });
  ArduinoJson::JsonDocument d;
  d["id"] = "dream";
  orch::ToolResult r = reg.dispatch("loop.cancel", d.as<ArduinoJson::JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(r.error.find("reserved") != std::string::npos);
  d["id"] = "lp000001";
  r = reg.dispatch("loop.cancel", d.as<ArduinoJson::JsonObjectConst>(), nimbus::orch::principalForRole("test", nimbus::orch::Role::Admin));
  TEST_ASSERT_TRUE(r.success);
}

// ---- (2) idle gate ----------------------------------------------------------

static dream::GateInputs idleInputs() {
  dream::GateInputs in;
  in.nowMs = 3600000;         // 1 h up
  in.lastTurnEndMs = 600000;  // last turn ended 50 min ago
  in.activeJobs = 0;
  in.freeHeap = 60000;
  return in;
}

static void test_gate_idle_fires() {
  dream::GateResult r = dream::evaluateGate(dream::DreamGate{}, idleInputs());
  TEST_ASSERT_EQUAL(0, (int)r.deferSec);
  TEST_ASSERT_NULL(r.why);
}

static void test_gate_recent_turn_defers() {
  dream::GateInputs in = idleInputs();
  in.lastTurnEndMs = in.nowMs - (9 * 60 * 1000);   // 9 min ago < 10 min quiet
  dream::GateResult r = dream::evaluateGate(dream::DreamGate{}, in);
  TEST_ASSERT_EQUAL((int)dream::kDeferSec, (int)r.deferSec);
  TEST_ASSERT_EQUAL_STRING("recent-turn", r.why);
  in.lastTurnEndMs = in.nowMs - (10 * 60 * 1000);  // exactly the window: quiet
  TEST_ASSERT_EQUAL(0, (int)dream::evaluateGate(dream::DreamGate{}, in).deferSec);
}

static void test_gate_active_jobs_defer() {
  dream::GateInputs in = idleInputs();
  in.activeJobs = 1;
  dream::GateResult r = dream::evaluateGate(dream::DreamGate{}, in);
  TEST_ASSERT_EQUAL_STRING("active-jobs", r.why);
  TEST_ASSERT_EQUAL((int)dream::kDeferSec, (int)r.deferSec);
  // requireNoJobs=false waives it.
  dream::DreamGate g;
  g.requireNoJobs = false;
  TEST_ASSERT_EQUAL(0, (int)dream::evaluateGate(g, in).deferSec);
}

static void test_gate_low_heap_defers() {
  dream::GateInputs in = idleInputs();
  in.freeHeap = 29999;
  dream::GateResult r = dream::evaluateGate(dream::DreamGate{}, in);
  TEST_ASSERT_EQUAL_STRING("low-heap", r.why);
  in.freeHeap = 30000;
  TEST_ASSERT_EQUAL(0, (int)dream::evaluateGate(dream::DreamGate{}, in).deferSec);
}

static void test_gate_boot_counts_as_activity() {
  // lastTurnEndMs==0 (no turn since boot) + a young uptime => elapsed==nowMs,
  // so the first quiet window after boot is waited out.
  dream::GateInputs in = idleInputs();
  in.lastTurnEndMs = 0;
  in.nowMs = 5 * 60 * 1000;   // 5 min after boot
  TEST_ASSERT_EQUAL_STRING("recent-turn",
                           dream::evaluateGate(dream::DreamGate{}, in).why);
  in.nowMs = 11 * 60 * 1000;  // past the window: dreamable
  TEST_ASSERT_EQUAL(0, (int)dream::evaluateGate(dream::DreamGate{}, in).deferSec);
}

// ---- (3) defer-not-fire semantics (the loops-core seam) ---------------------

static void test_defer_moves_next_run_without_counting_a_fire() {
  orch::LoopRecord l;
  l.id = "dream";
  l.firesToday = 3;
  l.tokensToday = 1234;
  l.lastRun = 500;
  l.nextRun = 1000;   // due at now=2000
  TEST_ASSERT_TRUE(orch::isDue(l.nextRun, 2000));
  orch::deferLoop(l, 2000, dream::kDeferSec);
  TEST_ASSERT_EQUAL(2900, (int)l.nextRun);            // now + 15 min
  TEST_ASSERT_FALSE(orch::isDue(l.nextRun, 2000));    // no longer due
  TEST_ASSERT_EQUAL(3, (int)l.firesToday);            // NOT a fire
  TEST_ASSERT_EQUAL(1234, (int)l.tokensToday);
  TEST_ASSERT_EQUAL(500, (int)l.lastRun);
  // A zero defer still moves strictly after now (isDue is inclusive).
  orch::deferLoop(l, 2000, 0);
  TEST_ASSERT_FALSE(orch::isDue(l.nextRun, 2000));
}

// ---- (4) episodic digest ----------------------------------------------------

static orch::EpisodicMessage msg(const char* role, const std::string& text) {
  orch::EpisodicMessage m;
  m.role = role;
  m.text = text;
  return m;
}

static void test_digest_renders_oldest_first_and_flattens_newlines() {
  // Query order is most-recent-first; the digest must read chronologically.
  std::vector<orch::EpisodicMessage> msgs = {
      msg("assistant", "later\nreply"),   // newest
      msg("user", "first message"),       // oldest
  };
  std::string d = dream::buildEpisodicDigest(msgs);
  size_t first = d.find("- user: first message\n");
  size_t later = d.find("- assistant: later reply\n");   // \n flattened to ' '
  TEST_ASSERT_TRUE(first != std::string::npos);
  TEST_ASSERT_TRUE(later != std::string::npos);
  TEST_ASSERT_TRUE(first < later);
  TEST_ASSERT_TRUE(d.find("omitted") == std::string::npos);   // nothing clipped
}

static void test_digest_caps_bytes_dropping_oldest() {
  std::vector<orch::EpisodicMessage> msgs;
  for (int i = 0; i < 100; i++)   // most-recent-first: i=0 is the NEWEST
    msgs.push_back(msg("user", "message number " + std::to_string(i) +
                                   std::string(80, 'x')));
  std::string d = dream::buildEpisodicDigest(msgs, 500);
  TEST_ASSERT_TRUE(d.size() <= 500 + 64);   // budget + the truncation marker
  TEST_ASSERT_TRUE(d.find("(earlier messages omitted") != std::string::npos);
  TEST_ASSERT_TRUE(d.find("message number 0") != std::string::npos);    // newest kept
  TEST_ASSERT_TRUE(d.find("message number 99") == std::string::npos);   // oldest dropped
}

static void test_digest_empty_placeholder_and_long_line_clip() {
  TEST_ASSERT_EQUAL_STRING("(no episodic messages captured in the last day)\n",
                           dream::buildEpisodicDigest({}).c_str());
  std::vector<orch::EpisodicMessage> one = {msg("user", std::string(400, 'y'))};
  std::string d = dream::buildEpisodicDigest(one);
  TEST_ASSERT_TRUE(d.find("...") != std::string::npos);   // per-line 300 B clip
  TEST_ASSERT_TRUE(d.size() < 400);
}

// ---- (5) the [DREAM] prompt -------------------------------------------------

static void test_dream_inputs_content_and_stats() {
  dream::MemStats s;
  s.vectors = 12; s.pruned = 3; s.deduped = 2; s.scratchItems = 4; s.episodicMsgs = 20;
  std::string in = dream::buildDreamInputs("- user: hello\n",
                                           "## SCRATCHPAD\n- goal A\n", s);
  TEST_ASSERT_TRUE(in.find("[DREAM]") != std::string::npos);
  // The four instructions + the quiet-reply contract.
  TEST_ASSERT_TRUE(in.find("up to 7 DURABLE") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("0 is the correct number on a quiet day") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("groom your scratchpad") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("refresh your running `memory` summary") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("contradict") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("EMPTY string unless") != std::string::npos);
  // Stats + sections, in order.
  TEST_ASSERT_TRUE(in.find("[MEMORY STATS]") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("vectors=12 (pruned tonight=3, deduplicated=2)") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(in.find("## SCRATCHPAD") != std::string::npos);
  size_t y = in.find("[YESTERDAY]");
  TEST_ASSERT_TRUE(y != std::string::npos);
  TEST_ASSERT_TRUE(in.find("- user: hello", y) != std::string::npos);
  // Empty scratchpad: the block is simply absent.
  std::string noScratch = dream::buildDreamInputs("d", "", s);
  TEST_ASSERT_TRUE(noScratch.find("SCRATCHPAD") == std::string::npos);
}

static void test_dream_inputs_recap_oversize_digest() {
  // Defensive: a caller bypassing buildEpisodicDigest still gets byte-capped.
  std::string huge(dream::kDigestCapBytes + 4096, 'z');
  std::string in = dream::buildDreamInputs(huge, "", dream::MemStats{});
  TEST_ASSERT_TRUE(in.find("truncated at byte budget") != std::string::npos);
  TEST_ASSERT_TRUE(in.size() < dream::kDigestCapBytes + 2048);
}

// ---- (6) maintenance phase against the REAL VectorMemory --------------------

static orch::VecEntry entry(const char* id, const std::vector<float>& f, float imp,
                            int32_t ttl, uint32_t created, bool permanent = false) {
  orch::VecEntry e;
  e.id = id;
  e.content = id;
  e.importance = imp;
  e.ttlHours = ttl;
  e.createdAtHours = created;
  e.permanentFlag = permanent;
  e.vec = orch::VectorMemory::quantize(f);
  return e;
}

static void test_maintenance_decay_prune_dedup_counts() {
  orch::VectorMemory v;
  v.configure(4);
  const uint32_t nowH = 100;
  // Permanent anchor: exempt from decay AND prune.
  TEST_ASSERT_TRUE(v.add(entry("perm", {1, 0, 0, 0}, 0.9f, -1, 0, true), false));
  // Duplicate twins (identical direction, different importance).
  TEST_ASSERT_TRUE(v.add(entry("dupA", {0, 1, 0, 0}, 0.8f, 720, nowH), false));
  TEST_ASSERT_TRUE(v.add(entry("dupB", {0, 1, 0, 0}, 0.6f, 720, nowH), false));
  // Expired: created at 0 with a 10 h TTL, now is 100.
  TEST_ASSERT_TRUE(v.add(entry("expired", {0, 0, 1, 0}, 0.8f, 10, 0), false));
  // Fades under the floor after one decay: 0.052 * 0.95 = 0.0494 < 0.05.
  TEST_ASSERT_TRUE(v.add(entry("faded", {0, 0, 0, 1}, 0.052f, 720, nowH), false));
  TEST_ASSERT_EQUAL(5, v.size());

  // The dream maintenance order: decay -> prune -> dedup.
  v.decayImportance();
  int pruned = v.pruneExpired(nowH);
  int deduped = v.deduplicate();
  TEST_ASSERT_EQUAL(2, pruned);    // expired + faded
  TEST_ASSERT_EQUAL(1, deduped);   // one dup twin removed
  TEST_ASSERT_EQUAL(2, v.size());  // perm + the higher-importance dup

  bool sawPerm = false, sawDupA = false;
  for (const auto& e : v.getAll()) {
    if (e.id == "perm") {
      sawPerm = true;
      TEST_ASSERT_EQUAL_FLOAT(0.9f, e.importance);   // permanent: no decay
    }
    if (e.id == "dupA") sawDupA = true;   // higher-importance twin survives
  }
  TEST_ASSERT_TRUE(sawPerm);
  TEST_ASSERT_TRUE(sawDupA);
}

// ---- (7) day in the life: the dream turn through the TurnEngine -------------

static const char* kDreamTurn =
    "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\",\"mem_write\":[{\"content\":"
    "\"owner prefers green tea in the morning\",\"importance\":0.8}]}";

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;

  struct Attempt {
    std::string host, inputs;
    bool scheduledDuring = false;
  };
  std::vector<Attempt> attempts;
  std::string outJson = kDreamTurn;
  std::vector<std::pair<std::string, std::string>> delivered;
  std::vector<std::string> cues;
  std::vector<std::string> memWrites;   // memDispatch("memory.write") contents
  int persists = 0;

  std::unique_ptr<JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    JobEngine::Deps jd;
    jd.platform = plat.contract();
    jd.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
    };
    jobs.reset(new JobEngine(std::move(jd)));

    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = jd.deliver;
    d.fire = [this](const char* c) { cues.push_back(c); };
    d.recall = [](const std::string&, const nimbus::orch::Principal&) { return std::vector<std::string>{}; };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.hostLabel = "anthropic / model-anthropic";
      return in;
    };
    d.toolSpecs = [](const orch::Principal&) { return std::vector<orch::ToolRegistry::Spec>{}; };
    d.mcpDispatch = [](const std::string&, const nimbus::orch::Principal&) { return std::string("{}"); };
    d.connectorsCatalog = [] { return std::string("## PROVIDERS & CONNECTORS\n"); };
    d.modelChoices = [](const std::string& p) { return "model-" + p; };
    d.firstAllowedChat = [] { return std::string("1001"); };
    d.apply.deliver = d.deliver;
    d.apply.fire = d.fire;
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst a,
                             const nimbus::orch::Principal&)
        -> orch::ToolResult {
      if (std::string(name) == "memory.write")
        memWrites.push_back(a["content"] | "");
      return orch::ToolResult::ok("stored");
    };
    d.apply.persistMemory = [this] { persists++; };
    d.hosts.add("anthropic",
                [this](std::string& convId, const std::string&, const std::string& inp,
                       std::string& out, std::string&, const agent::HeadTools*,
                       orch::TokenUsage* usage) {
                  Attempt a;
                  a.host = "anthropic";
                  a.inputs = inp;
                  a.scheduledDuring = eng && eng->inScheduledTurn();
                  attempts.push_back(a);
                  convId = "c1";
                  out = outJson;
                  if (usage) usage->add(100, 20);
                  return true;
                });
    eng.reset(new TurnEngine(std::move(d)));
  }
};

static void test_dream_day_in_the_life() {
  Rig r;
  // The device fire path: digest + scratchpad + stats -> buildDreamInputs ->
  // an ordinary injectScheduledTurn with loopId="dream" and quietOk.
  dream::MemStats s;
  s.vectors = 10; s.pruned = 2; s.deduped = 1; s.scratchItems = 3; s.episodicMsgs = 20;
  std::vector<orch::EpisodicMessage> msgs = {
      msg("assistant", "brewed results delivered"),
      msg("user", "I always want green tea first thing"),
  };
  std::string inputs =
      dream::buildDreamInputs(dream::buildEpisodicDigest(msgs), "", s);
  orch::FireOutcome o =
      r.eng->injectScheduledTurn("", inputs, "Dream", "dream", /*quietOk=*/true);

  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  // The scheduled rails were armed during the provider call, and both the
  // scheduler preamble and the dream block reached the model.
  TEST_ASSERT_TRUE(r.attempts[0].scheduledDuring);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[SCHEDULED LOOP]") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[DREAM]") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("green tea first thing") !=
                   std::string::npos);
  // The model's mem_write landed through the memory dispatch + persisted.
  TEST_ASSERT_EQUAL(1, (int)r.memWrites.size());
  TEST_ASSERT_EQUAL_STRING("owner prefers green tea in the morning",
                           r.memWrites[0].c_str());
  TEST_ASSERT_EQUAL(1, r.persists);
  // Spend attributed to the dream loop.
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("loop:dream", r.cfg.recorded[0].tag.c_str());
  TEST_ASSERT_EQUAL(120, (int)o.tokens.total());
  // QUIET: the empty reply delivered NOTHING (no 03:30 "Done." ping), and the
  // FireOutcome carries the empty reply (the device glue fingerprints it).
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("", o.detail.c_str());
  TEST_ASSERT_FALSE(r.eng->inScheduledTurn());   // flag cleared after
}

static void test_empty_scheduled_turn_is_silent() {
  // The hardcoded "Done." ack was removed, so an empty-reply scheduled turn now
  // delivers NOTHING whether or not quietOk was passed (the quiet/non-quiet
  // distinction for empty turns is gone - a scheduled task that has nothing to
  // report simply stays silent).
  Rig r;
  r.outJson = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->injectScheduledTurn("1001", "check the weather", "morning", "L1");
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  // A turn with a NON-empty reply still delivers it (quiet or not).
  Rig r2;
  r2.outJson = "{\"reply\":\"owner: your notes contradict\",\"memory\":\"\",\"ask\":\"\"}";
  orch::FireOutcome o =
      r2.eng->injectScheduledTurn("", "in", "Dream", "dream", /*quietOk=*/true);
  TEST_ASSERT_EQUAL(1, (int)r2.delivered.size());
  TEST_ASSERT_EQUAL_STRING("owner: your notes contradict", r2.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("owner: your notes contradict", o.detail.c_str());
}


// ---- quiet-night skip predicate (the paid-turn gate) ------------------------
// Truth table for skipReflection: skip ONLY a provably quiet night - empty
// digest + unchanged scratchpad hash + a baseline exists + not forced.
static void test_skip_reflection_truth_table() {
  const uint64_t H = 0xabcdef1234567890ULL;
  // the one skip case
  TEST_ASSERT_TRUE (agent::dream::skipReflection(true,  H, H, false));
  // conversation happened -> run
  TEST_ASSERT_FALSE(agent::dream::skipReflection(false, H, H, false));
  // scratchpad moved since the last dream -> run
  TEST_ASSERT_FALSE(agent::dream::skipReflection(true,  H, H ^ 1, false));
  // no baseline yet (first night / pre-feature device) -> run
  TEST_ASSERT_FALSE(agent::dream::skipReflection(true,  H, 0, false));
  // console DREAM drill forces through regardless
  TEST_ASSERT_FALSE(agent::dream::skipReflection(true,  H, H, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reserved_record_shape);
  RUN_TEST(test_reserved_id_and_cancel_refusal);
  RUN_TEST(test_loop_cancel_tool_refuses_reserved);
  RUN_TEST(test_gate_idle_fires);
  RUN_TEST(test_gate_recent_turn_defers);
  RUN_TEST(test_gate_active_jobs_defer);
  RUN_TEST(test_gate_low_heap_defers);
  RUN_TEST(test_gate_boot_counts_as_activity);
  RUN_TEST(test_defer_moves_next_run_without_counting_a_fire);
  RUN_TEST(test_digest_renders_oldest_first_and_flattens_newlines);
  RUN_TEST(test_digest_caps_bytes_dropping_oldest);
  RUN_TEST(test_digest_empty_placeholder_and_long_line_clip);
  RUN_TEST(test_dream_inputs_content_and_stats);
  RUN_TEST(test_dream_inputs_recap_oversize_digest);
  RUN_TEST(test_maintenance_decay_prune_dedup_counts);
  RUN_TEST(test_skip_reflection_truth_table);
  RUN_TEST(test_dream_day_in_the_life);
  RUN_TEST(test_empty_scheduled_turn_is_silent);
  UNITY_END();
  return 0;
}
