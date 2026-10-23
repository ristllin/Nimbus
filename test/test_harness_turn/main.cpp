#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nimbus/harness/prompt_layout.h"   // the static/summary/per-turn split

#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../support/code_snippet.h"   // CUM-485: the previous reply's code block
#include "../support/fake_config.h"
#include "../support/fake_fabric.h"   // CUM-538: the opt-in sub-agent fabric
#include "../support/fake_platform.h"
#include "../support/mock_claim_turns.h"   // the LAN mock's claim-check turns
#include "../support/zai_replies.h"   // CUM-495: the captured Z.ai prose reply
#include "nimbus/harness/dream.h"      // CUM-485: buildConversationWindow
#include "nimbus/harness/claim_check.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"   // lane K's host park (hostParked / noteHostFailed)
#include "nimbus/harness/turn_contract.h"
#include "nimbus/orch/model_defaults.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/billing_refusal.h"   // the one-notice-per-episode latch
#include "nimbus/orch/spawn_claim.h"
#include "nimbus/harness/turn_anatomy.h"    // the /api/lastturn head
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/connectors_wire.h"   // CUM-536: connectorNotCalledNote
#include "nimbus/harness/rate_limit.h"   // CUM-460 the 429 reply table
#include "nimbus/harness/builtin_skills.h"   // CUM-523: the device's real built-in capsules
#include "nimbus/harness/skill_md.h"   // W15: skillsIndexText for the rig index
#include "nimbus/orch/model_choices.h"   // CUM-486: the one model-choice derivation
#include "nimbus/orch/provider_slots.h"

// Stage G suite - the turn orchestration, host-tested for the first time: the
// TurnGuard head arc, the recall gate, the "host|convId" conversation state,
// the per-provider budget failover + all-exhausted refusal, the same-host
// fresh-conv retry, the priority failover walk with the owner notice, the
// no-retry-once-a-tool-ran guard, usage/turn-counter accounting, the parse
// SALVAGE path, the scheduled-turn rails (reboot refused, FireOutcome shape),
// synthesis consolidation + raw fallback, the turn-debug hook, and the
// stuck-turn reaper.

using agent::JobEngine;
using agent::RateLimit;
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
    std::string toolName = "memory_search";   // the tool a dispatchTools call names
    std::string served;                       // the model the provider echoes ("" = none)
    bool usageOnFail = false;                 // a failed attempt still bills its tokens
    std::string toolArgs = "{\"query\":\"x\"}";   // the arguments a dispatchTools call sends
    bool partialOnFail = false;               // a failed attempt still hands back outJson (a cut turn)
    bool checkFinal = false;                  // run HeadTools' final-answer check on outJson
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
  std::vector<std::string> finalChecks;   // what HeadTools.cfg.validateFinal said (checkFinal)
  std::vector<std::pair<std::string, std::string>> assistantRows;   // ApplyDeps.captureAssistant
  std::function<void()> onProviderCall;   // in-turn hook (stuck-turn reaper test)

  std::vector<std::pair<std::string, std::string>> delivered;  // chat, text
  std::vector<std::vector<std::string>> fabricCalls;  // hostLists routed to the fabric
  // Opt-in fabric switch: the first host bills 50/10 and echoes fabricFirstEcho, then
  // the round moves to the next host in the list, which bills 100/20 and echoes nothing.
  bool fabricSwitch = false;
  std::string fabricFirstEcho;
  // Lane T: a scripted fabric run. Each entry is one host the fabric visits in list
  // order with what it bills; every host but the last fails (reported through
  // onStepError, then a switch), and the last ends the turn with fabricOk / fabricErr.
  struct FabricVisit { uint32_t in = 0, out = 0; std::string err = "network"; };
  std::vector<FabricVisit> fabricVisits;
  bool fabricOk = true;
  std::string fabricErr = "network";
  std::function<void()> onDeliver;   // runs inside every delivery (apply-time effects)
  std::string fabricOut = kGoodTurn;  // what a scripted fabric run answers
  // L19-28: what the fabric loop reports back as the turn's transcript brief
  // ("" = the loop hands back no brief, so the turn reads as single-shot).
  std::string fabricBrief;
  int fabricDispatch = 0;   // tools the scripted fabric runs (ht.dispatch) before its visits
  std::vector<attn::Event> events;
  std::vector<std::string> cues;
  std::vector<agent::ComposeInputs> composeCalls;   // (count only)
  std::vector<std::string> recallQueries;
  std::vector<std::string> recallResult{"[92%] owner likes tea"};
  std::string recallDegraded;   // lane P: the cause when recall was keyword-only
  std::vector<orch::ToolRegistry::Spec> specs;      // advertised registry tools
  std::vector<std::string> mcpRequests;
  std::string mcpResponse =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"isError\":false,"
      "\"content\":[{\"type\":\"text\",\"text\":\"tool-ok\"}]}}";
  struct Captured { std::string chatId, text, fromTag; };
  std::vector<Captured> captured;
  std::vector<orch::ValidatedAction> staged;   // ApplyDeps.stageDevice recorder
  std::vector<orch::ScratchUpdate> scratchApplied;   // ApplyDeps.applyScratch recorder
  agent::ScratchOutcome scratchOutcome = agent::ScratchOutcome::Applied;   // what the save reports
  std::vector<std::string> memCalls;   // ApplyDeps.memDispatch recorder (tool names)
  orch::ToolResult memResult = orch::ToolResult::ok("stored");   // what every memory tool answers
  bool adminChats = false;   // opt-in: chats resolve to an Admin principal (scratchpad writes)
  std::string recentConversation;   // CUM-485: the window composeInputs hands the engine
  std::string chatSummary;          // L19-28: the compacted summary composeInputs hands the engine
  // Optional [AVAILABLE MODELS] source (CUM-486); null = the fixed fake lists.
  std::function<std::string(const std::string&)> modelChoicesFn;
  int journalGcs = 0;

  struct DebugCap {
    std::string host, instructions, inputs, rawOut;
    bool convContinued = false, ok = false;
    std::string header;            // turnAnatomyHeader over the event (the /api/lastturn head)
    agent::HistoryFacts history;
  };
  std::vector<DebugCap> debug;

  // HookRecorder - the lifecycle observer contract (Hooks consumers polish).
  std::vector<agent::TurnStartEv> hookStarts;
  std::vector<agent::TurnEndEv> hookEnds;
  std::vector<orch::HeadToolCall> hookToolCalls;
  std::vector<orch::HeadToolResult> hookToolResults;

  // Opt-in sub-agent fabric (one scripted "anthropic" adapter over an in-memory
  // journal) for tests that need a real dispatch; the default rig has none.
  struct MemJournalStore : orch::JournalStore {
    std::map<int, std::string> slots;
    std::string get(int slot) override {
      auto it = slots.find(slot);
      return it == slots.end() ? std::string() : it->second;
    }
    bool put(int slot, const std::string& v) override { slots[slot] = v; return true; }
    bool remove(int slot) override { slots.erase(slot); return true; }
    void clearNs() override { slots.clear(); }
  };
  harness_test::FakeAdapter subAdapter;
  agent::HeavyFabric subFabric;
  MemJournalStore journalStore;
  orch::Journal journal;

  std::unique_ptr<JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;
  orch::Hardware hw;   // the manifest the device closure would report (default: struct defaults)

  // spendBatch (lane T): wire BudgetConfig::recordSpend, the device's one call per turn
  // (the default keeps the per-slot recordTokens fallback the host rigs use).
  explicit Rig(bool withSubFabric = false, bool spendBatch = false) {
    cfg.spendBatch = spendBatch;
    JobEngine::Deps jd;
    jd.platform = plat.contract();
    jd.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
    };
    if (withSubFabric) {
      subAdapter.backend = "anthropic";
      subFabric.registerAdapter(&subAdapter);
      journal.begin(&journalStore);
      jd.fabric = &subFabric;
      jd.journal = &journal;
    }
    jobs.reset(new JobEngine(std::move(jd)));

    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = [this](const std::string& c, const std::string& t) {
      delivered.push_back({c, t});
      if (onDeliver) onDeliver();
    };
    d.event = [this](const attn::Event& e) { events.push_back(e); };
    d.fire = [this](const char* c) { cues.push_back(c); };
    d.recall = [this](const std::string& q, const nimbus::orch::Principal&) {
      recallQueries.push_back(q);
      return nimbus::orch::RecallOut{recallResult, recallDegraded};
    };
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.hw = hw;
      in.devName = "Nimbus";
      in.hostLabel = "anthropic / model-anthropic";
      in.recentConversation = recentConversation;
      in.chatSummary = chatSummary;
      composeCalls.push_back(in);
      return in;
    };
    d.toolSpecs = [this](const orch::Principal&) { return specs; };
    d.mcpDispatch = [this](const std::string& req, const nimbus::orch::Principal&) {
      mcpRequests.push_back(req);
      return mcpResponse;
    };
    d.connectorsCatalog = [] { return std::string("## PROVIDERS & CONNECTORS\n"); };
    // W15: the rig advertises the skills index the way the device does
    // (skills::indexText -> agent::skillsIndexText over the capsule list), over the
    // device's REAL built-ins (CUM-523: a copied line drifted from the device text).
    d.skillsIndex = [] {
      std::vector<agent::SkillIndexEntry> es;
      for (size_t i = 0; i < agent::builtinSkillCount(); ++i)
        es.push_back({agent::builtinSkill(i).id, agent::builtinSkill(i).desc, false});
      return agent::skillsIndexText(es);
    };
    d.modelChoices = [this](const std::string& p) {
      if (modelChoicesFn) return modelChoicesFn(p);
      return "model-" + p + "-a,model-" + p + "-b";
    };
    d.episodicCaptureUser = [this](const std::string& c, const std::string& t,
                                   const std::string& f) {
      captured.push_back({c, t, f});
    };
    d.firstAllowedChat = [] { return std::string("1001"); };
    d.journalGc = [this] { journalGcs++; };
    d.hooks.onTurnDebug = [this](const agent::TurnDebugEv& ev) {
      debug.push_back({ev.host, *ev.instructions, *ev.inputs, *ev.rawOut,
                       ev.convContinued, ev.ok, agent::turnAnatomyHeader(ev), ev.history});
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
    wireLaneL(d, withSubFabric);
    d.apply.applyScratch = [this](const orch::ScratchUpdate& u) { scratchApplied.push_back(u); return scratchOutcome; };
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst,
                                 const orch::Principal&) {
      memCalls.push_back(name);
      return memResult;
    };
    // Default = the harness's deny-all principal; adminChats = true opts into admin.
    d.apply.principalFor = [this](const std::string& chat) {
      if (adminChats) return orch::principalForRole(chat, orch::Role::Admin);
      orch::Principal p;
      p.ns = orch::nsForChat(chat, false);
      return p;
    };
    // CUM-242: cumulo/zai are first-class heads on the device now, registered the
    // same way - the rig mirrors that so a router-fallback turn has a head to run.
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"})
      d.hosts.add(h, providerFor(h));
    wireFabricStub(d);
    wirePark(d);
    eng.reset(new TurnEngine(std::move(d)));
  }

  // Lane K's host park, bound like the device: the real providers::hostParked /
  // noteHostFailed / unparkHost over a ProviderDeps whose key and model are this rig's
  // config, so a new key un-parks a host. The park and the billing-notice latch are
  // per process, so every rig starts with neither.
  agent::providers::ProviderDeps parkDeps;
  void wirePark(TurnEngine::Deps& d) {
    agent::providers::clearHostParks();
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"})
      orch::billingEpisodeEnd(h);
    parkDeps.key = [this](const char* h) {
      auto it = cfg.keys.find(h);
      return it == cfg.keys.end() ? std::string() : it->second;
    };
    parkDeps.orchModel = [this](const char* h) { return modelOf(h); };
    parkDeps.nowMs = [this] { return plat.ms; };
    d.hosts.parked = [this](const std::string& h, agent::providers::HostPark* why) {
      return agent::providers::hostParked(parkDeps, h, why);
    };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) {
      agent::providers::noteHostFailed(parkDeps, h, k, status);
    };
    d.hosts.answered = [](const std::string& h) { agent::providers::unparkHost(h); };
  }

  // Fabric stub (recording): only reachable when a test opts in with
  // cfg.midFail = true - the rig default keeps every legacy test on the
  // scripted single-shot path. Mirrors the device: fabricSupports knows the
  // three cloud hosts, so a custom head must fall back to hosts.run.
  // Like providers::runFabricLoop: each host bills into its own counter, filed
  // into the spend table once at the end under the model configured for it.
  // (Out of the constructor only to keep the lizard length ceiling honest.)
  void wireFabricStub(TurnEngine::Deps& d) {
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string&,
                            const std::string&, std::string& out, std::string& err,
                            const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const FabricNotify& notify) {
      return fabricRun(hl, out, err, ht, spend, notify);
    };
    d.hosts.fabricSupports = [](const std::string& h) {
      return h == "openai" || h == "anthropic" || h == "mistral";
    };
  }

  // Lane L: the delivered text is captured, and the sub-agent rig queues spawns
  // into its job engine, like the device.
  void wireLaneL(TurnEngine::Deps& d, bool withSubFabric) {
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      assistantRows.push_back({c, t});
    };
    if (!withSubFabric) return;
    d.apply.enqueueSpawn = [this](const orch::Spawn& sp, const std::string& chat, bool quiet) {
      return jobs->enqueueSpawn(sp, chat, quiet);
    };
    d.apply.noteSpawned = [this] { jobs->noteSpawned(); };
  }

  // Lane L: what HeadTools' final-answer check says about this script's answer.
  void recordFinalCheck(const agent::HeadTools* tools, const Script& s) {
    if (!tools || !s.checkFinal) return;
    finalChecks.push_back(tools->cfg.validateFinal ? tools->cfg.validateFinal(s.outJson)
                                                   : std::string("<no check>"));
  }

  // Lane L: a failed attempt that still hands back its (cut) turn.
  static void keepPartial(const Script& s, std::string& out) {
    if (s.partialOnFail) out = s.outJson;
  }

  using FabricNotify = std::function<void(const std::string&, const std::string&)>;

  // One host's billed usage into the spend table, under the model configured for it.
  void fileSpend(orch::TurnSpend* spend, const std::string& host, uint32_t in, uint32_t o,
                 const std::string& echo) {
    orch::TokenUsage u;
    u.add(in, o);
    u.servedModel = echo;
    if (spend) spend->add(host.c_str(), modelOf(host).c_str(), u);
  }

  // The scripted fabricVisits run: every host but the last fails (onStepError, then a
  // switch), the last ends the turn with fabricOk / fabricErr.
  bool fabricVisitsRun(const std::vector<std::string>& hl, std::string& out, std::string& err,
                       const agent::HeadTools& ht, orch::TurnSpend* spend,
                       const FabricNotify& notify) {
    const size_t n = std::min(fabricVisits.size(), hl.size());
    for (size_t i = 0; i < n; ++i) {
      fileSpend(spend, hl[i], fabricVisits[i].in, fabricVisits[i].out, "");
      const bool last = i + 1 == n;
      if (last && fabricOk) break;
      if (ht.onStepError) ht.onStepError(hl[i], last ? fabricErr : fabricVisits[i].err);
      if (!last && notify) notify(hl[i], hl[i + 1]);
    }
    if (!fabricOk) { out.clear(); err = fabricErr; }
    return fabricOk;
  }

  // Like providers::runFabricLoop: each host bills into its own counter, filed into
  // the spend table under the model configured for it.
  bool fabricRun(const std::vector<std::string>& hl, std::string& out, std::string& err,
                 const agent::HeadTools& ht, orch::TurnSpend* spend, const FabricNotify& notify) {
    fabricCalls.push_back(hl);
    out = fabricOut;
    // L19-28: the rig can hand back a transcript brief, so a scripted fabric
    // turn reads as a loop turn.
    if (!fabricBrief.empty() && ht.onBrief) ht.onBrief(fabricBrief);
    for (int i = 0; i < fabricDispatch && ht.dispatch; ++i) {
      orch::HeadToolCall c;
      c.id = "t" + std::to_string(i);
      c.name = "memory_search";
      c.argsJson = "{\"query\":\"x\"}";
      toolResults.push_back(ht.dispatch(c));
    }
    if (!fabricVisits.empty()) return fabricVisitsRun(hl, out, err, ht, spend, notify);
    if (fabricSwitch && hl.size() > 1) {
      fileSpend(spend, hl[0], 50, 10, fabricFirstEcho);
      if (ht.onStepError) ht.onStepError(hl[0], "network");
      if (notify) notify(hl[0], hl[1]);
      fileSpend(spend, hl[1], 100, 20, "");
      return true;
    }
    fileSpend(spend, hl[0], 100, 20, "");
    return true;
  }

  std::string modelOf(const std::string& h) const {
    auto it = cfg.orchModels.find(h);
    return it == cfg.orchModels.end() ? std::string("model-" + h) : it->second;
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
          c.name = s.toolName;
          c.argsJson = s.toolArgs;
          toolResults.push_back(tools->dispatch(c));
        }
      }
      recordFinalCheck(tools, s);
      if (onProviderCall) onProviderCall();
      // Like the adapters: the echo is captured on the HTTP 200, before the turn is
      // known to parse, so a failed attempt can carry one too.
      if (usage && !s.served.empty()) usage->servedModel = s.served;
      if (s.ok) {
        convId = s.convOut;
        out = s.outJson;
        if (usage) usage->add(s.usageIn, s.usageOut);
      } else {
        err = s.err.empty() ? "scripted failure" : s.err;
        keepPartial(s, out);
        if (usage && s.usageOnFail) usage->add(s.usageIn, s.usageOut);
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
  int countDelivered(const char* needle) const {
    int n = 0;
    for (auto& p : delivered)
      if (p.second.find(needle) != std::string::npos) n++;
    return n;
  }
  int attemptsOn(const std::string& host) const {
    int n = 0;
    for (auto& a : attempts)
      if (a.host == host) n++;
    return n;
  }
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
  // gate (loopMinHeap 12000 + 2000), every later read is in the trough below it.
  // The loop MUST still be advertised + wired. (handleMessage would consume one
  // read for its own entry floor, so drive runTurn directly to isolate the gate.)
  r.plat.heapScript = {41000, 13000, 13000, 13000, 13000};
  r.eng->runTurn("inputs", "1001", "user text");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE_MESSAGE(r.attempts[0].hadTools,
                           "loop deferred by the post-recall heap dip, not entry heap");
}

static void test_loop_defers_when_entry_heap_is_genuinely_low() {
  Rig r;
  // Entry itself is under the gate: a real low-memory turn still falls back to
  // single-shot - the fix must not defeat the genuine guard. 13000 < loopMinHeap
  // (12000) + 2000 = 14000, but > recallMinHeap (12000) so recall still runs.
  r.plat.heapScript = {13000, 13000, 13000};
  r.eng->runTurn("inputs", "1001", "user text");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_FALSE_MESSAGE(r.attempts[0].hadTools,
                            "a genuinely low-heap turn must fall back to single-shot");
}

// ---- CUM-404: recalibrated floors + the largest-block fragmentation guard ----

// The native ASSERTION for the recalibrated floors: the invariant, not one point.
// A careless future bump that re-raises the floor into the pre-PSRAM danger zone,
// inverts the background/interactive ordering, or drops the largest-block guard
// FAILS here. (The device-side authoritative macros carry matching static_asserts
// in agent_config.h, checked by the esp32s3/esp32s3-cyd builds.)
static void test_recalibrated_floor_invariants() {
  TurnEngine::Tuning t;   // the shipped defaults, mirroring agent_config.h
  // Recalibrated DOWN from the pre-PSRAM 28-30 KB floors now that turn buffers ride PSRAM.
  TEST_ASSERT_TRUE(t.turnHardFloor   < 28000);
  TEST_ASSERT_TRUE(t.autoTurnMinHeap < 30000);
  TEST_ASSERT_TRUE(t.loopMinHeap     < 28000);
  // In the calibrated ~10-12 KB band, above the genuine-starvation backstop (relay's 8000).
  TEST_ASSERT_TRUE(t.turnHardFloor >= 8000 && t.turnHardFloor <= 12000);
  // Background work stays the most conservative total-free floor: it yields to a live turn.
  TEST_ASSERT_TRUE(t.autoTurnMinHeap > t.turnHardFloor);
  // Turn / recall / loop-round all make the SAME single mbedTLS handshake: one floor.
  TEST_ASSERT_EQUAL(t.turnHardFloor, t.recallMinHeap);
  TEST_ASSERT_EQUAL(t.turnHardFloor, t.loopMinHeap);
  // ...and so does a sub-agent dispatch (CUM-514, L20 audit: the portable default
  // was still the pre-PSRAM 28000 while the device passes ORCH_DISPATCH_MIN_HEAP).
  TEST_ASSERT_EQUAL(t.turnHardFloor, JobEngine::Tuning{}.dispatchMinHeap);
  // The largest-block guard is the real gate. CUM-404 v2: it REUSES relay_heap.h's
  // relay-proven per-connection floor (5000), NOT provider_verify's one-shot 8000 -
  // 8000 spuriously deferred every turn on the CYD/freenove, whose ~5 KB DMA bounce
  // buffer pins the largest internal block near the floor. Never above the total-free
  // floor it complements, and <= 5000 so it stays CYD-safe.
  TEST_ASSERT_EQUAL(5000, (int)t.turnMinLargestBlock);
  TEST_ASSERT_TRUE(t.turnMinLargestBlock <= t.turnHardFloor);
  TEST_ASSERT_TRUE(t.turnMinLargestBlock <= 5000);   // must not exceed the CYD-safe value
  // The pure predicate: genuinely fragmented (below the handshake need) refused;
  // the CYD healthy-but-fragmented case (largest ~5 KB, ample total) ADMITTED;
  // healthy-but-low-total admitted; genuinely starved-total refused.
  TEST_ASSERT_FALSE(agent::turnHeapOk(100000, 4999, t.turnHardFloor, t.turnMinLargestBlock));
  TEST_ASSERT_TRUE (agent::turnHeapOk(26000, 5000, t.turnHardFloor, t.turnMinLargestBlock));
  TEST_ASSERT_TRUE (agent::turnHeapOk(12000, 5000, t.turnHardFloor, t.turnMinLargestBlock));
  TEST_ASSERT_FALSE(agent::turnHeapOk(11999, 100000, t.turnHardFloor, t.turnMinLargestBlock));
}

// The chat gate defers on FRAGMENTATION (largest block below the handshake need)
// even when total free is ample - the case the old total-free-only gate missed, and
// the one that used to run a turn straight into a "connect failed" transport error.
static void test_chat_gate_defers_on_fragmentation_not_total_free() {
  Rig r;
  r.plat.heap = 100000;   // total internal free is ample...
  r.plat.largest = 4999;  // ...but the largest contiguous block is below the 5000 guard
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());        // no paid provider round-trip
  TEST_ASSERT_EQUAL(0, (int)r.eng->turnCount());
  TEST_ASSERT_TRUE(r.anyDelivered("low on working memory"));   // the honest defer reply
  TEST_ASSERT_FALSE(r.anyDelivered("unavailable"));            // NOT a provider-fault message
}

// At the recalibrated floor (12000 free / 5000 largest) the turn RUNS - the old
// 28000 floor deferred it here while PSRAM sat ~98% empty (the CUM-404 bug).
static void test_chat_gate_admits_at_the_recalibrated_floor() {
  Rig r;
  r.plat.heap = 12000;    // == ORCH_TURN_HARD_FLOOR
  r.plat.largest = 5000;  // == ORCH_TURN_MIN_LARGEST_BLOCK
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
}

