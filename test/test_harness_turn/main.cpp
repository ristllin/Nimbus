#include <unity.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/skill_md.h"   // W15: skillsIndexText for the rig index

// Stage G suite - the turn orchestration, host-tested for the first time: the
// TurnGuard head arc, the recall gate, the "host|convId" conversation state,
// the per-provider budget failover + all-exhausted refusal, the same-host
// fresh-conv retry, the priority failover walk with the owner notice, the
// no-retry-once-a-tool-ran guard, usage/turn-counter accounting, the parse
// SALVAGE path, the scheduled-turn rails (reboot refused, FireOutcome shape),
// synthesis consolidation + raw fallback, the turn-debug hook, and the
// stuck-turn reaper.

using agent::JobEngine;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;
namespace attn = nimbus::attn;
using solide::ring::Status;

static const char* kGoodTurn = "{\"reply\":\"hello there\",\"memory\":\"\",\"ask\":\"\"}";

// ---- rig: engine + fakes ----------------------------------------------------
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;

  // Scripted provider steps, per host; the last script repeats once exhausted.
  struct Script {
    bool ok = true;
    std::string outJson = kGoodTurn;
    std::string err;
    int dispatchTools = 0;          // with non-null HeadTools: CALL dispatch n times
    std::string convOut = "c1";
    uint32_t usageIn = 100, usageOut = 20;
  };
  std::map<std::string, std::vector<Script>> scripts;
  std::map<std::string, int> scriptIdx;

  struct Attempt {
    std::string host, convIn, instructions, inputs;
    bool hadTools = false;
    bool scheduledDuring = false;   // engine.inScheduledTurn() at call time
  };
  std::vector<Attempt> attempts;
  std::vector<orch::HeadToolResult> toolResults;
  std::function<void()> onProviderCall;   // in-turn hook (stuck-turn reaper test)

  std::vector<std::pair<std::string, std::string>> delivered;  // chat, text
  std::vector<std::vector<std::string>> fabricCalls;  // hostLists routed to the fabric
  std::vector<attn::Event> events;
  std::vector<std::string> cues;
  std::vector<agent::ComposeInputs> composeCalls;   // (count only)
  std::vector<std::string> recallQueries;
  std::vector<std::string> recallResult{"[92%] owner likes tea"};
  std::vector<orch::ToolRegistry::Spec> specs;      // advertised registry tools
  std::vector<std::string> mcpRequests;
  std::string mcpResponse =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"isError\":false,"
      "\"content\":[{\"type\":\"text\",\"text\":\"tool-ok\"}]}}";
  struct Captured { std::string chatId, text, fromTag; };
  std::vector<Captured> captured;
  std::vector<orch::ValidatedAction> staged;   // ApplyDeps.stageDevice recorder
  int journalGcs = 0;

  struct DebugCap {
    std::string host, instructions, inputs, rawOut;
    bool convContinued = false, ok = false;
  };
  std::vector<DebugCap> debug;

  // HookRecorder - the lifecycle observer contract (Hooks consumers polish).
  std::vector<agent::TurnStartEv> hookStarts;
  std::vector<agent::TurnEndEv> hookEnds;
  std::vector<orch::HeadToolCall> hookToolCalls;
  std::vector<orch::HeadToolResult> hookToolResults;

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
    d.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
    };
    d.event = [this](const attn::Event& e) { events.push_back(e); };
    d.fire = [this](const char* c) { cues.push_back(c); };
    d.recall = [this](const std::string& q, const nimbus::orch::Principal&) {
      recallQueries.push_back(q);
      return recallResult;
    };
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.hostLabel = "anthropic / model-anthropic";
      composeCalls.push_back(in);
      return in;
    };
    d.toolSpecs = [this](const orch::Principal&) { return specs; };
    d.mcpDispatch = [this](const std::string& req, const nimbus::orch::Principal&) {
      mcpRequests.push_back(req);
      return mcpResponse;
    };
    d.connectorsCatalog = [] { return std::string("## PROVIDERS & CONNECTORS\n"); };
    // W15: the rig advertises a minimal skills index the way the device does
    // (skills::indexText -> agent::skillsIndexText over the capsule list).
    d.skillsIndex = [] {
      std::vector<agent::SkillIndexEntry> es = {
          {"deliver-pdf", "Build a PDF via an openai sub's code_interpreter and send it with files.send", false}};
      return agent::skillsIndexText(es);
    };
    d.modelChoices = [](const std::string& p) { return "model-" + p + "-a,model-" + p + "-b"; };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t,
                                   const std::string& f) {
      captured.push_back({c, t, f});
    };
    d.firstAllowedChat = [] { return std::string("1001"); };
    d.journalGc = [this] { journalGcs++; };
    d.hooks.onTurnDebug = [this](const agent::TurnDebugEv& ev) {
      debug.push_back({ev.host, *ev.instructions, *ev.inputs, *ev.rawOut,
                       ev.convContinued, ev.ok});
    };
    d.hooks.onTurnStart = [this](const agent::TurnStartEv& ev) { hookStarts.push_back(ev); };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) { hookEnds.push_back(ev); };
    d.hooks.onToolCall = [this](const orch::HeadToolCall& c) { hookToolCalls.push_back(c); };
    d.hooks.onToolResult = [this](const orch::HeadToolResult& tr) {
      hookToolResults.push_back(tr);
    };
    // ApplyDeps - minimal recording execution table (policy is test_harness_apply's).
    d.apply.deliver = d.deliver;
    d.apply.stageDevice = [this](const orch::ValidatedAction& va) { staged.push_back(va); };
    d.apply.fire = [this](const char* c) { cues.push_back(c); };
    // CUM-242: cumulo/zai are first-class heads on the device now, registered the
    // same way - the rig mirrors that so a router-fallback turn has a head to run.
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"})
      d.hosts.add(h, providerFor(h));
    // Fabric stub (recording): only reachable when a test opts in with
    // cfg.midFail = true - the rig default keeps every legacy test on the
    // scripted single-shot path. Mirrors the device: fabricSupports knows the
    // three cloud hosts, so a custom head must fall back to hosts.run.
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string&,
                            const std::string&, std::string& out, std::string&,
                            const agent::HeadTools&, orch::TokenUsage* usage,
                            const std::function<void(const std::string&,
                                                     const std::string&)>&) {
      fabricCalls.push_back(hl);
      out = kGoodTurn;
      if (usage) { usage->promptTokens = 100; usage->completionTokens = 20; }
      return true;
    };
    d.hosts.fabricSupports = [](const std::string& h) {
      return h == "openai" || h == "anthropic" || h == "mistral";
    };
    eng.reset(new TurnEngine(std::move(d)));
  }

  agent::ProviderTurnFn providerFor(const std::string& host) {
    return [this, host](std::string& convId, const std::string& ins, const std::string& inp,
                        std::string& out, std::string& err, const agent::HeadTools* tools,
                        orch::TokenUsage* usage) -> bool {
      Attempt a;
      a.host = host;
      a.convIn = convId;
      a.instructions = ins;
      a.inputs = inp;
      a.hadTools = tools != nullptr;
      a.scheduledDuring = eng && eng->inScheduledTurn();
      attempts.push_back(a);
      Script s;
      {
        auto& v = scripts[host];
        if (!v.empty()) {
          int i = scriptIdx[host];
          s = v[(size_t)i < v.size() ? i : v.size() - 1];
          scriptIdx[host] = i + 1;
        }
      }
      if (tools && s.dispatchTools > 0) {
        for (int i = 0; i < s.dispatchTools; i++) {
          orch::HeadToolCall c;
          c.id = "t1";
          c.name = "memory_search";
          c.argsJson = "{\"query\":\"x\"}";
          toolResults.push_back(tools->dispatch(c));
        }
      }
      if (onProviderCall) onProviderCall();
      if (s.ok) {
        convId = s.convOut;
        out = s.outJson;
        if (usage) usage->add(s.usageIn, s.usageOut);
      } else {
        err = s.err.empty() ? "scripted failure" : s.err;
      }
      return s.ok;
    };
  }

  int headEventsWith(Status st) const {
    const uint32_t key = agent::keyFromTag("head");
    int n = 0;
    for (const auto& e : events)
      if (e.type == attn::Event::Type::JobState && e.key == key && e.status == (uint8_t)st)
        n++;
    return n;
  }
  std::string lastText() const { return delivered.empty() ? "" : delivered.back().second; }
  bool anyDelivered(const char* needle) const {
    for (auto& p : delivered)
      if (p.second.find(needle) != std::string::npos) return true;
    return false;
  }
};

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- loop gate reads the TURN-ENTRY heap, not recall's transient trough -----
// On the device a turn dips ~15 KB below its entry heap during recall+compose and
// recovers a moment later. The loop gate used to re-sample that trough and defer
// the tool loop on essentially every recall turn, contradicting the head-loop
// contract (round-0 gating is the caller's admission, already granted at entry).

