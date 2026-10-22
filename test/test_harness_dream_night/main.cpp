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

// Lane M - quiet, honest background turns. The night pipeline (dream::runNight)
// driven end to end with fake seams, and the TurnEngine rails a background turn
// relies on:
//   - a night's outcome is "ok" only when every memory write landed, and the row
//     says what happened (outcome property over generated nights);
//   - a quiet background turn delivers NO engine notice on any failure path, while
//     the same failure on an ordinary routine or an owner turn still does;
//   - the dream sees and can call memory tools only, and its orch_turn device /
//     session fields are dropped (allowlist property over every registry tool);
//   - a failed fire reports its own spend, never the previous turn's;
//   - same-night retries are bounded, maintenance runs once per local day, the
//     reserved loop survives the consec-fail breaker, the busy gate is bounded;
//   - dream state lives in a file (fresh device, legacy NVS migration, zero NVS
//     writes), and [MEMORY RESULTS] go back to the chat that asked;
//   - a night the restart cut short is reconciled at boot: the marker written
//     before the turn names the attempt, the boot turns it into the night's
//     verdict with one row and a bounded retry inside the window, and no
//     previous night's verdict ever shows under a newer lastRun;
//   - the boot reconcile runs only once storage, clock and sinks are up, never
//     trusts a torn or foreign file or a superseded marker, keeps the working copy
//     and is bounded on a refusing store.

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

  Rig() {
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
    d.apply.speak = [this](const std::string&) { speaks++; };
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
  orch::FireOutcome dreamTurn(const std::string& inputs = "[DREAM] test", const std::string& chat = "1001") {
    return eng->injectScheduledTurn(chat, inputs, "Dream", "dream", /*quietOk=*/true);
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
    orch::FireOutcome o = q.dreamTurn();
    char msg[96];
    snprintf(msg, sizeof msg, "quiet dream turn delivered a notice (%s)", f.name);
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
    // The quiet flag never outlives the turn: the owner's next failure is told.
    script(q, f);
    q.eng->handleMessage("hello again", "Roy", "1001");
    TEST_ASSERT_TRUE(q.delivered.size() >= 1);
  }
}

// ---- (1b) the quiet-turn notice matrix (PG4: unit L, 21 Oct) ------------------------
// With memory writes failing, the bench dream sent its own summary ("Memory was also
// unavailable during tonight") to Telegram: deliver() held the engine's notices, but
// the turn's reply went out through the apply path. The class rule over failure kind
// x channel: whatever the dream's turn ends on - a failure notice, a provider switch,
// a billing refusal, a cut or unreadable reply, a reply, an ask, a degraded summary -
// nothing reaches the owner on any channel while it runs (no message, cue or history
// row: a held salvage notice used to be captured as if the owner had read it); its
// reply and ask come back held for the owner's next conversation. The same kinds on an ordinary routine still reach the owner, and the
// post-update quiet turn keeps its reply (the hold is the dream's alone).

struct QuietKind {
  const char* name; int fail; const char* out; bool memFails; int sw; bool held;
  const char* err;   // a scripted provider error of its own (the cut reply)
};
static const QuietKind kQuietKinds[] = {
    // fail: index into kFails (-1 none); sw: 0 none, 1 billing switch, 2 transport switch
    {"server error", 0, nullptr, false, 0, false},
    {"rate limit", 1, nullptr, false, 0, false},
    {"timeout", 2, nullptr, false, 0, false},
    {"low memory", 3, nullptr, false, 0, false},
    {"key", 4, nullptr, false, 0, false},
    {"credit", 5, nullptr, false, 0, false},
    {"network", 6, nullptr, false, 0, false},
    {"no text", 7, nullptr, false, 0, false},
    {"parse", 8, nullptr, false, 0, false},
    {"budget", 9, nullptr, false, 0, false},
    {"billing switch", -1, "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}", false, 1, false},
    {"transport switch", -1, "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}", false, 2, false},
    {"reply", -1, "{\"reply\":\"Dream check: the bench note was never saved.\",\"memory\":\"\",\"ask\":\"\"}", false, 0, true},
    {"ask", -1, "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"Which Anna did you mean?\"}", false, 0, true},
    {"reply and ask", -1, "{\"reply\":\"Two notes clash.\",\"memory\":\"\",\"ask\":\"Keep the newer one?\"}", false, 0, true},
    {"cut reply", -1, "{\"reply\":\"Two notes cl", false, 0, false, "cut: stop_reason max_tokens"},
    {"degraded summary", -1,
     "{\"reply\":\"Memory was unavailable tonight; two notes were not saved.\",\"memory\":\"\",\"ask\":\"\","
     "\"mem_write\":[{\"content\":\"owner works from home on Fridays\"},{\"content\":\"Anna's birthday is 3 March\"}]}",
     true, 0, true},
};
static const char* const kChannels[] = {"1001", "web", "serial", ""};

static void scriptQuiet(Rig& r, const QuietKind& k) {
  if (k.fail >= 0) script(r, kFails[k.fail]);
  if (k.out) r.outJson = k.out;
  if (k.err) {
    r.ok = false;
    r.err = k.err;
  }
  r.memFails = k.memFails;
  if (k.sw) {
    r.cfg.midFail = true;   // the engine routes the turn through the fabric
    r.billingSwitch = k.sw == 1;
    r.switchKind = k.sw == 2 ? orch::StepErr::Transport : orch::StepErr::None;
    orch::billingEpisodeEnd("anthropic");   // a fresh billing episode per rig
  }
}

static void test_quiet_dream_contacts_nobody_over_the_matrix() {
  for (const QuietKind& k : kQuietKinds) {
    for (const char* chat : kChannels) {
      char msg[96];
      snprintf(msg, sizeof msg, "%s on '%s'", k.name, chat);
      Rig q;
      scriptQuiet(q, k);
      const orch::FireOutcome o = q.dreamTurn("[DREAM] test", chat);
      TEST_ASSERT_EQUAL_MESSAGE(0, q.ownerContacts(), msg);
      TEST_ASSERT_EQUAL_MESSAGE(k.held, !o.held.empty(), msg);
      if (k.held && k.out) {
        JsonDocument want;
        deserializeJson(want, k.out);
        const std::string reply = want["reply"] | "", ask = want["ask"] | "";
        if (!reply.empty()) TEST_ASSERT_TRUE_MESSAGE(o.held.find(reply) != std::string::npos, msg);
        if (!ask.empty()) TEST_ASSERT_TRUE_MESSAGE(o.held.find(ask) != std::string::npos, msg);
      }
      // Counter-test: the same kind on an ordinary routine reaches the owner.
      Rig n;
      scriptQuiet(n, k);
      n.routineTurn(chat);
      TEST_ASSERT_TRUE_MESSAGE(n.delivered.size() >= 1, msg);
      // The hold never outlives the dream: the next owner turn's reply is sent.
      q.ok = true;
      q.cfg.overBudgetHosts.clear();
      q.outJson = "{\"reply\":\"morning\",\"memory\":\"\",\"ask\":\"\"}";
      q.eng->handleMessage("hi", "Roy", "1001");
      TEST_ASSERT_TRUE_MESSAGE(q.delivered.size() >= 1, msg);
    }
  }
  // The post-update quiet turn holds its engine notices but still sends its reply.
  Rig sys;
  sys.outJson = "{\"reply\":\"Updated to the new version.\",\"memory\":\"\",\"ask\":\"\"}";
  const orch::FireOutcome so = sys.eng->injectScheduledTurn("1001", "post-update", "Update", "system", true);
  TEST_ASSERT_TRUE(so.held.empty());
  TEST_ASSERT_EQUAL(1, (int)sys.delivered.size());
}

static void test_held_note_reaches_the_owner_once_in_plain_words() {
  TEST_ASSERT_EQUAL_STRING("", dream::ownerNoteText("", "").c_str());
  TEST_ASSERT_EQUAL_STRING("streak", dream::ownerNoteText("streak", "").c_str());
  TEST_ASSERT_EQUAL_STRING("From the nightly memory upkeep:\nTwo notes clash.",
                           dream::ownerNoteText("", "Two notes clash.").c_str());
  TEST_ASSERT_EQUAL_STRING("streak\n\nFrom the nightly memory upkeep:\nTwo notes clash.",
                           dream::ownerNoteText("streak", "Two notes clash.").c_str());
  // A long note is clipped UTF-8 safe.
  std::string big;
  while (big.size() < 3 * dream::kHeldNoteMax) big += "caf\xC3\xA9 ";
  const std::string note = dream::ownerNoteText("", big);
  TEST_ASSERT_TRUE(note.size() <= dream::kHeldNoteMax + 40);
  TEST_ASSERT_TRUE(validUtf8(note));
  TEST_ASSERT_TRUE(note.size() >= 3 && note.compare(note.size() - 3, 3, "...") == 0);
}

