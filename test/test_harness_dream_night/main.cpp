#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/compose.h"   // loopToolName - the advertised (wire) tool names
#include "nimbus/harness/dream.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"  // setTestFabricSwitch - the scripted fabric's switch record
#include "nimbus/mem_cap.h"
#include "nimbus/orch/billing_refusal.h"   // billingEpisodeEnd - a fresh episode per test
#include "nimbus/orch/loops.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_memory.h"

#include <algorithm>

#include "morning_after.h"   // RB-Q2b: the morning after a scripted night, both trees

// Lane M, then RB-Q2b - the nightly upkeep and quiet, honest background turns:
//   - a scripted night makes no model call, bills nothing and still runs decay, prune and
//     dedup; the morning after carries at least the earlier conversation rb-main's did
//     (morning_after.h, two trees, owner ruling 81);
//   - maintenance runs once per synced local day whatever the store does; every fire
//     writes exactly one row (outcome property over generated nights);
//   - dream state lives in a file, and a file earlier firmware wrote loads safely;
//   - the reserved loop survives the consec-fail breaker, the busy gate is bounded;
//   - a quiet background turn delivers NO engine notice on any failure path, while
//     the same failure on an ordinary routine or an owner turn still does;
//   - a failed fire reports its own spend, never the previous turn's, and
//     [MEMORY RESULTS] go back to the chat that asked;
//   - no turn source holds a note during quiet hours (the wake capture).

using agent::JobEngine;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace dream = agent::dream;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static uint32_t g_seed = 1;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static bool validUtf8(const std::string& s) {
  return nimbus::utf8CapLen(s.c_str(), (int)s.size(), (int)s.size()) == (int)s.size();
}

// Every tool the device registers (src/ and lib/), so the allowlist property covers
// the real surface. A tool added on the device and missing here is still covered by
// the default-deny rule (toolAllowed lists names; it never lists prefixes).
static const char* const kDeviceTools[] = {
    "artifact.save", "device.selftest", "device.status", "docs.list", "docs.read",
    "docs.search", "files.fetch", "files.list", "files.read", "files.search", "files.send",
    "files.share", "files.stat", "image.generate", "loop.cancel", "loop.create", "loop.list",
    "media.list", "media.pause", "media.play", "media.stop", "memory.archive", "memory.config",
    "memory.episodic", "memory.pin", "memory.scratchpad", "memory.search", "memory.update",
    "memory.write", "ota.status", "reply.speak", "reply.telegram", "results.get",
    "results.list", "session.list", "session.poll", "session.spawn", "session.tell",
    "session.terminate", "skill.delete", "skill.get", "skill.list", "skill.save",
    "system.health", "tenant.list", "tenant.set_quota", "tenant.set_role", "wakeup.set",
    "web.search"};