static void test_loop_gate_uses_entry_heap_not_the_recall_dip() {
  Rig r;
  // runTurn captures its entry heap FIRST; the gate uses that. Entry clears the
  // gate (loopMinHeap 28000 + 2000), every later read is in the trough below it.
  // The loop MUST still be advertised + wired. (handleMessage would consume one
  // read for its own entry floor, so drive runTurn directly to isolate the gate.)
  r.plat.heapScript = {41000, 25000, 25000, 25000, 25000};
  r.eng->runTurn("inputs", "1001", "user text");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE_MESSAGE(r.attempts[0].hadTools,
                           "loop deferred by the post-recall heap dip, not entry heap");
}

static void test_loop_defers_when_entry_heap_is_genuinely_low() {
  Rig r;
  // Entry itself is under the gate: a real low-memory turn still falls back to
  // single-shot - the fix must not defeat the genuine guard.
  r.plat.heapScript = {25000, 25000, 25000};
  r.eng->runTurn("inputs", "1001", "user text");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_FALSE_MESSAGE(r.attempts[0].hadTools,
                            "a genuinely low-heap turn must fall back to single-shot");
}

// ---- (1) happy turn ---------------------------------------------------------

static void test_happy_turn_delivers_and_accounts() {
  Rig r;
  r.eng->handleMessage("hi there", "Roy", "1001");
  // One provider attempt on the priority-top host, tool loop advertised.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts[0].host.c_str());
  TEST_ASSERT_TRUE(r.attempts[0].hadTools);
  TEST_ASSERT_EQUAL_STRING("", r.attempts[0].convIn.c_str());   // no stored conv
  // The per-turn input block carries channel + user text + dynamic context.
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[USER]\nhi there") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[ACTIVE SESSIONS]\nNo active jobs.") !=
                   std::string::npos);
  // The spawn capacity is surfaced honestly (W5): per-turn headroom + the "not a
  // hard limit, runs over waves" framing, so the model neither under- nor
  // over-spawns. No sub-agents live/queued here => can start up to kAgentMaxJobs.
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
                       "[SPAWN CAPACITY] 0 running, 0 queued. You can start up to 6") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("NOT a hard total limit") != std::string::npos);
  // W15: the ambient skills index rides every turn - the model must SEE the
  // playbooks (id + desc) to have a thread to pull with skill.get.
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[SKILLS]") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("- deliver-pdf: Build a PDF") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
                       "[CHANNEL] This message arrived via Telegram") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("## PROVIDERS & CONNECTORS") !=
                   std::string::npos);
  // Reply delivered; conv stored in the per-chat map (B2); usage recorded; counter++.
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  TEST_ASSERT_EQUAL_STRING("c1", agent::convMapGet(r.cfg.convId, "1001", "anthropic").c_str());
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.cfg.recorded[0].host.c_str());
  TEST_ASSERT_EQUAL(100, (int)r.cfg.recorded[0].in);
  TEST_ASSERT_EQUAL(20, (int)r.cfg.recorded[0].out);
  TEST_ASSERT_EQUAL(1, (int)r.eng->turnCount());
  TEST_ASSERT_EQUAL(120, (int)r.eng->sessionUsage().total());
  TEST_ASSERT_EQUAL(120, (int)r.eng->lastTurnUsage().total());
  // Episodic capture of the owner message with the sanitized sender tag.
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());
  TEST_ASSERT_EQUAL_STRING("from:Roy", r.captured[0].fromTag.c_str());
  // TurnGuard: head arc Running at start, Offline (freed) at end; turnstart cue.
  TEST_ASSERT_EQUAL(1, r.headEventsWith(Status::Running));
  TEST_ASSERT_EQUAL(1, r.headEventsWith(Status::Offline));
  TEST_ASSERT_EQUAL_STRING("turnstart", r.cues[0].c_str());
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
}

// ---- (2) recall gate --------------------------------------------------------

static void test_recall_injected_for_user_text_skipped_for_synthesis() {
  Rig r;
  r.eng->handleMessage("what do I drink?", "", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.recallQueries.size());
  TEST_ASSERT_EQUAL_STRING("what do I drink?", r.recallQueries[0].c_str());
  // The recalled memory landed in the composed instructions the provider saw.
  TEST_ASSERT_TRUE(r.attempts[0].instructions.find("owner likes tea") != std::string::npos);
  TEST_ASSERT_TRUE(r.eng->lastInstructions().find("owner likes tea") != std::string::npos);

  // A synthesis turn (empty userText) never recalls.
  r.jobs->addFreshResult("job0000", "modelX", "sub result");
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());   // the synthesis turn ran
  TEST_ASSERT_EQUAL(1, (int)r.recallQueries.size());   // ...without recall

  // Heap below the recall floor skips recall but still runs the turn.
  r.plat.heap = 27999;   // < recallMinHeap (28000) but engine floor only gates recall here
  r.eng->runTurn("inputs", "1001", "user text");
  TEST_ASSERT_EQUAL(1, (int)r.recallQueries.size());
}