// A quiet turn does not consume the one-notice billing latch (lane M x lane K).
// The fabric's billing switch tells the owner once per episode; billingNoticeDue
// consumes the latch when it says yes. A quiet background turn holds its notices,
// so it must not consume the latch either - otherwise a night that fails on an
// out-of-credit provider eats the episode's one notice and the owner never hears
// of it. The next attended switch then tells the owner, and only once.
static void test_quiet_billing_switch_keeps_the_notice_latch_armed() {
  Rig r;
  r.cfg.midFail = true;       // the engine routes the turn through the fabric
  r.billingSwitch = true;
  orch::billingEpisodeEnd("anthropic");   // arm a fresh episode
  const orch::FireOutcome quiet = r.dreamTurn();
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

// ---- (2) the dream's surface: memory tools only ---------------------------------

static void test_dream_can_never_call_files_or_session_tools() {
  // Property over every registry tool, in both spellings a model may use (the
  // advertised wire name and the dotted registry name).
  Rig r;
  for (const char* n : kDeviceTools) {
    r.callNames.push_back(agent::loopToolName(n));
    r.callNames.push_back(n);
  }
  r.dreamTurn();
  TEST_ASSERT_FALSE(r.advertised.empty());
  for (const auto& a : r.advertised) {
    bool ok = false;
    for (const char* n : kDeviceTools)
      if (a == agent::loopToolName(n)) ok = dream::toolAllowed(n);
    TEST_ASSERT_TRUE_MESSAGE(ok, a.c_str());
  }
  for (const auto& c : r.mcpCalls) {
    TEST_ASSERT_TRUE_MESSAGE(dream::toolAllowed(c), c.c_str());
    TEST_ASSERT_TRUE(c.rfind("files.", 0) != 0 && c.rfind("session.", 0) != 0);
  }
  // Every allowed memory tool still works.
  std::set<std::string> reached(r.mcpCalls.begin(), r.mcpCalls.end());
  TEST_ASSERT_TRUE(reached.count("memory.write") == 1);
  TEST_ASSERT_TRUE(reached.count("memory.update") == 1);
  // The class rule itself: no files.*, session.*, web.*, loop.*, reply.* or device.*
  // tool is ever allowed.
  for (const char* n : kDeviceTools) {
    const std::string s = n;
    const bool memory = s.rfind("memory.", 0) == 0 && s != "memory.config" && s != "memory.archive";
    TEST_ASSERT_EQUAL_MESSAGE(memory, dream::toolAllowed(s), n);
  }
  // Counter-test: an ordinary routine keeps its full surface.
  Rig n;
  n.callNames = {"files_read", "session_terminate"};
  n.routineTurn();
  TEST_ASSERT_TRUE(n.advertised.size() > 20);
  TEST_ASSERT_EQUAL(2, (int)n.mcpCalls.size());
}

static void test_dream_turn_fields_cannot_spawn_or_act() {
  // Property: whatever device / spawn / session fields the dream's orch_turn carries,
  // none runs; its memory fields still do.
  g_seed = 4242;
  for (int i = 0; i < 50; ++i) {
    Rig r;
    std::string turn = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\",\"device\":[";
    const int nd = (int)(rnd() % 3);
    for (int k = 0; k < nd; ++k) turn += std::string(k ? "," : "") + "{\"led\":\"red\"}";
    turn += "],\"spawn\":[";
    const int ns = (int)(rnd() % 3);
    for (int k = 0; k < ns; ++k) turn += std::string(k ? "," : "") + "{\"task\":\"poll the jobs\"}";
    turn += "],\"session_ops\":[{\"op\":\"spawn\",\"task\":\"check job0016\"},{\"op\":\"terminate\",\"id\":\"job1\"}],"
            "\"mem_write\":[{\"content\":\"owner likes flat white\"}]}";
    r.outJson = turn;
    orch::FireOutcome o = r.dreamTurn();
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_EQUAL(0, r.spawns);
    TEST_ASSERT_EQUAL(0, r.deviceActions);
    TEST_ASSERT_EQUAL(0, (int)r.delivered.size());   // not even "No running session"
  }
  // Counter-test: the same turn on an ordinary routine spawns.
  Rig n;
  n.outJson = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\",\"spawn\":[{\"task\":\"research x\"}]}";
  n.routineTurn();
  TEST_ASSERT_EQUAL(1, n.spawns);
  // restrictTurn on its own.
  orch::Turn t;
  t.device.resize(2);
  t.spawn.resize(1);
  t.await_.push_back("x");
  t.session_ops.resize(3);
  t.mem_write.resize(1);
  t.mem_query.push_back("q");
  t.reply = "r";
  TEST_ASSERT_EQUAL(7, dream::restrictTurn(t));
  TEST_ASSERT_EQUAL(1, (int)t.mem_write.size());
  TEST_ASSERT_EQUAL(1, (int)t.mem_query.size());
  TEST_ASSERT_EQUAL_STRING("r", t.reply.c_str());
}

static void test_quiet_preamble_never_asks_for_a_reply() {
  Rig r;
  r.dreamTurn();
  TEST_ASSERT_EQUAL(1, (int)r.inputsSeen.size());
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("reply ONLY if the owner must see something") != std::string::npos);
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
  orch::FireOutcome o = r.dreamTurn();
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

// ---- (5) the night pipeline with fake seams --------------------------------------

struct Night {
  dream::DreamState saved;
  bool haveSaved = false;
  int saves = 0, nvsWrites = 0;
  bool saveRefused = false;
  bool mounted = true;             // the store's mount state (storeReady)
  int failAfter = -1;              // >= 0: the store takes this many writes, then refuses
  bool useMemo = true;             // the device keeps a working copy for the boot
  dream::DreamMemo memo;
  bool crash = false;              // simulate a restart mid-turn: freeze the file as
  dream::DreamState crashedFile;   // it stood when the reflection turn began
  std::string legacyHex;           // the NVS key (read only)
  uint64_t scratch = 0xABCDEF;
  bool quietDay = false;
  bool embedUp = true;
  int maintRuns = 0, turns = 0;
  orch::MemOpCounters ops;
  std::function<orch::FireOutcome()> turn = [] {
    orch::FireOutcome o;
    o.ok = true;
    return o;
  };
  std::string lastInputs;

  dream::NightDeps deps() {
    dream::NightDeps d;
    d.loadState = [this](dream::DreamState& s) {
      if (haveSaved) { s = saved; return true; }
      s = dream::stateFromLegacyHash(legacyHex);   // the device's one-time migration
      return true;
    };
    d.saveState = [this](const dream::DreamState& s) {
      saves++;
      if (saveRefused || (failAfter >= 0 && saves > failAfter)) return false;
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
      m.persisted = true;
      return m;
    };
    d.yesterday = [this] {
      dream::Yesterday y;
      y.empty = quietDay;
      y.digest = quietDay ? "(none)\n" : "- user: I switched to flat white\n";
      return y;
    };
    d.scratchBlock = [] { return std::string(); };
    d.scratchHash = [this] { return scratch; };
    d.stats = [] { return dream::MemStats{}; };
    d.embeddingsUp = [this] { return embedUp; };
    d.memOps = [this] { return ops; };
    d.injectTurn = [this](const std::string& in) {
      turns++;
      lastInputs = in;
      if (crash) crashedFile = saved;   // the pre-turn save (marker) just landed
      return turn();
    };
    return d;
  }
};

static dream::NightClock clockAt(uint32_t day, uint16_t minute) {
  dream::NightClock c;
  c.localDay = day;
  c.minuteOfDay = minute;
  c.epoch = (uint64_t)day * 86400u + minute * 60u;
  c.synced = true;
  return c;
}

static orch::FireOutcome failWith(const char* err) {
  orch::FireOutcome o;
  o.ok = false;
  o.error = err;
  return o;
}

// The class rules one generated night must satisfy (test_night_outcome_property).
static void checkRow(const dream::NightReport& r, const std::string& row) {
  TEST_ASSERT_TRUE(row.rfind("Nightly dream ", 0) == 0);
  TEST_ASSERT_TRUE(row.size() <= 240);
  TEST_ASSERT_TRUE(validUtf8(row));
  TEST_ASSERT_TRUE(row.find('\n') == std::string::npos);
  TEST_ASSERT_EQUAL(r.outcome == dream::DreamOutcome::Ran, row.rfind("Nightly dream ok", 0) == 0);
}

static void checkRetry(const dream::NightResult& res, int kind, uint16_t minute, uint8_t spent) {
  const dream::NightReport& r = res.report;
  const bool open = spent + 1 < dream::kMaxAttempts &&
                    minute + dream::kDeferSec / 60 < dream::kWindowEndMinute;
  if (r.retryAfterSec) {
    TEST_ASSERT_TRUE(kind == 2 || (kind == 1 && dream::causeRetryable(r.cause)));
    TEST_ASSERT_TRUE(open);
    TEST_ASSERT_EQUAL(r.retryAfterSec, res.fire.retryAfterSec);
  }
  if (kind == 1 && dream::causeRetryable(r.cause) && open) TEST_ASSERT_TRUE(r.retryAfterSec > 0);
}

struct NightCase {
  int kind;          // 0 ok, 1 fail, 2 defer, 3 quiet
  uint32_t failed;   // writes the turn saw fail
  bool embedUp;
  int turns;         // reflection turns the night ran
  uint16_t minute;
  uint8_t spent;     // attempts already spent tonight
};

static void checkNightRules(const dream::NightResult& res, const NightCase& k) {
  const dream::NightReport& r = res.report;
  const int kind = k.kind;
  const uint32_t failed = k.failed;
  const std::string row = dream::renderDreamRow(r);
  checkRow(r, row);
  TEST_ASSERT_FALSE(res.fire.detail.empty());   // per-night fingerprint, never paused as a repeat
  if (kind == 3) {
    TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Skipped, (int)r.outcome);
    TEST_ASSERT_EQUAL(0, k.turns);
    return;
  }
  if (r.outcome == dream::DreamOutcome::Ran) TEST_ASSERT_TRUE(kind == 0 && failed == 0 && k.embedUp);
  if (kind == 0 && (failed > 0 || !k.embedUp))
    TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Degraded, (int)r.outcome);
  if (failed > 0 && kind != 2) {
    char want[24];
    snprintf(want, sizeof want, "%u failed", (unsigned)failed);
    TEST_ASSERT_TRUE_MESSAGE(row.find(want) != std::string::npos, row.c_str());
  }
  if (kind == 2) {
    TEST_ASSERT_TRUE(row.find("FAILED") == std::string::npos);
    TEST_ASSERT_TRUE(r.outcome == dream::DreamOutcome::Deferred ||
                     r.outcome == dream::DreamOutcome::Starved);
  }
  if (kind == 1) TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Failed, (int)r.outcome);
  checkRetry(res, kind, k.minute, k.spent);
}

static void test_night_outcome_property() {
  // Generated nights: the turn's result x the writes it made x memory availability x
  // the clock x attempts already spent. The class rules:
  //   ok (Ran) only when the turn ran, every write landed and memory was up;
  //   a deferred night never renders FAILED; a skipped night never renders ok;
  //   a night with a failed write never renders ok and names the failed count;
  //   retries only for transient causes, within attempts and the window;
  //   every row starts "Nightly dream ", fits its cap and is valid UTF-8.
  const char* errs[] = {"chat HTTP 500: overloaded", "chat HTTP 429: rate limited",
                        "timeout: 120 s", "low memory: alloc", "network: no response",
                        "chat HTTP 401: bad key", "budget: over", "parse: not json",
                        "chat HTTP 400: credit balance is too low", "chat HTTP 400: bad body",
                        "weird \xE2\x80\xA8 line\nbreak \x01 error text that runs on and on and on "
                        "and on and on and on and on and on and on and on and on and on and on"};
  g_seed = 20261019u;
  for (int i = 0; i < 600; ++i) {
    Night n;
    const int kind = (int)(rnd() % 4);   // 0 ok, 1 fail, 2 defer, 3 quiet
    const char* err = errs[rnd() % (sizeof errs / sizeof errs[0])];
    const uint32_t added = rnd() % 3, failed = rnd() % 3 == 0 ? 1 + rnd() % 3 : 0;
    n.embedUp = rnd() % 5 != 0;
    n.quietDay = kind == 3;
    const uint16_t minute = (uint16_t)(200 + rnd() % 280);
    const uint8_t spent = (uint8_t)(rnd() % 4);
    n.haveSaved = true;
    n.saved.attemptDay = 9;
    n.saved.attemptsTonight = spent;
    n.saved.maintDay = rnd() % 2 ? 9 : 8;
    if (kind == 3) n.saved.scrHash = n.scratch;   // baseline == scratchpad: a quiet skip
    n.turn = [&n, kind, err, added, failed] {
      n.ops.added += added;
      n.ops.failed += failed;
      orch::FireOutcome o;
      o.ok = kind == 0;
      o.deferred = kind == 2;
      if (kind == 1) o.error = err;
      o.tokens.add(kind == 0 ? 500 : 0, kind == 0 ? 50 : 0);
      return o;
    };
    dream::NightResult res = dream::runNight(n.deps(), clockAt(9, minute), false);
    checkNightRules(res, NightCase{kind, failed, n.embedUp, n.turns, minute, spent});
  }
}