// ---- an engine rig with scriptable provider failures --------------------------

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  std::vector<std::pair<std::string, std::string>> delivered;
  std::vector<std::string> mcpCalls;      // registry names that reached dispatch
  std::vector<std::string> advertised;    // tool names the provider was shown
  std::vector<std::string> inputsSeen;
  std::vector<std::string> callNames;     // names the scripted model calls mid-turn
  int spawns = 0, deviceActions = 0;
  int asks = 0, cues = 0, completes = 0, speaks = 0, captures = 0;   // the other channels
  bool memFails = false;                  // memory writes refused (an embedding outage)
  // Lane ECHO owner design (9/11 Nov): quiet hours on => a background turn's reply
  // is captured into TurnEndEv.wakeReply (never delivered at night, never held).
  bool quietHours = false;
  std::vector<std::string> wakeReplies;   // every TurnEndEv.wakeReply seen
  // Per call: ok + error + output + usage.
  bool ok = true;
  std::string err;
  std::string outJson = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";
  uint32_t usageIn = 0, usageOut = 0;
  // Opt-in scripted fabric (the billing-switch test): anthropic refuses with a
  // billing 400 and the turn moves to the next host, which answers.
  bool billingSwitch = false;
  orch::StepErr switchKind = orch::StepErr::None;   // any other scripted fabric switch
  std::unique_ptr<JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;

  // The billing latches live in the one process-wide park: every rig starts a fresh
  // episode (a billing refusal is told once per episode, then answered once, then not).
  static void freshBillingEpisodes() {
    for (const char* h : {"anthropic", "openai", "mistral"}) orch::billingEpisodeEnd(h);
  }

  Rig() {
    freshBillingEpisodes();
    auto deliverFn = [this](const std::string& c, const std::string& t) { delivered.push_back({c, t}); };
    JobEngine::Deps jd;
    jd.platform = plat.contract();
    jd.deliver = deliverFn;
    jobs.reset(new JobEngine(std::move(jd)));
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = deliverFn;
    d.recall = [](const std::string&, const orch::Principal&) { return orch::RecallOut{}; };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      return in;
    };
    d.toolSpecs = [](const orch::Principal&) {
      std::vector<orch::ToolRegistry::Spec> v;
      for (const char* n : kDeviceTools) v.push_back(orch::ToolRegistry::Spec{n, "d", "{}"});
      return v;
    };
    d.mcpDispatch = [this](const std::string& req, const orch::Principal&) {
      JsonDocument rq;
      deserializeJson(rq, req);
      mcpCalls.push_back(rq["params"]["name"] | "");
      return std::string("{\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}");
    };
    d.firstAllowedChat = [] { return std::string("1001"); };
    d.hooks.quietHoursNow = [this] { return quietHours; };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      if (!ev.wakeReply.empty()) wakeReplies.push_back(ev.wakeReply);
    };
    d.apply.deliver = deliverFn;
    d.apply.principalFor = [](const std::string& chat) {
      return orch::principalForRole(chat, orch::Role::Admin);
    };
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst a, const orch::Principal&) {
      if (std::string(name) == "memory.search")
        return orch::ToolResult::ok(std::string("found: ") + (a["query"] | ""));
      if (memFails) return orch::ToolResult::fail("embedding unavailable");
      return orch::ToolResult::ok("stored memory m1");
    };
    d.apply.emitAsk = [this] { asks++; };
    d.apply.fire = [this](const char*) { cues++; };
    d.apply.turnComplete = [this](const std::string&) { completes++; };
    d.apply.speak = [this](const std::string&, const std::string&) {
      speaks++;
      return orch::SpeakOutcome::spoke();
    };
    d.apply.captureAssistant = [this](const std::string&, const std::string&) { captures++; };
    d.apply.enqueueSpawn = [this](const orch::Spawn&, const std::string&, bool) { spawns++; return true; };
    d.apply.stageDevice = [this](const orch::ValidatedAction&) { deviceActions++; };
    for (const char* h : {"anthropic", "openai", "mistral"}) {
      const std::string host = h;
      d.hosts.add(host, [this](std::string& conv, const std::string&, const std::string& inp,
                               std::string& out, std::string& e, const agent::HeadTools* tools,
                               orch::TokenUsage* usage) {
        inputsSeen.push_back(inp);
        if (tools) {
          advertised.clear();
          for (const auto& s : tools->specs) advertised.push_back(s.name);
          for (const auto& n : callNames) {
            orch::HeadToolCall c;
            c.id = "t";
            c.name = n;
            c.argsJson = "{}";
            tools->dispatch(c);
          }
        }
        if (usage && (usageIn || usageOut)) usage->add(usageIn, usageOut);
        conv = "c";
        if (!ok) { e = err; return false; }
        out = outJson;
        return true;
      });
    }
    wireFabricStub(d);
    eng.reset(new TurnEngine(std::move(d)));
  }

  // Opt-in scripted fabric (out of the constructor only to keep the lizard
  // ceiling honest): with billingSwitch set, the first host refuses with a
  // billing 400, the turn moves to the next host and answers there. The engine's
  // notify callback reads providers::lastFabricSwitch() inside the notify call,
  // so the rig pins the record the way the real loop would.
  void wireFabricStub(TurnEngine::Deps& d) {
    d.hosts.fabricSupports = [](const std::string& h) {
      return h == "anthropic" || h == "openai" || h == "mistral";
    };
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string&,
                            const std::string&, std::string& out, std::string& err,
                            const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      const orch::StepErr kind = billingSwitch ? orch::StepErr::Billing : switchKind;
      if (kind == orch::StepErr::None || hl.size() < 2) {
        err = "network: connect failed";
        return false;
      }
      const bool billing = kind == orch::StepErr::Billing;
      if (ht.onStepError)
        ht.onStepError(hl[0], billing ? "chat HTTP 400: credit balance is too low" : "network: no response");
      agent::providers::setTestFabricSwitch({hl[0], hl[1], kind, billing ? 400 : 0, false});
      if (notify) notify(hl[0], hl[1]);
      orch::TokenUsage u;
      u.add(40, 6);
      if (spend) spend->add(hl[1].c_str(), "model-next", u);
      out = outJson;
      return true;
    };
  }
  // A quiet routine (quietOk): an all-empty turn delivers nothing, its notices are held.
  orch::FireOutcome quietTurn(const std::string& inputs = "tidy the notes", const std::string& chat = "1001") {
    return eng->injectScheduledTurn(chat, inputs, "Tidy", "Q1", /*quietOk=*/true);
  }
  orch::FireOutcome routineTurn(const std::string& chat = "1001") {
    return eng->injectScheduledTurn(chat, "check the weather", "Morning", "L1", /*quietOk=*/false);
  }
  // Nothing reached the owner on any channel: no message, ask, sound, completion,
  // speech or history row.
  int ownerContacts() const {
    return (int)delivered.size() + asks + cues + completes + speaks + captures;
  }
};

// The failure paths a turn can end on, each scripted through the rig.
struct FailCase { const char* name; const char* err; bool garbage; bool overBudget; };
static const FailCase kFails[] = {
    {"server error", "chat HTTP 500: overloaded", false, false},
    {"rate limit", "chat HTTP 429: slow down", false, false},
    {"timeout", "timeout: no answer in 120 s", false, false},
    {"low memory", "low memory: alloc failed", false, false},
    {"key", "chat HTTP 401: invalid x-api-key", false, false},
    {"credit", "chat HTTP 400: credit balance is too low", false, false},
    {"network", "network: connect failed", false, false},
    {"no text", "", false, false},
    {"parse", "", true, false},
    {"budget", "", false, true},
};

static void script(Rig& r, const FailCase& f) {
  if (f.overBudget) {
    r.cfg.overBudgetHosts = {"anthropic", "openai", "mistral"};
  } else if (f.garbage) {
    r.outJson = "{\"reply\":\"half a turn";   // a truncated object: unusable, nothing to salvage
  } else {
    r.ok = false;
    r.err = f.err;
  }
}

// ---- (1) quiet background turns deliver no engine notice ------------------------

static void test_quiet_turn_notice_count_zero_on_every_failure_path() {
  for (const FailCase& f : kFails) {
    Rig q;
    script(q, f);
    orch::FireOutcome o = q.quietTurn();
    char msg[96];
    snprintf(msg, sizeof msg, "quiet turn delivered a notice (%s)", f.name);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)q.delivered.size(), msg);
    TEST_ASSERT_FALSE_MESSAGE(o.ok, f.name);
    TEST_ASSERT_FALSE_MESSAGE(o.error.empty(), f.name);   // the cause is kept for the row
    TEST_ASSERT_TRUE(q.eng->quietNoticesHeld() >= 1);
    // Counter-tests: the SAME failure on an ordinary routine and on an owner turn
    // still tells the owner (the gate is the quiet flag, not "scheduled").
    Rig n;
    script(n, f);
    n.routineTurn();
    snprintf(msg, sizeof msg, "routine turn lost its failure notice (%s)", f.name);
    TEST_ASSERT_TRUE_MESSAGE(n.delivered.size() >= 1, msg);
    Rig owner;
    script(owner, f);
    owner.eng->handleMessage("hello", "Roy", "1001");
    snprintf(msg, sizeof msg, "owner turn lost its failure notice (%s)", f.name);
    TEST_ASSERT_TRUE_MESSAGE(owner.delivered.size() >= 1, msg);
    // The quiet flag never outlives the turn: the owner's next failure is told (on a
    // fresh billing episode: the other rigs above took this process's latch).
    script(q, f);
    Rig::freshBillingEpisodes();
    q.eng->handleMessage("hello again", "Roy", "1001");
    TEST_ASSERT_TRUE(q.delivered.size() >= 1);
  }
}