// CUM-404 v2 regression: the CYD/freenove healthy state - ~26 KB total free but the
// largest INTERNAL block pinned near 5 KB by the persistent DMA bounce buffer - must
// NOT be deferred. The imported 8000 floor (provider_verify's one-shot value) failed
// here at ~5 KB even though the turn's real handshake need is ~4 KB; the relay-proven
// 5000 floor admits it.
static void test_chat_gate_admits_fragmented_cyd_is_not_deferred() {
  Rig r;
  r.plat.heap = 26000;    // ample total free (the CYD at rest)
  r.plat.largest = 5100;  // largest block pinned near 5 KB by the DMA bounce buffer
  r.eng->handleMessage("hi there", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());   // admitted, not deferred
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  TEST_ASSERT_FALSE(r.anyDelivered("working memory"));   // no spurious low-memory defer
}

// A memory-pressure transport failure ("low memory" from transport_tls) degrades
// with the honest low-memory reply and does NOT walk the failover ladder to a
// healthy provider (which would read as a wrong-provider fallback - the symptom).
static void test_oom_transport_error_yields_low_memory_reply_no_failover() {
  Rig r;
  Rig::Script oom; oom.ok = false; oom.err = "connect failed: low memory";
  r.scripts["anthropic"] = {oom};   // the head host OOMs; openai/mistral stay healthy
  r.eng->handleMessage("hi there", "Roy", "1001");
  // Exactly ONE attempt: no same-host retry, no failover to a healthy provider.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts[0].host.c_str());
  // Honest low-memory reply, not a transport-error "That didn't finish" cause, and
  // no "switching to <provider>" failover notice.
  TEST_ASSERT_TRUE(r.anyDelivered("ran out of working memory for this request"));
  TEST_ASSERT_FALSE(r.anyDelivered("could not be reached"));
  TEST_ASSERT_FALSE(r.anyDelivered("switching to"));
}

// CUM-404 v2 regression: a GENUINE network error must still fail over, even on a
// fragmented board. transport_tls tags a failure "low memory" ONLY when the exchange
// began starved (startedStarved from the pre-attempt largest block); a real outage
// begins with adequate memory, so it emits a plain "connect failed" (no low-memory
// tag) and the engine walks the CUM-41 failover ladder to a healthy provider. The
// earlier version tagged every sub-8000 failure OOM and silently skipped failover.
static void test_network_error_still_fails_over_not_misclassified_oom() {
  Rig r;
  // Plain network failure on the head host (NOT the "low memory" token), repeats so
  // the same-host retry fails too; a fragmented board is represented by a low largest.
  r.plat.largest = 5100;   // CYD-style fragmented but healthy (>= the 5000 floor)
  r.scripts["anthropic"] = {{false, "", "connect failed", 0, "", 0, 0}};
  r.eng->runTurn("inputs", "1001", "");
  // The ladder walked: anthropic (initial + retry) then openai serves the turn.
  TEST_ASSERT_EQUAL(3, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[2].host.c_str());
  TEST_ASSERT_TRUE(r.anyDelivered("switching to openai"));
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  // NOT misreported as a low-memory failure.
  TEST_ASSERT_FALSE(r.anyDelivered("working memory"));
}

// ---- (1) happy turn ---------------------------------------------------------

// The engine end of the prefix-stability class (test_prompt_layout pins the
// composer): two turns of one chat that differ in user text, recall and the
// conversation window hand the provider the SAME static and summary parts.
static void test_two_turns_share_the_static_prompt_block() {
  Rig r;
  r.recentConversation = "\n## RECENT CONVERSATION\n- user: earlier\n";
  r.eng->handleMessage("hi there", "Roy", "1001");
  r.recentConversation += "- assistant: hello\n- user: hi there\n";
  r.recallResult = {"[80%] the owner likes tea"};
  r.eng->handleMessage("log my lunch please", "Roy", "1001");
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());
  const agent::layout::PromptParts a = agent::layout::split(r.attempts[0].instructions);
  const agent::layout::PromptParts b = agent::layout::split(r.attempts[1].instructions);
  TEST_ASSERT_TRUE(a.split && b.split);
  TEST_ASSERT_TRUE_MESSAGE(a.stat == b.stat, "the static block changed between two turns");
  TEST_ASSERT_TRUE(a.summary == b.summary);
  TEST_ASSERT_TRUE(a.dyn != b.dyn);
  TEST_ASSERT_TRUE(b.dyn.find("the owner likes tea") != std::string::npos);
  // The per-turn input keeps only the per-turn context.
  TEST_ASSERT_TRUE(r.attempts[1].inputs.find("[ACTIVE SESSIONS]") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[1].inputs.find("[AVAILABLE MODELS]") == std::string::npos);
}

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
  // playbooks (id + desc) to have a thread to pull with skill.get. The stable
  // catalogs ride the END of the static prompt block (prompt_layout.h), where a
  // provider reads them from its cache, not the per-turn input.
  const agent::layout::PromptParts parts = agent::layout::split(r.attempts[0].instructions);
  TEST_ASSERT_TRUE(parts.split);
  TEST_ASSERT_TRUE(parts.stat.find("[SKILLS]") != std::string::npos);
  TEST_ASSERT_TRUE(parts.stat.find("- deliver-pdf: Build a PDF") != std::string::npos);
  TEST_ASSERT_TRUE(parts.stat.find("## PROVIDERS & CONNECTORS") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("[SKILLS]") == std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("## PROVIDERS & CONNECTORS") == std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
                       "[CHANNEL] This message arrived via Telegram") != std::string::npos);
  // Reply delivered; conv stored in the per-chat map (B2); usage recorded; counter++.
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  TEST_ASSERT_EQUAL_STRING("c1", agent::convMapGet(r.cfg.convId, "1001", "anthropic").c_str());
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.cfg.recorded[0].host.c_str());
  TEST_ASSERT_EQUAL(100, (int)r.cfg.recorded[0].in);
  TEST_ASSERT_EQUAL(20, (int)r.cfg.recorded[0].out);
  // Per-model prices: no echo -> filed under the host's configured model.
  TEST_ASSERT_EQUAL_STRING("model-anthropic", r.cfg.recorded[0].model.c_str());
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

// Lane P (F5): a recall seam returns RecallOut, so a fourth copy that hides an
// outage behind an empty list does not compile.
static_assert(std::is_same<decltype(std::declval<TurnEngine::Deps>().recall(
                                std::string(), nimbus::orch::Principal())),
                           nimbus::orch::RecallOut>::value,
              "the recall seam must say when it was degraded");

// An embedding outage is announced in the prompt, never silent: degraded with no
// hits still gets the keyword-only line; degraded with hits gets both; a healthy
// recall gets no line (no notice spam).
static void test_recall_outage_is_announced_not_silent() {
  const char* kLine = "Automatic recall is keyword-only right now: out of embedding credit at openai.";
  {
    Rig r;
    r.recallResult.clear();
    r.recallDegraded = "out of embedding credit at openai";
    r.eng->handleMessage("what is my locker code?", "", "1001");
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find(kLine) != std::string::npos);
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find("## RELEVANT MEMORIES") != std::string::npos);
  }
  {
    Rig r;
    r.recallResult = {"[pending, not stored yet] my locker code is 4417"};
    r.recallDegraded = "out of embedding credit at openai";
    r.eng->handleMessage("what is my locker code?", "", "1001");
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find(kLine) != std::string::npos);
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find("my locker code is 4417") != std::string::npos);
  }
  {
    Rig r;   // healthy: the hits, and no notice
    r.eng->handleMessage("what do I drink?", "", "1001");
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find("owner likes tea") != std::string::npos);
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find("keyword-only") == std::string::npos);
  }
  {
    Rig r;   // nothing relevant and healthy: no section at all, as before
    r.recallResult.clear();
    r.eng->handleMessage("hello", "", "1001");
    TEST_ASSERT_TRUE(r.eng->lastInstructions().find("## RELEVANT MEMORIES") == std::string::npos);
  }
}

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
  r.plat.heap = 11999;   // < recallMinHeap (12000) but engine floor only gates recall here
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

// Per-model prices (2026-10): a turn files its spend under the model that served
// it - the configured model when the echo is only its alias, the served model on a
// real substitution, and the failover-final host's model, never the first host's.
static void test_turn_spend_files_under_the_serving_model() {
  {
    Rig r;
    r.cfg.orchModels["anthropic"] = "claude-haiku-4-5";
    Rig::Script s;
    s.served = "claude-haiku-4-5-20251001";   // a dated alias echo
    r.scripts["anthropic"] = {s};
    r.eng->runTurn("inputs", "1001", "");
    TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
    TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", r.cfg.recorded[0].model.c_str());
  }
  {
    Rig r;
    r.cfg.orchModels["anthropic"] = "claude-opus-5";
    r.cfg.orchModels["openai"] = "gpt-5.6";
    r.scripts["anthropic"] = {{false, "", "down", 0, "", 0, 0}};
    Rig::Script s;
    s.served = "gpt-5.6-luna";                // a real substitution
    r.scripts["openai"] = {s};
    r.eng->runTurn("inputs", "1001", "");
    TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
    TEST_ASSERT_EQUAL_STRING("openai", r.cfg.recorded[0].host.c_str());
    TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", r.cfg.recorded[0].model.c_str());
  }
}

// Gate G5: only the attempt that served the turn names its model. A failed attempt
// that echoed a model (captured on its HTTP 200, before the turn failed to parse) and
// a host that then serves the turn WITHOUT an echo: the serving host's spend is filed
// under its configured model, never the failed attempt's echo - on the failover
// ladder, on the same-host retry and across a fabric switch. Lane T: each attempt's
// spend is filed under ITS host and model (the usage RCA: switched turns used to book
// every earlier host's rounds under the host that finished).
static void test_failed_attempt_echo_never_names_the_serving_model() {
  {   // failover ladder: anthropic echoes and fails, openai serves with no echo
    Rig r;
    r.cfg.orchModels["anthropic"] = "claude-opus-5";
    r.cfg.orchModels["openai"] = "gpt-5.6";
    Rig::Script bad;
    bad.ok = false;
    bad.err = "no orch_turn tool_use (stop_reason=max_tokens)";
    bad.served = "claude-haiku-4-5";   // would be a substitution under any host
    bad.usageOnFail = true;
    r.scripts["anthropic"] = {bad};
    r.scripts["openai"] = {Rig::Script{}};
    TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_TRUE(r.attempts.size() >= 2);
    // The failed attempts' tokens still count - under the host and model that billed
    // them (the echoed substitution), and the serving host's under its own model.
    TEST_ASSERT_EQUAL(2, (int)r.cfg.recorded.size());
    TEST_ASSERT_EQUAL_STRING("anthropic", r.cfg.recorded[0].host.c_str());
    TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", r.cfg.recorded[0].model.c_str());
    TEST_ASSERT_EQUAL((int)(100 * (r.attempts.size() - 1)), (int)r.cfg.recorded[0].in);
    TEST_ASSERT_EQUAL_STRING("openai", r.cfg.recorded[1].host.c_str());
    TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.cfg.recorded[1].model.c_str());
    TEST_ASSERT_EQUAL(100, (int)r.cfg.recorded[1].in);
  }
  {   // same-host retry: the first attempt echoes a cheaper model and fails
    Rig r;
    r.cfg.orchModels["anthropic"] = "claude-opus-5";
    Rig::Script bad;
    bad.ok = false;
    bad.err = "network";
    bad.served = "claude-haiku-4-5";
    r.scripts["anthropic"] = {bad, Rig::Script{}};
    TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_EQUAL(2, (int)r.attempts.size());
    TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts[1].host.c_str());
    TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
    TEST_ASSERT_EQUAL_STRING("claude-opus-5", r.cfg.recorded[0].model.c_str());
  }
  {   // fabric: anthropic echoes, the round moves to openai, which echoes nothing
    Rig r;
    r.cfg.midFail = true;
    r.cfg.orchHost = "anthropic";
    r.cfg.priority = "anthropic,openai";
    r.cfg.orchModels["anthropic"] = "claude-opus-5";
    r.cfg.orchModels["openai"] = "gpt-5.6";
    r.fabricSwitch = true;
    r.fabricFirstEcho = "claude-haiku-4-5";
    TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
    TEST_ASSERT_EQUAL(1, (int)r.fabricCalls.size());
    TEST_ASSERT_EQUAL(2, (int)r.cfg.recorded.size());
    TEST_ASSERT_EQUAL_STRING("anthropic", r.cfg.recorded[0].host.c_str());
    TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", r.cfg.recorded[0].model.c_str());
    TEST_ASSERT_EQUAL(50, (int)r.cfg.recorded[0].in);
    TEST_ASSERT_EQUAL(10, (int)r.cfg.recorded[0].out);
    TEST_ASSERT_EQUAL_STRING("openai", r.cfg.recorded[1].host.c_str());
    TEST_ASSERT_EQUAL_STRING("gpt-5.6", r.cfg.recorded[1].model.c_str());
    TEST_ASSERT_EQUAL(100, (int)r.cfg.recorded[1].in);
    TEST_ASSERT_EQUAL(20, (int)r.cfg.recorded[1].out);
    TEST_ASSERT_EQUAL(150, (int)r.eng->lastTurnUsage().promptTokens);   // the display total
  }
}

// CUM-514 (L20 docs audit): the round cap the prompt states IS the loop's cap -
// the owner-tuned value, the same number headTools.cfg.maxRounds runs with.
static void test_prompt_round_cap_follows_the_configured_loop() {
  Rig r;
  r.cfg.rounds = 3;
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.debug.size());
  TEST_ASSERT_TRUE(r.debug[0].instructions.find("several rounds (at most 3)") != std::string::npos);
}

// ---- CUM-477: a send the router loop could not finish is a memory failure ----
// Bench C2: a tool had run, then round 1 could not be sent (heap 9940); the owner
// read "the provider could not be reached (check the connection)". The loop now
// reports "low memory: ..." and the reply names the real cause and the next step.
static void test_cum477_unsent_request_reply_blames_memory_not_connection() {
  Rig r;
  r.scripts["anthropic"] = {{false, "", "low memory: the device could not send the request",
                             1 /*a tool ran first*/, "", 0, 0}};
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING(
      "That didn't finish - the device ran out of working memory for this request. Nothing "
      "is still running; try a shorter question or wait a minute.",
      r.lastText().c_str());
  TEST_ASSERT_FALSE(r.anyDelivered("check the connection"));
}

// prism (peer review): only the custom head's explicit "timeout:" token skips the
// same-host retry - a provider error that merely mentions a timeout (a router 504
// text) keeps it.
static void test_cum544_only_the_timeout_token_skips_the_retry() {
  Rig r;
  r.cfg.keyed = {"openai", "anthropic"};
  r.cfg.orchHost = "openai";
  r.cfg.priority = "anthropic";
  r.scripts["openai"] = {{false, "", "resp HTTP 504: upstream request timeout", 0, "", 0, 0}};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[1].host.c_str());   // the same-host retry ran
  TEST_ASSERT_FALSE(r.anyDelivered("did not answer in time"));
}

// prism: each failover notice names the reason of the host that just failed - a
// second switch must not inherit the first host's "did not answer in time".
static void test_cum544_second_notice_names_its_own_reason() {
  Rig r;
  r.cfg.keyed = {"zai", "openai", "mistral"};
  r.cfg.orchHost = "zai";
  r.cfg.priority = "openai,mistral";
  r.scripts["zai"] = {{false, "", "timeout: the provider did not answer in time", 0, "", 0, 0}};
  r.scripts["openai"] = {{false, "", "network", 0, "", 0, 0}};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_TRUE(r.anyDelivered("zai did not answer in time - switching to"));
  TEST_ASSERT_TRUE(r.anyDelivered("openai is unavailable - switching to"));
  TEST_ASSERT_FALSE(r.anyDelivered("openai did not answer in time"));
}

// The class: every Telegram-only tool is named by isTelegramOnlyTool, and nothing
// else is (a new Telegram delivery tool must be added there, or it is offered on a
// device with no bot).
static void test_cum523_telegram_only_tools_are_exactly_these() {
  TEST_ASSERT_TRUE(agent::isTelegramOnlyTool("reply.telegram"));
  TEST_ASSERT_TRUE(agent::isTelegramOnlyTool("files.send"));
  for (const char* other : {"reply.speak", "files.list", "artifact.save", "memory.write",
                            "device.control", "wakeup.set"})
    TEST_ASSERT_FALSE_MESSAGE(agent::isTelegramOnlyTool(other), other);
}

// A scheduled one-shot with no chat on a Telegram-less device gets no delivery
// line (channelOf("") would name Telegram, which this device does not have).
static void test_cum523_no_delivery_line_without_a_real_target() {
  Rig r;
  r.cfg.telegramOn = false;
  r.scripts["anthropic"] = {{true, "{\"reply\":\"ok\"}", "", 0}};
  // firstAllowedChat() is "1001" in the rig; a device with no allowlist has "".
  // The engine resolves "" -> firstAllowedChat; here the Telegram chat 1001 is
  // not a device channel and there is no bot, so no line.
  orch::FireOutcome o = r.eng->injectScheduledTurn("", "check the build", "wakeup", "lp1",
                                                   /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("Your reply is delivered to") == std::string::npos);
}

// ---- CUM-542: the composition's web page chat is named the web UI ----------
// Joiner-VN3: a message from the hosted web page was told "[CHANNEL] This message
// arrived via Telegram": the hosted page runs its turns as "owner"
// (nimbus::orch::kHostedWebChatId), and only "web" was a web chat. Both page chats are the
// web UI; every other chat id (a Telegram chat id is a number) is still Telegram.
static bool channelSays(const Rig& r, const char* where) {
  return r.attempts.back().inputs.find(std::string("[CHANNEL] This message arrived via ") + where) !=
         std::string::npos;
}

static void test_cum542_web_page_chat_is_the_web_ui() {
  // A device never opts in: "owner" stays a Telegram chat id there (gate finding).
  TEST_ASSERT_FALSE(agent::isWebChat(nimbus::orch::kHostedWebChatId));
  nimbus::orch::setHostedWebChatAlias(true);   // what the hosted daemon does at startup
  TEST_ASSERT_TRUE(agent::isWebChat("web") && agent::isWebChat(nimbus::orch::kHostedWebChatId));
  for (const char* other : {"1001", "-100200300", "voice", "serial", "", "owner2"})
    TEST_ASSERT_FALSE_MESSAGE(agent::isWebChat(other), other);
  TEST_ASSERT_FALSE(agent::isTelegramChat(nimbus::orch::kHostedWebChatId));   // one rule for every reader

  Rig hosted;   // a hosted instance: its page chats as "owner"
  hosted.eng->handleMessage("hello from the page", "", "owner");
  TEST_ASSERT_TRUE(channelSays(hosted, "the web UI."));
  TEST_ASSERT_FALSE(channelSays(hosted, "Telegram"));
  hosted.eng->handleMessage("hello from Telegram", "Ana", "424242");
  TEST_ASSERT_TRUE(channelSays(hosted, "Telegram;"));
  hosted.eng->handleMessage("hi", "", "web");
  TEST_ASSERT_TRUE(channelSays(hosted, "the web UI."));

  // A one-shot set from the page is answered in the web chat, not "this Telegram chat".
  orch::FireOutcome o = hosted.eng->injectScheduledTurn("owner", "stretch now", "wakeup", "lp1",
                                                        /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_TRUE(hosted.attempts.back().inputs.find("(Your reply is delivered to the web chat.") !=
                   std::string::npos);
  nimbus::orch::setHostedWebChatAlias(false);
}

// ---- CUM-544: a timeout fails over at once, and the notice says why ---------
// Bench (nimbus-light): the Z.ai head timed out, the engine retried the same host
// on a fresh conversation (another full timeout), then failed over at 268 s with
// "zai is unavailable". A timeout is model latency: no same-host retry, and the
// owner hears "did not answer in time".
static void test_cum544_timeout_fails_over_without_same_host_retry() {
  Rig r;
  r.cfg.keyed = {"zai", "openai"};
  r.cfg.orchHost = "zai";
  r.cfg.priority = "openai";
  r.scripts["zai"] = {{false, "", "timeout: the provider did not answer in time", 0, "", 0, 0}};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());   // zai once, then openai - no retry
  TEST_ASSERT_EQUAL_STRING("zai", r.attempts[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[1].host.c_str());
  TEST_ASSERT_EQUAL(0, (int)r.plat.delays.size());   // no 400 ms same-host retry pause
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: zai did not answer in time - failing over without a retry"));
  TEST_ASSERT_FALSE(LogCapture::contains("fresh conv retry"));
  TEST_ASSERT_TRUE(r.anyDelivered(
      "\xE2\x9A\xA0\xEF\xB8\x8F zai did not answer in time - switching to openai "
      "(your recent messages carry over; the provider-side thread restarts)."));
  TEST_ASSERT_FALSE(r.anyDelivered("is unavailable"));
  TEST_ASSERT_FALSE(r.anyDelivered("formatting error"));
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
}

// Every provider times out: the final line names the real cause, never the
// connection. Counter-test: a plain network error keeps its one same-host retry
// (test_same_host_fresh_conv_retry) - only the Timeout class skips it.
static void test_cum544_all_timeouts_say_no_answer_in_time() {
  Rig r;
  r.cfg.keyed = {"zai", "openai"};
  r.cfg.orchHost = "zai";
  r.cfg.priority = "openai";
  const Rig::Script slow{false, "", "timeout: the provider did not answer in time", 0, "", 0, 0};
  r.scripts["zai"] = {slow};
  r.scripts["openai"] = {slow};
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());
  // Lane T: the first cause in the typed words, and the other host that failed after
  // it; a timeout is in transit, so asking again is offered.
  TEST_ASSERT_EQUAL_STRING(
      "That didn't finish - zai did not answer in time; openai also failed. "
      "Your message was not answered. Nothing is still running; ask again to retry.",
      r.lastText().c_str());
  TEST_ASSERT_FALSE(r.anyDelivered("check the connection"));

  Rig n;   // the counter-test, same rig shape: "network" still retries same-host once
  n.cfg.keyed = {"zai", "openai"};
  n.cfg.orchHost = "zai";
  n.cfg.priority = "openai";
  n.scripts["zai"] = {{false, "", "network", 0, "", 0, 0}};
  n.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL(3, (int)n.attempts.size());   // zai, zai (retry), openai
  TEST_ASSERT_EQUAL_STRING("zai", n.attempts[1].host.c_str());
  TEST_ASSERT_TRUE(n.anyDelivered("zai is unavailable - switching to openai"));
}

// ---- CUM-530 bench: a router refusal is not an outage --------------------------
// nimbus-light: the Cumulo router refused the picked model and the owner read
// "cumulo is unavailable - switching to openai". When the cumulo head fails with
// a refusal no router model could answer (router_refusal.h causes), the failover
// notice carries that cause and its fix; an outage keeps "is unavailable".
static void test_cum530_router_refusal_notice_is_never_unavailable() {
  Rig r;
  r.cfg.keyed = {"cumulo", "openai"};
  r.cfg.orchHost = "cumulo";
  r.cfg.priority = "openai";
  r.scripts["cumulo"] = {{false, "",
                          "the Cumulo router does not offer mistral/mistral/zai-glm-5-3 right now. "
                          "Choose another model under Models",
                          0, "", 0, 0}};
  r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_TRUE(r.anyDelivered(
      "\xE2\x9A\xA0\xEF\xB8\x8F The Cumulo router does not offer mistral/mistral/zai-glm-5-3 right now. "
      "Choose another model under Models. Switching to openai (your recent messages carry over; "
      "the provider-side thread restarts)."));
  TEST_ASSERT_FALSE(r.anyDelivered("unavailable"));
  TEST_ASSERT_EQUAL_STRING("hello there", r.lastText().c_str());
  // Counter-test: an outage of the same head still reads as one.
  Rig n;
  n.cfg.keyed = {"cumulo", "openai"};
  n.cfg.orchHost = "cumulo";
  n.cfg.priority = "openai";
  n.scripts["cumulo"] = {{false, "", "network", 0, "", 0, 0}};
  n.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_TRUE(n.anyDelivered("cumulo is unavailable - switching to openai"));
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
      "That didn't finish - anthropic: mid-loop death. Nothing is still running; ask again to "
      "retry.",
      r.lastText().c_str());
  // The head arc is still freed on the error path (TurnGuard dtor).
  TEST_ASSERT_EQUAL(1, r.headEventsWith(Status::Offline));
}