static void test_dream_night_retry_window() {
  // A low-memory defer at 03:30 retries at 03:45 and 04:00; maintenance (which decays
  // importance) ran exactly once that day: 0.5 -> 0.475 after all three attempts.
  orch::VectorMemory v;
  v.configure(4);
  orch::VecEntry e;
  e.id = "fact";
  e.content = "fact";
  e.importance = 0.5f;
  e.ttlHours = 0;
  e.vec = orch::VectorMemory::quantize({1, 0, 0, 0});
  TEST_ASSERT_TRUE(v.add(e));
  Night n;
  dream::NightDeps d = n.deps();
  d.maintain = [&] { return dream::maintainVectors(v, 0.95f, 1000, 1500, [] { return true; }); };
  n.turn = [] {
    orch::FireOutcome o;
    o.deferred = true;
    return o;
  };
  dream::NightResult a = dream::runNight(d, clockAt(30, 210), false);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Deferred, (int)a.report.outcome);
  TEST_ASSERT_EQUAL(900, (int)a.fire.retryAfterSec);
  TEST_ASSERT_TRUE(dream::renderDreamRow(a.report).find("retry 2 of 3 at 03:45") != std::string::npos);
  dream::NightResult b = dream::runNight(d, clockAt(30, 225), false);
  TEST_ASSERT_EQUAL(900, (int)b.fire.retryAfterSec);
  TEST_ASSERT_FALSE(b.report.maint.ran);
  TEST_ASSERT_TRUE(b.report.maintDoneToday);
  n.turn = [] { orch::FireOutcome o; o.ok = true; return o; };
  dream::NightResult c = dream::runNight(d, clockAt(30, 240), false);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Ran, (int)c.report.outcome);
  TEST_ASSERT_EQUAL(3, (int)c.report.attempt);
  TEST_ASSERT_EQUAL_FLOAT(0.475f, v.getAll()[0].importance);
  // The next day maintenance runs again, once.
  dream::runNight(d, clockAt(31, 210), false);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.45125f, v.getAll()[0].importance);

  // Counter-tests. Attempts are capped at 3: the third defer of a night starves it.
  Night m;
  m.turn = [] { orch::FireOutcome o; o.deferred = true; return o; };
  dream::NightDeps md = m.deps();
  for (uint16_t t : {210, 225}) TEST_ASSERT_EQUAL(900, (int)dream::runNight(md, clockAt(5, t), false).fire.retryAfterSec);
  dream::NightResult last = dream::runNight(md, clockAt(5, 240), false);
  TEST_ASSERT_EQUAL(0, (int)last.fire.retryAfterSec);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Starved, (int)last.report.outcome);
  // A defer after the window (07:05) does not retry.
  Night late;
  late.turn = [] { orch::FireOutcome o; o.deferred = true; return o; };
  TEST_ASSERT_EQUAL(0, (int)dream::runNight(late.deps(), clockAt(5, 425), false).fire.retryAfterSec);
  // Budget, key, billing and parse failures never retry tonight; transient ones do.
  for (const char* err : {"budget: over", "chat HTTP 401: key", "parse: bad", "chat HTTP 400: credit balance"}) {
    Night f;
    f.turn = [err] { return failWith(err); };
    dream::NightResult r = dream::runNight(f.deps(), clockAt(5, 210), false);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.fire.retryAfterSec, err);
    TEST_ASSERT_TRUE(dream::renderDreamRow(r.report).find("no retry tonight") != std::string::npos);
  }
  for (const char* err : {"chat HTTP 503: down", "chat HTTP 429: busy", "timeout: x", "network: y"}) {
    Night f;
    f.turn = [err] { return failWith(err); };
    TEST_ASSERT_EQUAL_MESSAGE(900, (int)dream::runNight(f.deps(), clockAt(5, 210), false).fire.retryAfterSec, err);
  }
}

static void test_failed_write_night_row_says_failed_with_counts() {
  // The bench drill's host twin: the reflection ran, 1 write landed, 2 failed.
  Night n;
  n.turn = [&n] {
    n.ops.added += 1;
    n.ops.failed += 2;
    orch::FireOutcome o;
    o.ok = true;
    o.tokens.add(900, 100);
    return o;
  };
  dream::NightResult r = dream::runNight(n.deps(), clockAt(7, 210), false);
  const std::string row = dream::renderDreamRow(r.report);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Degraded, (int)r.report.outcome);
  TEST_ASSERT_TRUE_MESSAGE(row.find("Nightly dream degraded (memory writes failed): 1 of 3 memories "
                                    "stored (1 new, 0 updated, 0 queued, 2 failed)") == 0, row.c_str());
  TEST_ASSERT_TRUE(row.find("spent 1000 tokens") != std::string::npos);
  // And a failed turn with a failed write: FAILED, the cause and the counts.
  Night f;
  f.turn = [&f] {
    f.ops.failed += 1;
    return failWith("chat HTTP 400: credit balance is too low");
  };
  const std::string frow = dream::renderDreamRow(dream::runNight(f.deps(), clockAt(7, 210), false).report);
  TEST_ASSERT_TRUE_MESSAGE(frow.find("Nightly dream FAILED (billing: chat HTTP 400: credit balance is too low), "
                                     "no retry tonight; 0 of 1 memories stored (0 new, 0 updated, 0 queued, 1 failed)") == 0,
                           frow.c_str());
}

// Lane P meets lane M: a write queued during the night (embedding outage) is
// never counted as stored. The row names it queued, so a billing outage during
// the night can never read as "memories stored".
static void test_queued_write_night_row_names_it_queued() {
  Night n;
  n.turn = [&n] {
    n.ops.queued += 1;
    orch::FireOutcome o;
    o.ok = true;
    o.tokens.add(900, 100);
    return o;
  };
  dream::NightResult r = dream::runNight(n.deps(), clockAt(7, 210), false);
  const std::string row = dream::renderDreamRow(r.report);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Ran, (int)r.report.outcome);
  TEST_ASSERT_TRUE_MESSAGE(row.find("ok: 0 of 1 memories stored (0 new, 0 updated, 1 queued, 0 failed)") !=
                               std::string::npos,
                           row.c_str());
}

static void test_memory_unavailable_night_says_so_and_is_degraded() {
  Night n;
  n.embedUp = false;
  dream::NightResult r = dream::runNight(n.deps(), clockAt(3, 210), false);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Degraded, (int)r.report.outcome);
  TEST_ASSERT_TRUE(n.lastInputs.find("long-term memory writes are unavailable tonight") != std::string::npos);
  TEST_ASSERT_TRUE(dream::renderDreamRow(r.report).find("degraded (memory unavailable)") == 14);
  Night ok;
  dream::runNight(ok.deps(), clockAt(3, 210), false);
  TEST_ASSERT_TRUE(ok.lastInputs.find("unavailable tonight") == std::string::npos);
}

static void test_streak_alerts_and_breaker_exemption() {
  // Ten failed nights: the dream loop stays enabled, its breaker counter stays below
  // the cap, and exactly two owner alerts are due (streak 3 and streak 10).
  Night n;
  n.turn = [] { return failWith("chat HTTP 401: key rejected"); };
  orch::LoopRecord loop = dream::reservedLoopRecord();
  orch::LoopRecord normal = loop;
  normal.id = "L1";
  orch::LoopCaps caps;
  orch::DeviceCounters dev;
  int alerts = 0, enableFlips = 0;
  bool wasEnabled = loop.enabled;
  for (uint32_t night = 1; night <= 10; ++night) {
    dream::NightResult r = dream::runNight(n.deps(), clockAt(100 + night, 210), false);
    TEST_ASSERT_EQUAL(night, r.report.streak);
    if (r.report.alert) {
      alerts++;
      TEST_ASSERT_TRUE(dream::streakAlertText(r.report).find("has not worked for") != std::string::npos);
    }
    orch::onFireResult(loop, r.fire.ok, r.fire.tokens, orch::fnv64(r.fire.detail), night * 86400);
    if (orch::breakerTrips(loop, caps.maxConsecFails, /*reserved=*/true)) loop.enabled = false;
    if (loop.enabled != wasEnabled) enableFlips++;
    wasEnabled = loop.enabled;
    TEST_ASSERT_TRUE(loop.enabled);
    TEST_ASSERT_TRUE(loop.consecFails < caps.maxConsecFails);
    loop.nextRun = 1;
    TEST_ASSERT_TRUE(orch::evaluate(loop, dev, caps, 2, true, true) != orch::FireDecision::BlockedConsecFails);
    // Counter-test: a normal routine is disabled at 3, as before.
    orch::onFireResult(normal, false, {}, night, night * 86400);
    if (night < 3) TEST_ASSERT_FALSE(orch::breakerTrips(normal, caps.maxConsecFails, false));
    else TEST_ASSERT_TRUE(orch::breakerTrips(normal, caps.maxConsecFails, false));
  }
  TEST_ASSERT_EQUAL(2, alerts);
  TEST_ASSERT_EQUAL(0, enableFlips);   // no enable/disable thrash
  // A good night resets the streak.
  n.turn = [] { orch::FireOutcome o; o.ok = true; return o; };
  TEST_ASSERT_EQUAL(0, dream::runNight(n.deps(), clockAt(111, 210), false).report.streak);
  // A dream the breaker already paused (old firmware) is resumed; an owner pause is not.
  orch::LoopRecord paused = dream::reservedLoopRecord();
  paused.enabled = false;
  paused.pauseReason = orch::PauseReason::ConsecFails;
  paused.consecFails = 3;
  TEST_ASSERT_TRUE(orch::reviveReservedLoop(paused, caps.maxConsecFails));
  TEST_ASSERT_TRUE(paused.enabled);
  TEST_ASSERT_TRUE(paused.consecFails < caps.maxConsecFails);
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
  starved.attempt = 12;
  TEST_ASSERT_EQUAL_STRING("Nightly dream skipped tonight (device busy after 12 tries); "
                           "maintenance not run tonight",
                           dream::renderDreamRow(starved).c_str());
  starved.cause = "low-heap";
  TEST_ASSERT_TRUE(dream::renderDreamRow(starved).find("(low memory after") != std::string::npos);
  // The window and the next slot.
  TEST_ASSERT_EQUAL(420, dream::windowEndFor(210));
  TEST_ASSERT_EQUAL(1439, dream::windowEndFor(1400));
  dream::NightClock c = clockAt(1, 425);
  TEST_ASSERT_EQUAL((1440 - 425 + 210) * 60, (int)dream::secondsToNextSlot(c, 210));
  TEST_ASSERT_EQUAL(1440 * 60, (int)dream::secondsToNextSlot(clockAt(1, 210), 210));
}