// Lane ECHO review (11 Nov): the exact time-sensitive reply from the Lumi case - a
// background turn at ~03:31Z wrote this as an owner note and it was delivered at ~12:33Z.
static const char* kTimeSensitiveReply =
    "06:00 commute brief: Great Northern has crew shortages on the 06:00 service; "
    "leave by 07:10 to catch it.";

static void test_quiet_billing_switch_keeps_the_notice_latch_armed() {
  Rig r;
  r.cfg.midFail = true;       // the engine routes the turn through the fabric
  r.billingSwitch = true;
  orch::billingEpisodeEnd("anthropic");   // arm a fresh episode
  const orch::FireOutcome quiet = r.quietTurn();
  TEST_ASSERT_TRUE(quiet.ok);                 // the turn answered on the next host
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());   // nothing at night, as ever
  TEST_ASSERT_TRUE(r.eng->quietNoticesHeld() >= 1);
  // The latch was NOT consumed by the held notice: the same episode's billing
  // switch on an attended turn still reaches the owner...
  const orch::FireOutcome attended = r.routineTurn();
  TEST_ASSERT_TRUE(attended.ok);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("out of credit") != std::string::npos);
  // ...and the episode's second notice stays quiet (one per episode).
  const orch::FireOutcome attended2 = r.routineTurn();
  TEST_ASSERT_TRUE(attended2.ok);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
}

static void test_self_wakeup_turn_is_one_short_toolless_turn() {
  Rig r;
  orch::WakeNote note;
  note.kind = orch::WakeKind::Upkeep;
  note.text = "06:00 commute brief: leave by 07:10 to catch the 06:00 service";
  note.noticedEpoch = 7ull * 86400u + 211ull * 60u;
  note.noticedMinute = 211;
  note.dedupKey = orch::fnv64(note.text);
  r.outJson = "{\"reply\":\"Noticed at 03:31:\\n06:00 commute brief: leave by 07:10\",\"memory\":\"\","
              "\"ask\":\"\",\"device\":[{\"led\":\"red\"}],\"spawn\":[{\"task\":\"rebook\"}],"
              "\"session_ops\":[{\"op\":\"spawn\",\"task\":\"x\"}],\"mem_write\":[{\"content\":\"y\"}]}";
  const orch::FireOutcome o = r.eng->injectScheduledTurn(
      "1001", orch::dumpWakeNote(note), "Self-wakeup: upkeep", "sw00a1b2",
      /*quietOk=*/false, /*once=*/true, /*ownerReminder=*/false, /*selfWakeup=*/true);
  TEST_ASSERT_TRUE(o.ok);
  // The preamble names the wake and renders the note text, not the envelope JSON.
  TEST_ASSERT_FALSE(r.inputsSeen.empty());
  const std::string& inputs = r.inputsSeen.back();
  TEST_ASSERT_TRUE(inputs.find("[SELF-WAKEUP]") != std::string::npos);
  TEST_ASSERT_TRUE(inputs.find("06:00 commute brief") != std::string::npos);
  TEST_ASSERT_TRUE(inputs.find("\"selfWakeup\"") == std::string::npos);
  // No tools were advertised to the head (one short request: lane RB-Q2a, ruling 76).
  TEST_ASSERT_TRUE(r.advertised.empty());
  // Nothing the model wrote changed anything.
  TEST_ASSERT_EQUAL(0, r.deviceActions);
  TEST_ASSERT_EQUAL(0, r.spawns);
  // The reply was delivered (the wake fires at the end of quiet hours).
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("Noticed at 03:31") != std::string::npos);
  // The self-wakeup turn itself never captures a wake reply (no wakeup loop).
  TEST_ASSERT_TRUE(r.wakeReplies.empty());
}

static void test_quiet_preamble_never_asks_for_a_reply() {
  Rig r;
  r.quietTurn();
  TEST_ASSERT_EQUAL(1, (int)r.inputsSeen.size());
  // The 22 Oct amendment (SILENT-ROUTINE-COST, lane I): the quiet preamble says
  // when silence is right and when a short reply is expected - never a blanket
  // "reply to the owner".
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("Silence is right when the owner needs nothing") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("reply to the owner concisely") == std::string::npos);
  Rig n;
  n.routineTurn();
  TEST_ASSERT_TRUE(n.inputsSeen[0].find("do the task now and reply to the owner concisely") != std::string::npos);
}

// ---- (3) honest tokens -----------------------------------------------------------

static void test_failed_fire_reports_no_stale_tokens() {
  Rig r;
  r.usageIn = 100;
  r.usageOut = 20;
  r.outJson = "{\"reply\":\"hi\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("hello", "Roy", "1001");
  TEST_ASSERT_EQUAL(120, (int)r.eng->lastTurnUsage().total());
  // A dream that fails before any billed round reports 0, never the owner's 120.
  r.usageIn = r.usageOut = 0;
  r.ok = false;
  r.err = "chat HTTP 500: overloaded";
  orch::FireOutcome o = r.quietTurn();
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_EQUAL(0, (int)o.tokens.total());
  // The same holds for an ordinary routine.
  r.ok = true;
  r.usageIn = 100; r.usageOut = 20;
  r.eng->handleMessage("hello", "Roy", "1001");
  r.ok = false; r.usageIn = r.usageOut = 0;
  TEST_ASSERT_EQUAL(0, (int)r.routineTurn().tokens.total());
}

// ---- (4) [MEMORY RESULTS] go back to the chat that asked ------------------------