// ---- (5b) a 429 names the window that refused (CUM-460) ---------------------
// Every keyed provider refuses with the same 429; the owner hears the window the
// provider named (the adapter's tag, else its own text), never a blanket "wait a
// minute". The old copy was wrong for a 0/min plan and a spent daily quota.
static void test_rate_limit_reply_names_the_window() {
  struct Case { const char* err; RateLimit want; };
  const Case cases[] = {
      {"conversations HTTP 429 [rl:day-utc]: Custom connector rate limit reached.",
       RateLimit::DailyUtc},
      {"chat HTTP 429 [rl:plan]: Requests rate limit exceeded", RateLimit::NotAllowed},
      {"resp HTTP 429: Rate limit reached for gpt-4o-mini on requests per day (RPD)",
       RateLimit::Daily},
      {"messages HTTP 429: This request would exceed the rate limit of 50 requests per "
       "minute.", RateLimit::PerMinute},
      {"conversations HTTP 429: Custom connector rate limit reached.", RateLimit::Unknown},
  };
  for (const Case& c : cases) {
    Rig r;
    Rig::Script limited;
    limited.ok = false;
    limited.err = c.err;
    for (const char* h : {"anthropic", "openai", "mistral"}) r.scripts[h] = {limited};
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
    // The same-host retry and the failover ladder still ran BEFORE the reply:
    // anthropic twice, then both alternates. Their switch notices are held and the
    // failed turn sends ONE message (L19-13 d).
    TEST_ASSERT_EQUAL_MESSAGE(4, (int)r.attempts.size(), c.err);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("switching to openai"), c.err);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), c.err);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(agent::rateLimitReply(c.want), r.lastText().c_str(), c.err);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("rate-limited this device"), c.err);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("That didn't finish -"), c.err);
  }
}

// ---- (5c) a billing refusal is not a rate limit (lane K) -------------------
// OpenAI's "you exceeded your current quota" / "no credits remaining" comes as a
// 429 but waiting never helps: it is a Billing refusal. No same-host retry (it is
// deterministic until credit is added), the ladder still tries the alternates
// with a notice that names the cause, and the reply says what to do.
static void test_billing_refusal_is_not_a_rate_limit() {
  for (const char* err : {"resp HTTP 429: You exceeded your current quota, please check your plan and "
                          "billing details.",
                          "resp HTTP 429: You have no credits remaining. Add credits to continue using the API.",
                          "messages HTTP 400: Your credit balance is too low to access the Anthropic API."}) {
    Rig r;
    Rig::Script refused;
    refused.ok = false;
    refused.err = err;
    for (const char* h : {"anthropic", "openai", "mistral"}) r.scripts[h] = {refused};
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)r.attempts.size(), err);   // no same-host retry
    TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("says the account is out of credit"), err);
    TEST_ASSERT_TRUE_MESSAGE(r.lastText().find("out of credit") != std::string::npos, err);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("wait"), err);
  }
}

// ---- (5d) the billing park holds on every non-fabric path (lane K, PG2) -------
// Lane K parks a host that refused for billing or auth and tells the owner once per
// episode, but only the fabric loop honoured it: the single-shot ladder (tool loop
// off, mid-turn failover off, a custom head) re-sent to the refusing host and
// re-notified on every turn. The class, over every recorded billing refusal, every
// head, both non-fabric paths, and one or two refusing hosts: three turns ask each
// refusing host once and tell the owner once per host, and every turn is answered.
static const char* const kBillingRefusals[] = {
    "messages HTTP 400: Your credit balance is too low to access the Anthropic API. Please go to "
    "Plans & Billing to upgrade or purchase credits.",
    "resp HTTP 429: You exceeded your current quota, please check your plan and billing details.",
    "resp HTTP 429: You have no credits remaining. Add credits to continue using the API.",
    "chat HTTP 402: Payment Required",
};

static void test_billing_park_holds_on_the_single_shot_ladder() {
  const char* const hosts[] = {"anthropic", "openai", "mistral"};
  int cases = 0;
  for (int h = 0; h < 3; h++) {
    for (const char* refusal : kBillingRefusals) {
      for (int loop = 0; loop < 2; loop++) {   // single shot, then the tool loop without the fabric
        for (int refusing = 1; refusing <= 2; refusing++) {   // the head, then the head and its first alternate
          Rig r;
          r.cfg.toolLoop = loop == 1;
          const std::string head = hosts[h], alt = hosts[(h + 1) % 3];
          r.cfg.priority = head + "," + alt + "," + hosts[(h + 2) % 3];
          Rig::Script refused;
          refused.ok = false;
          refused.err = refusal;
          r.scripts[head] = {refused};
          if (refusing == 2) r.scripts[alt] = {refused};
          for (int t = 0; t < 3; t++) r.eng->handleMessage("hi", "Roy", "1001");
          char msg[192];
          snprintf(msg, sizeof msg, "head %s, refusing %d, loop %d, refusal %.60s", head.c_str(),
                   refusing, loop, refusal);
          TEST_ASSERT_EQUAL_MESSAGE(1, r.attemptsOn(head), msg);
          TEST_ASSERT_EQUAL_MESSAGE(refusing == 2 ? 1 : 3, r.attemptsOn(alt), msg);
          TEST_ASSERT_EQUAL_MESSAGE(refusing, r.countDelivered("out of credit"), msg);
          TEST_ASSERT_EQUAL_MESSAGE(3, r.countDelivered("hello there"), msg);
          TEST_ASSERT_TRUE_MESSAGE(r.fabricCalls.empty(), msg);
          cases++;
        }
      }
    }
  }
  printf("property: %d (head, refusal, path, refusing hosts) cases, each refusing host asked "
         "once and told once over 3 turns\n", cases);
}

// The rest of the park's life on the ladder and the fold: a parked alternate is
// skipped, the fold never asks a parked host, the probe after kParkProbeMs is one
// request and a second refusal stays quiet, a new key ends the park (and the episode).
static void test_billing_park_lifecycle_on_the_ladder_and_the_fold() {
  Rig r;   // anthropic, openai, mistral; single shot
  r.cfg.toolLoop = false;
  Rig::Script refused;
  refused.ok = false;
  refused.err = kBillingRefusals[0];
  r.scripts["anthropic"] = {refused};
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL(1, r.countDelivered("out of credit"));
  TEST_ASSERT_TRUE(LogCapture::contains("fabric: anthropic parked (billing, HTTP 400)"));

  // A turn that starts past the parked head runs on a fresh thread, and the
  // /api/lastturn capture says so (not the parked head's held conversation).
  r.cfg.convId = "1001=anthropic|abc;";
  r.eng->handleMessage("thread", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts.back().host.c_str());
  TEST_ASSERT_FALSE(r.debug.back().convContinued);

  // The fold skips the parked host, and a fold that answers leaves the park alone.
  const std::vector<std::string> cands = r.eng->foldHostCandidates();
  TEST_ASSERT_EQUAL(2, (int)cands.size());
  TEST_ASSERT_EQUAL_STRING("openai", cands[0].c_str());
  std::string sum;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "digest", sum) == TurnEngine::FoldResult::Ok);
  TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));

  // A parked alternate is not a failover target: openai fails in transit (its one
  // retry), the ladder goes past the parked anthropic to mistral.
  Rig::Script down;
  down.ok = false;
  down.err = "network";
  r.scripts["openai"] = {down};
  r.scriptIdx["openai"] = 0;
  r.attempts.clear();
  r.eng->handleMessage("again", "Roy", "1001");
  TEST_ASSERT_EQUAL(0, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL(2, r.attemptsOn("openai"));
  TEST_ASSERT_EQUAL(1, r.attemptsOn("mistral"));
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: anthropic is parked - not a failover target"));
  r.scripts["openai"] = {Rig::Script()};
  r.scriptIdx["openai"] = 0;

  // After kParkProbeMs the next turn sends one probe; refused again, the host is
  // parked again and the owner is not told a second time in the same episode.
  r.plat.ms += agent::providers::kParkProbeMs;
  r.attempts.clear();
  r.eng->handleMessage("later", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL(1, r.countDelivered("out of credit"));
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: anthropic billing notice already given this episode"));
  r.attempts.clear();
  r.eng->handleMessage("and later", "Roy", "1001");
  TEST_ASSERT_EQUAL(0, r.attemptsOn("anthropic"));

  // A new key ends the park and the episode: the host is asked again and, still
  // refusing, the owner hears of the new episode once.
  r.cfg.keys["anthropic"] = "sk-other-ant";
  r.attempts.clear();
  r.eng->handleMessage("new key", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL(2, r.countDelivered("out of credit"));
  TEST_ASSERT_TRUE(LogCapture::contains("fabric: anthropic un-parked (its key changed)"));
}

// A probe that is answered ends the park and its episode: the host serves again, and
// a later refusal there is a new episode the owner hears about once.
static void test_billing_park_answered_probe_ends_the_episode() {
  Rig r;
  r.cfg.toolLoop = false;
  Rig::Script refused;
  refused.ok = false;
  refused.err = kBillingRefusals[0];
  r.scripts["anthropic"] = {refused, Rig::Script(), refused};
  r.eng->handleMessage("hi", "Roy", "1001");   // refused: parked, told
  r.plat.ms += agent::providers::kParkProbeMs;
  r.eng->handleMessage("later", "Roy", "1001");   // the probe is answered
  TEST_ASSERT_EQUAL(2, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts.back().host.c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("fabric: anthropic answered again - un-parked"));
  r.eng->handleMessage("again", "Roy", "1001");   // refused again: a new episode
  TEST_ASSERT_EQUAL(3, r.attemptsOn("anthropic"));
  TEST_ASSERT_EQUAL(2, r.countDelivered("out of credit"));
}

// A check that sends nothing (the fold's canFoldNow, run on every pump pass) may be
// the first to see kParkProbeMs pass; the probe is still one request, sent by the
// next turn, and a refusal re-parks the host for another kParkProbeMs.
static void test_billing_park_probe_is_one_request_whoever_checks_first() {
  Rig r;
  r.cfg.toolLoop = false;
  Rig::Script refused;
  refused.ok = false;
  refused.err = kBillingRefusals[0];
  r.scripts["anthropic"] = {refused};
  r.eng->handleMessage("hi", "Roy", "1001");
  r.plat.ms += agent::providers::kParkProbeMs;
  for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(r.eng->canFoldNow());   // pump passes, no fold due
  r.attempts.clear();
  for (int t = 0; t < 3; t++) r.eng->handleMessage("later", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));   // the probe, then parked again
  TEST_ASSERT_EQUAL(3, r.attemptsOn("openai"));
  TEST_ASSERT_EQUAL(1, r.countDelivered("out of credit"));
  TEST_ASSERT_EQUAL(2, (int)r.eng->foldHostCandidates().size());
}

// The latch is shared and a quiet turn leaves it armed, on the ladder as in the
// fabric: an episode another seam already told (the embedding notice) is not told
// again by a chat turn, and a refusal met by the nightly dream is told once by the
// next attended turn, which starts past the parked head.
static void test_billing_notice_latch_is_shared_and_kept_by_quiet_turns() {
  {
    Rig r;
    r.cfg.toolLoop = false;
    Rig::Script refused;
    refused.ok = false;
    refused.err = kBillingRefusals[0];
    r.scripts["anthropic"] = {refused};
    TEST_ASSERT_TRUE(orch::billingNoticeDue("anthropic"));   // the embedding seam told it
    r.eng->handleMessage("hi", "Roy", "1001");
    TEST_ASSERT_EQUAL(1, r.attemptsOn("anthropic"));
    TEST_ASSERT_EQUAL(0, r.countDelivered("out of credit"));
    TEST_ASSERT_EQUAL(1, r.countDelivered("hello there"));
  }
  Rig q;
  q.cfg.toolLoop = false;
  Rig::Script refused;
  refused.ok = false;
  refused.err = kBillingRefusals[0];
  q.scripts["anthropic"] = {refused};
  const orch::FireOutcome night = q.eng->injectScheduledTurn("1001", "dream", "Dream", "dream", true);
  TEST_ASSERT_TRUE(night.ok);
  TEST_ASSERT_EQUAL(0, q.countDelivered("out of credit"));   // nothing at night
  TEST_ASSERT_TRUE(q.eng->quietNoticesHeld() >= 1);
  q.eng->handleMessage("morning", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, q.attemptsOn("anthropic"));   // the night's one request, none since
  TEST_ASSERT_EQUAL(1, q.countDelivered("anthropic says the account is out of credit (HTTP 400)"));
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: anthropic is parked (billing) - starting on openai"));
  q.eng->handleMessage("and now", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, q.countDelivered("out of credit"));
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
  // CUM-483: `ask`/`memory` are optional now, so a turn missing them is a VALID
  // turn (applied, ok) - the salvage path is reserved for a turn with no usable
  // `reply`. Its job is unchanged: never hand a human raw wire JSON.
  Rig r;
  r.scripts["anthropic"] = {{true, "{\"ask\":\"Which repo?\",\"memory\":\"\",\"junk\":42}", "", 0}};
  const bool ok = r.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_FALSE(ok);   // parse failed (missing `reply`)
  TEST_ASSERT_EQUAL_STRING("Which repo?", r.lastText().c_str());   // salvaged text, NOT the JSON
  TEST_ASSERT_FALSE(r.anyDelivered("{"));
  TEST_ASSERT_TRUE(LogCapture::contains("orchestrator: turn parse fail"));

  Rig r2;
  r2.scripts["anthropic"] = {{true, "{\"nothing\":true}", "", 0}};
  r2.eng->runTurn("inputs", "1001", "");
  // Lane L: a plain cause line, not a "formatting error" that reads as a device bug.
  TEST_ASSERT_EQUAL_STRING(agent::kUnreadableNothingSalvaged, r2.lastText().c_str());
}

// CUM-483: the EXACT orch_turn claude-sonnet-5 returned on bench unit B (no
// `memory` key). It used to fail the parse, deliver only the salvaged reply,
// FAIL the turn and drop the scratchpad update; now the turn applies in full.
static const char* kL4AnthropicNoMemory =
    "{\"reply\":\"- **Sampling:** 3 cores per site at 15 monospecific *Posidonia oceanica* meadows\","
    "\"ask\":\"\",\"device\":[],\"mem_write\":[],\"mem_query\":[],\"session_ops\":[],"
    "\"scratchpad\":{\"active\":null,\"short\":[\"[complete] Verified Apostolaki et al. (2024) method\","
    "\"[complete] Delivered paragraph + 3-bullet versions to Omar\"],\"mid\":null,\"long\":null}}";

static void test_cum483_turn_without_memory_keeps_side_effects() {
  Rig r;
  r.adminChats = true;   // web chat = the owner (scratchpad writes are admin-only)
  r.scripts["anthropic"] = {{true, kL4AnthropicNoMemory, "", 0}};
  const bool ok = r.eng->runTurn("inputs", "web", "now in 3 bullet points");
  TEST_ASSERT_TRUE_MESSAGE(ok, "a turn without `memory` must not FAIL");
  TEST_ASSERT_FALSE(LogCapture::contains("orchestrator: turn parse fail"));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());   // no retry, no failover
  // The side effect the old parse failure threw away: the scratchpad update.
  TEST_ASSERT_EQUAL(1, (int)r.scratchApplied.size());
  TEST_ASSERT_TRUE(r.scratchApplied[0].hasShort);
  TEST_ASSERT_EQUAL(2u, r.scratchApplied[0].shortItems.size());
  // The reply reaches the owner, and the turn ends ok in the lifecycle hook.
  TEST_ASSERT_TRUE(r.anyDelivered("Sampling"));
  TEST_ASSERT_FALSE(r.anyDelivered("{"));
  TEST_ASSERT_EQUAL(1, (int)r.hookEnds.size());
  TEST_ASSERT_TRUE(r.hookEnds[0].ok);
}

// ---- CUM-486: [AVAILABLE MODELS] == what the spawn validator accepts -------

// The provider -> advertised models map parsed off the turn's [AVAILABLE MODELS]
// line ("" value = the "p: ;" shape the bug produced).
static std::map<std::string, std::string> advertisedModels(const std::string& inputs) {
  std::map<std::string, std::string> out;
  const std::string tag = "[AVAILABLE MODELS] (yours + sub-agents')";
  size_t b = inputs.find(tag);
  if (b == std::string::npos) return out;
  b += tag.size();
  const std::string line = inputs.substr(b, inputs.find('\n', b) - b);
  size_t s = 0;
  while (s < line.size()) {
    size_t semi = line.find(';', s);
    if (semi == std::string::npos) break;
    std::string item = line.substr(s, semi - s);
    s = semi + 1;
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    std::string p = item.substr(0, colon), m = item.substr(colon + 1);
    p.erase(0, p.find_first_not_of(' '));
    m.erase(0, m.find_first_not_of(' '));
    out[p] = m;
  }
  return out;
}

// The class rule over EVERY provider slot, with and without a live harvest: the
// prompt advertises exactly the derivation the spawn validator reads (the device
// wires both to nimbus::orch::effectiveModelChoices), so every advertised model
// spawns as named and no keyed provider is ever shown as "p: ;" (bench unit A:
// "zai: ;" made the head refuse Z.ai; o4-mini-deep-research was offered and then
// coerced away on every spawn).
static void test_cum486_advertised_models_equal_spawnable_for_every_slot() {
  for (int live = 0; live < 2; ++live) {
    Rig r;
    std::map<std::string, std::string> harvest;
    for (const orch::ProviderSlot& slot : orch::kProviderSlots) {
      r.cfg.keyed.insert(slot.slug);
      if (live) harvest[slot.slug] = std::string("live-") + slot.slug + "-1,live-" + slot.slug + "-2";
    }
    auto derive = [&harvest](const std::string& p) {
      auto it = harvest.find(p);
      return orch::effectiveModelChoices(p.c_str(), it == harvest.end() ? std::string() : it->second);
    };
    r.modelChoicesFn = derive;
    r.eng->handleMessage("which models can you use?", "Owner", "1001");
    TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
    const auto adv = advertisedModels(r.attempts[0].instructions);
    for (const orch::ProviderSlot& slot : orch::kProviderSlots) {
      auto it = adv.find(slot.slug);
      TEST_ASSERT_TRUE_MESSAGE(it != adv.end(), slot.slug);   // every keyed slot is listed
      TEST_ASSERT_FALSE_MESSAGE(it->second.empty(), slot.slug);   // never "p: ;"
      TEST_ASSERT_EQUAL_STRING_MESSAGE(derive(slot.slug).c_str(), it->second.c_str(), slot.slug);
      // ...and each advertised id passes the validator's own check.
      size_t s = 0;
      const std::string& csv = it->second;
      while (s <= csv.size()) {
        size_t c = csv.find(',', s);
        if (c == std::string::npos) c = csv.size();
        const std::string id = csv.substr(s, c - s);
        TEST_ASSERT_TRUE_MESSAGE(orch::modelInChoices(derive(slot.slug), id), id.c_str());
        s = c + 1;
      }
    }
  }
}

// The two configs bench unit A could not even test: a Z.ai-only and a Cumulo-only
// device. Each advertises its cold-start list (no harvest yet), nothing else, and
// a source that returns nothing still yields the model a spawn would run.
static void test_cum486_router_only_devices_advertise_their_models() {
  for (const char* only : {"zai", "cumulo"}) {
    Rig r;
    r.cfg.keyed = {only};
    r.cfg.routerHost = only;
    r.modelChoicesFn = [](const std::string& p) { return orch::effectiveModelChoices(p.c_str(), ""); };
    r.eng->handleMessage("hi", "Owner", "1001");
    TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
    const auto adv = advertisedModels(r.attempts[0].instructions);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)adv.size(), only);
    TEST_ASSERT_EQUAL_STRING(orch::staticModelChoices(only), adv.at(only).c_str());
    TEST_ASSERT_TRUE(r.attempts[0].instructions.find(std::string(" ") + only + ": ;") == std::string::npos);
  }
  // Belt and braces: a keyed provider whose list source is empty is shown with
  // its sub default (what a spawn there runs after coercion), never "zai: ;".
  Rig r;
  r.cfg.keyed = {"zai"};
  r.cfg.routerHost = "zai";
  r.modelChoicesFn = [](const std::string&) { return std::string(); };
  r.eng->handleMessage("hi", "Owner", "1001");
  TEST_ASSERT_TRUE(r.attempts[0].instructions.find("zai: sub-zai;") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].instructions.find("zai: ;") == std::string::npos);
}

// ---- CUM-485: the previous reply reaches the next stateless tool-loop turn ---

// Bench unit B, step 6b: gpt-5.6 head, tool loop on. The fabric loop keeps NO
// provider-side thread (convId ""), so the RECENT CONVERSATION window is the only
// place the next turn sees the previous reply - and the old 300 B row clip cut
// that reply's code before `stock_by_core` was defined. With the device's window
// builder at the head's derived budget and a device-sized tool registry, the
// whole 1.2 KB block must reach the instructions of the next (fabric) turn.
static void test_cum485_prev_reply_code_reaches_the_next_tool_loop_turn() {
  const std::string code = harness_test::kPrevReplyCodeSnippet;
  for (const char* host : {"openai", "anthropic"}) {
    Rig r;
    r.cfg.orchHost = host;
    r.cfg.orchModels[host] = std::string(host) == "openai" ? "gpt-5.6" : "claude-sonnet-5";
    r.cfg.midFail = true;   // the device default: loop turns run on the stateless fabric
    for (int i = 0; i < 42; ++i)   // a device-sized registry (~42 tools, ~309 B each)
      r.specs.push_back(orch::ToolRegistry::Spec{"tool_" + std::to_string(i), std::string(309, 'd'), "{}"});
    std::vector<orch::EpisodicMessage> rows(2);
    rows[0].role = "assistant"; rows[0].text = code;   // newest
    rows[1].role = "user";      rows[1].text = "Can you give me a Python snippet to parse a CSV?";
    const auto b = orch::deriveBudget(orch::modelCtxTokens(host, r.cfg.orchModels[host]), {});
    r.recentConversation = "\n## RECENT CONVERSATION (this chat, oldest first; the current "
                           "message is NOT here - it is in [USER])\n" +
                           agent::dream::buildConversationWindow(rows, b.recentConvBytes);
    r.eng->handleMessage("add a plot", "Omar", "web");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.fabricCalls.size(), host);   // stateless tool-loop turn
    TEST_ASSERT_EQUAL(1, (int)r.debug.size());
    const std::string& ins = r.debug[0].instructions;
    size_t s = 0;
    while (s <= code.size()) {
      size_t e = code.find('\n', s);
      if (e == std::string::npos) e = code.size();
      const std::string line = code.substr(s, e - s);
      if (!line.empty())
        TEST_ASSERT_TRUE_MESSAGE(ins.find(line) != std::string::npos, (std::string(host) + ": " + line).c_str());
      s = e + 1;
    }
    TEST_ASSERT_TRUE(ins.find("\n  print(stock_by_core)\n") != std::string::npos);   // line breaks kept
  }
}