// ---- (6) dream state: a file, never NVS --------------------------------------------

static void test_dream_state_store_fresh_legacy_and_full_nvs() {
  // (a) Fresh device: no state file, no NVS key. The first night runs the reflection
  // and writes the file; the second, quiet night is skipped on the file's baseline.
  Night fresh;
  dream::NightResult first = dream::runNight(fresh.deps(), clockAt(50, 210), false);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Ran, (int)first.report.outcome);
  TEST_ASSERT_TRUE(fresh.haveSaved);
  TEST_ASSERT_EQUAL(0xABCDEF, (int)fresh.saved.scrHash);
  fresh.quietDay = true;
  dream::NightResult second = dream::runNight(fresh.deps(), clockAt(51, 210), false);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Skipped, (int)second.report.outcome);
  TEST_ASSERT_EQUAL(1, fresh.turns);
  TEST_ASSERT_TRUE(dream::renderDreamRow(second.report).find("Nightly dream skipped (quiet day)") == 0);
  // (b) Legacy NVS key, no file: read once into the first state; the night is quiet
  // and skipped on the migrated baseline; five nights later the dream path made zero
  // NVS writes (it has no NVS writer at all - the deps carry none).
  Night legacy;
  legacy.legacyHex = "0000000000abcdef";
  legacy.quietDay = true;
  for (uint32_t d = 0; d < 5; ++d)
    TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Skipped,
                      (int)dream::runNight(legacy.deps(), clockAt(60 + d, 210), false).report.outcome);
  TEST_ASSERT_EQUAL(0, legacy.turns);
  TEST_ASSERT_EQUAL(0, legacy.nvsWrites);
  TEST_ASSERT_TRUE(legacy.haveSaved);
  // (c) Storage refuses every write (the full-NVS Lumi state reached LittleFS too):
  // the night still runs, the row says the state was not saved, nothing crashes.
  Night full;
  full.saveRefused = true;
  dream::NightResult r = dream::runNight(full.deps(), clockAt(70, 210), false);
  TEST_ASSERT_FALSE(r.report.stateSaved);
  TEST_ASSERT_TRUE(dream::renderDreamRow(r.report).find("state not saved") != std::string::npos);
  // State file round trip, and an unreadable file is refused (caller starts fresh).
  dream::DreamState s;
  s.maintDay = 20500; s.scrHash = 0xFEDCBA9876543210ULL; s.attemptDay = 20500;
  s.attemptsTonight = 2; s.streakDay = 20499; s.streak = 4;
  s.lastOutcome = dream::DreamOutcome::Failed; s.lastCause = "provider-5xx";
  dream::DreamState back;
  TEST_ASSERT_TRUE(dream::loadDreamState(dream::dumpDreamState(s), back));
  TEST_ASSERT_TRUE(back.scrHash == s.scrHash);
  TEST_ASSERT_EQUAL(s.maintDay, back.maintDay);
  TEST_ASSERT_EQUAL(2, back.attemptsTonight);
  TEST_ASSERT_EQUAL(4, back.streak);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Failed, (int)back.lastOutcome);
  TEST_ASSERT_EQUAL_STRING("provider-5xx", back.lastCause.c_str());
  TEST_ASSERT_TRUE(dream::dumpDreamState(s).size() < 200);
  TEST_ASSERT_FALSE(dream::loadDreamState("{not json", back));
  TEST_ASSERT_FALSE(dream::loadDreamState("[1,2]", back));
  TEST_ASSERT_TRUE(dream::stateFromLegacyHash("").scrHash == 0);
}

// ---- (6b) a store that is not mounted, or fails after N writes -------------------

// One night as the scheduler drives it: fire at the slot, then again at every retry
// the night asks for, until it asks for none. Returns the fires it took.
static int driveNight(Night& n, uint32_t day, dream::NightResult* last = nullptr) {
  uint16_t minute = dream::kDefaultMinuteOfDay;
  int fires = 0;
  dream::NightResult r;
  do {
    r = dream::runNight(n.deps(), clockAt(day, minute), false);
    fires++;
    minute = (uint16_t)(minute + r.report.retryAfterSec / 60u);
  } while (r.report.retryAfterSec && fires < 40);
  if (last) *last = r;
  return fires;
}

static void test_unmounted_store_is_never_written_and_the_night_stays_bounded() {
  // Internal storage is not mounted all boot (the unit L bench suspicion): no write is
  // ever tried (fail fast, no file-system error per fire), the row says why, and the
  // boot's working copy still bounds the night.
  Night n;
  n.mounted = false;
  n.turn = [] { return failWith("chat HTTP 503: overloaded"); };   // transient: retries
  dream::NightResult last;
  TEST_ASSERT_EQUAL(dream::kMaxAttempts, driveNight(n, 80, &last));
  TEST_ASSERT_EQUAL(dream::kMaxAttempts, n.turns);
  TEST_ASSERT_EQUAL(1, n.maintRuns);   // decay once that day, not once per retry
  TEST_ASSERT_EQUAL(0, n.saves);
  TEST_ASSERT_FALSE(last.report.stateSaved);
  TEST_ASSERT_EQUAL_STRING("storage not mounted", last.report.stateWhy.c_str());
  const std::string row = dream::renderDreamRow(last.report);
  TEST_ASSERT_TRUE_MESSAGE(row.find("; state not saved (storage not mounted)") != std::string::npos,
                           row.c_str());
  // Three failed nights in a row still reach the owner alert, exactly once.
  n.turn = [] { return failWith("messages HTTP 401: invalid x-api-key"); };
  int alerts = 0;
  for (uint32_t day = 81; day <= 82; ++day) {
    driveNight(n, day, &last);
    alerts += last.report.alert ? 1 : 0;
  }
  TEST_ASSERT_EQUAL(3, last.report.streak);
  TEST_ASSERT_EQUAL(1, alerts);
  TEST_ASSERT_EQUAL(0, n.saves);
  // Counter-test: without the working copy every fire re-reads a file that never
  // landed, so the attempt count never moves and the night runs to the window end.
  Night bare;
  bare.mounted = false;
  bare.useMemo = false;
  bare.turn = [] { return failWith("chat HTTP 503: overloaded"); };
  TEST_ASSERT_TRUE(driveNight(bare, 80) > dream::kMaxAttempts);
  TEST_ASSERT_TRUE(bare.maintRuns > 1);
}

static void test_store_failing_after_n_writes_keeps_counts_and_says_so() {
  // The store takes two writes (night 1: the attempt mark and the result), then
  // refuses every write (worn or full flash).
  Night n;
  n.failAfter = 2;
  dream::NightResult last;
  TEST_ASSERT_EQUAL(1, driveNight(n, 90, &last));
  TEST_ASSERT_TRUE(last.report.stateSaved);
  TEST_ASSERT_TRUE(last.report.stateWhy.empty());
  TEST_ASSERT_TRUE(dream::renderDreamRow(last.report).find("state not saved") == std::string::npos);
  n.turn = [] { return failWith("timed out after 90000 ms"); };   // transient: retries
  TEST_ASSERT_EQUAL(dream::kMaxAttempts, driveNight(n, 91, &last));
  TEST_ASSERT_EQUAL(2, n.maintRuns);   // one per day
  TEST_ASSERT_FALSE(last.report.stateSaved);
  TEST_ASSERT_EQUAL_STRING("storage write failed", last.report.stateWhy.c_str());
  TEST_ASSERT_TRUE(dream::renderDreamRow(last.report).find("; state not saved (storage write failed)") !=
                   std::string::npos);
  // The file still holds night 1; this boot's copy holds tonight.
  TEST_ASSERT_EQUAL(90, (int)n.saved.attemptDay);
  TEST_ASSERT_EQUAL(91, (int)n.memo.st.attemptDay);
  TEST_ASSERT_EQUAL(dream::kMaxAttempts, n.memo.st.attemptsTonight);
  // A store that recovers lands the next save, and the row stops saying "not saved".
  n.failAfter = -1;
  n.turn = [] {
    orch::FireOutcome o;
    o.ok = true;
    return o;
  };
  TEST_ASSERT_EQUAL(1, driveNight(n, 92, &last));
  TEST_ASSERT_TRUE(last.report.stateSaved);
  TEST_ASSERT_EQUAL(92, (int)n.saved.attemptDay);
}

// ---- (6c) a night the restart cut short (L19-31) ---------------------------------