// ---- (3) same-host fresh-conv retry -----------------------------------------

static void test_same_host_fresh_conv_retry() {
  Rig r;
  r.cfg.convId = "1001=anthropic|abc;";   // stored per-CHAT conversation (B2 map)
  r.scripts["anthropic"] = {{false, "", "boom", 0, "", 0, 0}, Rig::Script{}};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("abc", r.attempts[0].convIn.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.attempts[1].convIn.c_str());   // fresh conv on retry
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: turn err (boom) -> fresh conv retry"));
  // The 400 ms pause between the attempts rode the platform delay seam.
  TEST_ASSERT_EQUAL(1, (int)r.plat.delays.size());
  TEST_ASSERT_EQUAL(400, (int)r.plat.delays[0]);
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  TEST_ASSERT_EQUAL_STRING("c1", agent::convMapGet(r.cfg.convId, "1001", "anthropic").c_str());
}

// ---- (4) priority failover with the owner notice ----------------------------

static void test_failover_walks_priority_with_owner_notice() {
  Rig r;
  r.scripts["anthropic"] = {{false, "", "down", 0, "", 0, 0}};   // repeats: retry fails too
  r.eng->runTurn("inputs", "1001", "");
  // anthropic (initial + fresh retry) then the walk lands on openai.
  TEST_ASSERT_EQUAL(3, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts[1].host.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[2].host.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.attempts[2].convIn.c_str());   // fresh conv on the new host
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: anthropic down -> failover to openai"));
  TEST_ASSERT_TRUE(r.anyDelivered(
      "\xE2\x9A\xA0\xEF\xB8\x8F anthropic is unavailable - switching to openai "
      "(your recent messages carry over; the provider-side thread restarts)."));
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  TEST_ASSERT_EQUAL_STRING("c1", agent::convMapGet(r.cfg.convId, "1001", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("openai", r.cfg.recorded[0].host.c_str());
}

// ---- (5) NO retry after a tool dispatched -----------------------------------

static void test_no_retry_after_tool_dispatched() {
  Rig r;
  r.scripts["anthropic"] = {{false, "", "mid-loop death", 1 /*dispatch one tool*/, "", 0, 0}};
  const bool ok = r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_FALSE(ok);
  // Exactly ONE provider attempt: no same-host retry, no failover walk.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(1, (int)r.toolResults.size());
  TEST_ASSERT_EQUAL_STRING("tool-ok", r.toolResults[0].output.c_str());
  TEST_ASSERT_FALSE(r.toolResults[0].isError);
  TEST_ASSERT_EQUAL(1, (int)r.mcpRequests.size());
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: loop ran 1 tool(s) before failing - skipping retry/failover"));
  // Honest failure copy: the reply names the real cause (here the scripted
  // "mid-loop death" adapter token) and says nothing is still running - the old
  // fixed "trouble reaching the orchestrator" line was flagged dishonest by the
  // QA judge when tools HAD run.
  TEST_ASSERT_EQUAL_STRING(
      "That didn't finish - mid-loop death. Nothing is still running; ask again to retry.",
      r.lastText().c_str());
  // The head arc is still freed on the error path (TurnGuard dtor).
  TEST_ASSERT_EQUAL(1, r.headEventsWith(Status::Offline));
}

// ---- (6) budget failover + all-exhausted refusal ----------------------------

static void test_budget_failover_and_exhausted_refusal() {
  Rig r;
  r.cfg.overBudgetHosts = {"anthropic"};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: anthropic over token budget -> failover openai"));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[0].host.c_str());

  Rig r2;
  r2.cfg.overBudgetHosts = {"anthropic", "openai", "mistral"};
  const bool ok = r2.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(0, (int)r2.attempts.size());
  TEST_ASSERT_EQUAL_STRING(
      "\xE2\x9A\xA0\xEF\xB8\x8F anthropic has hit its monthly token budget. "
      "Raise the limit in Usage & Budget or wait for the reset.",
      r2.lastText().c_str());
}

// ---- (7) salvage: never deliver raw JSON ------------------------------------

static void test_salvage_never_delivers_raw_json() {
  Rig r;
  r.scripts["anthropic"] = {{true, "{\"reply\":\"hi\",\"memory\":\"\",\"junk\":42}", "", 0}};
  const bool ok = r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_FALSE(ok);   // parse failed (missing `ask`)
  TEST_ASSERT_EQUAL_STRING("hi", r.lastText().c_str());   // salvaged text, NOT the JSON
  TEST_ASSERT_FALSE(r.anyDelivered("{"));
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: turn parse fail"));

  Rig r2;
  r2.scripts["anthropic"] = {{true, "{\"nothing\":true}", "", 0}};
  r2.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL_STRING(
      "I hit a formatting error composing that reply - please resend.",
      r2.lastText().c_str());
}

// ---- (8) scheduled turn: rails + FireOutcome --------------------------------

static void test_scheduled_turn_rails_and_fire_outcome() {
  Rig r;
  r.scripts["anthropic"] = {
      {true,
       "{\"reply\":\"done\",\"memory\":\"\",\"ask\":\"\",\"device\":[{\"reboot\":true}]}",
       "", 0}};
  orch::FireOutcome o = r.eng->injectScheduledTurn("", "check the weather", "morning");
  TEST_ASSERT_TRUE(o.ok);
  // Empty chatId targets the first allow-listed chat.
  TEST_ASSERT_EQUAL_STRING("1001", r.delivered[0].first.c_str());
  // The scheduled flag was live DURING the provider call and the reboot was
  // refused inside applyTurn (the ApplyState.scheduledTurn rail).
  TEST_ASSERT_TRUE(r.attempts[0].scheduledDuring);
  TEST_ASSERT_TRUE(LogCapture::contains("reboot-refused(scheduled)"));
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());
  TEST_ASSERT_FALSE(r.eng->inScheduledTurn());   // cleared after
  // FireOutcome shape: real tokens + the delivered reply as detail.
  TEST_ASSERT_EQUAL(120, (int)o.tokens.total());
  TEST_ASSERT_EQUAL_STRING("done", o.detail.c_str());
  // The [SCHEDULED LOOP] preamble + prompt-driven recall.
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
      "[SCHEDULED LOOP]\nYour recurring task \"morning\" is firing on its schedule.") !=
      std::string::npos);
  TEST_ASSERT_EQUAL(1, (int)r.recallQueries.size());
  TEST_ASSERT_EQUAL_STRING("check the weather", r.recallQueries[0].c_str());

  // Low heap: deferred without a provider attempt.
  Rig r2;
  r2.plat.heap = 27999;
  orch::FireOutcome o2 = r2.eng->injectScheduledTurn("1001", "p", "n");
  TEST_ASSERT_FALSE(o2.ok);
  TEST_ASSERT_EQUAL_STRING("deferred: low heap", o2.detail.c_str());
  TEST_ASSERT_EQUAL(0, (int)r2.attempts.size());
}