// prism: the device sizes the conversation window for headHost(), which must be
// the very host runTurn dispatches on - including the router fallback on a
// Z.ai-only or Cumulo-only device.
static void test_head_host_is_the_host_the_turn_runs_on() {
  for (const char* only : {"zai", "cumulo", "openai"}) {
    Rig r;
    r.cfg.keyed = {only};
    if (std::string(only) != "openai") r.cfg.routerHost = only;
    const std::string h = r.eng->headHost();
    r.eng->handleMessage("hi", "Omar", "web");
    TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.attempts[0].host.c_str(), h.c_str(), only);
  }
}

// ---- CUM-495: a Z.ai prose reply is the answer, not a formatting error -------

// Bench unit B, step 7: the Z.ai head answered 200 with Markdown prose (the
// captured shape) and the owner got "I hit a formatting error ... please
// resend" instead. The prose must be delivered verbatim, the turn must end OK,
// and nothing may retry or fail over (only a genuine failure does that).
static void test_cum495_zai_prose_reply_is_delivered_not_apologized() {
  Rig r;
  r.cfg.keyed = {"zai"};
  r.cfg.routerHost = "zai";
  r.scripts["zai"] = {{true, harness_test::kZaiProseReply, "", 0}};
  r.eng->handleMessage("Translate the eddy covariance explanation to Arabic", "Omar", "web");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());   // no same-host retry, no failover
  TEST_ASSERT_EQUAL_STRING("zai", r.attempts[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING(harness_test::kZaiProseReply, r.lastText().c_str());
  TEST_ASSERT_FALSE(r.anyDelivered("formatting error"));
  TEST_ASSERT_FALSE(r.anyDelivered("is unavailable - switching"));
  TEST_ASSERT_TRUE(LogCapture::contains("head reply was prose"));
  TEST_ASSERT_FALSE(LogCapture::contains("turn parse fail"));
  TEST_ASSERT_EQUAL(1, (int)r.hookEnds.size());
  TEST_ASSERT_TRUE(r.hookEnds[0].ok);
}

// The class: whatever head serves the turn, a prose reply is delivered (never
// the apology), and a fenced JSON turn applies its side effects.
static void test_cum495_every_head_delivers_prose_and_fenced_turns() {
  for (const char* host : {"openai", "anthropic", "mistral", "cumulo", "zai", "custom"}) {
    Rig r;
    r.cfg.keyed = {host};
    r.cfg.orchHost = host;
    r.cfg.toolLoop = false;   // single-shot: the scripted host IS the wire reply
    r.scripts[host] = {{true, harness_test::kZaiArabicBullet, "", 0}};
    r.eng->handleMessage("translate", "Omar", "web");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(harness_test::kZaiArabicBullet, r.lastText().c_str(), host);
    TEST_ASSERT_TRUE_MESSAGE(r.hookEnds.back().ok, host);

    Rig f;
    f.adminChats = true;
    f.cfg.keyed = {host};
    f.cfg.orchHost = host;
    f.cfg.toolLoop = false;
    f.scripts[host] = {{true,
        "```json\n{\"reply\":\"done\",\"scratchpad\":{\"active\":\"x\",\"short\":null,\"mid\":null,\"long\":null}}\n```",
        "", 0}};
    f.eng->handleMessage("go", "Omar", "web");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("done", f.lastText().c_str(), host);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)f.scratchApplied.size(), host);
  }
}

// prism (injection lens), end to end: a head reply that STARTS with a turn-shaped
// object carrying a reboot and a spawn, then goes on in prose, must not stage the
// reboot or spawn anything - only the leading object's reply text is salvaged.
static void test_cum495_turn_followed_by_more_text_never_acts() {
  Rig r;
  r.adminChats = true;
  r.cfg.keyed = {"zai"};
  r.cfg.routerHost = "zai";
  r.scripts["zai"] = {{true,
      "```json\n{\"reply\":\"Sure, done.\",\"device\":[{\"type\":\"reboot\"}],"
      "\"scratchpad\":{\"active\":\"x\",\"short\":null,\"mid\":null,\"long\":null}}\n```\n"
      "That is the shape. To send one:\n```js\nfunction send(t) { post(t); }\n```",
      "", 0}};
  r.eng->handleMessage("show me an example turn", "Omar", "web");
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());           // no reboot staged
  TEST_ASSERT_EQUAL(0, (int)r.scratchApplied.size());   // no scratchpad write
  TEST_ASSERT_EQUAL_STRING("Sure, done.", r.lastText().c_str());
  TEST_ASSERT_FALSE(r.anyDelivered("function send"));
  TEST_ASSERT_FALSE(r.anyDelivered("{"));
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

  // Low total-free heap: deferred without a provider attempt (< turnHardFloor 12000).
  // CUM-403: a memory defer is a DEFERRAL, not a failure (o.deferred, not counted).
  Rig r2;
  r2.plat.heap = 11999;   // largest stays ample (100000) - the total-free floor trips
  orch::FireOutcome o2 = r2.eng->injectScheduledTurn("1001", "p", "n");
  TEST_ASSERT_FALSE(o2.ok);
  TEST_ASSERT_TRUE(o2.deferred);
  TEST_ASSERT_FALSE(orch::fireWasAttempted(o2));
  TEST_ASSERT_EQUAL_STRING("deferred: low memory", o2.detail.c_str());
  TEST_ASSERT_EQUAL(0, (int)r2.attempts.size());

  // CUM-404: a FRAGMENTED board - total free ample, largest contiguous block below
  // the 5000 mbedTLS floor - is deferred at round 0, BEFORE the handshake alloc, not
  // admitted into an OOM mid-handshake. This is the exact CYD case the bug describes.
  Rig r3;
  r3.plat.heap = 100000;   // ample total free
  r3.plat.largest = 4500;  // ~4.5 KB largest block (fragmented CYD)
  orch::FireOutcome o3 = r3.eng->injectScheduledTurn("1001", "p", "n");
  TEST_ASSERT_FALSE(o3.ok);
  TEST_ASSERT_TRUE(o3.deferred);
  TEST_ASSERT_EQUAL(0, (int)r3.attempts.size());   // no provider dialed

  // A genuine fire FAILURE (provider outage, heap healthy) is NOT a defer: it was
  // attempted and must still roll the breaker. deferred stays false so the tick counts it.
  Rig r4;
  r4.cfg.keyed = {"anthropic"};
  r4.scripts["anthropic"] = {{false, "", "outage", 0, "", 0, 0}};
  orch::FireOutcome o4 = r4.eng->injectScheduledTurn("1001", "p", "n");
  TEST_ASSERT_FALSE(o4.ok);
  TEST_ASSERT_FALSE(o4.deferred);
  TEST_ASSERT_TRUE(orch::fireWasAttempted(o4));
  TEST_ASSERT_TRUE(r4.attempts.size() >= 1);   // provider WAS dialed (retry may add more)
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

// ---- CUM-523: a reminder on a Telegram-less device stays in its own chat -----
// Bench (nimbus-light, T15, no Telegram bot): the web reminder's wakeup turn
// called reply_telegram, got "no allowed Telegram target", and the reminder ended
// "I couldn't send a separate Telegram alert". With no bot the wakeup turn is
// never offered the Telegram tool or any Telegram hint, and it is told its reply
// (to the web chat that set it) IS the reminder.
static void test_cum523_telegramless_wakeup_has_no_telegram_leg() {
  Rig r;
  r.cfg.telegramOn = false;
  r.specs.push_back(orch::ToolRegistry::Spec{"reply.telegram", "Send a Telegram message.", "{}"});
  r.specs.push_back(orch::ToolRegistry::Spec{"files.send", "Send a stored file on Telegram.", "{}"});
  r.specs.push_back(orch::ToolRegistry::Spec{"reply.speak", "Speak a line.", "{}"});
  r.scripts["anthropic"] = {{true, "{\"reply\":\"Time to ring the crane company.\"}", "", 0}};
  orch::FireOutcome o = r.eng->injectScheduledTurn(
      "web", "Remind Dana now: ring the crane company about the 20 Oct delivery.", "wakeup",
      "lp11566d", /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(1, (int)r.debug.size());
  const std::string& sys = r.debug[0].instructions;
  const std::string& in = r.attempts[0].inputs;
  TEST_ASSERT_TRUE(sys.find("reply_telegram") == std::string::npos);   // not advertised
  TEST_ASSERT_TRUE(sys.find("files_send") == std::string::npos);       // nor the Telegram file send
  TEST_ASSERT_TRUE(sys.find("reply_speak") != std::string::npos);      // the rest still are
  TEST_ASSERT_TRUE(in.find("reply.telegram") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("(Your reply is delivered to the web chat. Do not send a second copy "
                           "over another messaging channel.)") != std::string::npos);
  TEST_ASSERT_EQUAL(0, (int)r.toolResults.size());   // nothing was even attempted on Telegram
  for (const auto& d : r.delivered) TEST_ASSERT_TRUE(d.second.find("Telegram") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("web", r.delivered.back().first.c_str());
}

// Counter-test: with a bot configured the tool stays advertised (a Telegram owner
// keeps reply.telegram), and an ordinary web message still gets the full hint.
static void test_cum523_telegram_device_keeps_the_tool() {
  Rig r;
  r.specs.push_back(orch::ToolRegistry::Spec{"reply.telegram", "Send a Telegram message.", "{}"});
  r.eng->handleMessage("hi", "Dana", "web");
  TEST_ASSERT_TRUE(r.debug[0].instructions.find("reply_telegram") != std::string::npos);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("reply.telegram is ONLY for messaging") !=
                   std::string::npos);

  Rig w;   // the same web message on a Telegram-less device: no Telegram anywhere
  w.cfg.telegramOn = false;
  w.specs.push_back(orch::ToolRegistry::Spec{"reply.telegram", "Send a Telegram message.", "{}"});
  w.eng->handleMessage("hi", "Dana", "web");
  TEST_ASSERT_TRUE(w.debug[0].instructions.find("reply_telegram") == std::string::npos);
  TEST_ASSERT_TRUE(w.attempts[0].inputs.find("reply.telegram") == std::string::npos);
}

// CUM-523 helpers: the Telegram-only tool names (registry and wire spellings), and
// a rig in public mode with no allowlisted chat (deliverable only from a Telegram
// turn) that registers both Telegram-only tools.
static const char* const kTelegramOnlyNames[] = {"files.send", "reply.telegram", "files_send",
                                                 "reply_telegram"};
static void addTelegramSpecs(Rig& r) {
  r.specs.push_back(orch::ToolRegistry::Spec{"reply.telegram", "Send a Telegram message.", "{}"});
  r.specs.push_back(orch::ToolRegistry::Spec{"files.send", "Send a stored file.", "{}"});
}
static void publicNoAllowlist(Rig& r) {
  r.cfg.telegramPublic = true;
  r.cfg.telegramAllowlist = "";
  addTelegramSpecs(r);
}

// CUM-523: the Telegram predicate reads live device state, so sampling it
// several times per turn let the hint and the tool list disagree when the owner
// changed the allowlist mid-turn. ONE sample per turn: a predicate that answers
// true once and false afterwards must leave hint, tool list and compose wording
// all on the first answer, on every turn entry.
static void test_cum523_telegram_is_sampled_once_per_turn() {
  int calls = 0;
  Rig r;
  // The turn's own chat is asked once; "" (is a chat allowlisted, for files.send)
  // is a different question and is not counted.
  r.cfg.telegramFn = [&calls](const std::string& c) { return c.empty() || ++calls == 1; };
  addTelegramSpecs(r);
  r.eng->handleMessage("hi", "Dana", "web");
  TEST_ASSERT_EQUAL(1, calls);
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find("reply.telegram is ONLY for messaging") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(r.debug[0].instructions.find("reply_telegram") != std::string::npos);

  // A scheduled one-shot: the delivery line and the tool list share the sample.
  Rig s;
  calls = 0;
  s.cfg.telegramFn = [&calls](const std::string& c) { return c.empty() || ++calls == 1; };
  addTelegramSpecs(s);
  s.eng->injectScheduledTurn("1001", "stretch", "wakeup", "lp1", /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_EQUAL(1, calls);
  TEST_ASSERT_TRUE(s.attempts[0].inputs.find("(Your reply is delivered to this Telegram chat.") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(s.debug[0].instructions.find("reply_telegram") != std::string::npos);

  // The synthesis turn and a direct runTurn sample once too.
  Rig y;
  calls = 0;
  y.cfg.telegramFn = [&calls](const std::string& c) { calls += c.empty() ? 0 : 1; return false; };
  y.jobs->addFreshResult("job0000", "modelX", "findings");
  y.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(1, calls);
  calls = 0;
  y.eng->runTurn("inputs", "1001", "");
  TEST_ASSERT_EQUAL(1, calls);
}

// The per-turn rule as a truth table (the device answers its predicate with it).
static void test_cum523_deliverability_rule() {
  using agent::telegramDeliverableFor;
  TEST_ASSERT_FALSE(telegramDeliverableFor(false, true, true, "5550001"));   // no bot, nothing
  TEST_ASSERT_TRUE(telegramDeliverableFor(true, true, false, "web"));       // an allowlisted target
  TEST_ASSERT_TRUE(telegramDeliverableFor(true, false, true, "5550001"));   // public: its own chat
  for (const char* dev : {"web", "voice", "serial", ""})
    TEST_ASSERT_FALSE_MESSAGE(telegramDeliverableFor(true, false, true, dev), dev);
  TEST_ASSERT_FALSE(telegramDeliverableFor(true, false, false, "5550001"));
  TEST_ASSERT_TRUE(agent::isTelegramChat("5550001"));
  for (const char* dev : {"web", "voice", "serial", ""}) TEST_ASSERT_FALSE(agent::isTelegramChat(dev));
}

// CUM-523: public mode with an empty allowlist. A web, voice or serial turn has
// no Telegram chat it may deliver to, so it is not offered reply.telegram or
// files.send (and the reminder fired into the web chat is not either); a turn that
// came in over Telegram in that mode still is - its own chat is the target.
static void test_cum523_public_mode_empty_allowlist_is_decided_per_turn() {
  for (const char* chat : {"web", "voice", "serial"}) {
    Rig r;
    publicNoAllowlist(r);
    r.eng->handleMessage("hi", "Dana", chat);
    const std::string& sys = r.debug[0].instructions;
    TEST_ASSERT_TRUE_MESSAGE(sys.find("reply_telegram") == std::string::npos, chat);
    TEST_ASSERT_TRUE_MESSAGE(sys.find("files_send") == std::string::npos, chat);
    TEST_ASSERT_TRUE_MESSAGE(r.attempts[0].inputs.find("reply.telegram") == std::string::npos, chat);
  }
  Rig w;   // the reminder set in the web chat fires into the web chat
  publicNoAllowlist(w);
  w.eng->injectScheduledTurn("web", "stretch", "reminder", "lp1", false, true, /*ownerReminder=*/true);
  TEST_ASSERT_TRUE(w.debug[0].instructions.find("reply_telegram") == std::string::npos);
  TEST_ASSERT_TRUE(w.debug[0].instructions.find("files_send") == std::string::npos);
  TEST_ASSERT_TRUE(w.attempts[0].inputs.find("(Your reply is delivered to the web chat.") !=
                   std::string::npos);

  Rig t;   // a guest's Telegram turn in the same mode keeps its own chat
  publicNoAllowlist(t);
  t.eng->handleMessage("hi", "Guest", "5550001");
  TEST_ASSERT_TRUE(t.debug[0].instructions.find("reply_telegram") != std::string::npos);
}

// CUM-523 gate: public mode with an empty allowlist. A guest's Telegram turn may
// answer its own chat with reply.telegram, but files.send delivers only to an
// allowlisted chat (its handler refuses any other), so that turn is not offered
// files.send, and neither the capability line nor a built-in served on it names it.
static void test_cum523_public_guest_turn_gets_reply_but_no_file_send() {
  Rig t;
  publicNoAllowlist(t);
  t.hw.files = t.hw.sd = t.hw.wifi = true;
  t.hw.telegram = true;
  t.eng->handleMessage("send me the report as a PDF", "Guest", "5550001");
  TEST_ASSERT_TRUE(t.eng->turnTelegram());
  TEST_ASSERT_FALSE(t.eng->turnFileSend());
  const std::string& sys = t.debug[0].instructions;
  TEST_ASSERT_TRUE(sys.find("reply_telegram") != std::string::npos);
  TEST_ASSERT_TRUE(sys.find("files_send") == std::string::npos);
  const std::string prompt = sys + "\n" + t.debug[0].inputs;
  TEST_ASSERT_TRUE_MESSAGE(prompt.find("files.send") == std::string::npos, "the capability line");
  for (size_t i = 0; i < agent::builtinSkillCount(); ++i) {
    const std::string body =
        agent::builtinSkillBody(agent::builtinSkill(i).id, t.eng->turnTelegram(), t.eng->turnFileSend());
    TEST_ASSERT_TRUE_MESSAGE(body.find("files.send") == std::string::npos, agent::builtinSkill(i).id);
  }
  // ...while the playbooks that name reply.telegram still do (the tool list offers it).
  TEST_ASSERT_TRUE(agent::builtinSkillBody("architecture", true, false).find("reply.telegram") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(agent::builtinSkillBody("tools-guide", true, false).find("reply.telegram") !=
                   std::string::npos);
  // Counter-test: with a chat allowlisted the same guest turn keeps files.send.
  Rig a;
  a.cfg.telegramPublic = true;
  addTelegramSpecs(a);
  a.hw = t.hw;
  a.eng->handleMessage("send me the report as a PDF", "Guest", "5550001");
  TEST_ASSERT_TRUE(a.eng->turnFileSend());
  TEST_ASSERT_TRUE(a.debug[0].instructions.find("files_send") != std::string::npos);
  // The rule for no chat ("") is files.send's own: the bot runs and a chat is
  // allowlisted. The device text served mid-turn follows the files.send sample.
  TEST_ASSERT_FALSE(agent::telegramDeliverableFor(true, false, true, ""));
  TEST_ASSERT_FALSE(agent::telegramDeliverableFor(false, true, true, ""));
  TEST_ASSERT_TRUE(agent::telegramDeliverableFor(true, true, true, ""));
  for (const char* src : {"src/agent/skills.cpp", "src/agent/files_subsystem.cpp"}) {
    FILE* f = std::fopen(src, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, src);
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    TEST_ASSERT_TRUE_MESSAGE(text.find("agent::orchestrator::fileSendForTurn()") != std::string::npos, src);
  }
}

// CUM-523: on a turn with no Telegram, nothing the engine composes names a
// Telegram-only tool - the tool list, the channel hint, the [SKILLS] index (the
// device's real built-ins), the capability line (SD files bullet) - and every
// built-in body served on that turn (skill.get) follows the same sample. (The rig
// stubs the connector catalog; its own wording is pinned in test_connectors_wire.)
static void test_cum523_telegramless_prompt_names_no_telegram_tool() {
  Rig r;
  r.cfg.telegramOn = false;
  r.hw.files = r.hw.sd = r.hw.speaker = r.hw.voiceReplies = r.hw.wifi = true;
  r.hw.telegram = true;   // the bot "runs" in the manifest; the turn's sample says no
  addTelegramSpecs(r);
  r.specs.push_back(orch::ToolRegistry::Spec{"reply.speak", "Read text aloud.", "{}"});
  r.eng->handleMessage("make me a PDF report", "Dana", "web");
  TEST_ASSERT_FALSE(r.eng->turnTelegram());
  const std::string prompt = r.debug[0].instructions + "\n" + r.debug[0].inputs;
  for (const char* t : kTelegramOnlyNames)
    TEST_ASSERT_TRUE_MESSAGE(prompt.find(t) == std::string::npos, t);
  TEST_ASSERT_TRUE(prompt.find("- deliver-pdf: ") != std::string::npos);   // the index still rides
  TEST_ASSERT_TRUE(prompt.find("Connectivity: WiFi.\n") != std::string::npos);   // no Telegram
  for (size_t i = 0; i < agent::builtinSkillCount(); ++i) {
    const std::string body = agent::builtinSkillBody(agent::builtinSkill(i).id, r.eng->turnTelegram());
    for (const char* t : kTelegramOnlyNames)
      TEST_ASSERT_TRUE_MESSAGE(body.find(t) == std::string::npos, agent::builtinSkill(i).id);
  }

  // Counter-test: the same turn with a deliverable bot keeps every Telegram name.
  Rig b;
  b.hw = r.hw;
  b.specs = r.specs;
  b.eng->handleMessage("make me a PDF report", "Dana", "web");
  TEST_ASSERT_TRUE(b.eng->turnTelegram());
  TEST_ASSERT_TRUE(b.debug[0].instructions.find("files_send") != std::string::npos);
  TEST_ASSERT_TRUE(b.debug[0].instructions.find("deliver one to the owner with files.send") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(agent::builtinSkillBody("deliver-pdf", b.eng->turnTelegram()).find("files.send") !=
                   std::string::npos);
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
  // The owner gets the HUMAN rendering (CUM-480): preamble + title + findings,
  // none of the model-facing seed scaffolding.
  const std::string& raw = r2.delivered.back().second;
  TEST_ASSERT_EQUAL_STRING(
      "Here are the raw findings; the summary step did not complete.\n"
      "\nmodelY\nprecious result\n",
      raw.c_str());
  TEST_ASSERT_TRUE(raw.find("[FRESH RESULTS]") == std::string::npos);
  TEST_ASSERT_TRUE(raw.find("job0001") == std::string::npos);

  // Heap gate: deferral leaves the fresh results parked (the 60 s fallback's case).
  Rig r3;
  r3.jobs->addFreshResult("job0002", "modelZ", "kept");
  r3.plat.heap = 13999;   // < autoTurnMinHeap (14000); largest ample
  r3.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(0, (int)r3.attempts.size());
  TEST_ASSERT_TRUE(r3.jobs->hasFreshResults());
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: defer auto-synthesis (heap 13999/floor 14000 largest 100000/floor 5000)"));

  // CUM-404: the synthesis turn dials a provider too, so a fragmented board (total
  // free ample, largest block below the mbedTLS floor) must defer here as well - the
  // fresh results stay parked, no unattended turn OOMs mid-handshake.
  Rig r4;
  r4.jobs->addFreshResult("job0003", "modelW", "still kept");
  r4.plat.heap = 100000;   // ample total free
  r4.plat.largest = 4000;  // fragmented: below ORCH_TURN_MIN_LARGEST_BLOCK (5000)
  r4.eng->maybeConsolidate("1001");
  TEST_ASSERT_EQUAL(0, (int)r4.attempts.size());
  TEST_ASSERT_TRUE(r4.jobs->hasFreshResults());
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: defer auto-synthesis (heap 100000/floor 14000 largest 4000/floor 5000)"));
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

// L19-28: the /api/lastturn head tells the truth about what the model saw. A
// legacy single-shot OpenAI turn on a stored response chain is CONTINUED (the
// counter-test: the honesty fix must not hide a real chain); a stored stateless
// marker ("messages") holds nothing on the provider, so it is no chain; and the
// device history line is measured on the prompt the engine composed.
static void test_lastturn_head_names_the_chain_and_the_device_history() {
  Rig r;
  r.cfg.keyed = {"openai"};
  r.cfg.priority = "openai";
  r.cfg.convId = "1001=openai|resp_7;";
  r.eng->runTurn("the inputs", "1001", "");
  TEST_ASSERT_EQUAL(1, (int)r.debug.size());
  TEST_ASSERT_EQUAL_STRING("openai", r.debug[0].host.c_str());
  TEST_ASSERT_TRUE(r.debug[0].convContinued);
  TEST_ASSERT_TRUE(r.debug[0].header.find(
                       "provider chain: CONTINUED - the provider held history from prior turns\n") !=
                   std::string::npos);

  Rig s;
  s.cfg.convId = "1001=anthropic|messages;";   // what an Anthropic single shot stores
  s.recentConversation = "\n## RECENT CONVERSATION (this chat, oldest first)\n- user: earlier\n"
                         "- assistant: hello\n";
  s.eng->runTurn("the inputs", "1001", "");
  TEST_ASSERT_EQUAL(1, (int)s.debug.size());
  TEST_ASSERT_FALSE(s.debug[0].convContinued);
  TEST_ASSERT_TRUE(s.debug[0].header.find("provider chain: none (single shot)\n") != std::string::npos);
  TEST_ASSERT_TRUE(s.debug[0].header.find("FRESH") == std::string::npos);
  TEST_ASSERT_EQUAL(2, s.debug[0].history.windowRows);
  TEST_ASSERT_TRUE(s.debug[0].header.find("recent window 2 rows / ") != std::string::npos);
  // The legend names the window section the prompt carries.
  TEST_ASSERT_TRUE(s.debug[0].header.find("per-turn  ## RECENT CONVERSATION (this chat, oldest first)") !=
                   std::string::npos);
}

// ---- golden helpers (same contract as test_harness_compose) -----------------
static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}
static bool readWholeFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
static void writeWholeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
static void checkTextGolden(const char* name, const std::string& current) {
  const std::string golden = std::string("test/golden/") + name;
  if (blessMode()) {
    writeWholeFile(golden, current);
    return;
  }
  std::string blessed;
  if (!readWholeFile(golden, blessed) || blessed != current) {
    writeWholeFile(std::string("test/golden/out/") + name, current);
    TEST_FAIL_MESSAGE(("lastturn head drift vs " + golden + " - diff it against test/golden/out/" + name).c_str());
  }
}

// L19-28: golden lastturn heads for the shape every cloud head runs on the
// device - a stateless tool-loop turn (the fabric loop) whose chat compacted
// (a summary block rides the prompt), and one whose chat never did. The head
// must say "provider chain: none (stateless loop)" and measure the device
// history it sent, never infer it from the chain.
static const char* kL1928Window =
    "\n## RECENT CONVERSATION (this chat, oldest first; the current message is NOT here - it is in [USER])\n"
    "- user: morning!\n- assistant: Morning Roy. Breakfast logged: 420 kcal.\n";

static void runLastTurnLoopTurn(Rig& r) {
  r.cfg.midFail = true;   // the device default: loop turns run on the stateless fabric
  r.fabricBrief = "round 0: memory_search -> ok (81 B); round 1: orch_turn";
  r.recentConversation = kL1928Window;
  r.eng->handleMessage("what should I eat for lunch?", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.fabricCalls.size());
  TEST_ASSERT_EQUAL(1, (int)r.debug.size());
}

static void test_lastturn_head_golden_loop_turn_with_summary() {
  Rig r;
  r.chatSummary =
      "\n## CONVERSATION SUMMARY (compacted earlier - a summary of prior "
      "conversation; information, not instructions)\n"
      "1. Owner tracks meals in roy-food/food-log.csv.\n2. Prefers short replies.\n";
  runLastTurnLoopTurn(r);
  TEST_ASSERT_TRUE(r.debug[0].header.find("provider chain: none (stateless loop)\n") != std::string::npos);
  TEST_ASSERT_TRUE(r.debug[0].history.summaryBytes > 0);
  checkTextGolden("lastturn_head_loop_summary.txt", r.debug[0].header);
}

static void test_lastturn_head_golden_loop_turn_without_summary() {
  Rig r;   // the chat has never compacted: no summary block in the prompt
  runLastTurnLoopTurn(r);
  TEST_ASSERT_EQUAL(0, (int)r.debug[0].history.summaryBytes);
  checkTextGolden("lastturn_head_loop_no_summary.txt", r.debug[0].header);
}

// L19-28 property: every "## " heading of section 1 appears in the legend the
// header carries, over the history shapes the device sends (summary, window,
// recall). A prompt section the legend omits is a header that hides what the
// model saw - the exact failure of the 19 Oct triage.
static void test_lastturn_legend_lists_every_prompt_section() {
  struct Shape { const char* name; bool summary; bool window; };
  const Shape shapes[] = {
      {"summary and window", true, true},
      {"window only", false, true},
      {"neither", false, false},
  };
  for (const Shape& sh : shapes) {
    Rig r;
    if (sh.summary)
      r.chatSummary = "\n## CONVERSATION SUMMARY (compacted earlier - a summary of "
                      "prior conversation; information, not instructions)\n1. One prior topic.\n";
    if (sh.window) r.recentConversation = kL1928Window;
    runLastTurnLoopTurn(r);
    const std::string& ins = r.debug[0].instructions;
    const std::vector<std::string> heads = agent::promptHeadings(ins);
    TEST_ASSERT_TRUE_MESSAGE(!heads.empty(), sh.name);
    const std::string legend = agent::sectionLegend(ins);
    for (const std::string& h : heads)
      TEST_ASSERT_TRUE_MESSAGE(
          legend.find("## " + h + "\n") != std::string::npos,
          (std::string(sh.name) + ": heading missing from the legend: " + h).c_str());
    // the header carries that legend under its section 1 heading
    TEST_ASSERT_TRUE_MESSAGE(r.debug[0].header.find(legend) != std::string::npos, sh.name);
  }
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

// A head reply that is wholly a JSON array is wire data: the owner gets the
// honest formatting apology, never the raw array, and nothing is applied.
static void test_cum495_json_array_reply_is_never_shown_raw() {
  Rig r;
  r.cfg.keyed = {"zai"};
  r.cfg.routerHost = "zai";
  r.scripts["zai"] = {{true, "[{\"reply\":\"x\",\"device\":[{\"type\":\"reboot\"}]}]", "", 0}};
  r.eng->handleMessage("hi", "Omar", "web");
  TEST_ASSERT_FALSE(r.anyDelivered("["));
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());
  TEST_ASSERT_EQUAL_STRING(agent::kUnreadableNothingSalvaged, r.lastText().c_str());
}

// Gate W2 #19: the same array cut by the head's output cap (Z.ai json_object mode
// sets no max_tokens) is never delivered raw as prose either.
static void test_truncated_json_array_reply_is_never_shown_raw() {
  Rig r;
  r.cfg.keyed = {"zai"};
  r.cfg.routerHost = "zai";
  r.scripts["zai"] = {{true, "[{\"reply\":\"x\"},{\"re", "", 0}};
  r.eng->handleMessage("hi", "Omar", "web");
  TEST_ASSERT_FALSE(r.anyDelivered("["));
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());
  TEST_ASSERT_EQUAL_STRING(agent::kCutNothingSalvaged, r.lastText().c_str());   // it was cut
}

// prism (logic lens): a ladder of LIVE attempts (Z.ai head request up to 120 s,
// its same-host retry, then a failover) can outlast "loop deadline + 120 s" in
// total. Each attempt re-arms the reaper, so it never reaps a turn that is still
// making progress - only a turn task that stopped.
static void test_reaper_spares_a_live_retry_and_failover_ladder() {
  Rig r;
  r.cfg.keyed = {"zai", "openai"};
  r.cfg.orchHost = "zai";
  r.cfg.priority = "zai,openai";
  r.cfg.toolLoop = false;
  r.scripts["zai"] = {{false, "", "network", 0}};
  bool reaped = false;
  r.onProviderCall = [&] {
    r.plat.ms += 300000u;   // each attempt takes 5 min of wall clock
    reaped |= r.eng->reapStuckTurn(r.plat.ms);
  };
  r.eng->handleMessage("hi", "Omar", "web");
  TEST_ASSERT_EQUAL(3, (int)r.attempts.size());   // zai, zai retry, openai failover
  TEST_ASSERT_EQUAL_STRING("openai", r.attempts[2].host.c_str());
  TEST_ASSERT_FALSE_MESSAGE(reaped, "the reaper fired on a live attempt");
  TEST_ASSERT_TRUE(r.hookEnds.back().ok);
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
  r.plat.heap = 13000;   // below autoTurnMinHeap (14000)
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  r.plat.heap = 100000;
  // CUM-404: the fold dials a provider too - a fragmented board (total free ample,
  // largest block below 5000) DEFERS on the largest-block floor, no provider call.
  r.plat.largest = 4200;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "d", sum) == FR::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  r.plat.largest = 100000;
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
  r.plat.heap = 13000;                       // below autoTurnMinHeap (14000)
  TEST_ASSERT_FALSE(r.eng->canFoldNow());
  r.plat.heap = 100000;
  r.plat.largest = 4800;                     // CUM-404: fragmented (largest < 5000) also blocks
  TEST_ASSERT_FALSE(r.eng->canFoldNow());
  r.plat.largest = 100000;
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

// CUM-530: the Cumulo picker saves the router default as "openai/gpt-5.6". The
// turn's budget line (the window /api/lastturn and the errlog show) must read the
// gpt-5.x window, not the 100K default the raw selector used to fall to.
static void test_cum530_router_selector_head_gets_its_real_window() {
  for (const char* sel : {"openai/gpt-5.6", "gpt-5.6"}) {
    Rig r;
    r.cfg.keyed.clear();
    r.cfg.routerKeyed = true;
    r.cfg.routerHost = "cumulo";
    r.cfg.orchModels["cumulo"] = sel;
    r.scripts["cumulo"] = {{true, kGoodTurn, "", 0, "c1", 50, 10}};
    LogCapture::lines().clear();
    TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("budget: ctx=272K"), sel);
    TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("budget: ctx=100K"), sel);
  }
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
  // ...under the router's model key ("<upstream>/<bare>"; a bare selector is openai).
  TEST_ASSERT_EQUAL_STRING("openai/model-cumulo", r.cfg.recorded[0].model.c_str());
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

// ---- (14) CUM-536: an unverified connector result closes memory for its turn --
// T02b: the synthesis turn read a Mistral sub's invented calendar week and stored
// it as memory mb82a0935. With the result flagged (its connector never ran), the
// synthesis input explains the marker, the loop's memory_write and the turn's
// mem_write are both refused, and the NEXT turn hears why and can store again.
static void test_unverified_connector_result_closes_memory_for_that_turn() {
  Rig r;
  r.specs.push_back(orch::ToolRegistry::Spec{"memory.write", "store a fact", "{}"});
  const std::string withWrite =
      "{\"reply\":\"Your week\",\"memory\":\"\",\"ask\":\"\","
      "\"mem_write\":[{\"content\":\"Dana's week: Team Stand-up Mon 09:00\","
      "\"importance\":0.5,\"permanent\":false}]}";
  Rig::Script synth;
  synth.outJson = withWrite;
  synth.dispatchTools = 1;
  synth.toolName = "memory_write";
  r.scripts["anthropic"] = {synth};
  r.jobs->addFreshResult("job0000", "gcal-fetch",
                         nimbus::orch::connectorNotCalledNote("Google Calendar") +
                             "\n\nMon 12 Oct 09:00 Team Stand-up",
                         "", /*failed=*/false, /*unverified=*/true);
  r.eng->maybeConsolidate("web");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_TRUE(r.attempts[0].inputs.find(
      "A result above opens with a note that its connector was NOT called") != std::string::npos);
  // Mid-loop memory_write: refused before the registry is ever reached.
  TEST_ASSERT_EQUAL(1, (int)r.toolResults.size());
  TEST_ASSERT_TRUE(r.toolResults[0].isError);
  TEST_ASSERT_EQUAL_STRING(agent::kUnverifiedMemoryRefusal, r.toolResults[0].output.c_str());
  TEST_ASSERT_EQUAL(0, (int)r.mcpRequests.size());
  // End-of-turn mem_write: refused too.
  TEST_ASSERT_EQUAL(0, (int)r.memCalls.size());

  // The next owner turn carries the refusal and stores normally.
  Rig::Script owner;
  owner.outJson = withWrite;
  r.scripts["anthropic"] = {owner};
  r.scriptIdx.clear();
  r.eng->handleMessage("ok, and remember my week", "", "web");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("[MEMORY] Not stored: this turn carries") !=
                   std::string::npos);
  TEST_ASSERT_EQUAL(1, (int)r.memCalls.size());
  TEST_ASSERT_EQUAL_STRING("memory.write", r.memCalls[0].c_str());
  // A synthesis over a VERIFIED result is unchanged: no provenance sentence.
  r.jobs->addFreshResult("job0001", "gcal-fetch", "Standup at 10:00");
  r.eng->maybeConsolidate("web");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("connector was NOT called") == std::string::npos);
}

// The class, not the instance: EVERY loop tool that stores new long-term content
// is shut on a flagged turn - memory.update falls back to a plain store when
// nothing matches, so it was a second door (prism review).
static void test_unverified_turn_shuts_every_memory_store_tool() {
  for (const char* tool : {"memory_write", "memory_update"}) {
    Rig r;
    r.specs.push_back(orch::ToolRegistry::Spec{"memory.write", "store a fact", "{}"});
    r.specs.push_back(orch::ToolRegistry::Spec{"memory.update", "update a fact", "{}"});
    Rig::Script sc;
    sc.dispatchTools = 1;
    sc.toolName = tool;
    r.scripts["anthropic"] = {sc};
    r.jobs->addFreshResult("job0000", "gcal-fetch", "guess", "", false, /*unverified=*/true);
    r.eng->maybeConsolidate("web");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.toolResults.size(), tool);
    TEST_ASSERT_TRUE_MESSAGE(r.toolResults[0].isError, tool);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(agent::kUnverifiedMemoryRefusal,
                                     r.toolResults[0].output.c_str(), tool);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.mcpRequests.size(), tool);
  }
}