static void test_interrupted_night_reconciles_at_boot() {
  // The 19 Oct class: an unexplained reset mid-reflection left the previous night's
  // verdict under the new lastRun, no row and no retry. The marker is written before
  // the turn; the boot reconciles it into the night's verdict.
  Night n;
  n.crash = true;
  n.turn = [] { return failWith("chat HTTP 500: overloaded"); };   // never lands
  dream::runNight(n.deps(), clockAt(9, 210), false);
  TEST_ASSERT_EQUAL(9, (int)n.crashedFile.inflightNight);
  TEST_ASSERT_EQUAL(1, n.crashedFile.inflightAttempt);
  TEST_ASSERT_TRUE(n.crashedFile.inflightStart == clockAt(9, 210).epoch);
  TEST_ASSERT_TRUE(dream::dumpDreamState(n.crashedFile).find("\"inNight\":9") != std::string::npos);
  // Reload at 03:50, still inside the window: the row, the counters, a retry.
  dream::DreamState st = n.crashedFile;
  dream::InterruptedNight v = dream::reconcileBoot(st, clockAt(9, 230));
  TEST_ASSERT_TRUE(v.interrupted);
  const dream::NightReport& r = v.report;
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Failed, (int)r.outcome);
  TEST_ASSERT_EQUAL_STRING("restarted", r.cause.c_str());
  TEST_ASSERT_EQUAL_STRING("the device restarted during this run", r.error.c_str());
  TEST_ASSERT_EQUAL(1, r.attempt);
  TEST_ASSERT_EQUAL(900, (int)r.retryAfterSec);
  TEST_ASSERT_EQUAL(245, r.retryAtMinute);   // 04:05, inside the 07:00 window
  TEST_ASSERT_EQUAL(1, r.streak);
  TEST_ASSERT_FALSE(r.alert);
  TEST_ASSERT_EQUAL_STRING("Nightly dream interrupted (the device restarted during the "
                           "reflection), retry 2 of 3 at 04:05",
                           dream::renderDreamRow(r).c_str());
  // The state: marker cleared (this save IS the outcome write), the interrupted
  // attempt kept as one of the bounded retries, the verdict replaces the old one.
  TEST_ASSERT_EQUAL(0, (int)st.inflightNight);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Failed, (int)st.lastOutcome);
  TEST_ASSERT_EQUAL_STRING("restarted", st.lastCause.c_str());
  TEST_ASSERT_EQUAL(9, (int)st.attemptDay);
  TEST_ASSERT_EQUAL(1, st.attemptsTonight);
  // Idempotence: a second boot over the reconciled state moves nothing.
  dream::DreamState again = st;
  TEST_ASSERT_FALSE(dream::reconcileBoot(again, clockAt(9, 235)).interrupted);
  // The loop side (the scheduler's post-fire write never ran) lands once.
  orch::LoopRecord loop = dream::reservedLoopRecord();
  const uint64_t boot = clockAt(9, 230).epoch;
  dream::applyInterruptedToLoop(loop, boot, r.retryAfterSec);
  TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)loop.lastResult);
  TEST_ASSERT_EQUAL(1, loop.consecFails);
  TEST_ASSERT_TRUE(loop.nextRun == boot + 900);
  // The retry the boot scheduled opens at attempt 2 and stays bounded.
  Night after;
  after.haveSaved = true;
  after.saved = st;
  after.memo.loaded = true;
  after.memo.st = st;   // this boot's working copy, as pumpBootReconcile primes it
  after.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::NightResult res = dream::runNight(after.deps(), clockAt(9, 245), false);
  TEST_ASSERT_EQUAL(2, res.report.attempt);
  TEST_ASSERT_EQUAL(900, (int)res.fire.retryAfterSec);
  // That retry can die too: the second reconcile of the SAME night counts the
  // attempt and schedules the last retry, but the streak moves once per night,
  // not once per crash.
  Night twice;
  twice.crash = true;
  twice.haveSaved = true;
  twice.saved = st;
  twice.memo.loaded = true;
  twice.memo.st = st;
  twice.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::runNight(twice.deps(), clockAt(9, 245), false);
  TEST_ASSERT_EQUAL(2, twice.crashedFile.inflightAttempt);
  dream::DreamState st2 = twice.crashedFile;
  dream::InterruptedNight v3 = dream::reconcileBoot(st2, clockAt(9, 250));
  TEST_ASSERT_TRUE(v3.interrupted);
  TEST_ASSERT_EQUAL(900, (int)v3.report.retryAfterSec);   // retry 3 of 3
  TEST_ASSERT_EQUAL(1, v3.report.streak);                  // once for the night
  dream::applyInterruptedToLoop(loop, clockAt(9, 250).epoch, v3.report.retryAfterSec);
  TEST_ASSERT_EQUAL(2, loop.consecFails);   // one per failed fire, as the scheduler does
}

static void test_interrupted_night_after_the_window_counts_once_and_never_retries() {
  // A crash at 06:20, boot at 07:05: the verdict and the counters still land,
  // but the window is closed, so the loop keeps its next slot.
  Night late;
  late.crash = true;
  late.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::runNight(late.deps(), clockAt(9, 380), false);
  dream::DreamState st = late.crashedFile;
  dream::InterruptedNight v = dream::reconcileBoot(st, clockAt(9, 425));
  TEST_ASSERT_TRUE(v.interrupted);
  TEST_ASSERT_EQUAL(0, (int)v.report.retryAfterSec);
  const std::string row = dream::renderDreamRow(v.report);
  TEST_ASSERT_TRUE_MESSAGE(row.find("Nightly dream interrupted (the device restarted "
                                    "during the reflection), no retry tonight") == 0,
                           row.c_str());
  orch::LoopRecord loop = dream::reservedLoopRecord();
  const uint64_t boot = clockAt(9, 425).epoch;
  const uint64_t slot = loop.nextRun;
  dream::applyInterruptedToLoop(loop, boot, 0);
  TEST_ASSERT_EQUAL(1, loop.consecFails);
  TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)loop.lastResult);
  TEST_ASSERT_TRUE(loop.nextRun == slot);   // no retry: the next slot stands
  // A boot the NEXT day (the device stayed off): no retry either, the marker's
  // night is over, and the streak still moves exactly once for it.
  Night stale;
  stale.crash = true;
  stale.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::runNight(stale.deps(), clockAt(9, 380), false);
  dream::DreamState gone = stale.crashedFile;
  dream::InterruptedNight nv = dream::reconcileBoot(gone, clockAt(10, 100));
  TEST_ASSERT_TRUE(nv.interrupted);
  TEST_ASSERT_EQUAL(0, (int)nv.report.retryAfterSec);
  TEST_ASSERT_EQUAL(1, nv.report.streak);
  // A quiet night's outcome write also clears a marker a failing store left
  // behind from an earlier night (the skip path saves state too).
  Night skip;
  skip.crash = true;
  skip.quietDay = true;
  dream::runNight(skip.deps(), clockAt(9, 380), true);   // force: the turn runs
  TEST_ASSERT_TRUE(skip.saved.inflightNight == 0);
}

static void test_interrupted_night_never_shows_an_older_verdict() {
  // Property (L19-31): generated sequences of (fire, optional outcome, reload).
  // Whatever happens, the verdict the state holds after a reload belongs to the
  // NEWEST fire (its lastRun), never to a previous night's; the loop's consecFails
  // moves exactly once per interruption; retries stay bounded; a completed night
  // leaves no marker.
  g_seed = 191031u;
  for (int i = 0; i < 300; ++i) {
    Night n;
    orch::LoopRecord loop = dream::reservedLoopRecord();
    int expectOutcome = -1;   // (int)DreamOutcome the newest fire must have left
    std::string expectCause;
    uint32_t day = 200;
    for (int night = 0; night < 6; ++night, ++day) {
      const uint32_t kind = rnd() % 3;   // 0 ok, 1 failed turn, 2 restart mid-turn
      const uint16_t minute = (uint16_t)(210 + rnd() % 90);
      if (kind == 2) {
        // Fire, no outcome: the device restarts mid-reflection.
        n.crash = true;
        n.turn = [] { return failWith("chat HTTP 503: down"); };
        dream::runNight(n.deps(), clockAt(day, minute), false);
        dream::DreamState st = n.crashedFile;
        // The boot: reload + reconcile. The interrupted verdict REPLACES whatever
        // the previous night left under this newer lastRun.
        dream::InterruptedNight v = dream::reconcileBoot(st, clockAt(day, minute + 5));
        TEST_ASSERT_TRUE(v.interrupted);
        TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Failed, (int)st.lastOutcome);
        TEST_ASSERT_EQUAL_STRING("restarted", st.lastCause.c_str());
        TEST_ASSERT_TRUE(st.inflightNight == 0);
        orch::LoopRecord before = loop;
        dream::applyInterruptedToLoop(loop, clockAt(day, minute + 5).epoch, v.report.retryAfterSec);
        TEST_ASSERT_EQUAL(before.consecFails < 255 ? before.consecFails + 1 : before.consecFails,
                          loop.consecFails);
        TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)loop.lastResult);
        expectOutcome = (int)dream::DreamOutcome::Failed;
        expectCause = "restarted";
        // The boot persists the reconciled state and primes its working copy.
        n.crash = false;
        TEST_ASSERT_TRUE(dream::loadDreamState(dream::dumpDreamState(st), n.saved));
        n.haveSaved = true;
        n.memo.loaded = true;
        n.memo.st = st;
        if (v.report.retryAfterSec && rnd() % 2) {
          // The scheduled retry fires tonight and completes: attempt 2 or 3.
          const bool ok = rnd() % 2 == 0;
          n.turn = [ok] {
            orch::FireOutcome o;
            o.ok = ok;
            if (!ok) o.error = "chat HTTP 401: bad key";
            return o;
          };
          dream::NightResult res =
              dream::runNight(n.deps(), clockAt(day, v.report.retryAtMinute), false);
          TEST_ASSERT_TRUE(res.report.attempt <= dream::kMaxAttempts);
          TEST_ASSERT_TRUE(n.saved.inflightNight == 0);
          orch::onFireResult(loop, res.fire.ok, res.fire.tokens,
                             orch::fnv64(res.fire.detail), clockAt(day, v.report.retryAtMinute).epoch);
          expectOutcome = (int)res.report.outcome;
          expectCause = res.report.cause;
        }
      } else {
        // The night completes (with same-night retries the scheduler drives).
        const bool ok = kind == 0;
        n.turn = [ok] {
          orch::FireOutcome o;
          o.ok = ok;
          if (!ok) o.error = "timeout: no answer";
          return o;
        };
        uint16_t m = minute;
        int attempts = 0;
        dream::NightResult res;
        do {
          res = dream::runNight(n.deps(), clockAt(day, m), false);
          attempts++;
          orch::onFireResult(loop, res.fire.ok, res.fire.tokens,
                             orch::fnv64(res.fire.detail), clockAt(day, m).epoch);
          m = (uint16_t)(m + res.report.retryAfterSec / 60u);
        } while (res.report.retryAfterSec && attempts < 40);
        TEST_ASSERT_TRUE(attempts <= dream::kMaxAttempts);
        TEST_ASSERT_TRUE(n.saved.inflightNight == 0);   // a completed night leaves no marker
        expectOutcome = (int)res.report.outcome;
        expectCause = res.report.cause;
      }
      // A reload after the night (the next boot, before anything fired): with no
      // marker the reconcile must leave the verdict alone, and that verdict is
      // the newest fire's, never an older night's.
      dream::DreamState st = n.saved;
      dream::InterruptedNight v = dream::reconcileBoot(st, clockAt(day + 1, 100));
      TEST_ASSERT_FALSE(v.interrupted);
      TEST_ASSERT_EQUAL(expectOutcome, (int)st.lastOutcome);
      TEST_ASSERT_EQUAL_STRING(expectCause.c_str(), st.lastCause.c_str());
    }
  }
}

