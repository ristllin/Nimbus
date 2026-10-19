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
#include "nimbus/mem_cap.h"
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
//     writes), and [MEMORY RESULTS] go back to the chat that asked.

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
  // Per call: ok + error + output + usage.
  bool ok = true;
  std::string err;
  std::string outJson = "{\"reply\":\"\",\"memory\":\"\",\"ask\":\"\"}";
  uint32_t usageIn = 0, usageOut = 0;
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
    d.recall = [](const std::string&, const orch::Principal&) { return std::vector<std::string>{}; };
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
    d.apply.memDispatch = [](const char* name, ArduinoJson::JsonObjectConst a, const orch::Principal&) {
      if (std::string(name) == "memory.search")
        return orch::ToolResult::ok(std::string("found: ") + (a["query"] | ""));
      return orch::ToolResult::ok("stored memory m1");
    };
    d.apply.enqueueSpawn = [this](const orch::Spawn&, const std::string&, bool) { spawns++; };
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
    eng.reset(new TurnEngine(std::move(d)));
  }
  orch::FireOutcome dreamTurn(const std::string& inputs = "[DREAM] test") {
    return eng->injectScheduledTurn("1001", inputs, "Dream", "dream", /*quietOk=*/true);
  }
  orch::FireOutcome routineTurn() {
    return eng->injectScheduledTurn("1001", "check the weather", "Morning", "L1", /*quietOk=*/false);
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
  // Bounded: many chats parking results keep at most 8 slots, the newest included.
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\",\"mem_query\":[\"q\"]}";
  for (int i = 0; i < 20; ++i) r.eng->handleMessage("x", "M", "tg:" + std::to_string(i));
  r.outJson = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  r.inputsSeen.clear();
  r.eng->handleMessage("y", "M", "tg:19");
  TEST_ASSERT_TRUE(r.inputsSeen[0].find("[MEMORY RESULTS]") != std::string::npos);
}

// ---- (5) the night pipeline with fake seams --------------------------------------

struct Night {
  dream::DreamState saved;
  bool haveSaved = false;
  int saves = 0, nvsWrites = 0;
  bool saveRefused = false;
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
      if (saveRefused) return false;
      // Round-trip through the file format, as the device does.
      dream::DreamState back;
      TEST_ASSERT_TRUE(dream::loadDreamState(dream::dumpDreamState(s), back));
      saved = back;
      haveSaved = true;
      return true;
    };
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
                                    "stored (1 new, 0 updated, 2 failed)") == 0, row.c_str());
  TEST_ASSERT_TRUE(row.find("spent 1000 tokens") != std::string::npos);
  // And a failed turn with a failed write: FAILED, the cause and the counts.
  Night f;
  f.turn = [&f] {
    f.ops.failed += 1;
    return failWith("chat HTTP 400: credit balance is too low");
  };
  const std::string frow = dream::renderDreamRow(dream::runNight(f.deps(), clockAt(7, 210), false).report);
  TEST_ASSERT_TRUE_MESSAGE(frow.find("Nightly dream FAILED (billing: chat HTTP 400: credit balance is too low), "
                                     "no retry tonight; 0 of 1 memories stored (0 new, 0 updated, 1 failed)") == 0,
                           frow.c_str());
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_turn_notice_count_zero_on_every_failure_path);
  RUN_TEST(test_dream_can_never_call_files_or_session_tools);
  RUN_TEST(test_dream_turn_fields_cannot_spawn_or_act);
  RUN_TEST(test_quiet_preamble_never_asks_for_a_reply);
  RUN_TEST(test_failed_fire_reports_no_stale_tokens);
  RUN_TEST(test_memory_results_stay_with_their_chat);
  RUN_TEST(test_night_outcome_property);
  RUN_TEST(test_dream_night_retry_window);
  RUN_TEST(test_failed_write_night_row_says_failed_with_counts);
  RUN_TEST(test_memory_unavailable_night_says_so_and_is_degraded);
  RUN_TEST(test_streak_alerts_and_breaker_exemption);
  RUN_TEST(test_gate_starvation_bound);
  RUN_TEST(test_dream_state_store_fresh_legacy_and_full_nvs);
  RUN_TEST(test_unsynced_clock_skips_maintenance_honestly);
  RUN_TEST(test_memory_write_counters_on_the_registry_path);
  return UNITY_END();
}