// CUM-536 gate: on that turn the mem_write reply field is shut too, so a head that
// calls it as a tool hears so, with the reason - never the usual recovery text
// ("call memory_write now"), which the same turn would then refuse.
static void test_unverified_turn_recovery_for_mem_write_names_the_refusal() {
  for (const char* tool : {"mem_write", "Mem-Write"}) {
    Rig r;
    r.specs.push_back(orch::ToolRegistry::Spec{"memory.write", "store a fact", "{}"});
    Rig::Script sc;
    sc.dispatchTools = 1;
    sc.toolName = tool;
    r.scripts["anthropic"] = {sc};
    r.jobs->addFreshResult("job0000", "gcal-fetch", "guess", "", false, /*unverified=*/true);
    r.eng->maybeConsolidate("web");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.toolResults.size(), tool);
    const std::string& out = r.toolResults[0].output;
    TEST_ASSERT_TRUE_MESSAGE(r.toolResults[0].isError, tool);
    TEST_ASSERT_TRUE_MESSAGE(out.find("is not a tool, and memory writes are refused this turn") !=
                                 std::string::npos, out.c_str());
    TEST_ASSERT_TRUE_MESSAGE(out.find(agent::kUnverifiedMemoryRefusal) != std::string::npos, out.c_str());
    TEST_ASSERT_TRUE_MESSAGE(out.find("call memory_write now") == std::string::npos, out.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.mcpRequests.size(), tool);   // never reached the registry
  }
  // A verified turn keeps the ordinary recovery path (the registry answers it).
  Rig v;
  Rig::Script sc;
  sc.dispatchTools = 1;
  sc.toolName = "mem_write";
  v.scripts["anthropic"] = {sc};
  v.jobs->addFreshResult("job0000", "web-research", "facts", "", false, /*unverified=*/false);
  v.eng->maybeConsolidate("web");
  TEST_ASSERT_EQUAL(1, (int)v.mcpRequests.size());
}

// CUM-538 (item 5): the head's next turn hears that a spawn did not start and why,
// once; a turn with nothing refused carries no such block.
static void test_spawn_refusal_reaches_the_next_turn_once() {
  Rig r;
  for (int i = 0; i < orch::kMaxPendingSpawns; i++) {
    orch::Spawn s;
    s.task = "wave item";
    r.jobs->enqueueSpawn(s, "web");
  }
  orch::Spawn extra;
  extra.task = "fetch the calendar";
  extra.name = "gcal-fetch";
  r.jobs->enqueueSpawn(extra, "web");
  // Another chat's turn never sees it (the note carries this chat's task text).
  r.eng->handleMessage("hello", "", "1001");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("[SPAWN REFUSED]") == std::string::npos);
  r.eng->handleMessage("did it start?", "", "web");
  const std::string& in = r.attempts.back().inputs;
  TEST_ASSERT_TRUE(in.find("[SPAWN REFUSED] (background: sub-agents you asked for that did NOT "
                           "start") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("- 'gcal-fetch' did not start: the spawn queue is full") !=
                   std::string::npos);
  r.eng->handleMessage("and now?", "", "web");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("[SPAWN REFUSED]") == std::string::npos);
}

// CUM-538 gate: a sub whose dispatch timed out most likely ran. The next turn
// hears it "may have started" and must not spawn it again (never the "did NOT
// start ... spawn again" block), and [SPAWN CAPACITY] counts it as possibly
// running. A plain refusal keeps the original block byte-for-byte.
static void test_timed_out_spawn_is_not_respawned_by_the_next_turn() {
  Rig r(/*withSubFabric=*/true);
  r.subAdapter.dispatchErr = agent::FabricErr::Timeout;
  orch::Spawn s;
  s.task = "fetch the calendar";
  s.name = "gcal-fetch";
  s.provider = "anthropic";
  r.jobs->enqueueSpawn(s, "web");
  r.jobs->pump();
  r.eng->handleMessage("did it run?", "", "web");
  const std::string in = r.attempts.back().inputs;
  TEST_ASSERT_TRUE_MESSAGE(in.find("A 'may have started' one may have run on its provider - do "
                                   "NOT spawn it again unless the owner asks.") != std::string::npos,
                           in.c_str());
  TEST_ASSERT_TRUE(in.find("- 'gcal-fetch' may have started: ") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("Nothing is running for them") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("'gcal-fetch' did not start") == std::string::npos);
  TEST_ASSERT_TRUE(in.find("[SPAWN CAPACITY] 0 running (+1 possibly still running on its "
                           "provider after a timeout; its result will not arrive), 0 queued.") !=
                   std::string::npos);
  // Another chat sees the count (the device's sub-agents are shared, like
  // [ACTIVE SESSIONS]) but never the order not to spawn, which is this chat's.
  r.eng->handleMessage("hello", "", "1001");
  const std::string other = r.attempts.back().inputs;
  TEST_ASSERT_TRUE(other.find("possibly still running on its provider") != std::string::npos);
  TEST_ASSERT_TRUE(other.find("Do not spawn it again") == std::string::npos);
  TEST_ASSERT_TRUE(other.find("[SPAWN REFUSED]") == std::string::npos);
  // The window closes: no stale "possibly running" forever.
  r.plat.ms += JobEngine::kMaybeRunningMs;
  r.eng->handleMessage("and now?", "", "web");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("[SPAWN CAPACITY] 0 running, 0 queued.") !=
                   std::string::npos);
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
  TEST_ASSERT_EQUAL_STRING("No AI provider has a key: add one under Assistant > Models.",
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
  TEST_ASSERT_FALSE(r.anyDelivered("No AI provider has a key"));
  // The turn ran the normal path instead of being short-circuited.
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
}


// ---- HOST-FALLBACK (22 Oct, lane B's bench on unit L) ---------------------------
// A pinned head whose key was cleared was still dialed (HTTP 401, no fallback), and
// with no keyed provider in the priority list the turn ran on nothing and said
// "Done.". The head is now the first KEYED host (resolveHeadHost), the owner hears
// once that the pin has no key, and with no key anywhere every turn kind answers
// honestly without a request.
static void test_keyless_pinned_head_runs_on_a_keyed_host_and_says_so_once() {
  Rig r;
  r.cfg.keyed = {"mistral"};
  r.cfg.orchHost = "anthropic";      // pinned, key cleared
  r.cfg.priority = "anthropic";
  r.scripts["mistral"] = {{true, kGoodTurn, "", 0, "m1", 50, 10}, {true, kGoodTurn, "", 0, "m2", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("hello", "1001", "hello"));
  TEST_ASSERT_TRUE(r.eng->runTurn("again", "1001", "again"));
  TEST_ASSERT_EQUAL(2, (int)r.attempts.size());
  for (const auto& a : r.attempts) TEST_ASSERT_EQUAL_STRING("mistral", a.host.c_str());   // never the keyless pin
  int told = 0;
  for (const auto& d : r.delivered)
    told += d.second.find("Anthropic has no key, so Mistral is answering instead") != std::string::npos;
  TEST_ASSERT_EQUAL(1, told);                     // once, not every turn
  TEST_ASSERT_FALSE(r.anyDelivered("rejected the key"));
  // The key comes back, then goes again: told again.
  r.cfg.keyed = {"mistral", "anthropic"};
  r.scripts["anthropic"] = {{true, kGoodTurn, "", 0, "a1", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("pinned", "1001", "pinned"));
  TEST_ASSERT_EQUAL_STRING("anthropic", r.attempts.back().host.c_str());
  r.cfg.keyed = {"mistral"};
  r.scripts["mistral"] = {{true, kGoodTurn, "", 0, "m3", 50, 10}};
  TEST_ASSERT_TRUE(r.eng->runTurn("cleared", "1001", "cleared"));
  told = 0;
  for (const auto& d : r.delivered) told += d.second.find("Anthropic has no key") != std::string::npos;
  TEST_ASSERT_EQUAL(2, told);
}

// The second bench shape: no pin, a priority list naming only keyless providers, and
// a keyed provider outside it. The turn runs on that provider - not on nothing.
static void test_keyed_provider_outside_the_priority_list_runs_the_turn() {
  Rig r;
  r.cfg.keyed = {"mistral"};
  r.cfg.orchHost = "";
  r.cfg.priority = "anthropic";
  r.scripts["mistral"] = {{true, kGoodTurn, "", 0, "m1", 50, 10}};
  r.eng->handleMessage("hello", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());
  TEST_ASSERT_EQUAL_STRING("mistral", r.attempts[0].host.c_str());
  TEST_ASSERT_TRUE(r.anyDelivered("hello there"));
  TEST_ASSERT_FALSE(r.anyDelivered("has no key"));   // nothing was pinned: nothing to tell
}

// No key anywhere: an owner message, a routine and a synthesis turn each get the
// honest sentence and no request is made.
static void test_no_key_anywhere_answers_every_turn_kind_honestly() {
  Rig r;
  r.cfg.keyed.clear();
  r.cfg.orchHost = "anthropic";
  r.eng->handleMessage("hello", "Roy", "1001");
  TEST_ASSERT_FALSE(r.eng->runTurn("routine", "1001", ""));
  int honest = 0;
  for (const auto& d : r.delivered)
    honest += d.second == "No AI provider has a key: add one under Assistant > Models.";
  TEST_ASSERT_EQUAL(2, honest);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  TEST_ASSERT_FALSE(r.anyDelivered("Done."));
  // A keyed pin: no notice, the pin runs.
  Rig k;
  k.cfg.keyed = {"anthropic", "mistral"};
  k.cfg.orchHost = "anthropic";
  k.scripts["anthropic"] = {{true, kGoodTurn, "", 0, "a1", 50, 10}};
  TEST_ASSERT_TRUE(k.eng->runTurn("hi", "1001", "hi"));
  TEST_ASSERT_EQUAL_STRING("anthropic", k.attempts[0].host.c_str());
  TEST_ASSERT_FALSE(k.anyDelivered("has no key"));
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


// ---- (lane L) the turn contract and spawn honesty --------------------------------
// The class: a turn field the head filled is either applied or reported, a cut
// answer is salvaged with the honest marker exactly when it was cut, markup never
// reaches a channel, and every spawn the head expresses either starts or is
// reported to the owner and the head - never silence.

static std::string jsonOf(const std::function<void(ArduinoJson::JsonDocument&)>& fill) {
  ArduinoJson::JsonDocument d;
  fill(d);
  std::string out;
  serializeJson(d, out);
  return out;
}

static void test_lane_l_a_cut_answer_is_salvaged_marked_and_captured() {
  Rig r;
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = "{\"reply\":\"First part of a long answer is complete. Second part was cut mi";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("hi", "Roy", "1001");
  const std::string want =
      std::string("First part of a long answer is complete.") + "\n\n" + orch::kSalvageCutMarker;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.lastText().c_str());
  // The owner's text is what the next turn sees (the old salvage was never captured).
  TEST_ASSERT_EQUAL(1, (int)r.assistantRows.size());
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.assistantRows[0].second.c_str());
  TEST_ASSERT_FALSE(r.hookEnds.back().ok);
  TEST_ASSERT_TRUE(r.hookEnds.back().error.rfind("cut: ", 0) == 0);
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());

  // Counter-test: a complete answer followed by stray text is NOT cut - no marker.
  Rig c;
  c.cfg.toolLoop = false;
  s.outJson = "{\"reply\":\"All done.\"} and then some chatter";
  c.scripts["anthropic"] = {s};
  c.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("All done.", c.lastText().c_str());
  TEST_ASSERT_TRUE(c.hookEnds.back().error.rfind("parse: ", 0) == 0);
}