static void test_completed_night_leaves_no_marker_and_is_byte_identical() {
  // Counter-test for the marker: a night that completes writes EXACTLY the
  // state today's format has (no marker keys ride a finished night), so the
  // file a finished night leaves is byte-identical to a pre-marker build's.
  Night n;
  n.turn = [] {
    orch::FireOutcome o;
    o.ok = true;
    return o;
  };
  dream::runNight(n.deps(), clockAt(50, 210), false);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"maintDay\":50,\"scrHash\":\"0000000000abcdef\",\"attemptDay\":50,"
      "\"attempts\":1,\"streakDay\":0,\"streak\":0,\"last\":\"ran\",\"cause\":\"\"}",
      dream::dumpDreamState(n.saved).c_str());
  // The marker keys exist only while the turn is in flight, and round-trip.
  Night c;
  c.crash = true;
  dream::runNight(c.deps(), clockAt(51, 210), false);
  const std::string mid = dream::dumpDreamState(c.crashedFile);
  TEST_ASSERT_TRUE(mid.find("\"inNight\":51") != std::string::npos);
  TEST_ASSERT_TRUE(mid.find("\"inAttempt\":1") != std::string::npos);
  TEST_ASSERT_TRUE(mid.find("\"inStart\":\"") != std::string::npos);
  dream::DreamState back;
  TEST_ASSERT_TRUE(dream::loadDreamState(mid, back));
  TEST_ASSERT_EQUAL(51, (int)back.inflightNight);
  TEST_ASSERT_EQUAL(1, back.inflightAttempt);
  TEST_ASSERT_TRUE(back.inflightStart == c.crashedFile.inflightStart);
}

// ---- (6d) the boot reconcile driver (PG4: unit L, 21 Oct) -------------------------
// The bench leg died at "panic during boot" right after PANIC arm=turn. The only
// crash on record was the armed one (the leg read its own panic report as a boot
// failure); these pin the class the reconcile itself must hold: it runs only once
// its seams are up, never trusts a torn or foreign file, never counts a marker a
// later fire superseded, never re-reads over the working copy, and never loops.

struct BootRig {
  bool store = true, clock = true, sinks = true;
  bool haveFile = true;
  std::string file;            // the raw /data/dream.json text
  bool refuse = false;         // the store refuses every save
  uint64_t lastRun = 0;        // the reserved loop's lastRun
  int loads = 0, saves = 0, loopApplied = 0, rows = 0;
  dream::NightReport row;
  dream::DreamMemo memo;

  dream::BootDeps deps() {
    dream::BootDeps d;
    d.storeReady = [this] { return store; };
    d.clockSynced = [this] { return clock; };
    d.sinksReady = [this] { return sinks; };
    d.loadState = [this](dream::DreamState& s) {
      loads++;
      if (!haveFile) { s = dream::DreamState{}; return true; }
      return dream::loadDreamState(file, s);
    };
    d.saveState = [this](const dream::DreamState& s) {
      saves++;
      if (refuse) return false;
      file = dream::dumpDreamState(s);
      haveFile = true;
      return true;
    };
    d.loopLastRun = [this] { return lastRun; };
    d.applyLoop = [this](const dream::NightReport&) { loopApplied++; };
    d.publish = [this](const dream::NightReport& r) { rows++; row = r; };
    d.memo = &memo;
    return d;
  }
};

// The file a restart mid-reflection leaves: night `day`, attempt 1, started at 03:30.
static std::string markerFile(uint32_t day) {
  Night n;
  n.crash = true;
  n.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::runNight(n.deps(), clockAt(day, 210), false);
  TEST_ASSERT_EQUAL(day, n.crashedFile.inflightNight);
  return dream::dumpDreamState(n.crashedFile);
}

static void test_boot_reconcile_waits_for_its_seams_then_runs_once() {
  // Before the store is mounted, the clock is real or the timeline and loop records
  // are up, the pump waits and reads nothing; the old glue gave up for the whole
  // boot when the store was not mounted on its first pass.
  BootRig b;
  b.file = markerFile(9);
  dream::BootReconcile rec;
  const dream::NightClock c = clockAt(9, 230);
  b.store = false;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Waiting, (int)rec.pump(b.deps(), c));
  b.store = true;
  b.clock = false;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Waiting, (int)rec.pump(b.deps(), c));
  b.clock = true;
  b.sinks = false;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Waiting, (int)rec.pump(b.deps(), c));
  TEST_ASSERT_EQUAL(0, b.loads);
  TEST_ASSERT_FALSE(b.memo.loaded);
  TEST_ASSERT_FALSE(rec.done);
  // Every seam up: the night is reconciled exactly once, saved FIRST.
  b.sinks = true;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)rec.pump(b.deps(), c));
  TEST_ASSERT_EQUAL(1, b.saves);
  TEST_ASSERT_EQUAL(1, b.loopApplied);
  TEST_ASSERT_EQUAL(1, b.rows);
  TEST_ASSERT_EQUAL_STRING("restarted", b.row.cause.c_str());
  TEST_ASSERT_EQUAL(900, (int)b.row.retryAfterSec);
  TEST_ASSERT_TRUE(b.file.find("inNight") == std::string::npos);   // cleared durably
  TEST_ASSERT_TRUE(b.memo.loaded);
  TEST_ASSERT_EQUAL(0, (int)b.memo.st.inflightNight);
  TEST_ASSERT_EQUAL(1, b.memo.st.attemptsTonight);
  // Later passes: nothing more, whatever the seams say.
  for (int i = 0; i < 5; ++i)
    TEST_ASSERT_EQUAL((int)dream::BootStep::Done, (int)rec.pump(b.deps(), c));
  TEST_ASSERT_EQUAL(1, b.rows);
  TEST_ASSERT_EQUAL(1, b.loopApplied);
  TEST_ASSERT_EQUAL(1, b.loads);
  // Null seams never crash: a rig with nothing bound waits.
  dream::BootReconcile bare;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Waiting, (int)bare.pump(dream::BootDeps{}, c));
}

static void test_boot_reconcile_never_trusts_a_torn_file() {
  // Property over every truncation of a marker file (a write cut mid-way, a
  // corrupted block): unreadable, so no marker, no row, no counter, and the boot's
  // working copy starts fresh instead of half-trusting the bytes.
  const std::string full = markerFile(9);
  for (size_t cut = 0; cut < full.size(); ++cut) {
    BootRig b;
    b.file = full.substr(0, cut);
    dream::DreamState probe;
    TEST_ASSERT_FALSE_MESSAGE(dream::loadDreamState(b.file, probe), b.file.c_str());
    dream::BootReconcile rec;
    TEST_ASSERT_EQUAL((int)dream::BootStep::Clean, (int)rec.pump(b.deps(), clockAt(9, 230)));
    TEST_ASSERT_EQUAL(0, b.rows);
    TEST_ASSERT_EQUAL(0, b.loopApplied);
    TEST_ASSERT_EQUAL(0, b.saves);
    TEST_ASSERT_TRUE(b.memo.loaded);
    TEST_ASSERT_EQUAL(0, (int)b.memo.st.inflightNight);
    TEST_ASSERT_EQUAL(0, b.memo.st.streak);
  }
  // A file bigger than the state can be is unreadable too (never read into RAM on
  // the device), even when it is valid JSON with a marker.
  std::string big = full;
  big.insert(big.size() - 1, ",\"pad\":\"" + std::string(dream::kStateFileMax, 'x') + "\"");
  dream::DreamState probe;
  TEST_ASSERT_FALSE(dream::loadDreamState(big, probe));
  // Counter-test: the whole file is read and reconciled.
  BootRig whole;
  whole.file = full;
  dream::BootReconcile rec;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)rec.pump(whole.deps(), clockAt(9, 230)));
}