// W20: a Once wakeup gets the honest [WAKEUP] preamble - "recurring task" would
// be a lie about a one-shot the model armed for itself, and the framing must
// hand the model back its own note plus the it-has-retired fact.
static void test_once_wakeup_gets_wakeup_preamble_not_recurring() {
  Rig r;
  r.scripts["anthropic"] = {
      {true, "{\"reply\":\"followed up\",\"memory\":\"\",\"ask\":\"\"}", "", 0}};
  orch::FireOutcome o = r.eng->injectScheduledTurn(
      "", "resume the deploy check", "wakeup", "loop0009",
      /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_TRUE(o.ok);
  const std::string& in = r.attempts[0].inputs;
  TEST_ASSERT_TRUE(in.find("[WAKEUP]\nYour one-time wakeup \"wakeup\" just fired") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(in.find("it has now retired") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("Your note when you set it:\n\nresume the deploy check") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(in.find("[SCHEDULED LOOP]") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("recurring task") == std::string::npos);
  // Same rails as any scheduled turn: recall runs for the note, spend is tagged.
  TEST_ASSERT_EQUAL_STRING("resume the deploy check", r.recallQueries[0].c_str());
}

// W22: an OWNER-set one-shot (/remind) fires with [REMINDER] framing, NOT the
// self-wakeup text - "you scheduled this for yourself" is false for a reminder
// the owner set. Same Once mechanism; only createdBy/ownerReminder differ.
static void test_owner_reminder_gets_reminder_framing_not_wakeup() {
  Rig r;
  r.scripts["anthropic"] = {
      {true, "{\"reply\":\"Reminder: take the cake out\",\"memory\":\"\",\"ask\":\"\"}", "", 0}};
  orch::FireOutcome o = r.eng->injectScheduledTurn(
      "", "take the cake out", "reminder", "loop0011",
      /*quietOk=*/false, /*once=*/true, /*ownerReminder=*/true);
  TEST_ASSERT_TRUE(o.ok);
  const std::string& in = r.attempts[0].inputs;
  TEST_ASSERT_TRUE(in.find("[REMINDER]") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("The owner set a one-time reminder") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("take the cake out") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("[WAKEUP]") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("for yourself") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("[SCHEDULED LOOP]") == std::string::npos);
}

// ---- (9) synthesis consolidation + raw fallback -----------------------------

static void test_synthesis_consolidation_and_raw_fallback() {
  Rig r;
  r.jobs->addFreshResult("job0000", "modelX", "sub agent findings");
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
      "[FRESH RESULTS] (sub-agents that just finished)") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("sub agent findings") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
      "\n[SYSTEM]\nOne or more of your sub-agents just finished") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].scheduledDuring);   // unattended guard armed
  TEST_ASSERT_FALSE(r.eng->inScheduledTurn());       // restored after
  TEST_ASSERT_EQUAL(1, r.journalGcs);
  TEST_ASSERT_FALSE(r.jobs->hasFreshResults());
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: auto-synthesis turn (batch complete)"));

  // Fallback: the synthesis turn fails on every keyed host -> raw delivery.
  Rig r2;
  r2.cfg.keyed = {"anthropic"};   // no failover candidates
  r2.scripts["anthropic"] = {{false, "", "outage", 0, "", 0, 0}};
  r2.jobs->addFreshResult("job0001", "modelY", "precious result");
  r2.eng->maybeConsolidate("1001");
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: synthesis turn failed - delivering raw results (never lost)"));
  const std::string& raw = r2.delivered.back().second;
  TEST_ASSERT_TRUE(raw.rfind("[FRESH RESULTS] (sub-agents that just finished)\n", 0) == 0);
  TEST_ASSERT_TRUE(raw.find("precious result") != std::string::npos);

  // Heap gate: deferral leaves the fresh results parked (the 60 s fallback's case).
  Rig r3;
  r3.jobs->addFreshResult("job0002", "modelZ", "kept");
  r3.plat.heap = 29999;   // < autoTurnMinHeap (30000)
  r3.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(0, (int)r3.attempts.size());
  TEST_ASSERT_TRUE(r3.jobs->hasFreshResults());
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: defer auto-synthesis (heap 29999 < 30000)"));
}

// ---- (10) turn-debug hook on success AND failure ----------------------------

static void test_turn_debug_hook_success_and_failure() {
  Rig r;
  r.cfg.convId = "1001=anthropic|conv9;";
  r.eng->runTurn("the inputs", "1001", "");
  TEST_ASSERT_EQUAL(1, (int)r.debug.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.debug[0].host.c_str());
  TEST_ASSERT_TRUE(r.debug[0].convContinued);   // entered with a stored conversation
  TEST_ASSERT_TRUE(r.debug[0].ok);
  TEST_ASSERT_EQUAL_STRING(kGoodTurn, r.debug[0].rawOut.c_str());
  TEST_ASSERT_EQUAL_STRING("the inputs", r.debug[0].inputs.c_str());

  Rig r2;
  r2.cfg.keyed = {"anthropic"};
  r2.scripts["anthropic"] = {{false, "", "down", 0, "", 0, 0}};
  r2.eng->runTurn("in2", "1001", "");
  TEST_ASSERT_EQUAL(1, (int)r2.debug.size());
  TEST_ASSERT_FALSE(r2.debug[0].convContinued);
  TEST_ASSERT_FALSE(r2.debug[0].ok);   // fired on failure too
}

// ---- (11) stuck-turn reaper -------------------------------------------------