// The marker appears exactly when the answer was cut: over every byte cut of a
// real-shaped turn, never on the whole turn, always when salvaged text exists.
static void test_lane_l_the_salvage_marker_appears_exactly_when_cut() {
  const std::string full =
      "{\"reply\":\"Two facts. First, the lineup changed. Second, prices fell.\","
      "\"device\":[{\"type\":\"lights\",\"value\":\"off\"}],\"ask\":\"\"}";
  for (size_t L = 1; L <= full.size(); L++) {
    Rig r;
    r.cfg.toolLoop = false;
    Rig::Script s;
    s.outJson = full.substr(0, L);
    r.scripts["anthropic"] = {s};
    r.eng->handleMessage("hi", "Roy", "1001");
    const std::string got = r.lastText();
    const bool marked = got.find(orch::kSalvageCutMarker) != std::string::npos;
    char msg[48];
    snprintf(msg, sizeof msg, "cut %u of %u", (unsigned)L, (unsigned)full.size());
    if (L == full.size()) {
      TEST_ASSERT_FALSE_MESSAGE(marked, msg);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.staged.size(), msg);   // the whole turn applies
      continue;
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.staged.size(), msg);      // a cut turn never acts
    TEST_ASSERT_TRUE_MESSAGE(got.find("{\"") == std::string::npos, msg);   // never raw wire JSON
    const bool salvaged = got != agent::kCutNothingSalvaged;
    TEST_ASSERT_EQUAL_MESSAGE(salvaged, marked, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.hookEnds.back().error.rfind("cut: ", 0) == 0, msg);
  }
}

// A provider-reported cut (the adapter error) salvages the partial turn it kept.
static void test_lane_l_a_reported_cut_salvages_the_partial_turn() {
  Rig r;
  r.cfg.toolLoop = false;
  r.cfg.keyed = {"anthropic"};   // no alternate host to fail over to
  Rig::Script s;
  s.ok = false;
  s.err = "cut at the output limit (12000 tokens)";
  s.partialOnFail = true;
  s.outJson = "{\"reply\":\"The summary is ready. Details follow in the next";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_TRUE(r.lastText().rfind("The summary is ready.", 0) == 0);
  TEST_ASSERT_TRUE(r.lastText().find(orch::kSalvageCutMarker) != std::string::npos);
  TEST_ASSERT_TRUE(r.hookEnds.back().error.rfind("cut: cut at the output limit", 0) == 0);
  TEST_ASSERT_EQUAL(1, (int)r.assistantRows.size());
  Rig e;   // nothing kept: the plain cut line, never "formatting error"
  e.cfg.toolLoop = false;
  e.cfg.keyed = {"anthropic"};
  s.outJson = "";
  e.scripts["anthropic"] = {s};
  e.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING(agent::kCutNothingSalvaged, e.lastText().c_str());
}

// A stringified session_ops (the Anthropic head's shape) starts exactly one sub.
static void test_lane_l_a_stringified_spawn_list_starts_its_sub() {
  Rig r(true);
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = jsonOf([](ArduinoJson::JsonDocument& d) {
    d["reply"] = "Starting it now.";
    d["session_ops"] =
        "[{\"op\":\"spawn\",\"provider\":\"anthropic\",\"name\":\"e2e\",\"task\":\"Reply NIMBUS-1\"}]";
  });
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("go", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, r.jobs->pendingCount());
  r.jobs->pump();
  TEST_ASSERT_EQUAL(1, (int)r.subAdapter.dispatched.size());
  TEST_ASSERT_TRUE(r.subAdapter.dispatched[0].task.find("Reply NIMBUS-1") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("Starting it now.", r.delivered.front().second.c_str());   // no correction
  TEST_ASSERT_TRUE(LogCapture::contains("turn field session_ops arrived as a JSON string; repaired"));
}

// An empty-task spawn is reported to the owner (one line) and to the head.
static void test_lane_l_an_empty_task_spawn_is_reported_not_skipped() {
  Rig r(true);
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = "{\"reply\":\"Starting a research agent now.\",\"session_ops\":[{\"op\":\"spawn\",\"name\":\"lineup\"}]}";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("go", "Roy", "1001");
  const std::string want = std::string("Starting a research agent now.\n\n") + orch::kSpawnNotStartedLine;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.lastText().c_str());
  TEST_ASSERT_EQUAL(0, r.jobs->pendingCount());
  r.eng->handleMessage("and?", "Roy", "1001");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("'lineup' did not start: it was sent without a task") !=
                   std::string::npos);
}

// The promise-without-spawn class: a reply that says sub-agents started while
// the turn started none gets the correction line and a head note.
static void test_lane_l_a_promise_without_a_spawn_is_corrected() {
  Rig r(true);
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = "{\"reply\":\"I've started three sub-agents on the comparison.\"}";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("go", "Roy", "1001");
  TEST_ASSERT_TRUE(r.lastText().find(orch::kSpawnNotStartedLine) != std::string::npos);
  r.eng->handleMessage("and?", "Roy", "1001");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("your reply said sub-agents were started") != std::string::npos);
  // Counter-test: a TRUE claim (the spawn is in the turn) gets no correction.
  Rig t(true);
  t.cfg.toolLoop = false;
  s.outJson = "{\"reply\":\"I've started a sub-agent on it.\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"Do it\"}]}";
  t.scripts["anthropic"] = {s};
  t.eng->handleMessage("go", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("I've started a sub-agent on it.", t.delivered.front().second.c_str());
  TEST_ASSERT_FALSE(t.anyDelivered("Correction"));
}

// The class behind the 19 Oct bench row ("Starting three research sub-agents
// ..." with nothing started): every delivered text that claims sub-agents either
// has them started or carries the correction line, on every delivery path. A
// cut or unreadable answer runs nothing, so a claim in its salvaged text is
// always false; a prose answer carries no spawn item, so its claim is too.
static const char kClaimReply[] =
    "Starting three research sub-agents on anthropic: how sourdough starters work, proofing "
    "temperatures, and why bread goes stale. I'll send one combined answer.";

static void test_lane_l_a_cut_answer_that_claims_spawns_is_marked_and_corrected() {
  // Provider-reported cut (the adapter kept the partial turn), then an
  // unreported one (the JSON itself is cut): the marker AND the correction.
  for (int reported = 0; reported < 2; reported++) {
    Rig r(true);
    r.cfg.toolLoop = false;
    r.cfg.keyed = {"anthropic"};
    Rig::Script s;
    s.outJson = std::string("{\"reply\":\"") + kClaimReply + "\",\"session_ops\":[{\"op\":\"spawn\",\"ta";
    if (reported) {
      s.ok = false;
      s.err = "cut at the output limit (8192 tokens)";
      s.partialOnFail = true;
    }
    r.scripts["anthropic"] = {s};
    r.eng->handleMessage("research it", "Roy", "1001");
    const std::string got = r.lastText();
    TEST_ASSERT_TRUE_MESSAGE(got.rfind(kClaimReply, 0) == 0, got.c_str());
    TEST_ASSERT_TRUE_MESSAGE(got.find(orch::kSalvageCutMarker) != std::string::npos, got.c_str());
    TEST_ASSERT_TRUE_MESSAGE(got.find(orch::kSpawnNotStartedLine) != std::string::npos, got.c_str());
    TEST_ASSERT_EQUAL(0, r.jobs->pendingCount());
    TEST_ASSERT_TRUE(LogCapture::contains("spawn ledger spawned=0 claimed=1"));
    TEST_ASSERT_TRUE(r.hookEnds.back().error.rfind("cut: ", 0) == 0);
    TEST_ASSERT_EQUAL_STRING(got.c_str(), r.assistantRows.back().second.c_str());   // what the owner saw
    r.eng->handleMessage("and?", "Roy", "1001");   // the head hears it too
    TEST_ASSERT_TRUE(r.attempts.back().inputs.find("your reply said sub-agents were started, but that "
                                                   "answer was cut off or unreadable") != std::string::npos);
  }
  // An unreadable (not cut) answer: no cut marker, still the correction.
  Rig u(true);
  u.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = std::string("{\"reply\":\"") + kClaimReply + "\"} and then some chatter";
  u.scripts["anthropic"] = {s};
  u.eng->handleMessage("research it", "Roy", "1001");
  const std::string want = std::string(kClaimReply) + "\n\n" + orch::kSpawnNotStartedLine;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), u.lastText().c_str());
  TEST_ASSERT_TRUE(u.hookEnds.back().error.rfind("parse: ", 0) == 0);
}

// Over every byte cut of a turn whose reply claims three sub-agents and carries
// them: no prefix starts one, and any delivered text holding the claim carries
// the correction; only the whole turn starts all three, with no correction.
static void test_lane_l_no_cut_ever_delivers_a_bare_spawn_claim() {
  const std::string full = std::string("{\"reply\":\"") + kClaimReply +
                           "\",\"session_ops\":[{\"op\":\"spawn\",\"task\":\"sourdough\"},"
                           "{\"op\":\"spawn\",\"task\":\"proofing\"},{\"op\":\"spawn\",\"task\":\"stale\"}]}";
  int claims = 0;
  for (size_t L = 1; L <= full.size(); L++) {
    Rig r(true);
    r.cfg.toolLoop = false;
    Rig::Script s;
    s.outJson = full.substr(0, L);
    r.scripts["anthropic"] = {s};
    r.eng->handleMessage("research it", "Roy", "1001");
    const std::string got = r.lastText();
    char msg[48];
    snprintf(msg, sizeof msg, "cut %u of %u", (unsigned)L, (unsigned)full.size());
    if (L == full.size()) {
      TEST_ASSERT_EQUAL_MESSAGE(3, r.jobs->pendingCount(), msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kClaimReply, r.delivered.front().second.c_str(), msg);
      TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("Correction"), msg);
      continue;
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, r.jobs->pendingCount(), msg);
    if (!orch::replyClaimsSpawn(got)) continue;
    claims++;
    TEST_ASSERT_TRUE_MESSAGE(got.find(orch::kSpawnNotStartedLine) != std::string::npos, msg);
  }
  TEST_ASSERT_TRUE(claims > 100);   // the claim is whole well before the spawn list
  printf("property: %u cut prefixes, %d delivered a spawn claim, each with the correction\n",
         (unsigned)full.size() - 1, claims);
}

// A prose answer (no orch_turn, as a head can give on its last round) that
// claims sub-agents: delivered with the correction and counted in the ledger.
static void test_lane_l_a_prose_answer_that_claims_spawns_is_corrected() {
  Rig r(true);
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = kClaimReply;
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("research it", "Roy", "1001");
  const std::string want = std::string(kClaimReply) + "\n\n" + orch::kSpawnNotStartedLine;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.lastText().c_str());
  TEST_ASSERT_EQUAL(0, r.jobs->pendingCount());
  TEST_ASSERT_TRUE(LogCapture::contains("head reply was prose"));
  TEST_ASSERT_TRUE(LogCapture::contains("spawn ledger spawned=0 claimed=1"));
  // Counter-test: prose that claims nothing is delivered as it is.
  Rig c(true);
  c.cfg.toolLoop = false;
  s.outJson = "Sourdough rises because wild yeast and bacteria ferment the flour.";
  c.scripts["anthropic"] = {s};
  c.eng->handleMessage("why?", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING(s.outJson.c_str(), c.lastText().c_str());
  TEST_ASSERT_FALSE(c.anyDelivered("Correction"));
}

// Pseudo-tool salvage (subagents F2): session_ops called as a tool joins the turn
// once; a final orch_turn repeating it (re-spaced, re-cased) adds nothing, and a
// different task adds one (no over-deduping).
static void test_lane_l_pseudo_tool_spawns_join_the_turn_exactly_once() {
  for (int variant = 0; variant < 2; variant++) {
    Rig r(true);
    Rig::Script s;
    s.dispatchTools = 1;
    s.toolName = "session_ops";
    s.toolArgs =
        "{\"session_ops\":\"[{\\\"op\\\":\\\"spawn\\\",\\\"provider\\\":\\\"anthropic\\\","
        "\\\"task\\\":\\\"List   the lineup.\\\"}]\"}";
    s.outJson = variant == 0
        ? "{\"reply\":\"Started.\",\"session_ops\":[{\"op\":\"spawn\",\"provider\":\"Anthropic\",\"task\":\"list the lineup.\"}]}"
        : "{\"reply\":\"Started.\",\"session_ops\":[{\"op\":\"spawn\",\"provider\":\"anthropic\",\"task\":\"Check the prices.\"}]}";
    r.scripts["anthropic"] = {s};
    r.eng->handleMessage("go", "Roy", "1001");
    TEST_ASSERT_EQUAL(1, (int)r.toolResults.size());
    TEST_ASSERT_FALSE(r.toolResults[0].isError);
    TEST_ASSERT_TRUE(r.toolResults[0].output.find("taken as your orch_turn session_ops") != std::string::npos);
    TEST_ASSERT_EQUAL(0, (int)r.mcpRequests.size());   // never reached the registry
    TEST_ASSERT_EQUAL(variant == 0 ? 1 : 2, r.jobs->pendingCount());
    TEST_ASSERT_FALSE(r.anyDelivered("Correction"));
  }
}

// A pseudo-tool call that carried nothing usable is a reported attempt.
static void test_lane_l_an_empty_pseudo_tool_call_is_reported() {
  Rig r(true);
  Rig::Script s;
  s.dispatchTools = 1;
  s.toolName = "session_ops";
  s.toolArgs = "{}";
  s.outJson = "{\"reply\":\"Here is where things stand.\"}";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("go", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.mcpRequests.size());   // the registry's recovery text answered
  TEST_ASSERT_TRUE(r.lastText().find(orch::kSpawnNotStartedLine) != std::string::npos);
  r.eng->handleMessage("and?", "Roy", "1001");
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("the call carried no spawn item") != std::string::npos);
}

// The leak-repair gate (taint x repair): fields recovered from leaked tool markup
// apply only on an attended owner turn that read no external content.
static void test_lane_l_recovered_actions_apply_only_on_clean_owner_turns() {
  const std::string leaked = jsonOf([](ArduinoJson::JsonDocument& d) {
    d["reply"] = std::string("Done for today.\n</reply>\n") +
                 "<parameter name=\"device\">[{\"type\":\"lights\",\"value\":\"off\"}]</parameter>\n" +
                 "<parameter name=\"mem_write\">[{\"content\":\"Owner likes tea\",\"importance\":0.6}]</parameter>\n" +
                 "<parameter name=\"ask\">Which room?</parameter>";
  });
  enum Case { Owner, Scheduled, Synthesis, Member, ExternalTool, InternalTool };
  for (int c = Owner; c <= InternalTool; c++) {
    Rig r;
    r.adminChats = c != Member;
    Rig::Script s;
    s.outJson = leaked;
    if (c == ExternalTool || c == InternalTool) {
      const char* reg = c == ExternalTool ? "web.search" : "memory.search";
      r.specs.push_back(orch::ToolRegistry::Spec{reg, "tool", "{}"});
      s.dispatchTools = 1;
      s.toolName = c == ExternalTool ? "web_search" : "memory_search";
    }
    r.scripts["anthropic"] = {s};
    if (c == Scheduled) {
      r.eng->injectScheduledTurn("1001", "daily brief", "brief");
    } else if (c == Synthesis) {
      r.jobs->addFreshResult("job0001", "researcher", "findings");
      r.eng->maybeConsolidate("1001");
    } else {
      r.eng->handleMessage("hi", "Roy", "1001");
    }
    const bool applies = c == Owner || c == InternalTool;
    char msg[32];
    snprintf(msg, sizeof msg, "case %d", c);
    TEST_ASSERT_EQUAL_MESSAGE(applies ? 1 : 0, (int)r.staged.size(), msg);
    int writes = 0;
    for (const auto& m : r.memCalls) writes += m == "memory.write";
    TEST_ASSERT_EQUAL_MESSAGE(applies ? 1 : 0, writes, msg);
    TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("Done for today."), msg);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("parameter"), msg);   // markup never delivered
    TEST_ASSERT_EQUAL_MESSAGE(applies, r.anyDelivered("Which room?"), msg);   // a recovered ask too
  }
}

// A spawn recovered from leaked markup obeys the same gate, and when the gate
// drops it the owner and the head are told (the conservation law): it starts on
// a clean owner turn, and after a web search it is reported, never lost.
static void test_lane_l_a_gated_recovered_spawn_is_reported() {
  const std::string leaked = jsonOf([](ArduinoJson::JsonDocument& d) {
    d["reply"] = std::string("Looking into it.\n</reply>\n") +
                 "<parameter name=\"session_ops\">[{\"op\":\"spawn\",\"name\":\"kettles\","
                 "\"task\":\"Compare two kettles\"}]</parameter>";
  });
  for (const bool external : {false, true}) {
    LogCapture::install();
    Rig r(true);
    r.adminChats = true;
    Rig::Script s;
    s.outJson = leaked;
    if (external) {
      r.specs.push_back(orch::ToolRegistry::Spec{"web.search", "tool", "{}"});
      s.dispatchTools = 1;
      s.toolName = "web_search";
    }
    Rig::Script next;
    next.outJson = "{\"reply\":\"ok\"}";
    r.scripts["anthropic"] = {s, next};
    r.eng->handleMessage("compare kettles", "Roy", "1001");
    const char* msg = external ? "after a web search" : "clean owner turn";
    TEST_ASSERT_EQUAL_MESSAGE(external ? 0 : 1, r.jobs->pendingCount(), msg);
    TEST_ASSERT_FALSE_MESSAGE(r.anyDelivered("parameter"), msg);
    TEST_ASSERT_EQUAL_MESSAGE(external, r.anyDelivered(orch::kSpawnNotStartedLine), msg);
    TEST_ASSERT_EQUAL_MESSAGE(external, LogCapture::contains("spawn ledger spawned=0 claimed=1"), msg);
    r.eng->handleMessage("and?", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(external,
                              r.attempts.back().inputs.find("'kettles' did not start: it came from tool markup") !=
                                  std::string::npos,
                              msg);
  }
}

// The final-answer check is wired into every loop turn's HeadTools.
static void test_lane_l_the_final_check_rides_every_loop_turn() {
  Rig r;
  Rig::Script s;
  s.checkFinal = true;
  s.outJson = "{\"reply\":\"I've started two sub-agents.\",\"session_ops\":\"not json\"}";
  Rig::Script good = s;
  good.outJson = "{\"reply\":\"Plain answer.\"}";
  r.scripts["anthropic"] = {s, good};
  r.eng->handleMessage("go", "Roy", "1001");
  r.eng->handleMessage("again", "Roy", "1001");
  TEST_ASSERT_EQUAL(2, (int)r.finalChecks.size());
  TEST_ASSERT_TRUE(r.finalChecks[0].rfind("[TURN CHECK]", 0) == 0);
  TEST_ASSERT_TRUE(r.finalChecks[0].find("session_ops") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", r.finalChecks[1].c_str());   // a good turn costs no round
}

// Lane S: the claim check shares the final-answer check's one round. An unbacked
// claim gets "[CLAIM CHECK]" (and claimsCorrected moves); when the round could
// not fix it (here the scripted answer is applied as is), the delivered reply
// carries the owner line. A spawn promise is the shape check's alone, and a
// consistent reply costs no round.
static void test_lane_s_the_claim_check_joins_the_final_check() {
  Rig r;
  Rig::Script unbacked;
  unbacked.checkFinal = true;
  unbacked.outJson = "{\"reply\":\"The packing list is saved to your files as packing.md.\"}";
  Rig::Script spawn = unbacked;
  spawn.outJson = "{\"reply\":\"I've started two sub-agents.\",\"session_ops\":\"not json\"}";
  Rig::Script good = unbacked;
  good.outJson = "{\"reply\":\"Plain answer.\"}";
  r.scripts["anthropic"] = {unbacked, spawn, good};
  const uint32_t before = agent::ClaimCheck::correctedCount();
  r.eng->handleMessage("save my packing list", "Roy", "1001");
  r.eng->handleMessage("start two", "Roy", "1001");
  r.eng->handleMessage("again", "Roy", "1001");
  TEST_ASSERT_EQUAL(3, (int)r.finalChecks.size());
  TEST_ASSERT_TRUE(r.finalChecks[0].find("[CLAIM CHECK]") != std::string::npos);
  TEST_ASSERT_TRUE(r.finalChecks[0].find("packing.md") != std::string::npos);
  TEST_ASSERT_EQUAL(before + 1, agent::ClaimCheck::correctedCount());
  TEST_ASSERT_TRUE(r.anyDelivered("(Correction: nothing has been written for that yet.)"));
  TEST_ASSERT_TRUE(r.finalChecks[1].rfind("[TURN CHECK]", 0) == 0);
  TEST_ASSERT_TRUE(r.finalChecks[1].find("[CLAIM CHECK]") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("", r.finalChecks[2].c_str());
}

// Bench C1 on the host: the custom head (the LAN mock) answers the mock's exact
// "mock-claim-saved" turn on the serial chat, and the memory write does not land
// (FAULT embed_fail queues it; a full queue refuses it). The turn delivers exactly
// one reply, ending with the correction, and the history row is that reply. A
// stored write gives the same single reply with no line (counter-test).
static void test_pg5_custom_head_saved_claim_delivers_one_reply() {
  const struct {
    const char* code;
    const char* text;
    bool success, lands;
  } cases[] = {
      {orch::kMemOutcomeQueued, "queued memory p1, not stored yet", true, false},
      {orch::kMemOutcomeRefused, "not stored: the waiting list is full", false, false},
      {orch::kMemOutcomeStored, "stored memory m1", true, true},
  };
  for (const auto& c : cases) {
    Rig r;
    r.cfg.keyed.insert("custom");
    r.cfg.orchHost = "custom";
    r.adminChats = true;   // the serial console is an owner surface
    Rig::Script s;
    s.outJson = harness_test::kMockClaimSavedTurn;
    r.scripts["custom"] = {s};
    r.memResult = c.success ? orch::ToolResult::ok(c.text) : orch::ToolResult::fail(c.text);
    r.memResult.code = c.code;
    const uint32_t tailed = agent::ClaimCheck::tailedCount();
    r.eng->handleMessage("remember the bench note", "", "serial");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.attempts.size(), c.code);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("custom", r.attempts[0].host.c_str(), c.code);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.memCalls.size(), c.code);
    const std::string want = c.lands ? std::string("Saved it to memory.")
                                     : std::string("Saved it to memory.\n\n") + harness_test::kNotSavedLine;
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), c.code);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("serial", r.delivered[0].first.c_str(), c.code);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.delivered[0].second.c_str(), c.code);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.assistantRows.size(), c.code);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.assistantRows[0].second.c_str(), c.code);
    TEST_ASSERT_EQUAL_MESSAGE(tailed + (c.lands ? 0 : 1), agent::ClaimCheck::tailedCount(), c.code);
  }
}