static void test_memory_results_stay_with_their_chat() {
  Rig r;
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"mem_query\":[\"alarm code\"]}";
  r.eng->handleMessage("what is my alarm code?", "Roy", "1001");
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  r.inputsSeen.clear();
  r.eng->handleMessage("hi", "Guest", "2002");   // another principal's chat
  TEST_ASSERT_EQUAL(1, (int)r.inputsSeen.size());
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("[MEMORY RESULTS]") == std::string::npos);
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("alarm code") == std::string::npos);
  r.eng->handleMessage("and?", "Roy", "1001");
  TEST_ASSERT_TRUE(r.inputsSeen[1].find("[MEMORY RESULTS]") != std::string::npos);
  TEST_ASSERT_TRUE(r.inputsSeen[1].find("found: alarm code") != std::string::npos);
  // Consumed once.
  r.eng->handleMessage("again", "Roy", "1001");
  TEST_ASSERT_TRUE(r.inputsSeen[2].find("[MEMORY RESULTS]") == std::string::npos);
  // Bounded by PARK ORDER, never by key order: 20 chats park results in turn; the
  // 8 most recent keep theirs ("tg:10".."tg:19" sort before "tg:2".."tg:9" as
  // strings), and a chat that parked first ("a", the smallest key) is evicted.
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"mem_query\":[\"q\"]}";
  r.eng->handleMessage("x", "M", "a");
  for (int i = 0; i < 20; ++i) r.eng->handleMessage("x", "M", "tg:" + std::to_string(i));
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  auto parked = [&r](const std::string& chat) {
    r.inputsSeen.clear();
    r.eng->handleMessage("y", "M", chat);
    return r.inputsSeen[0].find("[MEMORY RESULTS]") != std::string::npos;
  };
  for (int i = 12; i < 20; ++i)
    TEST_ASSERT_TRUE_MESSAGE(parked("tg:" + std::to_string(i)), ("tg:" + std::to_string(i)).c_str());
  TEST_ASSERT_FALSE(parked("a"));
  TEST_ASSERT_FALSE(parked("tg:3"));
  TEST_ASSERT_FALSE(parked("tg:11"));
}

// ---- (5) the night: maintenance only, with fake seams ------------------------------

struct Night {
  dream::DreamState saved;
  bool haveSaved = false;
  int saves = 0;
  bool saveRefused = false;
  bool mounted = true;             // the store's mount state (storeReady)
  bool useMemo = true;             // the device keeps a working copy for the boot
  dream::DreamMemo memo;
  int maintRuns = 0;
  std::vector<std::string> rows;   // every ev:dream row the night emitted

  dream::NightDeps deps() {
    dream::NightDeps d;
    d.loadState = [this](dream::DreamState& s) {
      if (!haveSaved) return false;   // a fresh device: no file yet
      s = saved;
      return true;
    };
    d.saveState = [this](const dream::DreamState& s) {
      saves++;
      if (saveRefused) return false;
      // Round-trip through the file format, as the device does.
      dream::DreamState back;
      TEST_ASSERT_TRUE(dream::loadDreamState(dream::dumpDreamState(s), back));
      saved = back;
      haveSaved = true;
      return true;
    };
    d.storeReady = [this] { return mounted; };
    d.memo = useMemo ? &memo : nullptr;
    d.maintain = [this] {
      maintRuns++;
      dream::MaintResult m;
      m.ran = true;
      m.pruned = 2;
      m.deduped = 1;
      m.vectors = 40;
      m.persisted = true;
      return m;
    };
    d.emitRow = [this](const agent::DreamEndEv& ev) {
      TEST_ASSERT_NOT_NULL(ev.report);
      rows.push_back(dream::renderDreamRow(*ev.report));
    };
    return d;
  }
};

static dream::NightClock clockAt(uint32_t day, uint16_t minute, bool synced = true) {
  dream::NightClock c;
  c.localDay = day;
  c.minuteOfDay = minute;
  c.epoch = (uint64_t)day * 86400u + minute * 60u;
  c.synced = synced;
  return c;
}