static void test_stuck_turn_reaper() {
  Rig r;
  TEST_ASSERT_FALSE(r.eng->reapStuckTurn(r.plat.ms));   // nothing in flight
  bool reapedEarly = true, reapedLate = false, inFlightDuring = false;
  r.onProviderCall = [&] {
    inFlightDuring = r.eng->turnInFlight();
    // Inside the deadline: the reaper must NOT fire (legitimately long turn).
    reapedEarly = r.eng->reapStuckTurn(r.plat.ms + 1000);
    // Past loop deadline + 120 s margin: presumed dead -> reap.
    r.plat.ms += (uint32_t)r.cfg.deadlineS * 1000u + 120000u + 1u;
    reapedLate = r.eng->reapStuckTurn(r.plat.ms);
  };
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_TRUE(inFlightDuring);
  TEST_ASSERT_FALSE(reapedEarly);
  TEST_ASSERT_TRUE(reapedLate);
  // The reap freed the head arc (one Offline) and the TurnGuard dtor freed it
  // again at turn end - both Offline edges present, no strand.
  TEST_ASSERT_EQUAL(2, r.headEventsWith(Status::Offline));
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
}

// ---- (12) spend attribution tags -------------------------------------------

static void test_attribution_tags_per_turn_source() {
  Rig r;
  // Owner turn -> "turn".
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("turn", r.cfg.recorded[0].tag.c_str());
  // Synthesis turn -> "synthesis".
  r.jobs->addFreshResult("job0000", "modelX", "sub result");
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(2, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("synthesis", r.cfg.recorded[1].tag.c_str());
  // Scheduled loop turn -> "loop:<id>".
  r.eng->injectScheduledTurn("1001", "check weather", "morning", "L3");
  TEST_ASSERT_EQUAL(3, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("loop:L3", r.cfg.recorded[2].tag.c_str());
  // Back to the owner default after the scheduled turn.
  r.eng->handleMessage("hi again", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("turn", r.cfg.recorded[3].tag.c_str());
}

// ---- (12b) runFold - the v3.6.0 context fold ---------------------------------

static void test_runfold_returns_summary_no_side_effects() {
  Rig r;
  r.cfg.convId = "1001=anthropic|abc;";   // an existing chain the fold must not touch
  Rig::Script s;
  s.outJson = "{\"reply\":\"1. Owner intent: testing\",\"ask\":\"\",\"memory\":\"IGNORED\","
              "\"device\":[{\"type\":\"reboot\"}],\"mem_write\":[],\"mem_query\":[],"
              "\"session_ops\":[]}";
  s.convOut = "SHOULD-NEVER-PERSIST";
  r.scripts["anthropic"] = {s};
  std::string sum;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "prev summary", "- user: hi\n", sum) ==
                   TurnEngine::FoldResult::Ok);
  TEST_ASSERT_EQUAL_STRING("1. Owner intent: testing", sum.c_str());
  // The fold is NOT a turn: nothing delivered, nothing staged (the reboot device
  // action in the reply is inert), no assistant capture, conv map untouched.
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());
  TEST_ASSERT_EQUAL(0, (int)r.captured.size());
  TEST_ASSERT_EQUAL_STRING("1001=anthropic|abc;", r.cfg.convId.c_str());
  // Single-shot: no tools handed to the provider; fresh convId in.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_FALSE(r.attempts[0].hadTools);
  TEST_ASSERT_EQUAL_STRING("", r.attempts[0].convIn.c_str());
  // The fold prompt + anchored inputs actually rode the wire.
  TEST_ASSERT_TRUE(r.attempts[0].instructions.find("anchored summary") != std::string::npos ||
                   r.attempts[0].instructions.find("compacting") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[PREVIOUS SUMMARY]") != std::string::npos);
  // Spend ledgered as "compact", and lastTurnUsage_ untouched (loop metering).
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("compact", r.cfg.recorded[0].tag.c_str());
}

static void test_runfold_gates_and_failure() {
  Rig r;
  std::string sum;
  using FR = TurnEngine::FoldResult;
  // Heap gate: below autoTurnMinHeap -> DEFERRED (retry later, NO breaker burn -
  // the L15 degraded-fold row live-caught a deferral counted as a failure), and
  // no provider call.
  r.plat.heap = 20000;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  r.plat.heap = 100000;
  // Budget gate -> Deferred only when EVERY candidate host is capped (the
  // ladder means one capped provider no longer blocks the fold).
  for (const char* h : {"anthropic", "openai", "mistral"}) r.cfg.overBudgetHosts.insert(h);
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  r.cfg.overBudgetHosts.clear();
  // EVERY host failing -> FAILED, and the ladder provably walked all three
  // (a real attempt each; the breaker counts the episode as one failure).
  Rig::Script bad; bad.ok = false; bad.err = "network";
  r.scripts["anthropic"] = {bad};
  r.scripts["openai"] = {bad};
  r.scripts["mistral"] = {bad};
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Failed);
  TEST_ASSERT_EQUAL(3, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  r.attempts.clear();
  r.scripts.clear(); r.scriptIdx.clear();
  // Empty reply -> no summary -> Failed.
  Rig::Script empty;
  empty.outJson = "{\"reply\":\"\",\"ask\":\"\",\"memory\":\"\",\"device\":[],"
                  "\"mem_write\":[],\"mem_query\":[],\"session_ops\":[]}";
  r.scripts["anthropic"] = {empty};
  r.scriptIdx["anthropic"] = 0;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Failed);
  TEST_ASSERT_EQUAL_STRING("", sum.c_str());
}

// canFoldNow is the PRE-notice gate (prism v3.6.0 HIGH): the device must be able
// to ask "could a fold run?" before paying an owner-visible notice + a 64 KB
// digest, because runFold's own gates fire after both. Kill this predicate (make
// it always true) and the notice-loop regression returns.
static void test_can_fold_now_matches_the_gates() {
  Rig r;
  TEST_ASSERT_TRUE(r.eng->canFoldNow());
  r.plat.heap = 20000;                       // below autoTurnMinHeap
  TEST_ASSERT_FALSE(r.eng->canFoldNow());
  r.plat.heap = 100000;
  // One capped host no longer blocks the fold (the ladder has alternates)...
  r.cfg.overBudgetHosts.insert("anthropic");
  TEST_ASSERT_TRUE(r.eng->canFoldNow());
  // ...but ALL candidates capped does.
  r.cfg.overBudgetHosts.insert("openai");
  r.cfg.overBudgetHosts.insert("mistral");
  TEST_ASSERT_FALSE(r.eng->canFoldNow());
  r.cfg.overBudgetHosts.clear();
  TEST_ASSERT_TRUE(r.eng->canFoldNow());
  // Same for keys: the fold runs while ANY host is keyed, defers with none.
  r.cfg.keyed.erase("anthropic");
  TEST_ASSERT_TRUE(r.eng->canFoldNow());
  r.cfg.keyed.clear();
  TEST_ASSERT_FALSE(r.eng->canFoldNow());
}