// Bench C3/C4 on the host: the mock's "mock-claim-durable" turn ("so it will
// survive a restart" with a scratchpad item) on the custom head. The scratchpad
// saves to a flash file, so on a full NVS the save lands (Applied) and the claim
// is true; only a failed flash write (AppliedRamOnly) gets the RAM-only note; an
// update that changes nothing (Skipped) adds no line either. Exactly one reply each.
static void test_pg5_custom_head_durability_claim_follows_the_flash_save() {
  const char* ramLine = harness_test::kRamOnlyLine;
  const struct {
    agent::ScratchOutcome outcome;
    bool line;
    const char* what;
  } cases[] = {
      {agent::ScratchOutcome::Applied, false, "saved to flash (full NVS or room)"},
      {agent::ScratchOutcome::AppliedRamOnly, true, "flash write failed"},
      {agent::ScratchOutcome::Skipped, false, "nothing changed"},
  };
  for (const auto& c : cases) {
    Rig r;
    r.cfg.keyed.insert("custom");
    r.cfg.orchHost = "custom";
    r.adminChats = true;   // the scratchpad is admin-only; the serial console is an owner surface
    Rig::Script s;
    s.outJson = harness_test::kMockClaimDurableTurn;
    r.scripts["custom"] = {s};
    r.scratchOutcome = c.outcome;
    const uint32_t tailed = agent::ClaimCheck::tailedCount();
    r.eng->handleMessage("put the bench note in your notes", "", "serial");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.scratchApplied.size(), c.what);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), c.what);
    const std::string& got = r.delivered[0].second;
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)got.find("Put it in my notes, so it will survive a restart."), c.what);
    TEST_ASSERT_EQUAL_MESSAGE(c.line, got.find(ramLine) != std::string::npos, c.what);
    TEST_ASSERT_TRUE_MESSAGE(got.find("(Correction:") == std::string::npos, c.what);
    TEST_ASSERT_EQUAL_MESSAGE(tailed + (c.line ? 1 : 0), agent::ClaimCheck::tailedCount(), c.what);
  }
}

// Formatting F3/F4 at the engine: reasoning, wrappers and tool markup never reach
// the owner, on the turn path and the prose path; unknown <word> content stays.
static void test_lane_l_markup_never_reaches_the_owner() {
  Rig r;
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = "{\"reply\":\"<think>plan it</think>Hello <provider> fan.\",\"ask\":\"<reply>Which one?</reply>\"}";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("Hello <provider> fan.\n\nWhich one?", r.lastText().c_str());
  Rig p;
  p.cfg.toolLoop = false;
  s.outJson = "<thinking>private</thinking>Plain answer.";
  p.scripts["anthropic"] = {s};
  p.eng->handleMessage("hi", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("Plain answer.", p.lastText().c_str());
}

// The spawn ledger (UNIFIED 3.17): the next turn reads what the device started.
static void test_lane_l_the_spawn_ledger_rides_the_next_turn() {
  Rig r(true);
  r.cfg.toolLoop = false;
  Rig::Script s;
  s.outJson = "{\"reply\":\"On it.\",\"session_ops\":[{\"op\":\"spawn\",\"provider\":\"anthropic\",\"name\":\"lineup\",\"task\":\"List the lineup\"}]}";
  r.scripts["anthropic"] = {s};
  r.eng->handleMessage("go", "Roy", "1001");
  r.jobs->pump();
  r.eng->handleMessage("status?", "Roy", "1001");
  const std::string& in = r.attempts.back().inputs;
  TEST_ASSERT_TRUE(in.find("[SPAWN LEDGER]") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("- lineup (job0000, anthropic/") != std::string::npos);
  TEST_ASSERT_TRUE(in.find("running, started") != std::string::npos);
  r.eng->handleMessage("other chat", "Roy", "2002");   // never another chat's
  TEST_ASSERT_TRUE(r.attempts.back().inputs.find("[SPAWN LEDGER]") == std::string::npos);
}

// Honest capacity (subagents F6.2): every keyed backend is named with the way it
// really runs, from the one table - a new backend without a mode fails to build.
static void test_lane_l_capacity_names_every_keyed_provider_by_its_mode() {
  Rig r;
  r.cfg.keyed = {"anthropic", "openai", "mistral", "custom", "zai", "cumulo"};
  r.eng->handleMessage("hi", "Roy", "1001");
  const std::string& in = r.attempts.back().inputs;
  const size_t cap = in.find("[SPAWN CAPACITY]");
  const size_t syncAt = in.find("sub-agents run one at a time INSIDE the device", cap);
  const size_t asyncAt = in.find("sub-agents run in parallel on the provider", cap);
  TEST_ASSERT_TRUE(cap != std::string::npos && syncAt != std::string::npos && asyncAt != std::string::npos);
  const std::string syncClause = in.substr(cap, syncAt - cap);
  const std::string asyncClause = in.substr(syncAt, asyncAt - syncAt);
  for (int i = 0; i < agent::kDeviceBackendCount; i++) {
    const std::string b = agent::kDeviceBackends[i];
    const std::string& clause = agent::kDeviceBackendSync[i] ? syncClause : asyncClause;
    TEST_ASSERT_TRUE_MESSAGE(clause.find(b) != std::string::npos, b.c_str());
  }
}


// The conservation law at synthesis: a turn that succeeds with nothing to say
// must not swallow the results it was handed (seen live on a mistral-small head).
static void test_lane_l_a_silent_synthesis_never_drops_the_results() {
  Rig r;
  Rig::Script s;
  s.outJson = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\",\"mem_write\":[{\"content\":\"noted\"}]}";
  r.scripts["anthropic"] = {s};
  r.jobs->addFreshResult("job0001", "researcher", "NIMBUS-4242 is the answer");
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_TRUE(r.anyDelivered("NIMBUS-4242"));
  TEST_ASSERT_TRUE(LogCapture::contains("synthesis turn said nothing"));
  // Counter-test: a synthesis that reports gets no raw duplicate.
  Rig c;
  s.outJson = "{\"reply\":\"The answer is NIMBUS-4242.\"}";
  c.scripts["anthropic"] = {s};
  c.jobs->addFreshResult("job0001", "researcher", "NIMBUS-4242 is the answer");
  c.eng->maybeConsolidate("1001");
  int n = 0;
  for (const auto& d : c.delivered) n += d.second.find("NIMBUS-4242") != std::string::npos;
  TEST_ASSERT_EQUAL(1, n);
}

// ---- lane T: every failed exit carries a cause; spend metered once per turn -------

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

static size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// One failed-exit scenario: how to reach it, and the cause it must report first.
struct ExitCase {
  const char* exitName;    // the engine exit it reaches
  const char* firstHost;   // the host the chain must start with
  const char* firstHas;    // text the first cause must contain
  std::function<void(Rig&)> setup;
  bool scheduled = false;  // run as a scheduled fire (FireOutcome.error checked too)
};

static std::vector<ExitCase> exitCases() {
  std::vector<ExitCase> v;
  auto failAll = [](const char* err) {
    return [err](Rig& r) {
      for (const char* h : {"anthropic", "openai", "mistral"})
        r.scripts[h] = {{false, "", err, 0, "", 0, 0}};
    };
  };
  v.push_back({"budget", "device", "budget", [](Rig& r) {
                 r.cfg.overBudgetHosts = {"anthropic", "openai", "mistral"};
               }});
  // Lane L's cut exit: an answer cut at its output limit is salvaged, never a provider fault.
  v.push_back({"cut", "anthropic", "cut at the output limit",
               failAll("cut at the output limit: the answer did not finish")});
  v.push_back({"low memory", "anthropic", "low memory", [](Rig& r) {
                 r.scripts["anthropic"] = {{false, "", "low memory: the device could not send the request",
                                            0, "", 0, 0}};
               }});
  // L19-14: the device could not send its request (its first write left 0 bytes).
  v.push_back({"unsent", "anthropic", "unsent", [](Rig& r) {
                 r.scripts["anthropic"] = {{false, "", "unsent: the device could not finish sending its request",
                                            0, "", 0, 0}};
               }});
  // Lane K's billing reply (the refusal is not a rate limit): its own exit.
  v.push_back({"billing", "anthropic", "credit balance is too low",
               failAll("messages HTTP 400: Your credit balance is too low to access the Anthropic API.")});
  v.push_back({"429", "anthropic", "HTTP 429", failAll("messages HTTP 429: rate limited")});
  v.push_back({"generic (ladder exhausted)", "anthropic", "HTTP 400",
               failAll("messages HTTP 400: the request body is not valid JSON")});
  v.push_back({"generic (timeout)", "anthropic", "timeout",
               failAll("timeout: the provider did not answer in time")});
  v.push_back({"generic (fabric exhausted)", "anthropic", "network", [](Rig& r) {
                 r.cfg.midFail = true;
                 r.fabricVisits = {{100, 20, "network"}, {0, 0, "network"}, {0, 0, "network"}};
                 r.fabricOk = false;
                 r.fabricErr = "chat HTTP 400";
               }});
  // Lane DEF: a round crossed the monthly spend ceiling mid-turn and the next request was
  // refused before it left: a quiet exit (the notice pump tells the owner), with a cause.
  v.push_back({"spend ceiling (mid-turn)", "anthropic", "spend ceiling",
               failAll("spend ceiling: paid calls are paused")});
  // HOST-FALLBACK: no provider has a key (a routine or synthesis turn reaches the
  // engine without handleMessage's check): an honest local reply, never a dial.
  v.push_back({"no key", "device", "no provider has a key", [](Rig& r) {
                 r.cfg.keyed.clear();
                 r.cfg.orchHost = "anthropic";
               }});
  v.push_back({"parse", "device", "parse", [](Rig& r) {
                 Rig::Script bad;
                 bad.outJson = "{\"nothing\":true}";   // a turn with no usable reply
                 r.scripts["anthropic"] = {bad};
               }});
  ExitCase sched = v[6];   // the same exit reached by a scheduled fire (ladder exhausted)
  sched.exitName = "generic (scheduled fire)";
  sched.scheduled = true;
  v.push_back(sched);
  return v;
}

// H3: every `!ok` exit of a turn carries a non-empty failure chain that starts where
// the turn first went wrong; a successful turn carries none. A new exit added to the
// engine without a row here fails the source count below.
static void test_every_failed_turn_exit_carries_a_cause() {
  const std::vector<ExitCase> cases = exitCases();
  for (const ExitCase& c : cases) {
    Rig r;
    c.setup(r);
    std::string fireErr;
    if (c.scheduled) {
      const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "p", "Morning", "lp9");
      TEST_ASSERT_FALSE_MESSAGE(o.ok, c.exitName);
      fireErr = o.error;
    } else {
      TEST_ASSERT_FALSE_MESSAGE(r.eng->runTurn("inputs", "1001", "hi"), c.exitName);
    }
    TEST_ASSERT_FALSE_MESSAGE(r.hookEnds.empty(), c.exitName);
    const agent::TurnEndEv& ev = r.hookEnds.back();
    TEST_ASSERT_FALSE_MESSAGE(ev.ok, c.exitName);
    TEST_ASSERT_NOT_NULL_MESSAGE(ev.chain, c.exitName);
    TEST_ASSERT_FALSE_MESSAGE(ev.chain->empty(), c.exitName);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.firstHost, ev.chain->first()->host.c_str(), c.exitName);
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(ev.chain->first()->text.c_str(), c.firstHas), c.exitName);
    if (c.scheduled) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, fireErr.compare(0, std::strlen(c.firstHost), c.firstHost),
                                    fireErr.c_str());
    }
  }
  Rig ok;
  TEST_ASSERT_TRUE(ok.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_NULL(ok.hookEnds.back().chain);
  // The class guard: one table row per distinct failed exit in the engine.
  const std::string src = readSource("lib/harness/src/engine.cpp");
  const size_t exits = countOf(src, "fireTurnEnd(false");
  const char* names[] = {"budget", "cut", "unsent", "low memory", "billing", "429", "generic", "parse",
                         "spend ceiling", "no key"};
  for (const char* n : names) {
    bool covered = false;
    for (const ExitCase& c : cases) covered = covered || std::strncmp(c.exitName, n, std::strlen(n)) == 0;
    TEST_ASSERT_TRUE_MESSAGE(covered, n);
  }
  TEST_ASSERT_EQUAL_MESSAGE(sizeof(names) / sizeof(names[0]), exits,
                            "a fireTurnEnd(false, ...) exit was added or removed: give it a row in "
                            "exitCases() and in names[] above");
}

// The owner hears the FIRST cause and which hosts failed after it (the 18 Oct dream
// was reported as Mistral's bare "chat HTTP 400"), in the typed words: never the raw
// provider line, and no "ask again" for a refusal that would come back the same.
static const char kAntInvalidJson[] =
    "messages HTTP 400: The request body is not valid JSON: invalid unicode code point at line 1 "
    "column 55142";
static void test_owner_notice_names_the_first_cause() {
  Rig r;
  r.scripts["anthropic"] = {{false, "", kAntInvalidJson, 0, "", 0, 0}};
  r.scripts["openai"] = {{false, "", "resp HTTP 400: Invalid body", 0, "", 0, 0}};
  r.scripts["mistral"] = {{false, "", "chat HTTP 400", 0, "", 0, 0}};
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
  const std::string want = "That didn't finish - " +
                           orch::stepErrCause(orch::StepErr::BadRequest, "anthropic", 400) +
                           "; openai and mistral also failed. Your message was not answered. "
                           "Nothing is still running.";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.lastText().c_str());
  TEST_ASSERT_EQUAL_STRING("chat HTTP 400", r.hookEnds.back().error.c_str());   // still the last
  TEST_ASSERT_NOT_NULL(std::strstr(r.hookEnds.back().chain->first()->text.c_str(), "not valid JSON"));
}

// L19-13a, the class over every typed kind the generic notice can carry: the owner
// never reads "HTTP <code>:" provider text, and "ask again" appears exactly for the
// kinds in transit (transport, server error or overload, timeout). Rate limits and
// billing have their own replies; a cut is salvaged.
static void test_failure_notice_retry_advice_only_when_transient() {
  struct Case { const char* err; const char* raw; bool retry; };   // raw: the provider
  const Case cases[] = {                                          // body that must not leak
      {kAntInvalidJson, "not valid JSON", false},                        // bad request
      {"messages HTTP 401: invalid x-api-key", "x-api-key", false},      // auth
      {"messages HTTP 404: model: claude-nonexistent-test", "claude-nonexistent-test", false},  // rejected
      {"messages HTTP 529: Overloaded", "Overloaded", true},              // overloaded
      {"messages HTTP 503: upstream connect error", "upstream", true},    // server error
      {"timeout: the provider did not answer in time", "", true},         // timeout (device token)
      {"network", "", true},                                              // transport
  };
  for (const Case& c : cases) {
    Rig r;
    r.cfg.keyed = {"anthropic"};
    r.scripts["anthropic"] = {{false, "", c.err, 0, "", 0, 0}, {false, "", c.err, 0, "", 0, 0}};
    TEST_ASSERT_FALSE_MESSAGE(r.eng->runTurn("inputs", "1001", ""), c.err);
    const std::string t = r.lastText();
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(t.c_str(), "That didn't finish - anthropic "), t.c_str());
    if (c.raw[0]) TEST_ASSERT_NULL_MESSAGE(std::strstr(t.c_str(), c.raw), t.c_str());
    // Every "HTTP" in the notice is the typed parenthetical status ("(HTTP 400)"),
    // never the raw provider line ("messages HTTP 400: The request body ...").
    for (const char* h = t.c_str(); (h = std::strstr(h, "HTTP")) != nullptr; ++h)
      TEST_ASSERT_TRUE_MESSAGE(h >= t.c_str() + 2 && h[-1] == '(' && h[-2] == ' ',
                               t.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(c.retry, std::strstr(t.c_str(), "ask again") != nullptr, t.c_str());
  }
}

// L19-13b: "Nothing is still running" only when the job table holds no active job;
// with helpers in flight the notice says how many and that their results will arrive.
static void test_failure_notice_names_helpers_still_running() {
  for (int jobs = 0; jobs <= 2; ++jobs) {
    Rig r;
    for (int i = 0; i < jobs; ++i) {
      orch::Spawn sp;
      sp.task = "research item";
      r.jobs->enqueueSpawn(sp, "1001");
    }
    TEST_ASSERT_EQUAL(jobs, r.jobs->activeCount());
    r.scripts["anthropic"] = {{false, "", "network", 0, "", 0, 0}};
    r.scripts["openai"] = {{false, "", "network", 0, "", 0, 0}};
    r.scripts["mistral"] = {{false, "", "network", 0, "", 0, 0}};
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
    const std::string t = r.lastText();
    TEST_ASSERT_EQUAL_MESSAGE(jobs == 0, t.find("Nothing is still running") != std::string::npos, t.c_str());
    if (jobs == 1)
      TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(t.c_str(), "1 helper is still working; its result will arrive"),
                                   t.c_str());
    if (jobs == 2)
      TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(t.c_str(), "2 helpers are still working; their results will arrive"),
                                   t.c_str());
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(t.c_str(), "sk again to retry."), t.c_str());   // transient
  }
  // The low-memory reply keeps the same rule.
  Rig m;
  orch::Spawn sp;
  sp.task = "research item";
  m.jobs->enqueueSpawn(sp, "1001");
  m.scripts["anthropic"] = {{false, "", "low memory: the device could not send the request", 0, "", 0, 0}};
  TEST_ASSERT_FALSE(m.eng->runTurn("inputs", "1001", ""));
  TEST_ASSERT_EQUAL_STRING(
      "That didn't finish - the device ran out of working memory for this request. Your message "
      "was not answered. 1 helper is still working; its result will arrive when it finishes. Try a "
      "shorter question or wait a minute.",
      m.lastText().c_str());
}

// L19-13c: a turn where no host returned a billed response was never served by a
// fallback: fallback=false on its turn-end event (the row's source) and no [FALLBACK]
// note for the next turn, on the ladder and in the fabric. A switched turn that is
// served keeps both, and its reply is sent exactly once.
static void test_no_billed_response_means_no_fallback() {
  {   // legacy ladder: every host fails before any output
    Rig r;
    for (const char* h : {"anthropic", "openai", "mistral"})
      r.scripts[h] = {{false, "", "network", 0, "", 0, 0}, {false, "", "network", 0, "", 0, 0}};
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_NOT_EQUAL(std::string("anthropic"), r.hookEnds.back().host);   // it did switch
    TEST_ASSERT_FALSE(r.hookEnds.back().fallback);
    r.scripts.clear();
    r.eng->handleMessage("and now?", "Roy", "1001");
    TEST_ASSERT_NULL(std::strstr(r.attempts.back().inputs.c_str(), "[FALLBACK"));
  }
  {   // fabric: every host fails and none billed
    Rig r;
    r.cfg.midFail = true;
    r.fabricVisits = {{0, 0, "network"}, {0, 0, "network"}, {0, 0, "network"}};
    r.fabricOk = false;
    r.fabricErr = "network";
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_FALSE(r.hookEnds.back().fallback);
    r.fabricVisits.clear();
    r.fabricOk = true;
    r.eng->handleMessage("and now?", "Roy", "1001");
    TEST_ASSERT_NULL(std::strstr(r.attempts.empty() ? "" : r.attempts.back().inputs.c_str(), "[FALLBACK"));
  }
  {   // counter-test: a switched turn that is served keeps its fallback, reply sent once
    Rig r;
    r.cfg.midFail = true;
    r.fabricSwitch = true;
    TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", ""));
    TEST_ASSERT_TRUE(r.hookEnds.back().fallback);
    int replies = 0;
    for (const auto& d : r.delivered) replies += d.second == "hello there" ? 1 : 0;
    TEST_ASSERT_EQUAL(1, replies);
  }
}

// The 37,771-for-15,438 class: a failing scheduled fire reports ITS OWN spend, never
// the previous turn's, and its cause.
static void test_scheduled_failure_reports_cause_and_own_spend() {
  Rig r;
  TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", "hi"));   // an owner turn: 100/20
  TEST_ASSERT_EQUAL(120, (int)r.eng->lastTurnUsage().total());
  Rig::Script billedThenDead;
  billedThenDead.ok = false;
  billedThenDead.err = "messages HTTP 400: invalid unicode code point";
  billedThenDead.usageIn = 7000;
  billedThenDead.usageOut = 300;
  billedThenDead.usageOnFail = true;
  for (const char* h : {"anthropic", "openai", "mistral"}) r.scripts[h] = {billedThenDead};
  r.scripts["openai"][0].usageOnFail = false;   // the alternates billed nothing
  r.scripts["mistral"][0].usageOnFail = false;
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "dream", "Dream", "dream", true);
  TEST_ASSERT_FALSE(o.ok);
  // anthropic billed its one attempt; nothing else. An unreadable body is never sent
  // twice to the same host (lane K's rule), the alternates are still tried.
  TEST_ASSERT_EQUAL(7300, (int)o.tokens.total());
  TEST_ASSERT_EQUAL(7300, (int)r.eng->lastTurnUsage().total());
  TEST_ASSERT_NOT_NULL(std::strstr(o.error.c_str(), "anthropic"));
  TEST_ASSERT_NOT_NULL(std::strstr(o.error.c_str(), "invalid unicode"));
  // The failed turn's spend reached the ledger, under the host that billed it.
  const auto& rec = r.cfg.recorded.back();
  TEST_ASSERT_EQUAL_STRING("anthropic", rec.host.c_str());
  TEST_ASSERT_EQUAL_STRING("loop:dream", rec.tag.c_str());
  TEST_ASSERT_EQUAL(7000, (int)rec.in);
  TEST_ASSERT_EQUAL_STRING("dream", agent::turnSourceSlug(r.hookEnds.back().source,
                                                          r.hookEnds.back().attribution));
}

// The [FALLBACK] note says whose turn switched, when, the whole path and why; the
// turn that switched never sees a note about itself, and it is consumed once.
static void test_fallback_note_names_previous_turn_and_reason() {
  Rig r;
  r.cfg.midFail = true;
  r.fabricVisits = {{50, 10, "messages HTTP 503: overloaded"}, {100, 20, ""}};
  const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "dream", "Dream", "dream", true);
  TEST_ASSERT_TRUE(o.ok);
  const size_t dreamFabric = r.fabricCalls.size();
  TEST_ASSERT_EQUAL(1, (int)dreamFabric);
  r.fabricVisits.clear();
  r.cfg.midFail = false;
  TEST_ASSERT_TRUE(r.eng->runTurn("", "1001", "next"));   // consumes nothing yet: runTurn
  r.eng->handleMessage("did the dream work?", "Roy", "1001");
  const std::string& in = r.attempts.back().inputs;
  TEST_ASSERT_NOT_NULL_MESSAGE(
      std::strstr(in.c_str(), "[FALLBACK - previous turn (nightly dream"), in.c_str());
  TEST_ASSERT_NOT_NULL(std::strstr(in.c_str(), "anthropic -> openai (server_error)"));
  TEST_ASSERT_NULL(std::strstr(in.c_str(), "[FALLBACK] switched"));
  r.eng->handleMessage("and now?", "Roy", "1001");
  TEST_ASSERT_NULL(std::strstr(r.attempts.back().inputs.c_str(), "[FALLBACK"));   // once
}

// The per-turn metering matrix (usage RCA T4): every exit x {legacy ladder, fabric} x
// 0-2 switches meters ONCE (none when nothing billed), per host exactly what that host
// billed, and lastTurnUsage is this turn's total. TurnEndEv.model is a model that
// billed, or empty.
enum MeterExit { kMeterSuccess, kMeterParse, kMeterFailAfterBilled, kMeterBudget };
static const char* const kMeterHosts[3] = {"anthropic", "openai", "mistral"};