static void checkRow(const std::string& row) {
  TEST_ASSERT_TRUE_MESSAGE(row.rfind("Nightly dream ", 0) == 0, row.c_str());
  TEST_ASSERT_TRUE(row.size() <= 240);
  TEST_ASSERT_TRUE(validUtf8(row));
  TEST_ASSERT_TRUE(row.find('\n') == std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(row.find("token") == std::string::npos, row.c_str());
}

// The class over generated nights (fires at any minute, on any day, synced or not, with
// the store mounted, refusing or gone, the working copy kept or not): every fire is ok,
// bills nothing and writes exactly one row; maintenance runs exactly once per synced
// local day that saw a fire, never twice in a day, never on an unsynced clock.
static void test_night_outcome_property() {
  g_seed = 7;
  for (int run = 0; run < 60; ++run) {
    Night n;
    n.useMemo = run % 3 != 0;
    std::set<uint32_t> syncedDays;
    uint32_t day = 100;
    for (int fire = 0; fire < 12; ++fire) {
      day += rnd() % 3 == 0 ? 1 : 0;
      const bool synced = rnd() % 5 != 0;
      n.mounted = rnd() % 4 != 0;
      n.saveRefused = rnd() % 4 == 0;
      if (!n.useMemo && (!n.mounted || n.saveRefused)) n.mounted = true, n.saveRefused = false;
      const int maintBefore = n.maintRuns;
      const size_t rowsBefore = n.rows.size();
      const dream::NightResult res = dream::runNight(n.deps(), clockAt(day, (uint16_t)(rnd() % 1440), synced));
      TEST_ASSERT_TRUE(res.fire.ok);
      TEST_ASSERT_FALSE(res.fire.deferred);
      TEST_ASSERT_EQUAL_UINT32(0, res.fire.tokens.total());
      TEST_ASSERT_EQUAL(rowsBefore + 1, n.rows.size());
      checkRow(n.rows.back());
      const bool first = synced && !syncedDays.count(day);
      TEST_ASSERT_EQUAL_MESSAGE(first ? 1 : 0, n.maintRuns - maintBefore, n.rows.back().c_str());
      TEST_ASSERT_EQUAL(first, res.report.outcome == dream::DreamOutcome::Ran);
      TEST_ASSERT_EQUAL(first, n.rows.back().rfind("Nightly dream ok: maintenance pruned=2 deduped=1", 0) == 0);
      if (synced) syncedDays.insert(day);
    }
    TEST_ASSERT_EQUAL((int)syncedDays.size(), n.maintRuns);
  }
}

static void test_maintenance_once_per_day_and_the_rows() {
  Night n;
  dream::NightResult r = dream::runNight(n.deps(), clockAt(10, 210));
  TEST_ASSERT_EQUAL(1, n.maintRuns);
  TEST_ASSERT_EQUAL_STRING("Nightly dream ok: maintenance pruned=2 deduped=1 (40 memories)", n.rows.back().c_str());
  TEST_ASSERT_EQUAL_STRING("ran", dream::outcomeSlug(r.report.outcome));
  const std::string day10 = r.fire.detail;
  // The console drill (or a retry) later the same day: no second decay.
  r = dream::runNight(n.deps(), clockAt(10, 300));
  TEST_ASSERT_EQUAL(1, n.maintRuns);
  TEST_ASSERT_EQUAL_STRING("Nightly dream skipped (maintenance done earlier today)", n.rows.back().c_str());
  // The next night runs again, and its fingerprint differs (the loops' repeat guard never
  // pauses the upkeep for doing its job).
  r = dream::runNight(n.deps(), clockAt(11, 210));
  TEST_ASSERT_EQUAL(2, n.maintRuns);
  TEST_ASSERT_TRUE(r.fire.detail != day10);
  // An unsynced clock skips honestly and saves nothing.
  Night u;
  r = dream::runNight(u.deps(), clockAt(10, 210, /*synced=*/false));
  TEST_ASSERT_EQUAL(0, u.maintRuns);
  TEST_ASSERT_EQUAL(0, u.saves);
  TEST_ASSERT_EQUAL_STRING("Nightly dream skipped (clock not set)", u.rows.back().c_str());
  // A night with a maintenance whose vectors did not persist says so.
  Night p;
  dream::NightDeps d = p.deps();
  d.maintain = [] { dream::MaintResult m; m.ran = true; m.vectors = 3; return m; };
  dream::runNight(d, clockAt(10, 210));
  TEST_ASSERT_EQUAL_STRING("Nightly dream ok: maintenance pruned=0 deduped=0 (3 memories), not saved",
                           p.rows.back().c_str());
}

static void test_reserved_loop_survives_the_breaker() {
  // A night is never a failed fire now; the reserved loop's breaker exemption still
  // holds for one an earlier firmware paused, and an owner pause is never undone.
  orch::LoopRecord loop = dream::reservedLoopRecord();
  orch::LoopCaps caps;
  for (int k = 0; k < 10; ++k) {
    orch::onFireResult(loop, false, {}, (uint64_t)k, (uint64_t)k * 86400);
    TEST_ASSERT_FALSE(orch::breakerTrips(loop, caps.maxConsecFails, /*reserved=*/true));
  }
  orch::LoopRecord normal = dream::reservedLoopRecord();
  normal.id = "L1";   // counter-test: a normal routine is disabled at 3
  for (int k = 0; k < 3; ++k) orch::onFireResult(normal, false, {}, (uint64_t)k, (uint64_t)k * 86400);
  TEST_ASSERT_TRUE(orch::breakerTrips(normal, caps.maxConsecFails, false));
  orch::LoopRecord paused = dream::reservedLoopRecord();
  paused.enabled = false;
  paused.pauseReason = orch::PauseReason::ConsecFails;
  paused.consecFails = 3;
  TEST_ASSERT_TRUE(orch::reviveReservedLoop(paused, caps.maxConsecFails));
  TEST_ASSERT_TRUE(paused.enabled);
  orch::LoopRecord owner = dream::reservedLoopRecord();
  owner.enabled = false;
  owner.pauseReason = orch::PauseReason::Owner;
  TEST_ASSERT_FALSE(orch::reviveReservedLoop(owner, caps.maxConsecFails));
  TEST_ASSERT_FALSE(owner.enabled);
}

static void test_gate_starvation_bound() {
  dream::DreamGate g;
  dream::GateInputs in;
  in.nowMs = 5000;
  in.lastTurnEndMs = 4000;   // a turn just ended: busy
  in.freeHeap = 100000;
  int defers = 0;
  for (uint8_t k = 0; k < 40; ++k) {
    in.defersTonight = k;
    if (dream::evaluateGate(g, in).deferSec) defers++;
  }
  TEST_ASSERT_EQUAL(12, defers);   // then the gate passes
  in.lastTurnEndMs = 0;
  in.nowMs = 20u * 60u * 1000u;
  in.activeJobs = 1;   // a job stuck "running"
  defers = 0;
  for (uint8_t k = 0; k < 40; ++k) {
    in.defersTonight = k;
    if (dream::evaluateGate(g, in).deferSec) defers++;
  }
  TEST_ASSERT_EQUAL(12, defers);
  // Counter-test: low heap keeps deferring past the cap (the safety floor).
  in.activeJobs = 0;
  in.freeHeap = 1000;
  in.defersTonight = 200;
  dream::GateResult r = dream::evaluateGate(g, in);
  TEST_ASSERT_TRUE(r.deferSec > 0);
  TEST_ASSERT_EQUAL_STRING("low-heap", r.why);
  // The row the device writes for a night the gate starved (it never fired).
  dream::NightReport starved;
  starved.outcome = dream::DreamOutcome::Starved;
  starved.cause = "recent-turn";
  starved.defers = 12;
  TEST_ASSERT_EQUAL_STRING("Nightly dream skipped tonight (device busy after 12 tries); "
                           "maintenance not run tonight",
                           dream::renderDreamRow(starved).c_str());
  starved.cause = "low-heap";
  TEST_ASSERT_TRUE(dream::renderDreamRow(starved).find("(low memory after") != std::string::npos);
  checkRow(dream::renderDreamRow(starved));
  // The window and the next slot.
  TEST_ASSERT_EQUAL(420, dream::windowEndFor(210));
  TEST_ASSERT_EQUAL(1439, dream::windowEndFor(1400));
  dream::NightClock c = clockAt(1, 425);
  TEST_ASSERT_EQUAL((1440 - 425 + 210) * 60, (int)dream::secondsToNextSlot(c, 210));
  TEST_ASSERT_EQUAL(1440 * 60, (int)dream::secondsToNextSlot(clockAt(1, 210), 210));
}

// ---- (6) dream state: a file, never NVS --------------------------------------------

static void test_dream_state_file_and_earlier_firmware() {
  dream::DreamState s;
  s.maintDay = 20412;
  dream::DreamState back;
  TEST_ASSERT_TRUE(dream::loadDreamState(dream::dumpDreamState(s), back));
  TEST_ASSERT_EQUAL_UINT32(20412, back.maintDay);
  // A file v5.1.2 wrote (attempts, streak, the quiet-night hash, an in-flight marker):
  // the maintenance day is kept, the rest is ignored, nothing is half-trusted.
  const char* old =
      "{\"v\":1,\"maintDay\":20411,\"scrHash\":\"00000000abcdef12\",\"attemptDay\":20411,"
      "\"attempts\":2,\"streakDay\":20410,\"streak\":4,\"last\":\"failed\",\"cause\":\"network\","
      "\"inNight\":20411,\"inAttempt\":2,\"inStart\":\"0000000069000000\"}";
  TEST_ASSERT_TRUE(dream::loadDreamState(old, back));
  TEST_ASSERT_EQUAL_UINT32(20411, back.maintDay);
  // That device's night tomorrow runs maintenance once; today's does not run it again.
  Night n;
  n.saved = back;
  n.haveSaved = true;
  dream::runNight(n.deps(), clockAt(20411, 230));
  TEST_ASSERT_EQUAL(0, n.maintRuns);
  dream::runNight(n.deps(), clockAt(20412, 210));
  TEST_ASSERT_EQUAL(1, n.maintRuns);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"maintDay\":20412}", dream::dumpDreamState(n.saved).c_str());
  // Torn, foreign or oversized: unreadable, never half-trusted.
  for (const std::string& bad : {std::string("{\"v\":1,\"maintD"), std::string("[1,2]"), std::string("null"),
                                 std::string(dream::kStateFileMax + 1, ' ')})
    TEST_ASSERT_FALSE(dream::loadDreamState(bad, back));
  // A field of the wrong type reads as absent.
  TEST_ASSERT_TRUE(dream::loadDreamState("{\"maintDay\":\"x\"}", back));
  TEST_ASSERT_EQUAL_UINT32(0, back.maintDay);
  // Fresh device (no file): maintenance runs on the first synced night.
  Night fresh;
  dream::runNight(fresh.deps(), clockAt(5, 210));
  TEST_ASSERT_EQUAL(1, fresh.maintRuns);
  TEST_ASSERT_EQUAL(1, fresh.saves);
}