// Field 2026-08-11: the fold used to pin ONE host - the head failed over
// openai->anthropic while the fold burned its breaker on openai and spammed the
// owner. A fold is replay-safe (no applyTurn), so it walks the candidate ladder.
static void test_runfold_fails_over_to_the_next_keyed_host() {
  Rig r;                                        // priority anthropic,openai,mistral
  std::string sum;
  using FR = TurnEngine::FoldResult;
  Rig::Script bad; bad.ok = false; bad.err = "no response";
  r.scripts["anthropic"] = {bad};               // priority head is down...
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "digest", sum) == FR::Ok);
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size()); // ...one attempt there,
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[1].host.c_str());   // then the next
  TEST_ASSERT_TRUE(sum.size() > 0);             // the fold SUCCEEDED (no spam episode)
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());// and the engine stayed silent
  // Ladder order is orchHost-first when set (the explicit host outranks priority).
  Rig r2;
  r2.cfg.orchHost = "mistral";
  const auto cands = r2.eng->foldHostCandidates();
  TEST_ASSERT_EQUAL(3, (int)cands.size());
  TEST_ASSERT_EQUAL_STRING("mistral", cands[0].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic", cands[1].c_str());
}

static void test_clear_chat_conv_is_per_chat() {
  Rig r;
  r.cfg.convId = "1001=anthropic|abc;web=openai|resp_9;";
  r.eng->clearChatConv("1001");
  TEST_ASSERT_EQUAL_STRING("", agent::convMapGet(r.cfg.convId, "1001", "anthropic").c_str());
  TEST_ASSERT_EQUAL_STRING("resp_9", agent::convMapGet(r.cfg.convId, "web", "openai").c_str());
}