// Fabric: hosts 0..sw each bill 10*(i+1) in; every host but the last fails.
static void meterFabricCase(Rig& r, int ex, int sw, uint32_t want[3]) {
  for (int i = 0; i <= sw; ++i) {
    r.fabricVisits.push_back({(uint32_t)(10 * (i + 1)), 1, "network"});
    want[i] = 10 * (i + 1);
  }
  r.fabricOk = ex != kMeterFailAfterBilled;
  r.fabricErr = "chat HTTP 503";
  if (ex == kMeterParse) r.fabricOut = "{\"nothing\":true}";
}

// Legacy: each failing host bills on both of its attempts (the first host) or its one
// attempt (alternates); the last host decides the exit.
static void meterLegacyCase(Rig& r, int ex, int sw, uint32_t want[3]) {
  for (int i = 0; i <= sw; ++i) {
    Rig::Script s;
    s.usageIn = (uint32_t)(10 * (i + 1));
    s.usageOut = 1;
    const bool fails = i != sw || ex == kMeterFailAfterBilled;
    want[i] = fails ? s.usageIn * (i == 0 ? 2u : 1u) : s.usageIn;
    if (fails) {
      s.ok = false;
      s.err = "chat HTTP 503";
      s.usageOnFail = true;
    } else if (ex == kMeterParse) {
      s.outJson = "{\"nothing\":true}";
    }
    r.scripts[kMeterHosts[i]] = {s};
  }
  for (int i = sw + 1; i < 3; ++i) r.scripts[kMeterHosts[i]] = {{false, "", "down", 0, "", 0, 0}};
}

// One metered turn: once (none when nothing billed), per host what it billed, the
// total in lastTurnUsage, and TurnEndEv.model a model that billed (or empty).
static void meterCheck(Rig& r, const uint32_t want[3], const char* tag) {
  const uint32_t billed = want[0] + want[1] + want[2];
  TEST_ASSERT_EQUAL_MESSAGE(billed ? 1 : 0, (int)r.cfg.spendCalls.size(), tag);
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.cfg.recorded.size(), tag);   // the batch, only
  if (billed) {
    const orch::TurnSpend& sp = r.cfg.spendCalls[0].spend;
    for (int i = 0; i < 3; ++i)
      TEST_ASSERT_EQUAL_MESSAGE(want[i], sp.hostTotal(kMeterHosts[i]).in, tag);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("turn", r.cfg.spendCalls[0].tag.c_str(), tag);
  }
  TEST_ASSERT_EQUAL_MESSAGE(billed, r.eng->lastTurnUsage().promptTokens, tag);
  const agent::TurnEndEv& ev = r.hookEnds.back();
  if (billed == 0) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", ev.model.c_str(), tag);
    return;
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(ev.spend, tag);
  bool found = false;
  for (size_t k = 0; k < ev.spend->n; ++k) found = found || ev.model == ev.spend->slot[k].model;
  TEST_ASSERT_TRUE_MESSAGE(found, tag);
}

static void test_metering_matrix_once_per_turn_per_host() {
  for (int ex = kMeterSuccess; ex <= kMeterBudget; ++ex)
    for (int fabric = 0; fabric <= 1; ++fabric)
      for (int sw = 0; sw <= 2; ++sw) {
        Rig r(false, /*spendBatch=*/true);
        r.cfg.midFail = fabric == 1;
        uint32_t want[3] = {0, 0, 0};   // tokens in per host this turn
        if (ex == kMeterBudget) r.cfg.overBudgetHosts = {"anthropic", "openai", "mistral"};
        else if (fabric) meterFabricCase(r, ex, sw, want);
        else meterLegacyCase(r, ex, sw, want);
        const bool ok = r.eng->runTurn("inputs", "1001", "hi");
        char tag[64];
        std::snprintf(tag, sizeof tag, "exit=%d fabric=%d switches=%d", ex, fabric, sw);
        TEST_ASSERT_EQUAL_MESSAGE(ex == kMeterSuccess, ok, tag);
        meterCheck(r, want, tag);
      }
}

// Variant 3 of the usage RCA: a model changed by the turn's own device action (applied
// before the turn ends) never relabels the spend or the turn-end event.
static void test_model_change_during_the_turn_never_relabels_it() {
  Rig r(false, /*spendBatch=*/true);
  r.cfg.orchModels["anthropic"] = "claude-sonnet-5";
  r.onDeliver = [&] { r.cfg.orchModels["anthropic"] = "claude-opus-5"; };   // apply-time
  TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, (int)r.cfg.spendCalls.size());
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", r.cfg.spendCalls[0].spend.slot[0].model);
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", r.hookEnds.back().model.c_str());
}

// Lane K's park on the non-fabric paths (PG2), one group so main stays under the
// length ceiling.
static void runBillingParkTests() {
  RUN_TEST(test_billing_park_holds_on_the_single_shot_ladder);
  RUN_TEST(test_billing_park_lifecycle_on_the_ladder_and_the_fold);
  RUN_TEST(test_billing_park_answered_probe_ends_the_episode);
  RUN_TEST(test_billing_park_probe_is_one_request_whoever_checks_first);
  RUN_TEST(test_billing_notice_latch_is_shared_and_kept_by_quiet_turns);
}


// ---- L19-13 d/e, L19-14: one honest owner message per failed turn ----------------

// d: a two-switch failure on the fabric sends exactly ONE message (it used to send
// two causeless "switching to" notices and then the failure notice).
static void test_l19_13_two_switch_failure_sends_one_message() {
  Rig r;
  r.cfg.midFail = true;
  r.fabricVisits = {{0, 0, "network"}, {0, 0, "network"}, {0, 0, "network"}};
  r.fabricOk = false;
  r.fabricErr = "network";
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_FALSE(r.anyDelivered("switching to"));
  TEST_ASSERT_TRUE(r.lastText().rfind("That didn't finish - ", 0) == 0);
  // e: every host failed before anything ran: the message was not answered.
  TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("Your message was not answered."), r.lastText().c_str());
  TEST_ASSERT_FALSE(r.hookEnds.back().fallback);   // no host billed: never a fallback
  TEST_ASSERT_TRUE(LogCapture::contains("switch notice(s) folded into the final notice"));
}

// d: a billing switch in a failed turn: the notice's one next step rides the final
// message (and its once-per-episode latch is taken).
static void test_l19_13_failed_billing_switch_folds_the_credit_line() {
  Rig r;
  r.cfg.midFail = true;
  agent::providers::setTestFabricSwitch({"anthropic", "openai", orch::StepErr::Billing, 400, false});
  r.fabricVisits = {{0, 0, "messages HTTP 400: Your credit balance is too low"}, {0, 0, "network"}};
  r.fabricOk = false;
  r.fabricErr = "network";
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_TRUE_MESSAGE(r.anyDelivered("Add credit on anthropic's billing page."), r.lastText().c_str());
  TEST_ASSERT_FALSE(orch::billingNoticeDue("anthropic"));   // the latch was taken with it
  orch::billingEpisodeEnd("anthropic");
  agent::providers::setTestFabricSwitch({});
}

// d, counter-test: a switched turn that is served still sends its reply exactly once
// (the held switch notice goes out just before it).
static void test_l19_13_switched_success_sends_its_reply_once() {
  Rig r;
  r.cfg.midFail = true;
  r.fabricVisits = {{50, 10, "network"}, {100, 20, ""}};
  r.fabricOk = true;
  TEST_ASSERT_TRUE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, r.countDelivered("hello there"));
  TEST_ASSERT_EQUAL(1, r.countDelivered("switching to"));
  TEST_ASSERT_TRUE(r.delivered.back().second.find("hello there") != std::string::npos);
}

// d: the single-shot ladder (two failover notices, then failure) also sends one.
static void test_l19_13_ladder_failure_sends_one_message() {
  Rig r;
  for (const char* h : {"anthropic", "openai", "mistral"}) r.scripts[h] = {{false, "", "network", 0, "", 0, 0}};
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_TRUE(r.attempts.size() >= 3);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
}

// L19-14: a request the device could not send gets its own honest words, the same
// whatever the totals said, and no failover; with tools already run the message
// keeps the work instead of "not answered".
static void test_l19_14_unsent_reply_keeps_the_work() {
  Rig r;
  r.scripts["anthropic"] = {{false, "", "unsent: the device could not finish sending its request", 0, "", 0, 0}};
  TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, (int)r.attempts.size());   // no same-host retry, no failover
  TEST_ASSERT_EQUAL_STRING("That didn't finish - the device could not finish sending its request (memory is "
                           "tight). Your message was not answered. Nothing is still running; try again in a "
                           "minute.",
                           r.lastText().c_str());
  Rig w;
  w.cfg.midFail = true;
  w.fabricDispatch = 2;
  w.fabricBrief = "[user] hi\n[tool] memory_search {}\n[result] memory_search 3 hits\n"
                  "[tool] memory_write {}\n[result] memory_write saved\n";
  w.fabricVisits = {{100, 20, ""}};
  w.fabricOk = false;
  w.fabricErr = "unsent: the device could not finish sending its request";
  TEST_ASSERT_FALSE(w.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_EQUAL(1, (int)w.delivered.size());
  TEST_ASSERT_TRUE_MESSAGE(w.anyDelivered("Already done this turn: memory_search, memory_write."),
                           w.lastText().c_str());
  TEST_ASSERT_FALSE(w.anyDelivered("not answered"));
}

// Lane K2 (L19-13 d/e, L19-14): one honest owner message per failed turn.
static void runLaneK2Tests() {
  RUN_TEST(test_recall_outage_is_announced_not_silent);
  RUN_TEST(test_l19_13_two_switch_failure_sends_one_message);
  RUN_TEST(test_l19_13_failed_billing_switch_folds_the_credit_line);
  RUN_TEST(test_l19_13_switched_success_sends_its_reply_once);
  RUN_TEST(test_l19_13_ladder_failure_sends_one_message);
  RUN_TEST(test_l19_14_unsent_reply_keeps_the_work);
}

// ---- lane DEF: the monthly spend ceiling at the turn core ---------------------------
static bool g_turnFrozen = false;
static bool turnFrozen() { return g_turnFrozen; }
struct FreezeScope {
  explicit FreezeScope(bool frozen) { g_turnFrozen = frozen; orch::installSpendFreeze(&turnFrozen); }
  ~FreezeScope() { orch::installSpendFreeze(nullptr); g_turnFrozen = false; }
};

// While paid calls are paused an owner message reaches no provider (recall's embed
// included) and gets no reply of its own: the owner hears why once per channel from
// the device's notice pump. The message itself is still kept in the history.
static void test_spend_ceiling_refuses_an_owner_turn_before_recall() {
  Rig r;
  FreezeScope frozen(true);
  r.eng->handleMessage("what's the weather", "Roy", "1001");
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  TEST_ASSERT_EQUAL(0, (int)r.recallQueries.size());
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
  TEST_ASSERT_EQUAL(0, (int)r.hookStarts.size());
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());
  TEST_ASSERT_FALSE(r.eng->turnInFlight());
}

// A routine, a wakeup and the dream are deferred (no daily fire burned, no failure
// breaker rolled) and dial nothing; the fold waits too.
static void test_spend_ceiling_defers_scheduled_turns_and_the_fold() {
  Rig r;
  FreezeScope frozen(true);
  struct Case { const char* id; bool quiet, once; } cases[] = {
      {"lp1", false, false}, {"lp2", false, true}, {"dream", true, false}};
  for (const Case& c : cases) {
    orch::FireOutcome o = r.eng->injectScheduledTurn("", "do the task", "task", c.id, c.quiet, c.once);
    TEST_ASSERT_TRUE(o.deferred);
    TEST_ASSERT_FALSE(o.ok);
    TEST_ASSERT_TRUE_MESSAGE(o.detail.find("spend ceiling") != std::string::npos, o.detail.c_str());
    // The scheduler tells a ceiling defer from a memory one by this marker (lane DEF g3).
    TEST_ASSERT_TRUE(orch::isSpendFrozenError(o.error));
  }
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
  std::string sum;
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "digest", sum) == TurnEngine::FoldResult::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.attempts.size());
}

// Counter-test plus the background role: open, the same scheduled turn runs, and the
// provider sees it as a background turn (the cheapest tier, low effort); an owner
// turn right after runs as the owner's.
static void test_scheduled_turns_run_in_the_background_role() {
  Rig r;
  FreezeScope open(false);
  r.scripts["anthropic"] = {{true, "{\"reply\":\"ok\"}", "", 0}};
  std::vector<orch::TurnRole> roles;
  r.onProviderCall = [&] { roles.push_back(orch::currentTurnRole()); };
  orch::FireOutcome o = r.eng->injectScheduledTurn("", "do the task", "task", "lp1", false, false);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_FALSE(o.deferred);
  r.eng->handleMessage("hello", "Roy", "1001");
  TEST_ASSERT_EQUAL(2, (int)roles.size());
  TEST_ASSERT_TRUE(roles[0] == orch::TurnRole::Background);
  TEST_ASSERT_TRUE(roles[1] == orch::TurnRole::Owner);
  TEST_ASSERT_TRUE(orch::currentTurnRole() == orch::TurnRole::Owner);
}

// The freeze rides K2's park: a turn that passed the gate an instant before the ceiling
// froze finds every host "paused" in the park. It sends nothing (the notice pump tells
// the owner once); the owner's own pause, with no ceiling, still gets its reply.
static int g_freezeConsults = 0;
static bool freezesAfterTheGate() { return ++g_freezeConsults > 1; }
static void test_a_ceiling_pause_found_in_the_park_sends_nothing() {
  const char* paused = "paused: paid requests to anthropic are paused";
  {
    Rig r;
    g_freezeConsults = 0;
    orch::installSpendFreeze(&freezesAfterTheGate);
    r.cfg.midFail = true;
    r.fabricVisits = {{0, 0, paused}};
    r.fabricOk = false;
    r.fabricErr = paused;
    TEST_ASSERT_FALSE(r.eng->runTurn("inputs", "1001", "hi"));
    TEST_ASSERT_EQUAL(0, (int)r.delivered.size());
    orch::installSpendFreeze(nullptr);
  }
  Rig o;   // counter-test: the same paused answer with no ceiling is the owner's pause
  o.cfg.midFail = true;
  o.fabricVisits = {{0, 0, paused}};
  o.fabricOk = false;
  o.fabricErr = paused;
  TEST_ASSERT_FALSE(o.eng->runTurn("inputs", "1001", "hi"));
  TEST_ASSERT_TRUE(o.delivered.size() >= 1);
}

static void runSpendCeilingTests() {
  RUN_TEST(test_a_ceiling_pause_found_in_the_park_sends_nothing);
  RUN_TEST(test_spend_ceiling_refuses_an_owner_turn_before_recall);
  RUN_TEST(test_spend_ceiling_defers_scheduled_turns_and_the_fold);
  RUN_TEST(test_scheduled_turns_run_in_the_background_role);
}

// The later lanes' suites, in merge order (main sits at the length gate).
static void runLateLaneTests() {
  runLaneK2Tests();
  runSpendCeilingTests();
}

// The suite in two halves (one function per half keeps each under the length gate).
static void runFirstHalf() {
  RUN_TEST(test_lane_l_a_silent_synthesis_never_drops_the_results);
  RUN_TEST(test_lane_l_a_cut_answer_is_salvaged_marked_and_captured);
  RUN_TEST(test_lane_l_the_salvage_marker_appears_exactly_when_cut);
  RUN_TEST(test_lane_l_a_reported_cut_salvages_the_partial_turn);
  RUN_TEST(test_lane_l_a_stringified_spawn_list_starts_its_sub);
  RUN_TEST(test_lane_l_an_empty_task_spawn_is_reported_not_skipped);
  RUN_TEST(test_lane_l_a_promise_without_a_spawn_is_corrected);
  RUN_TEST(test_lane_l_a_cut_answer_that_claims_spawns_is_marked_and_corrected);
  RUN_TEST(test_lane_l_no_cut_ever_delivers_a_bare_spawn_claim);
  RUN_TEST(test_lane_l_a_prose_answer_that_claims_spawns_is_corrected);
  RUN_TEST(test_lane_l_pseudo_tool_spawns_join_the_turn_exactly_once);
  RUN_TEST(test_lane_l_an_empty_pseudo_tool_call_is_reported);
  RUN_TEST(test_lane_l_recovered_actions_apply_only_on_clean_owner_turns);
  RUN_TEST(test_lane_l_a_gated_recovered_spawn_is_reported);
  RUN_TEST(test_lane_l_the_final_check_rides_every_loop_turn);
  RUN_TEST(test_lane_s_the_claim_check_joins_the_final_check);
  RUN_TEST(test_pg5_custom_head_saved_claim_delivers_one_reply);
  RUN_TEST(test_pg5_custom_head_durability_claim_follows_the_flash_save);
  RUN_TEST(test_lane_l_markup_never_reaches_the_owner);
  RUN_TEST(test_lane_l_the_spawn_ledger_rides_the_next_turn);
  RUN_TEST(test_lane_l_capacity_names_every_keyed_provider_by_its_mode);
  RUN_TEST(test_every_failed_turn_exit_carries_a_cause);
  RUN_TEST(test_owner_notice_names_the_first_cause);
  RUN_TEST(test_failure_notice_retry_advice_only_when_transient);
  RUN_TEST(test_failure_notice_names_helpers_still_running);
  RUN_TEST(test_no_billed_response_means_no_fallback);
  RUN_TEST(test_scheduled_failure_reports_cause_and_own_spend);
  RUN_TEST(test_fallback_note_names_previous_turn_and_reason);
  RUN_TEST(test_metering_matrix_once_per_turn_per_host);
  RUN_TEST(test_model_change_during_the_turn_never_relabels_it);
  RUN_TEST(test_convmap_roundtrip_and_isolation);
  RUN_TEST(test_convmap_legacy_discarded_and_lru);
  RUN_TEST(test_no_provider_gives_honest_local_reply_not_silence);
  RUN_TEST(test_router_only_provider_is_not_told_no_provider);
  RUN_TEST(test_keyless_pinned_head_runs_on_a_keyed_host_and_says_so_once);
  RUN_TEST(test_keyed_provider_outside_the_priority_list_runs_the_turn);
  RUN_TEST(test_no_key_anywhere_answers_every_turn_kind_honestly);
  RUN_TEST(test_any_provider_configured_predicate);
  RUN_TEST(test_loop_gate_uses_entry_heap_not_the_recall_dip);
  RUN_TEST(test_loop_defers_when_entry_heap_is_genuinely_low);
  RUN_TEST(test_recalibrated_floor_invariants);
  RUN_TEST(test_chat_gate_defers_on_fragmentation_not_total_free);
  RUN_TEST(test_chat_gate_admits_at_the_recalibrated_floor);
  RUN_TEST(test_chat_gate_admits_fragmented_cyd_is_not_deferred);
  RUN_TEST(test_oom_transport_error_yields_low_memory_reply_no_failover);
  RUN_TEST(test_network_error_still_fails_over_not_misclassified_oom);
  RUN_TEST(test_two_turns_share_the_static_prompt_block);
  RUN_TEST(test_happy_turn_delivers_and_accounts);
  RUN_TEST(test_recall_injected_for_user_text_skipped_for_synthesis);
  RUN_TEST(test_same_host_fresh_conv_retry);
  RUN_TEST(test_failover_walks_priority_with_owner_notice);
  RUN_TEST(test_turn_spend_files_under_the_serving_model);
  RUN_TEST(test_failed_attempt_echo_never_names_the_serving_model);
  RUN_TEST(test_cum477_unsent_request_reply_blames_memory_not_connection);
  RUN_TEST(test_prompt_round_cap_follows_the_configured_loop);
  RUN_TEST(test_cum544_timeout_fails_over_without_same_host_retry);
  RUN_TEST(test_cum542_web_page_chat_is_the_web_ui);
  RUN_TEST(test_cum544_all_timeouts_say_no_answer_in_time);
  RUN_TEST(test_cum530_router_refusal_notice_is_never_unavailable);
}

static void runSecondHalf() {
  RUN_TEST(test_cum544_second_notice_names_its_own_reason);
  RUN_TEST(test_cum544_only_the_timeout_token_skips_the_retry);
  RUN_TEST(test_cum523_telegram_only_tools_are_exactly_these);
  RUN_TEST(test_cum523_no_delivery_line_without_a_real_target);
  RUN_TEST(test_no_retry_after_tool_dispatched);
  RUN_TEST(test_rate_limit_reply_names_the_window);
  RUN_TEST(test_billing_refusal_is_not_a_rate_limit);
  runBillingParkTests();
  RUN_TEST(test_budget_failover_and_exhausted_refusal);
  RUN_TEST(test_salvage_never_delivers_raw_json);
  RUN_TEST(test_cum483_turn_without_memory_keeps_side_effects);
  RUN_TEST(test_cum486_advertised_models_equal_spawnable_for_every_slot);
  RUN_TEST(test_cum486_router_only_devices_advertise_their_models);
  RUN_TEST(test_cum485_prev_reply_code_reaches_the_next_tool_loop_turn);
  RUN_TEST(test_cum530_router_selector_head_gets_its_real_window);
  RUN_TEST(test_cum495_zai_prose_reply_is_delivered_not_apologized);
  RUN_TEST(test_cum495_every_head_delivers_prose_and_fenced_turns);
  RUN_TEST(test_cum495_turn_followed_by_more_text_never_acts);
  RUN_TEST(test_reaper_spares_a_live_retry_and_failover_ladder);
  RUN_TEST(test_cum495_json_array_reply_is_never_shown_raw);
  RUN_TEST(test_truncated_json_array_reply_is_never_shown_raw);
  RUN_TEST(test_head_host_is_the_host_the_turn_runs_on);
  RUN_TEST(test_scheduled_turn_rails_and_fire_outcome);
  RUN_TEST(test_once_wakeup_gets_wakeup_preamble_not_recurring);
  RUN_TEST(test_owner_reminder_gets_reminder_framing_not_wakeup);
  RUN_TEST(test_cum523_telegramless_wakeup_has_no_telegram_leg);
  RUN_TEST(test_cum523_telegram_device_keeps_the_tool);
  RUN_TEST(test_cum523_telegram_is_sampled_once_per_turn);
  RUN_TEST(test_cum523_deliverability_rule);
  RUN_TEST(test_cum523_public_mode_empty_allowlist_is_decided_per_turn);
  RUN_TEST(test_cum523_public_guest_turn_gets_reply_but_no_file_send);
  RUN_TEST(test_cum523_telegramless_prompt_names_no_telegram_tool);
  RUN_TEST(test_synthesis_consolidation_and_raw_fallback);
  RUN_TEST(test_turn_debug_hook_success_and_failure);
  RUN_TEST(test_lastturn_head_names_the_chain_and_the_device_history);
  RUN_TEST(test_lastturn_head_golden_loop_turn_with_summary);
  RUN_TEST(test_lastturn_head_golden_loop_turn_without_summary);
  RUN_TEST(test_lastturn_legend_lists_every_prompt_section);
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
  RUN_TEST(test_unverified_connector_result_closes_memory_for_that_turn);
  RUN_TEST(test_spawn_refusal_reaches_the_next_turn_once);
  RUN_TEST(test_timed_out_spawn_is_not_respawned_by_the_next_turn);
  RUN_TEST(test_unverified_turn_shuts_every_memory_store_tool);
  RUN_TEST(test_unverified_turn_recovery_for_mem_write_names_the_refusal);
  RUN_TEST(test_hooks_turn_end_fires_on_failure_and_sources_track);
  runLateLaneTests();   // lanes K2 and DEF (and the recall-outage test, kept in order; main sits at the length gate)
}

int main(int, char**) {
  UNITY_BEGIN();
  runFirstHalf();
  runSecondHalf();
  UNITY_END();
  return 0;
}