static void test_unmounted_or_refusing_store_never_decays_twice() {
  // Not mounted: no write is tried, the row says why, and RAM keeps the day so the
  // drill or a retry the same day does not decay again.
  Night n;
  n.mounted = false;
  dream::NightResult r = dream::runNight(n.deps(), clockAt(30, 210));
  TEST_ASSERT_EQUAL(0, n.saves);
  TEST_ASSERT_FALSE(r.report.stateSaved);
  TEST_ASSERT_EQUAL_STRING("Nightly dream ok: maintenance pruned=2 deduped=1 (40 memories); state not saved "
                           "(storage not mounted)",
                           n.rows.back().c_str());
  dream::runNight(n.deps(), clockAt(30, 260));
  TEST_ASSERT_EQUAL(1, n.maintRuns);
  // Refusing writes: the same, and the row names it.
  Night f;
  f.saveRefused = true;
  r = dream::runNight(f.deps(), clockAt(30, 210));
  TEST_ASSERT_EQUAL(1, f.saves);
  TEST_ASSERT_EQUAL_STRING("storage write failed", r.report.stateWhy.c_str());
  TEST_ASSERT_TRUE(f.rows.back().find("; state not saved (storage write failed)") != std::string::npos);
  dream::runNight(f.deps(), clockAt(30, 260));
  TEST_ASSERT_EQUAL(1, f.maintRuns);
  // Counter-test: with no working copy and a refusing store, a second fire the same day
  // cannot know, and decays again - which is why the device always passes its memo.
  Night g;
  g.saveRefused = true;
  g.useMemo = false;
  dream::runNight(g.deps(), clockAt(30, 210));
  dream::runNight(g.deps(), clockAt(30, 260));
  TEST_ASSERT_EQUAL(2, g.maintRuns);
}

// ---- Lane ECHO owner design (9/11 Nov): no turn source HOLDS a note -------------
// REVIEW-FINDINGS.json: on Lumi v5.0.0 a background turn at ~03:31Z wrote a
// "06:00 commute brief: ..." line as an owner note; it was held and delivered at
// ~12:33Z, right after the owner's next unrelated message. The owner design
// replaces the hold with ONE self-wakeup at the end of quiet hours: during quiet
// hours a background turn's reply is captured into TurnEndEv.wakeReply (never
// delivered at night, never glued to the owner's next message); outside quiet
// hours it is delivered by its normal path.

static void test_no_turn_source_holds_a_note_during_quiet_hours() {
  // (a) every scheduled entry that returns a FireOutcome: during quiet hours the
  // reply is captured into wakeReply, not delivered - whatever the id.
  Rig r;
  r.quietHours = true;
  r.outJson = std::string("{\"reply\":\"") + kTimeSensitiveReply + "\",\"memory\":\"\",\"ask\":\"\"}";
  for (const char* id : {"L1", "system"}) {
    r.wakeReplies.clear();
    r.delivered.clear();
    const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "task", "T", id, /*quietOk=*/true);
    TEST_ASSERT_TRUE_MESSAGE(o.detail.find("commute brief") != std::string::npos, id);
    TEST_ASSERT_TRUE_MESSAGE(r.delivered.empty(), id);   // nothing at night
    TEST_ASSERT_TRUE_MESSAGE(r.wakeReplies.size() == 1, id);   // captured for the wake
    TEST_ASSERT_TRUE_MESSAGE(r.wakeReplies[0].find("commute brief") != std::string::npos, id);
  }
  // (b) outside quiet hours the same turn delivers by its normal path and captures
  // nothing - the wake capture is quiet-hours-only.
  Rig day;
  day.outJson = std::string("{\"reply\":\"") + kTimeSensitiveReply + "\",\"memory\":\"\",\"ask\":\"\"}";
  day.eng->injectScheduledTurn("1001", "task", "T", "L1", /*quietOk=*/false);
  TEST_ASSERT_TRUE(day.delivered.size() >= 1);
  TEST_ASSERT_TRUE(day.wakeReplies.empty());
}