// ---- (12b) fabric gate vs the custom head (2026-08-12 regression) -----------
// With the tool loop + mid-turn failover ON (device defaults), a custom head
// used to enter the fabric, whose step table only knows the cloud providers -
// the turn failed "unknown host custom" with ZERO HTTP, and both recovery
// blocks were fabric-guarded. The engine now gates fabricOn on fabricSupports.
static void test_custom_head_bypasses_fabric_and_runs() {
  Rig r;
  r.cfg.midFail = true;                  // fabric armed (device default)
  r.cfg.keyed.insert("custom");
  r.cfg.orchHost = "custom";
  r.scripts["custom"] = {{true, kGoodTurn, "", 0, "chat", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
  TEST_ASSERT_EQUAL(0, (int)r.fabricCalls.size());   // fabric never entered
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());      // single-shot on custom
  TEST_ASSERT_EQUAL_STRING("custom", r.attempts[0].host.c_str());
}

// custom inside providerPriority must be FILTERED from the fabric hostList -
// before the fix a mid-round switch onto it hit a null step and ended the
// failover ladder early (silently weaker failover, not a failed turn).
static void test_fabric_hostlist_filters_unsupported_hosts() {
  Rig r;
  r.cfg.midFail = true;
  r.cfg.keyed.insert("custom");
  r.cfg.orchHost = "anthropic";
  r.cfg.priority = "anthropic,custom,openai";
  TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
  TEST_ASSERT_EQUAL(1, (int)r.fabricCalls.size());
  const auto& hl = r.fabricCalls[0];
  TEST_ASSERT_EQUAL(2, (int)hl.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", hl[0].c_str());
  TEST_ASSERT_EQUAL_STRING("openai", hl[1].c_str());   // custom skipped, ladder intact
}

// ---- (12c) router-key head fallback (CUM-242) -------------------------------
// The flagship "one key, one balance" path: a device whose ONLY verified key is
// the Cumulo router key must run the whole assistant on it. With no BYOK head
// keyed, the engine resolves the head to routerFallbackHost() ("cumulo") instead
// of collapsing to the bare priority head (openai) with an empty key -> 401.
static void test_router_key_is_the_head_when_no_byok_key() {
  Rig r;
  r.cfg.keyed.clear();                    // no openai/anthropic/mistral key
  r.cfg.routerKeyed = true;               // ...but a provider IS configured (anyKeyed)
  r.cfg.routerHost = "cumulo";            // the verified Cumulo key
  r.scripts["cumulo"] = {{true, kGoodTurn, "", 0, "c1", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("cumulo", r.attempts[0].host.c_str());   // ran on the router
}

// A keyed BYOK head still WINS over the router key (CUM-201: BYOK-override first).
// The router key is the SOURCE only when nothing the owner brought is keyed.
static void test_byok_head_outranks_router_fallback() {
  Rig r;
  r.cfg.keyed = {"openai"};               // the owner brought an OpenAI key
  r.cfg.priority = "anthropic,openai,mistral";
  r.cfg.routerHost = "cumulo";            // Cumulo key also present
  r.scripts["openai"] = {{true, kGoodTurn, "", 0, "c1", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[0].host.c_str());   // BYOK wins
}

// CUM-288: a Cumulo-only instance (only the router key, no BYOK head) chatted
// fine but NEVER folded - foldHostCandidates() consulted orchHost + the keyed
// BYOK priority walk, but never routerFallbackHost(), so the candidate list was
// empty and canFoldNow()/runFold could not run. Context grew unbounded until
// turns failed. The fold ladder must fall back to the router head exactly as the
// turn head does.
static void test_cumulo_only_instance_folds_via_router_host() {
  Rig r;
  r.cfg.keyed.clear();                    // no BYOK head keyed
  r.cfg.routerKeyed = true;               // ...but the Cumulo router key IS configured
  r.cfg.routerHost = "cumulo";
  // The candidate ladder is exactly the router head (the failover list runFold walks).
  const auto cands = r.eng->foldHostCandidates();
  TEST_ASSERT_EQUAL(1, (int)cands.size());
  TEST_ASSERT_EQUAL_STRING("cumulo", cands[0].c_str());
  // The PRE-notice gate opens (heap ok, a candidate exists) - previously false.
  TEST_ASSERT_TRUE(r.eng->canFoldNow());
  // And the fold actually dispatches to the router head and succeeds.
  r.scripts["cumulo"] = {{true,
      "{\"reply\":\"1. Owner intent: testing\",\"ask\":\"\",\"memory\":\"\","
      "\"device\":[],\"mem_write\":[],\"mem_query\":[],\"session_ops\":[]}",
      "", 0, "c1", 50, 10}};
  std::string sum;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "prev", "- user: hi\n", sum) ==
                   TurnEngine::FoldResult::Ok);
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("cumulo", r.attempts[0].host.c_str());   // ran on the router
  TEST_ASSERT_EQUAL_STRING("1. Owner intent: testing", sum.c_str());
  // Ledgered as compact spend against the router host.
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("cumulo", r.cfg.recorded[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("compact", r.cfg.recorded[0].tag.c_str());
}

// CUM-288 counter-test (pin the class boundary): the router head is a LAST resort
// consulted ONLY when the BYOK ladder is empty, mirroring runTurn's head pick (a
// keyed BYOK head still wins). BYOK-only and mixed (BYOK + router) configs must
// fold on their BYOK heads exactly as before - the router must NOT sneak into the
// ladder while a BYOK head is keyed.
static void test_fold_router_fallback_only_when_no_byok_keyed() {
  // BYOK-only (no router configured): the priority walk, unchanged.
  {
    Rig r;                                 // keyed {openai,anthropic,mistral}, no routerHost
    const auto cands = r.eng->foldHostCandidates();
    TEST_ASSERT_EQUAL(3, (int)cands.size());
    TEST_ASSERT_EQUAL_STRING("anthropic", cands[0].c_str());
    TEST_ASSERT_EQUAL_STRING("openai", cands[1].c_str());
    TEST_ASSERT_EQUAL_STRING("mistral", cands[2].c_str());
  }
  // Mixed (a BYOK head keyed AND the router key present): the router is NOT
  // appended - the keyed BYOK heads own the ladder, byte-identical to before.
  {
    Rig r;
    r.cfg.keyed = {"openai"};              // one BYOK head keyed
    r.cfg.routerKeyed = true;             // ...and the Cumulo router key too
    r.cfg.routerHost = "cumulo";
    const auto cands = r.eng->foldHostCandidates();
    TEST_ASSERT_EQUAL(1, (int)cands.size());
    TEST_ASSERT_EQUAL_STRING("openai", cands[0].c_str());
  }
}

// ---- (13) lifecycle hooks (HookRecorder) ------------------------------------

static void test_hooks_fire_on_happy_turn_with_tools() {
  Rig r;
  r.specs.push_back(orch::ToolRegistry::Spec{"memory.search", "search", "{}"});
  r.scripts["anthropic"] = {{true, kGoodTurn, "", /*dispatchTools=*/2, "c1", 100, 20}};
  r.eng->handleMessage("hi", "Roy", "1001");
  // onTurnStart: once, Owner source, the routing chat.
  TEST_ASSERT_EQUAL(1, (int)r.hookStarts.size());
  TEST_ASSERT_EQUAL((int)agent::TurnSource::Owner, (int)r.hookStarts[0].source);
  TEST_ASSERT_EQUAL_STRING("1001", r.hookStarts[0].chatId.c_str());
  // onToolCall/onToolResult: once per executed tool.
  TEST_ASSERT_EQUAL(2, (int)r.hookToolCalls.size());
  TEST_ASSERT_EQUAL_STRING("memory_search", r.hookToolCalls[0].name.c_str());
  TEST_ASSERT_EQUAL(2, (int)r.hookToolResults.size());
  TEST_ASSERT_FALSE(r.hookToolResults[0].isError);
  TEST_ASSERT_EQUAL_STRING("tool-ok", r.hookToolResults[0].output.c_str());
  // onTurnEnd: once, ok, with the real usage + tool count + reply size.
  TEST_ASSERT_EQUAL(1, (int)r.hookEnds.size());
  TEST_ASSERT_TRUE(r.hookEnds[0].ok);
  TEST_ASSERT_EQUAL_STRING("anthropic", r.hookEnds[0].host.c_str());
  TEST_ASSERT_EQUAL(100, (int)r.hookEnds[0].usage.promptTokens);
  TEST_ASSERT_EQUAL(20, (int)r.hookEnds[0].usage.completionTokens);
  TEST_ASSERT_EQUAL(2, r.hookEnds[0].rounds);
  TEST_ASSERT_EQUAL((int)strlen("hello there"), (int)r.hookEnds[0].replyBytes);
}

static void test_hooks_turn_end_fires_on_failure_and_sources_track() {
  Rig r;
  // All hosts fail -> onTurnEnd(ok=false) still fires exactly once per turn.
  r.scripts["anthropic"] = {{false, "", "down", 0, "", 0, 0}};
  r.scripts["openai"]    = {{false, "", "down", 0, "", 0, 0}};
  r.scripts["mistral"]   = {{false, "", "down", 0, "", 0, 0}};
  r.eng->handleMessage("hi", "", "serial");
  TEST_ASSERT_EQUAL(1, (int)r.hookEnds.size());
  TEST_ASSERT_FALSE(r.hookEnds[0].ok);
  // Serial channel -> Serial source.
  TEST_ASSERT_EQUAL((int)agent::TurnSource::Serial, (int)r.hookStarts[0].source);

  // Scheduled + synthesis sources.
  r.scripts.clear();
  r.scriptIdx.clear();
  r.eng->injectScheduledTurn("1001", "p", "n", "L1");
  TEST_ASSERT_EQUAL((int)agent::TurnSource::Loop, (int)r.hookStarts.back().source);
  r.jobs->addFreshResult("job0000", "m", "res");
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL((int)agent::TurnSource::Synthesis, (int)r.hookStarts.back().source);
  // Every started turn also ended.
  TEST_ASSERT_EQUAL((int)r.hookStarts.size(), (int)r.hookEnds.size());
}


// ---- CUM-211: honest local reply when no provider is routable ---------------
// A chat turn with no keyed provider must NEVER be silent (owner: "tried chatting
// and it's not responding ... regardless of the channel I would expect an
// automated response"). handleMessage answers locally, names the fix, and never
// arms a paid turn or the head ring arc.
static void test_no_provider_gives_honest_local_reply_not_silence() {
  Rig r;
  r.cfg.keyed.clear();                 // no provider has a key (custom not keyed either)
  r.eng->handleMessage("hello?", "Roy", "1001");
  // Exactly one delivery: the honest local message; ZERO provider attempts.
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING(
      "No AI provider is set up yet. Add a provider key under Providers & models in "
      "the web app, then send your message again.",
      r.lastText().c_str());
  // No ring arc armed (no head Running/Offline churn), turn counter untouched, and
  // it did not read as a transient "resend in a few seconds" heap deferral.
  TEST_ASSERT_EQUAL(0, r.headEventsWith(Status::Running));
  TEST_ASSERT_EQUAL(0, (int)r.eng->turnCount());
  TEST_ASSERT_FALSE(r.anyDelivered("working memory"));
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: turn refused - no provider configured"));
  TEST_ASSERT_FALSE(r.eng->anyProviderConfigured());
}

// CUM-211 (bench-caught on nimbus-6): a device whose ONLY provider is a router
// (Cumulo / Z.ai) - configured + verified, but not a single-shot head host - must
// NOT be told "no provider is set up". anyKeyed reports device-truth, so the turn
// proceeds normally instead of being wrongly short-circuited.
static void test_router_only_provider_is_not_told_no_provider() {
  Rig r;
  r.cfg.keyed.clear();          // no head host keyed (openai/anthropic/mistral/custom)
  r.cfg.routerKeyed = true;     // ...but a router provider (e.g. Cumulo) IS configured
  TEST_ASSERT_TRUE(r.eng->anyProviderConfigured());
  r.eng->handleMessage("hello?", "Roy", "1001");
  // The honest "no provider" reply must NOT fire - the device has a provider.
  TEST_ASSERT_FALSE(r.anyDelivered("No AI provider is set up"));
  // The turn ran the normal path instead of being short-circuited.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
}

// The predicate is budget-agnostic and counts a configured keyless custom head:
// an over-budget provider is still "configured" (own reply), and a LAN endpoint
// needs no key - neither must read as "no provider".
static void test_any_provider_configured_predicate() {
  Rig r;
  TEST_ASSERT_TRUE(r.eng->anyProviderConfigured());        // default: three keyed hosts
  r.cfg.overBudgetHosts = {"anthropic", "openai", "mistral"};
  TEST_ASSERT_TRUE(r.eng->anyProviderConfigured());        // over budget != unconfigured
  r.cfg.overBudgetHosts.clear();
  r.cfg.keyed.clear();
  TEST_ASSERT_FALSE(r.eng->anyProviderConfigured());       // truly nothing keyed
  r.cfg.keyed.insert("custom");                            // configured LAN/custom head
  TEST_ASSERT_TRUE(r.eng->anyProviderConfigured());
}

// ---- per-chat conversation map (Release B2) ---------------------------------
static void test_convmap_roundtrip_and_isolation() {
  using agent::convMapGet;
  using agent::convMapSet;
  std::string raw;
  raw = convMapSet(raw, "6098", "openai", "resp_abc");
  raw = convMapSet(raw, "web", "openai", "resp_web");
  TEST_ASSERT_EQUAL_STRING("resp_abc", convMapGet(raw, "6098", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("resp_web", convMapGet(raw, "web", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("", convMapGet(raw, "voice", "openai").c_str());  // isolated
  // host switch on ONE chat resets only that chat's thread
  TEST_ASSERT_EQUAL_STRING("", convMapGet(raw, "6098", "mistral").c_str());
  // update one chat, the other survives
  raw = convMapSet(raw, "6098", "openai", "resp_def");
  TEST_ASSERT_EQUAL_STRING("resp_def", convMapGet(raw, "6098", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("resp_web", convMapGet(raw, "web", "openai").c_str());
  // empty convId removes the entry
  raw = convMapSet(raw, "web", "openai", "");
  TEST_ASSERT_EQUAL_STRING("", convMapGet(raw, "web", "openai").c_str());
}

static void test_convmap_legacy_discarded_and_lru() {
  using agent::convMapGet;
  using agent::convMapSet;
  // legacy single-slot value: no '=' -> every chat reads fresh; first write discards it
  TEST_ASSERT_EQUAL_STRING("", convMapGet("openai|resp_old", "6098", "openai").c_str());
  std::string raw = convMapSet("openai|resp_old", "6098", "openai", "resp_new");
  TEST_ASSERT_EQUAL_STRING("resp_new", convMapGet(raw, "6098", "openai").c_str());
  TEST_ASSERT_TRUE(raw.find("resp_old") == std::string::npos);
  // LRU: 9 distinct chats -> the OLDEST is evicted, the rest survive
  raw.clear();
  for (int i = 0; i < 9; i++)
    raw = convMapSet(raw, "chat" + std::to_string(i), "openai", "c" + std::to_string(i));
  TEST_ASSERT_EQUAL_STRING("", convMapGet(raw, "chat0", "openai").c_str());   // evicted
  TEST_ASSERT_EQUAL_STRING("c1", convMapGet(raw, "chat1", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("c8", convMapGet(raw, "chat8", "openai").c_str());
  // delimiter injection in a convId is sanitized, map stays parseable
  raw = convMapSet(raw, "chat8", "openai", "evil;x=y|z");
  TEST_ASSERT_EQUAL_STRING("evil_x_y_z", convMapGet(raw, "chat8", "openai").c_str());
  TEST_ASSERT_EQUAL_STRING("c7", convMapGet(raw, "chat7", "openai").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_convmap_roundtrip_and_isolation);
  RUN_TEST(test_convmap_legacy_discarded_and_lru);
  RUN_TEST(test_no_provider_gives_honest_local_reply_not_silence);
  RUN_TEST(test_router_only_provider_is_not_told_no_provider);
  RUN_TEST(test_any_provider_configured_predicate);
  RUN_TEST(test_loop_gate_uses_entry_heap_not_the_recall_dip);
  RUN_TEST(test_loop_defers_when_entry_heap_is_genuinely_low);
  RUN_TEST(test_happy_turn_delivers_and_accounts);
  RUN_TEST(test_recall_injected_for_user_text_skipped_for_synthesis);
  RUN_TEST(test_same_host_fresh_conv_retry);
  RUN_TEST(test_failover_walks_priority_with_owner_notice);
  RUN_TEST(test_no_retry_after_tool_dispatched);
  RUN_TEST(test_budget_failover_and_exhausted_refusal);
  RUN_TEST(test_salvage_never_delivers_raw_json);
  RUN_TEST(test_scheduled_turn_rails_and_fire_outcome);
  RUN_TEST(test_once_wakeup_gets_wakeup_preamble_not_recurring);
  RUN_TEST(test_owner_reminder_gets_reminder_framing_not_wakeup);
  RUN_TEST(test_synthesis_consolidation_and_raw_fallback);
  RUN_TEST(test_turn_debug_hook_success_and_failure);
  RUN_TEST(test_stuck_turn_reaper);
  RUN_TEST(test_attribution_tags_per_turn_source);
  RUN_TEST(test_runfold_returns_summary_no_side_effects);
  RUN_TEST(test_runfold_gates_and_failure);
  RUN_TEST(test_can_fold_now_matches_the_gates);
  RUN_TEST(test_runfold_fails_over_to_the_next_keyed_host);
  RUN_TEST(test_custom_head_bypasses_fabric_and_runs);
  RUN_TEST(test_fabric_hostlist_filters_unsupported_hosts);
  RUN_TEST(test_router_key_is_the_head_when_no_byok_key);
  RUN_TEST(test_byok_head_outranks_router_fallback);
  RUN_TEST(test_cumulo_only_instance_folds_via_router_host);
  RUN_TEST(test_fold_router_fallback_only_when_no_byok_keyed);
  RUN_TEST(test_clear_chat_conv_is_per_chat);
  RUN_TEST(test_hooks_fire_on_happy_turn_with_tools);
  RUN_TEST(test_hooks_turn_end_fires_on_failure_and_sources_track);
  UNITY_END();
  return 0;
}