static void test_boot_reconcile_reads_a_marker_from_another_firmware_safely() {
  // Another firmware's file: a newer version, extra keys, fields of the wrong type.
  // Known fields are used, a wrong-typed one reads as absent, counters are clamped.
  dream::DreamState st;
  TEST_ASSERT_TRUE(dream::loadDreamState(
      "{\"v\":2,\"maintDay\":\"x\",\"attempts\":900,\"streak\":-4,\"last\":\"exploded\","
      "\"cause\":17,\"inNight\":\"20747\",\"inAttempt\":3,\"inStart\":12345,\"future\":[1,2]}",
      st));
  TEST_ASSERT_EQUAL(0, (int)st.maintDay);
  TEST_ASSERT_EQUAL(255, st.attemptsTonight);
  TEST_ASSERT_EQUAL(0, st.streak);
  TEST_ASSERT_EQUAL((int)dream::DreamOutcome::Ran, (int)st.lastOutcome);
  TEST_ASSERT_EQUAL_STRING("", st.lastCause.c_str());
  TEST_ASSERT_EQUAL(0, (int)st.inflightNight);   // a string night is no marker
  BootRig foreign;
  foreign.file = "{\"v\":2,\"inNight\":\"20747\",\"inAttempt\":3}";
  dream::BootReconcile r0;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Clean, (int)r0.pump(foreign.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(0, foreign.rows);
  // A marker naming attempt 0 is still a started attempt: attempt 1.
  BootRig zero;
  zero.file = "{\"v\":1,\"inNight\":9,\"inAttempt\":0,\"inStart\":\"0000000000000000\"}";
  dream::BootReconcile r1;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)r1.pump(zero.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(1, zero.row.attempt);
  TEST_ASSERT_TRUE(dream::renderDreamRow(zero.row).find("retry 2 of 3 at 04:05") != std::string::npos);
  // A marker dated after today (a clock that ran ahead) is anchored to today: no
  // later night's streak day or attempt day is pinned.
  BootRig ahead;
  ahead.file = markerFile(40);
  dream::BootReconcile r2;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)r2.pump(ahead.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(9, (int)ahead.memo.st.streakDay);
  TEST_ASSERT_EQUAL(9, (int)ahead.memo.st.attemptDay);
  TEST_ASSERT_EQUAL(0, (int)ahead.memo.st.maintDay);   // day 40's stamp reads as never
  TEST_ASSERT_EQUAL(1, ahead.row.streak);
  // When night 40 comes it is a night like any other: maintenance runs, the streak
  // moves, all three attempts are there.
  Night later;
  later.haveSaved = true;
  later.saved = ahead.memo.st;
  later.turn = [] { return failWith("chat HTTP 401: bad key"); };
  const dream::NightResult n40 = dream::runNight(later.deps(), clockAt(40, 210), false);
  TEST_ASSERT_EQUAL(2, n40.report.streak);
  TEST_ASSERT_EQUAL(1, n40.report.attempt);
  TEST_ASSERT_EQUAL(1, later.maintRuns);
  // The same holds for a file with no marker at all.
  BootRig clean;
  clean.file = "{\"v\":1,\"maintDay\":40,\"attemptDay\":40,\"attempts\":3,\"streakDay\":40,\"streak\":2}";
  dream::BootReconcile r3;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Clean, (int)r3.pump(clean.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(0, (int)clean.memo.st.maintDay);
  TEST_ASSERT_EQUAL(0, (int)clean.memo.st.attemptsTonight);
  TEST_ASSERT_EQUAL(0, (int)clean.memo.st.streakDay);
  TEST_ASSERT_EQUAL(2, clean.memo.st.streak);   // the count itself is kept
}

static void test_boot_reconcile_drops_a_marker_a_later_fire_superseded() {
  // The marker of a night (21 Oct 2026, day 20747) survived on disk while the
  // reserved loop kept firing: a firmware that does not know the marker ran the next
  // three nights, or the outcome writes after it did not land. The loop's lastRun is
  // newer than the marker's start, so that night is already on record: cleared, with
  // no row and no counter, and the file's newer verdict stays.
  const uint32_t day = 20747;
  const std::string mark = markerFile(day);
  dream::DreamState fileSt;
  TEST_ASSERT_TRUE(dream::loadDreamState(mark, fileSt));
  TEST_ASSERT_TRUE(fileSt.inflightStart >= dream::kSyncedEpochMin);
  BootRig b;
  b.file = mark;
  b.lastRun = clockAt(day + 3, 210).epoch;
  dream::BootReconcile rec;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Stale, (int)rec.pump(b.deps(), clockAt(day + 3, 300)));
  TEST_ASSERT_EQUAL(0, b.rows);
  TEST_ASSERT_EQUAL(0, b.loopApplied);
  TEST_ASSERT_EQUAL(1, b.saves);
  TEST_ASSERT_TRUE(b.file.find("inNight") == std::string::npos);
  TEST_ASSERT_EQUAL((int)fileSt.lastOutcome, (int)b.memo.st.lastOutcome);
  TEST_ASSERT_EQUAL(fileSt.streak, b.memo.st.streak);
  TEST_ASSERT_TRUE(dream::markerStale(fileSt, fileSt.inflightStart + dream::kMarkerStaleSlackSec + 1));
  // Counter-tests: the fire that wrote the marker set lastRun just before it (the
  // scheduler claims the run first), and a drill never moves lastRun at all.
  for (uint64_t last : {fileSt.inflightStart - 1, fileSt.inflightStart, (uint64_t)0}) {
    BootRig same;
    same.file = mark;
    same.lastRun = last;
    dream::BootReconcile r;
    TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)r.pump(same.deps(), clockAt(day, 230)));
    TEST_ASSERT_EQUAL(1, same.rows);
  }
  // A console drill before the time sync, on a device up for days without it, writes
  // a marker whose start is not a real date: it cannot be compared with a real
  // lastRun, so the interruption is reconciled, never dropped as stale (review).
  BootRig unsynced;
  unsynced.file = markerFile(5);   // a 1970 "day 5" clock
  unsynced.lastRun = clockAt(day - 1, 210).epoch;   // the last real nightly fire
  dream::BootReconcile r2;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Reconciled, (int)r2.pump(unsynced.deps(), clockAt(day, 230)));
  TEST_ASSERT_EQUAL(1, unsynced.rows);
  TEST_ASSERT_EQUAL(1, unsynced.loopApplied);
}