static void test_wake_capture_class_over_every_turn_source() {
  // Class over every TurnSource during quiet hours: a source added to the enum
  // fails this test (the default arm). A person's turn delivers in context and
  // captures nothing; every background source captures its reply into wakeReply
  // and delivers nothing at night.
  for (int s = 0; s <= (int)agent::TurnSource::Serial; ++s) {
    const agent::TurnSource src = (agent::TurnSource)s;
    Rig r;
    r.quietHours = true;
    r.outJson = std::string("{\"reply\":\"") + kTimeSensitiveReply + "\",\"memory\":\"\",\"ask\":\"\"}";
    switch (src) {
      case agent::TurnSource::Owner:
      case agent::TurnSource::Serial:
        // A person's turn: the reply is delivered in context, never captured.
        r.eng->handleMessage("hi", "Roy", src == agent::TurnSource::Serial ? "serial" : "1001");
        TEST_ASSERT_TRUE_MESSAGE(r.delivered.size() >= 1, agent::turnSourceSlug(src, "turn"));
        TEST_ASSERT_TRUE_MESSAGE(r.wakeReplies.empty(), agent::turnSourceSlug(src, "turn"));
        break;
      case agent::TurnSource::Loop: {
        // A routine: nothing delivered at night; the reply rides wakeReply so the
        // glue arms ONE self-wakeup for the end of quiet hours.
        const orch::FireOutcome o = r.eng->injectScheduledTurn("1001", "task", "T", "L1", true);
        TEST_ASSERT_TRUE_MESSAGE(r.delivered.empty(), "loop");
        TEST_ASSERT_TRUE_MESSAGE(o.detail.find("commute brief") != std::string::npos, "loop");
        TEST_ASSERT_TRUE_MESSAGE(r.wakeReplies.size() == 1, "loop");
        break;
      }
      case agent::TurnSource::Synthesis: {
        // A sub-agent synthesis during quiet hours: captured, not delivered.
        r.jobs->addFreshResult("job1", "research-sub", "rail status: all lines running", "chat:1001");
        r.eng->maybeConsolidate("1001");
        TEST_ASSERT_TRUE_MESSAGE(r.delivered.empty(), "synthesis");
        TEST_ASSERT_TRUE_MESSAGE(r.wakeReplies.size() == 1, "synthesis");
        break;
      }
      default:
        TEST_FAIL_MESSAGE("a TurnSource was added: pin its wake-capture behavior here");
    }
  }
}

// ---- (7) write counters on the production write path -------------------------------

static void test_memory_write_counters_on_the_registry_path() {
  orch::VectorMemory v;
  v.configure(8);
  orch::Scratchpad sp;
  orch::MemConfig cfg;
  orch::MemOpCounters ops;
  bool embedUp = true;
  orch::MemoryContext mc;
  mc.vec = &v;
  mc.scratch = &sp;
  mc.cfg = &cfg;
  mc.ops = &ops;
  mc.nowHours = [] { return 1000u; };
  mc.embed = [&embedUp](const std::string& t, std::string& err) {
    if (!embedUp) { err = "embeddings: no key"; return std::vector<int8_t>{}; }
    std::vector<float> f(8, 0.0f);
    for (size_t i = 0; i < t.size(); ++i) f[(unsigned char)t[i] % 8] += 1.0f;
    return orch::VectorMemory::quantize(f);
  };
  orch::ToolRegistry reg;
  orch::registerMemoryTools(reg, mc);
  const orch::Principal owner = orch::principalForRole("1001", orch::Role::Admin);
  auto call = [&](const char* name, const char* args) {
    JsonDocument a;
    deserializeJson(a, args);
    return reg.dispatch(name, a.as<ArduinoJson::JsonObjectConst>(), owner);
  };
  TEST_ASSERT_TRUE(call("memory.write", "{\"content\":\"owner drinks flat white\"}").success);
  TEST_ASSERT_TRUE(call("memory.write", "{\"content\":\"owner drinks flat white\"}").success);
  TEST_ASSERT_TRUE(call("memory.update", "{\"content\":\"owner drinks oat flat white\",\"old\":\"flat white\"}").success);
  embedUp = false;
  TEST_ASSERT_FALSE(call("memory.write", "{\"content\":\"sister birthday 3 March\"}").success);
  TEST_ASSERT_FALSE(call("memory.update", "{\"content\":\"x\",\"old\":\"y\"}").success);
  TEST_ASSERT_EQUAL(1, (int)ops.added);
  TEST_ASSERT_EQUAL(1, (int)ops.dupSkipped);
  TEST_ASSERT_EQUAL(1, (int)ops.updated);
  TEST_ASSERT_EQUAL(2, (int)ops.failed);
  orch::MemOpCounters zero;
  orch::MemOpCounters d = ops - zero;
  TEST_ASSERT_EQUAL(2, (int)d.failed);
}

// The class rule for the routine record /api/loops reads: every write that records
// a failed dream leaves its cause in lastErr. Two writers exist: the scheduler's
// post-fire write (onFireResult + noteFireError with the night's FireOutcome) and the
// boot reconcile of a night the restart cut short (applyInterruptedToLoop). The
// reconcile used to set lastResult fail with no cause (or keep an older night's), so
// /api/loops showed a failed dream that named nothing (unit L, 22 Oct 2026).
// ---- (RB-Q2b) the night has no model turn; the morning after is no poorer -------------
// Owner ruling 81: removing the dream turn must not leave any later head request poorer in
// earlier conversation. morning_after.h runs one evening, one scripted night and one morning
// message; the same file ran on rb-main f5044ee1b and its morning request is the golden
// test/golden/dream_morning_rb_main.txt (PROGRESS.md note 2 lists what the dream produced).

static std::string readText(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::vector<std::string> goldenLines(const char* path) {
  std::vector<std::string> v;
  const std::string t = readText(path);
  size_t s = 0;
  while (s < t.size()) {
    size_t e = t.find('\n', s);
    if (e == std::string::npos) e = t.size();
    const std::string l = t.substr(s, e - s);
    if (!l.empty() && l[0] != '#') v.push_back(l);
    s = e + 1;
  }
  return v;
}

// Every line of `need` is in `have`, counted (a line twice in `need` must be there twice).
// Returns the first line missing ("" = all there).
static std::string firstMissing(const std::vector<std::string>& need, std::vector<std::string> have) {
  for (const std::string& l : need) {
    auto it = std::find(have.begin(), have.end(), l);
    if (it == have.end()) return l;
    have.erase(it);
  }
  return std::string();
}

static void test_scripted_night_makes_no_model_turn() {
  morning::Rig r;
  morning::evening(r);
  const size_t ownerRows = r.cfg.spendCalls.size();
  // Counter-test: the rig bills every provider turn, so the zeros below are not vacuous.
  TEST_ASSERT_EQUAL_INT(4, r.providerCalls);
  TEST_ASSERT_EQUAL_INT(4, (int)ownerRows);
  const morning::NightOut n = morning::scriptedNight(r);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, n.providerCalls, "the night asked a provider");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)n.spendRows, "the night put spend in the usage ledger");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)n.dreamSpendRows, "dream tokens in the usage ledger");
  TEST_ASSERT_TRUE_MESSAGE(n.maintained, "decay, prune and dedup did not run");
  TEST_ASSERT_TRUE_MESSAGE(n.row.rfind("Nightly dream ", 0) == 0, n.row.c_str());
  TEST_ASSERT_TRUE_MESSAGE(n.row.find("token") == std::string::npos, n.row.c_str());
}

static void test_night_offers_no_turn_seam() {
  // The class, not the instance: no seam of the night can start a model turn, and the
  // device glue and the engine hold no dream turn of their own.
  TEST_ASSERT_FALSE(morning::HasTurnSeam<dream::NightDeps>::value);
  const std::string glue = readText("src/agent/dream_subsystem.cpp");
  const std::string engine = readText("lib/harness/src/engine.cpp");
  TEST_ASSERT_FALSE_MESSAGE(glue.empty() || engine.empty(), "source files not found (run from the repo root)");
  TEST_ASSERT_TRUE_MESSAGE(glue.find("injectScheduledTurn") == std::string::npos, "the dream glue starts a turn");
  TEST_ASSERT_TRUE_MESSAGE(engine.find("dreamTurn_") == std::string::npos, "the engine keeps a dream turn");
  TEST_ASSERT_TRUE_MESSAGE(engine.find("dream::") == std::string::npos, "the engine still calls the dream");
}

static void test_morning_after_carries_at_least_rb_main() {
  const std::vector<std::string> base =
      morning::conversationLines(goldenLines("test/golden/dream_morning_rb_main.txt"));
  TEST_ASSERT_TRUE_MESSAGE(base.size() >= 20, "the rb-main golden is missing or short");
  morning::Rig r;
  morning::evening(r);
  morning::scriptedNight(r);
  morning::morningTurn(r);
  const std::vector<std::string> dump = morning::dumpMorning(r);
  // No dream output reaches the morning: there is no model turn left to write one.
  for (const std::string& l : dump) TEST_ASSERT_FALSE_MESSAGE(morning::isDreamWritten(l), l.c_str());
  const std::vector<std::string> mine = morning::conversationLines(dump);
  const std::string missing = firstMissing(base, mine);
  TEST_ASSERT_TRUE_MESSAGE(missing.empty(), ("the morning lost: " + missing).c_str());
  TEST_ASSERT_TRUE(mine.size() >= base.size());
  // What the dream used to take from the morning (note 2, item 14) is back: the evening's
  // memory search results and the refused spawn's note.
  const std::string all = [&mine] { std::string a; for (const auto& l : mine) a += l + "\n"; return a; }();
  TEST_ASSERT_TRUE_MESSAGE(all.find("trip budget -> found: trip budget") != std::string::npos, all.c_str());
  TEST_ASSERT_TRUE_MESSAGE(all.find("input|- 'price-check' did not start: the sub-agent limit was reached") != std::string::npos, all.c_str());
  for (const std::string& l : mine)
    if (std::find(base.begin(), base.end(), l) == base.end()) std::printf("RBQ2B-MORNING added: %s\n", l.c_str());
  // Counter-test: the comparison catches a lost line of conversation, in the history and in
  // the input alike.
  for (const char* where : {"history|", "input|"}) {
    std::vector<std::string> poorer = mine;
    for (auto it = poorer.begin(); it != poorer.end(); ++it)
      if (it->rfind(where, 0) == 0 && std::find(base.begin(), base.end(), *it) != base.end()) {
        poorer.erase(it);
        break;
      }
    TEST_ASSERT_FALSE_MESSAGE(firstMissing(base, poorer).empty(), where);
  }
  // Counter-test: the exemption is only for dream outputs (an evening line is not one).
  TEST_ASSERT_FALSE(morning::isDreamWritten("history|user|My sister Ana arrives on the 14th and stays two nights."));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scripted_night_makes_no_model_turn);
  RUN_TEST(test_night_offers_no_turn_seam);
  RUN_TEST(test_morning_after_carries_at_least_rb_main);
  RUN_TEST(test_quiet_turn_notice_count_zero_on_every_failure_path);
  RUN_TEST(test_quiet_billing_switch_keeps_the_notice_latch_armed);
  RUN_TEST(test_self_wakeup_turn_is_one_short_toolless_turn);
  RUN_TEST(test_quiet_preamble_never_asks_for_a_reply);
  RUN_TEST(test_failed_fire_reports_no_stale_tokens);
  RUN_TEST(test_memory_results_stay_with_their_chat);
  RUN_TEST(test_night_outcome_property);
  RUN_TEST(test_maintenance_once_per_day_and_the_rows);
  RUN_TEST(test_reserved_loop_survives_the_breaker);
  RUN_TEST(test_gate_starvation_bound);
  RUN_TEST(test_dream_state_file_and_earlier_firmware);
  RUN_TEST(test_unmounted_or_refusing_store_never_decays_twice);
  RUN_TEST(test_no_turn_source_holds_a_note_during_quiet_hours);
  RUN_TEST(test_wake_capture_class_over_every_turn_source);
  RUN_TEST(test_memory_write_counters_on_the_registry_path);
  return UNITY_END();
}