static void test_boot_reconcile_keeps_the_working_copy_a_fire_opened() {
  // A fire this boot already opened the state (a console drill before the clock
  // synced): its working copy is the authority, and the file it may lag behind is
  // never re-read over it.
  BootRig b;
  b.file = markerFile(9);
  b.memo.loaded = true;
  b.memo.st.attemptDay = 9;
  b.memo.st.attemptsTonight = 2;
  b.memo.st.streak = 4;
  dream::BootReconcile rec;
  TEST_ASSERT_EQUAL((int)dream::BootStep::Clean, (int)rec.pump(b.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(0, b.loads);
  TEST_ASSERT_EQUAL(0, b.rows);
  TEST_ASSERT_EQUAL(2, b.memo.st.attemptsTonight);
  TEST_ASSERT_EQUAL(4, b.memo.st.streak);
}

static void test_boot_reconcile_is_bounded_when_the_store_refuses() {
  // A store that refuses every save: nothing is counted (save first), the pump gives
  // up after the bound with the marker left for a boot with a working store, and it
  // never loops.
  BootRig b;
  b.file = markerFile(9);
  b.refuse = true;
  dream::BootReconcile rec;
  for (int i = 1; i < dream::kBootSaveTries; ++i)
    TEST_ASSERT_EQUAL((int)dream::BootStep::SaveFailed, (int)rec.pump(b.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL((int)dream::BootStep::GaveUp, (int)rec.pump(b.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL((int)dream::BootStep::Done, (int)rec.pump(b.deps(), clockAt(9, 230)));
  TEST_ASSERT_EQUAL(dream::kBootSaveTries, b.saves);
  TEST_ASSERT_EQUAL(0, b.rows);
  TEST_ASSERT_EQUAL(0, b.loopApplied);
  TEST_ASSERT_TRUE(b.file.find("\"inNight\":9") != std::string::npos);
}

// ---- (6e) one night, one row (PG4: unit L, 21 Oct) ---------------------------------
// The bench leg read "no ev:dream row was written" after a failed night on a full
// NVS; its reader stopped at the EPIQ summary line, before any row. The firmware
// side of the class is pinned here: every path through a night hands the timeline
// exactly one row, whatever the turn, the writes or the store did (a state store
// that is down still gets its row, saying "state not saved"). The night has no NVS
// seam at all; the device writes the row to the episodic store on the card.

struct RowCase {
  const char* name;
  int kind;          // 0 ok, 1 failed turn, 2 low-memory defer, 3 quiet day
  const char* err;   // the turn's error (kind 1)
  uint32_t failed;   // memory writes that failed
  bool embedUp;
  int store;         // 0 healthy, 1 not mounted, 2 refuses writes, 3 takes one write
  bool force;
};

static const RowCase kRowCases[] = {
    {"ok", 0, "", 0, true, 0, false},
    {"server error", 1, "chat HTTP 500: overloaded", 0, true, 0, false},
    {"rate limit", 1, "chat HTTP 429: slow down", 0, true, 0, false},
    {"timeout", 1, "timeout: no answer in 120 s", 0, true, 0, false},
    {"network", 1, "network: no response", 0, true, 0, false},
    {"key", 1, "chat HTTP 401: invalid x-api-key", 0, true, 0, false},
    {"credit", 1, "chat HTTP 400: credit balance is too low", 0, true, 0, false},
    {"bad request", 1, "chat HTTP 400: bad body", 0, true, 0, false},
    {"budget", 1, "budget: over the monthly ceiling", 0, true, 0, false},
    {"parse", 1, "parse: not json", 0, true, 0, false},
    {"other", 1, "something new", 0, true, 0, false},
    {"no text", 1, "", 0, true, 0, false},
    {"low memory defer", 2, "", 0, true, 0, false},
    {"failed writes", 0, "", 3, true, 0, false},
    {"memory unavailable", 0, "", 0, false, 0, false},
    {"quiet day", 3, "", 0, true, 0, false},
    {"quiet day, drill", 3, "", 0, true, 0, true},
    {"failed, store not mounted", 1, "network: no response", 0, true, 1, false},
    {"failed, store refuses", 1, "network: no response", 0, true, 2, false},
    {"failed, store dies mid-night", 1, "network: no response", 0, true, 3, false},
    {"degraded, store refuses", 0, "", 2, true, 2, false},
    {"quiet day, store not mounted", 3, "", 0, true, 1, false},
};

static void scriptNight(Night& n, const RowCase& k) {
  n.mounted = k.store != 1;
  n.saveRefused = k.store == 2;
  n.failAfter = k.store == 3 ? 1 : -1;
  n.embedUp = k.embedUp;
  n.quietDay = k.kind == 3;
  if (n.quietDay) {   // a provably quiet night: the scratchpad as the last dream left it
    n.haveSaved = true;
    n.saved.scrHash = n.scratch;
  }
  n.ops.failed = 0;
  const RowCase kk = k;
  Night* np = &n;
  n.turn = [kk, np] {
    np->ops.failed += kk.failed;   // the reflection's writes, as the counters see them
    orch::FireOutcome o;
    if (kk.kind == 1) return failWith(kk.err);
    if (kk.kind == 2) { o.deferred = true; return o; }
    o.ok = true;
    return o;
  };
}

static void test_every_night_path_writes_exactly_one_row() {
  for (const RowCase& k : kRowCases) {
    for (uint16_t minute : {(uint16_t)210, (uint16_t)400, (uint16_t)1200}) {
      Night n;
      scriptNight(n, k);
      int rows = 0;
      std::string text;
      bool reportSeen = false;
      dream::NightDeps d = n.deps();
      d.emitRow = [&](const agent::DreamEndEv& ev) {
        rows++;
        reportSeen = ev.report != nullptr;
        if (ev.report) text = dream::renderDreamRow(*ev.report);
      };
      const dream::NightResult res = dream::runNight(d, clockAt(30, minute), k.force);
      char msg[96];
      snprintf(msg, sizeof msg, "%s at minute %u", k.name, (unsigned)minute);
      TEST_ASSERT_EQUAL_MESSAGE(1, rows, msg);
      TEST_ASSERT_TRUE_MESSAGE(reportSeen, msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(dream::renderDreamRow(res.report).c_str(), text.c_str(), msg);
      TEST_ASSERT_TRUE_MESSAGE(text.rfind("Nightly dream ", 0) == 0, msg);
      if (!res.report.stateSaved)
        TEST_ASSERT_TRUE_MESSAGE(text.find("state not saved") != std::string::npos, msg);
      if (k.kind == 3 && !k.force)
        TEST_ASSERT_EQUAL_MESSAGE((int)dream::DreamOutcome::Skipped, (int)res.report.outcome, msg);
    }
  }
  // The event carries the night itself (the row, the cause, the spend).
  dream::NightReport rep;
  rep.outcome = dream::DreamOutcome::Failed;
  rep.cause = "network";
  rep.error = "network: no response";
  rep.tokens = 12;
  rep.maint.pruned = 2;
  rep.maint.deduped = 1;
  const agent::DreamEndEv ev = dream::dreamEndEvent(rep, false);
  TEST_ASSERT_TRUE(ev.report == &rep);
  TEST_ASSERT_FALSE(ev.turnOk);
  TEST_ASSERT_EQUAL_STRING("network: no response", ev.cause.c_str());
  TEST_ASSERT_EQUAL(12, (int)ev.tokens);
  TEST_ASSERT_EQUAL(2, ev.prunedVectors);
  TEST_ASSERT_EQUAL(1, ev.dedupedVectors);
  // The starved night and the interrupted night render through the same event.
  dream::NightReport starved;
  starved.outcome = dream::DreamOutcome::Starved;
  starved.cause = "recent-turn";
  starved.attempt = 12;
  TEST_ASSERT_TRUE(dream::renderDreamRow(*dream::dreamEndEvent(starved, false).report)
                       .rfind("Nightly dream skipped tonight (device busy after 12 tries)", 0) == 0);
}

// The night carries what the reflection held to the glue, which hands it to the
// owner at their next conversation (ownerNoteText).
static void test_the_night_carries_the_held_note() {
  Night n;
  n.turn = [] {
    orch::FireOutcome o;
    o.ok = true;
    o.held = "Two notes clash.";
    return o;
  };
  TEST_ASSERT_EQUAL_STRING("Two notes clash.", dream::runNight(n.deps(), clockAt(7, 210), false).report.held.c_str());
  Night quiet;
  TEST_ASSERT_TRUE(dream::runNight(quiet.deps(), clockAt(7, 210), false).report.held.empty());
}

// One generated store and night sequence for the bounded-retry property.
static void runBoundedSequence() {
  Night n;
  const uint32_t mode = rnd() % 3;   // 0 healthy, 1 not mounted, 2 fails after K writes
  n.mounted = mode != 1;
  if (mode == 2) n.failAfter = (int)(rnd() % 6);
  uint8_t streakBefore = 0;
  for (uint32_t day = 100; day < 110; ++day) {
    const uint32_t kind = rnd() % 4;   // 0 ok, 1 transient, 2 final failure, 3 low memory
    n.turn = [kind] {
      if (kind == 0) { orch::FireOutcome o; o.ok = true; return o; }
      if (kind == 3) { orch::FireOutcome o; o.deferred = true; return o; }
      return failWith(kind == 1 ? "chat HTTP 503: overloaded" : "messages HTTP 401: bad key");
    };
    const int maintBefore = n.maintRuns;
    dream::NightResult last;
    const int fires = driveNight(n, day, &last);
    TEST_ASSERT_TRUE(fires >= 1 && fires <= dream::kMaxAttempts);
    TEST_ASSERT_TRUE(n.maintRuns - maintBefore <= 1);
    if (mode == 1) TEST_ASSERT_EQUAL(0, n.saves);
    if (!last.report.stateSaved)
      TEST_ASSERT_TRUE(dream::renderDreamRow(last.report).find("state not saved (storage") != std::string::npos);
    const bool ok = last.report.outcome == dream::DreamOutcome::Ran;
    TEST_ASSERT_EQUAL(ok ? 0 : streakBefore + 1, last.report.streak);
    streakBefore = last.report.streak;
  }
}

static void test_bounded_retries_whatever_the_store_does() {
  // Property: 300 generated sequences of ten nights, each over a healthy store, a store
  // that is not mounted, or one that fails after a random number of writes, with a
  // random turn result per night. Every night takes 1 to 3 fires, maintenance runs at
  // most once a day, an unmounted store is never written, the streak moves exactly once
  // per night, and a night that could not save says so in its row.
  g_seed = 4242;
  for (int i = 0; i < 300; ++i) runBoundedSequence();
}

static void test_unsynced_clock_skips_maintenance_honestly() {
  Night n;
  dream::NightClock c = clockAt(5, 210);
  c.synced = false;
  dream::NightResult r = dream::runNight(n.deps(), c, true);
  TEST_ASSERT_EQUAL(0, n.maintRuns);
  TEST_ASSERT_TRUE(dream::renderDreamRow(r.report).find("maintenance skipped (clock not set)") != std::string::npos);
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
static void test_every_failed_loop_record_names_its_cause() {
  // The scheduler's write, over the ways a night's turn fails (an empty cause too).
  const char* const causes[] = {"chat HTTP 503: down", "messages HTTP 400: credit balance is too low",
                                "network: connect failed", "no text in the reply", ""};
  for (const char* cause : causes) {
    Night n;
    n.turn = [cause] { return failWith(cause); };
    dream::NightResult res = dream::runNight(n.deps(), clockAt(9, 210), false);
    TEST_ASSERT_FALSE(res.fire.ok);
    orch::LoopRecord loop = dream::reservedLoopRecord();
    orch::onFireResult(loop, res.fire.ok, res.fire.tokens, 0, clockAt(9, 210).epoch);
    orch::noteFireError(loop, res.fire.ok, res.fire.error);
    TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)loop.lastResult);
    TEST_ASSERT_FALSE_MESSAGE(loop.lastErr.empty(), cause);
    if (*cause) TEST_ASSERT_TRUE_MESSAGE(loop.lastErr.find(cause) != std::string::npos, loop.lastErr.c_str());
  }

  // The reconcile's write, after an older night that failed for another cause and
  // on a record whose last fire was ok: the restart is the cause either way.
  Night crashed;
  crashed.crash = true;
  crashed.turn = [] { return failWith("chat HTTP 503: down"); };
  dream::runNight(crashed.deps(), clockAt(9, 210), false);
  dream::DreamState st = crashed.crashedFile;
  dream::InterruptedNight v = dream::reconcileBoot(st, clockAt(9, 230));
  TEST_ASSERT_TRUE(v.interrupted);
  TEST_ASSERT_EQUAL_STRING(dream::kRestartedError, v.report.error.c_str());

  orch::LoopRecord older = dream::reservedLoopRecord();
  orch::onFireResult(older, false, orch::TokenUsage{}, 0, clockAt(8, 210).epoch);
  orch::noteFireError(older, false, "chat HTTP 503: down");
  orch::LoopRecord okBefore = dream::reservedLoopRecord();
  orch::onFireResult(okBefore, true, orch::TokenUsage{}, 7, clockAt(8, 210).epoch);
  orch::noteFireError(okBefore, true, "");
  TEST_ASSERT_TRUE(okBefore.lastErr.empty());
  for (orch::LoopRecord* loop : {&older, &okBefore}) {
    dream::applyInterruptedToLoop(*loop, clockAt(9, 230).epoch, v.report.retryAfterSec);
    TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)loop->lastResult);
    TEST_ASSERT_EQUAL_STRING(dream::kRestartedError, loop->lastErr.c_str());
  }

  // The cause survives loops.json (the file /api/loops is read from) and an ok fire
  // clears it again.
  std::vector<orch::LoopRecord> back;
  TEST_ASSERT_TRUE(orch::loadLoops(orch::dumpLoops({older}), back));
  TEST_ASSERT_EQUAL(1, (int)back.size());
  TEST_ASSERT_EQUAL((int)orch::LastResult::Fail, (int)back[0].lastResult);
  TEST_ASSERT_EQUAL_STRING(dream::kRestartedError, back[0].lastErr.c_str());
  orch::onFireResult(back[0], true, orch::TokenUsage{}, 9, clockAt(10, 210).epoch);
  orch::noteFireError(back[0], true, "");
  const std::string okJson = orch::dumpLoops(back);
  TEST_ASSERT_TRUE_MESSAGE(okJson.find("lastErr") == std::string::npos, okJson.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_turn_notice_count_zero_on_every_failure_path);
  RUN_TEST(test_quiet_dream_contacts_nobody_over_the_matrix);
  RUN_TEST(test_held_note_reaches_the_owner_once_in_plain_words);
  RUN_TEST(test_quiet_billing_switch_keeps_the_notice_latch_armed);
  RUN_TEST(test_dream_can_never_call_files_or_session_tools);
  RUN_TEST(test_dream_turn_fields_cannot_spawn_or_act);
  RUN_TEST(test_quiet_preamble_never_asks_for_a_reply);
  RUN_TEST(test_failed_fire_reports_no_stale_tokens);
  RUN_TEST(test_memory_results_stay_with_their_chat);
  RUN_TEST(test_night_outcome_property);
  RUN_TEST(test_dream_night_retry_window);
  RUN_TEST(test_failed_write_night_row_says_failed_with_counts);
  RUN_TEST(test_queued_write_night_row_names_it_queued);
  RUN_TEST(test_memory_unavailable_night_says_so_and_is_degraded);
  RUN_TEST(test_streak_alerts_and_breaker_exemption);
  RUN_TEST(test_gate_starvation_bound);
  RUN_TEST(test_dream_state_store_fresh_legacy_and_full_nvs);
  RUN_TEST(test_unmounted_store_is_never_written_and_the_night_stays_bounded);
  RUN_TEST(test_store_failing_after_n_writes_keeps_counts_and_says_so);
  RUN_TEST(test_interrupted_night_reconciles_at_boot);
  RUN_TEST(test_interrupted_night_after_the_window_counts_once_and_never_retries);
  RUN_TEST(test_interrupted_night_never_shows_an_older_verdict);
  RUN_TEST(test_completed_night_leaves_no_marker_and_is_byte_identical);
  RUN_TEST(test_boot_reconcile_waits_for_its_seams_then_runs_once);
  RUN_TEST(test_boot_reconcile_never_trusts_a_torn_file);
  RUN_TEST(test_boot_reconcile_reads_a_marker_from_another_firmware_safely);
  RUN_TEST(test_boot_reconcile_drops_a_marker_a_later_fire_superseded);
  RUN_TEST(test_boot_reconcile_keeps_the_working_copy_a_fire_opened);
  RUN_TEST(test_boot_reconcile_is_bounded_when_the_store_refuses);
  RUN_TEST(test_every_night_path_writes_exactly_one_row);
  RUN_TEST(test_the_night_carries_the_held_note);
  RUN_TEST(test_bounded_retries_whatever_the_store_does);
  RUN_TEST(test_unsynced_clock_skips_maintenance_honestly);
  RUN_TEST(test_memory_write_counters_on_the_registry_path);
  RUN_TEST(test_every_failed_loop_record_names_its_cause);
  return UNITY_END();
}
