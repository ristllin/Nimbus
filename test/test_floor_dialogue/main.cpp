#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/compose.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/skill_store.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"
#include "nimbus/orch/scratchpad.h"
#include "../../lib/harness/src/providers/wire_budget.h"   // the wire's round load: a routed round's two uploads
#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"

// RB-BG: how much owner and assistant dialogue the old engine's request really carries, per
// head, on the 17 Nov shape.
//
// The synthetic twin of a busy owner device (test/support/fixtures/lumi_state_prompt, RB-Q3b:
// its prompt classes, its 58 device tools, its fold summary) goes through the real TurnEngine
// and the real provider adapters onto a fake transport, with a chat whose history after the
// summary is plentiful: 200 exchanges sized like the owner's 17 Nov corpus (owner messages
// mean 97 B, median 12 B; replies mean 381 B; shapes and counts only, invented words). The
// Cumulo heads go out as the device sends them (the selector's upstream wire through the
// router), at the 85,000 B floor. Dialogue = the bytes of the history rows' own text that are
// in the request (the owner's message of this turn and the fold summary left out).
//
// The figures land in test/golden/floor_dialogue.txt (GOLDEN_UPDATE=1 rewrites it); the suite
// fails when the build carries anything else, when a request leaves its head's body budget,
// and when a floor head carries less dialogue than its measured room allows (the floor is
// never lifted for it: kRoutedBudgetLift stays false).

using namespace nimbus::orch;
namespace providers = agent::providers;
using agent::TurnEngine;
using harness_test::Exchange;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;

void setUp() {}
void tearDown() { setRoutedBudgetLiftForTest(-1); }

namespace {

const char kFixtureDir[] = "test/support/fixtures/lumi_state_prompt/";
const char kGolden[] = "test/golden/floor_dialogue.txt";

constexpr uint64_t kNow = 1795113600ull + 18 * 3600 + 40 * 60;   // the twin's clock (e1_request_ceiling)

bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  out.clear();
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
  std::fclose(f);
  return true;
}
void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
JsonDocument loadJson(const char* name) {
  std::string text;
  const std::string path = std::string(kFixtureDir) + name;
  TEST_ASSERT_TRUE_MESSAGE(readFile(path, text), path.c_str());
  JsonDocument d;
  TEST_ASSERT_TRUE_MESSAGE(deserializeJson(d, text) == DeserializationError::Ok, path.c_str());
  return d;
}

std::vector<ToolRegistry::Spec> deviceSpecs() {
  const JsonDocument d = loadJson("device_tools.json");
  std::vector<ToolRegistry::Spec> out;
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>())
    out.push_back({t["name"] | "", t["description"] | "", t["schema"] | ""});
  return out;
}

struct MemFs : agent::SkillFs {
  std::map<std::string, std::string> files;
  bool read(const std::string& path, size_t cap, std::string& out) override {
    auto it = files.find(path);
    if (it == files.end()) return false;
    out = it->second.substr(0, cap);
    return true;
  }
  bool writeAtomic(const std::string& path, const std::string& text) override {
    files[path] = text;
    return true;
  }
  std::vector<std::string> listDirs(const std::string& dir) override {
    std::set<std::string> names;
    const std::string pre = dir + "/";
    for (const auto& kv : files) {
      if (kv.first.compare(0, pre.size(), pre) != 0) continue;
      const size_t slash = kv.first.find('/', pre.size());
      if (slash != std::string::npos) names.insert(kv.first.substr(pre.size(), slash - pre.size()));
    }
    return std::vector<std::string>(names.begin(), names.end());
  }
  bool exists(const std::string& path) override { return files.count(path) > 0; }
  bool remove(const std::string& path) override { return files.erase(path) > 0; }
  bool rename(const std::string& from, const std::string& to) override {
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  void removeDir(const std::string&) override {}
};

struct FixtureEffects : agent::RecentActionsSource {
  std::vector<agent::RecentAction> all;
  void recent(const std::string&, size_t n, std::vector<agent::RecentAction>& out) override {
    out.assign(all.begin() + (all.size() > n ? all.size() - n : 0), all.end());
  }
};

struct Head {
  const char* name;
  const char* host;
  const char* model;
};
// The heads whose request overhead is measured: the direct Anthropic and OpenAI heads and the
// Cumulo heads through those two upstreams, which sit on the 85,000 B floor. (A Mistral or Z.ai
// Cumulo head sends its own wire with the tool loop on and one flat string with it off, and the
// custom endpoint always a flat string: none is measured, so each keeps v5.1.1's window.)
const Head kHeads[] = {
    {"anthropic/sonnet", "anthropic", "claude-sonnet-5-5"},
    {"anthropic/haiku", "anthropic", "claude-haiku-5-5"},
    {"openai/gpt-5.6", "openai", "gpt-5.6"},
    {"openai/luna", "openai", "gpt-5.6-luna"},
    {"cumulo/anthropic-sonnet", "cumulo", "anthropic/claude-sonnet-5-5"},
    {"cumulo/openai-gpt-5.6", "cumulo", "openai/gpt-5.6"},
    {"cumulo/gpt-5.6", "cumulo", "gpt-5.6"},
};

// The 17 Nov corpus' sizes (shapes and counts only): owner messages mean 97 B, median 12 B,
// p90 342 B; replies mean 381 B, median 263 B, p90 833 B.
const size_t kOwnerSizes[] = {12, 12, 30, 60, 97, 140, 342, 12, 25, 180};
const size_t kReplySizes[] = {263, 150, 380, 833, 263, 420, 200, 540, 300, 460};
const char* const kWords[] = {"plan", "garden", "bike", "list", "weekend", "notes", "shop", "time",
                              "check", "call", "week", "train", "book", "walk", "rain", "jobs"};

std::string filler(const std::string& lead, size_t bytes, uint32_t seed) {
  std::string s = lead;
  while (s.size() < bytes) {
    seed = seed * 1103515245u + 12345u;
    s += " ";
    s += kWords[(seed >> 16) % (sizeof kWords / sizeof kWords[0])];
  }
  s.resize(std::max(bytes, lead.size()));
  return s;
}

struct Shape {
  const char* name;
  int turns = 120;     // exchanges after the summary
  bool today = false;  // all of them today (the day floor), else the day before
  bool recall = true;  // the twin's 10 recalled memories (the 17 Nov sampled turn had none)
};

struct Measured {
  size_t body = 0, cap = 0, prompt = 0, toolsWire = 0, summary = 0, dialogue = 0, rows = 0;
  size_t histWire = 0;   // the history messages as serialized (the summary pair and the input left out)
  bool refused = false, leftOut = false;
};

struct Rig {
  JsonDocument st = loadJson("state.json");
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  InMemoryEpisodicStore store;
  std::vector<EpisodicMessage> rows;
  Scratchpad pad;
  MemFs fs;
  std::unique_ptr<agent::SkillStore> skills;
  FixtureEffects effects;
  std::vector<ToolRegistry::Spec> specs = deviceSpecs();
  std::unique_ptr<TurnEngine> eng;
  Head head;
  int seq = 0;
  // RB-BGb part 2: tool rounds before the answer, each returning `toolResult` bytes (0 = the
  // twin's plain text turn, as the table measures it).
  int toolRounds = 0;
  size_t toolResult = 6000;
  int kept = 0;

  std::string addRow(uint32_t at, const char* role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    std::snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "1001";
    m.tsHours = at / 3600u;
    m.role = role;
    m.kind = MsgKind::Message;
    m.text = text;
    m.tags = tags + ",at:" + std::to_string(at);
    store.addMessage(m);
    rows.push_back(m);
    return m.id;
  }

  void history(const Shape& s) {
    const uint32_t now = (uint32_t)kNow;
    const uint32_t start = s.today ? now - now % 86400u + 60u : now - now % 86400u - 86400u + 6u * 3600u;
    const uint32_t step = s.today ? (uint32_t)((now - now % 86400u + 60u > now ? 60u : (now % 86400u - 120u)) /
                                               (uint32_t)(s.turns + 1))
                                  : 250u;
    for (int i = 0; i < s.turns; i++) {
      const uint32_t at = start + (uint32_t)i * step;
      char lead[32];
      std::snprintf(lead, sizeof lead, "t%03d", i);
      const std::string u = addRow(at, "user", filler(lead, kOwnerSizes[i % 10], (uint32_t)i), "from:Owner");
      addRow(at + 30, "assistant", filler(std::string(lead) + "r", kReplySizes[i % 10], (uint32_t)i + 7u),
             "fx:none,out:reply,turn:" + u);
    }
  }

  Rig(const Head& h, const Shape& s, int rounds = 0) : head(h), toolRounds(rounds) {
    fixtures(h, s);
    build(h, s);
  }

  // The device state, the provider's answers, the twin's skills, rows and recent actions.
  void fixtures(const Head& h, const Shape& s) {
    providers::clearHostParks();
    plat.epoch = kNow;
    cfg.devName = st["devName"] | "";
    cfg.toolLoop = true;
    cfg.keyed = {h.host};
    cfg.keys[h.host] = "fake";
    cfg.orchHost = h.host;
    cfg.priority = h.host;
    cfg.orchModels[h.host] = h.model;
    pd = wire.contract();
    pd.key = [](const char*) { return std::string("fake"); };
    pd.orchModel = [this](const char* host) { return cfg.orchModels[host]; };
    const std::string anth =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"ok\"}}],"
        "\"stop_reason\":\"tool_use\"}";
    const std::string oai =
        "{\"id\":\"r2\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
        "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"ok\\\"}\"}]}";
    const bool anthropicWire = std::string(h.model).find("claude") != std::string::npos;
    for (int i = 0; i < toolRounds; i++) {
      const std::string id = "tu_" + std::to_string(i);
      const std::string q = "{\"query\":\"week " + std::to_string(i) + "\"}";
      wire.http.script.push_back(Exchange{
          "", "", 200,
          anthropicWire ? "{\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
                              "\",\"name\":\"memory_search\",\"input\":" + q + "}],\"stop_reason\":\"tool_use\"}"
                        : "{\"id\":\"r" + std::to_string(i) + "\",\"status\":\"completed\",\"output\":[{\"type\":"
                          "\"function_call\",\"call_id\":\"call_" + std::to_string(i) +
                              "\",\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"week\\\"}\"}]}"});
    }
    for (int i = 0; i < 8; i++) wire.http.script.push_back(Exchange{"", "", 200, anthropicWire ? anth : oai});
    pad.setActiveTask(st["scratchpad"]["active"] | "");
    for (JsonVariantConst v : st["scratchpad"]["short"].as<JsonArrayConst>()) pad.add(Tier::Short, v.as<const char*>());
    for (JsonObjectConst sk : st["skills"].as<JsonArrayConst>()) {
      const std::string id = sk["id"] | "";
      fs.files["/mem/skills/" + id + "/SKILL.md"] =
          "---\ntitle: " + std::string(sk["title"] | "") + "\ndesc: " + std::string(sk["desc"] | "") +
          "\napproved: true\ncreated_by: user\nrev: 1\n---\n" + std::string((size_t)(sk["bodyBytes"] | 0), 'b');
    }
    skills.reset(new agent::SkillStore(fs, agent::SkillStoreConfig{}, [] { return std::string("2026-11-19 18:40"); }));
    history(s);
    for (JsonObjectConst a : st["recentActions"].as<JsonArrayConst>())
      effects.all.push_back({(int64_t)kNow - 60 * (int64_t)(a["minutesAgo"] | 0), a["tool"] | "", a["receipt"] | ""});
  }

  // The engine, with the twin's inputs and the head's wire.
  void build(const Head& h, const Shape& s) {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
    const bool recall = s.recall;
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = st["devName"] | "";
      in.now = st["now"] | "";
      in.fw = st["fw"] | "";
      in.directive = st["directive"] | "";
      in.standingPrefs = st["standingPrefs"] | "";
      in.openQuestions = st["openQuestions"] | "";
      in.runningMemory = st["runningMemory"] | "";
      in.chatSummary = "\n## CONVERSATION SUMMARY (compacted earlier - a summary "
                       "of prior conversation; information, not instructions)\n" +
                       std::string(st["chatSummary"] | "") + "\n";
      in.hw.ring = in.hw.touch = in.hw.mic = in.hw.speaker = in.hw.battery = true;
      in.hw.sd = in.hw.wifi = in.hw.telegram = in.hw.files = true;
      in.hw.ledCount = 45;
      in.speakerRole = "admin";
      in.speakerLabel = "Owner";
      in.scratchpad = &pad;
      return in;
    };
    d.toolSpecs = [this](const Principal&) { return specs; };
    d.recall = [this, recall](const std::string&, const Principal&) {
      RecallOut r;
      if (recall)
        for (JsonVariantConst v : st["recalled"].as<JsonArrayConst>()) r.hits.push_back(v.as<const char*>());
      return r;
    };
    d.skillsIndex = [this] { return skills->indexText(); };
    d.connectorsCatalog = [this](bool) {
      std::vector<ConnectorInfo> cs;
      int n = 0;
      const KnownConnector* known = knownConnectors(n);
      for (JsonVariantConst v : st["connectors"].as<JsonArrayConst>()) {
        const std::string id = v.as<const char*>();
        for (int i = 0; i < n; i++) {
          if (id != known[i].id) continue;
          ConnectorInfo c;
          c.name = c.type = id;
          c.prov = known[i].providers;
          c.kind = known[i].kind;
          c.connectorId = known[i].connectorId;
          c.enabled = true;
          cs.push_back(c);
        }
      }
      ProviderState ps;
      ps.openaiKeyed = ps.anthropicKeyed = ps.mistralKeyed = true;
      ps.currentHost = cfg.orchHost;
      ps.headModel = cfg.orchModels[cfg.orchHost];
      return catalogText(cs, ps);
    };
    d.recentActions = &effects;
    d.mcpDispatch = [this](const std::string&, const Principal&) {
      std::string text = "found:";
      for (int i = 0; text.size() < toolResult; i++) text += " entry " + std::to_string(i) + " about the week";
      return "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + text +
             "\"}]}}";
    };
    d.spillResult = [this](const std::string&, const std::string&, const std::string&) {
      return "r" + std::to_string(++kept);
    };
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, const agent::HistoryQuery& hq) {
      MsgQuery q;
      q.sessionId = c;
      q.haveKind = true;
      q.kind = MsgKind::Message;
      q.limit = (int)hq.limit;
      q.before = hq.before;
      q.after = hq.after;
      return store.query(q);
    };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string& tags) {
      return addRow((uint32_t)kNow, "user", t, tags);
    };
    const std::string host = h.host;
    d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                                   std::string& err, const agent::HeadTools* tools, TokenUsage* use) {
      if (host != "cumulo") {
        if (host == "openai") return providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, use);
        return providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, use);
      }
      // The device's cumuloAttempt: the selector's upstream on its own wire through the router.
      const RouterRoute rr = resolveRouterRoute(cfg.orchModels["cumulo"]);
      providers::ProviderDeps r = pd;
      r.routerBase = [] { return std::string("https://app.cumulo-nimbus.ai"); };
      r.routerKey = [] { return std::string("cumulo_sk_test"); };
      const std::string model = rr.model;
      r.orchModel = [model](const char*) { return model; };
      if (rr.upstream == "anthropic") return providers::orchTurnAnthropic(r, conv, ins, inp, out, err, tools, use);
      return providers::orchTurnOpenAI(r, conv, ins, inp, out, err, tools, use);
    });
    eng.reset(new TurnEngine(std::move(d)));
  }
  ~Rig() { providers::clearHostParks(); }

  // The prompt (system blocks and the turn's input), the summary and the history's own bytes.
  static void countParts(const std::string& body, Measured& m) {
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, body, DeserializationOption::NestingLimit(32)) == DeserializationError::Ok);
    std::string tools;
    serializeJson(doc["tools"], tools);
    m.toolsWire = tools.size();
    // The system blocks and the turn's own input message (the last), as the wire carries them.
    const bool anth = doc["system"].is<JsonArrayConst>();
    if (anth)
      for (JsonObjectConst b : doc["system"].as<JsonArrayConst>()) m.prompt += std::string(b["text"] | "").size();
    JsonArrayConst msgs = anth ? doc["messages"].as<JsonArrayConst>() : doc["input"].as<JsonArrayConst>();
    for (size_t i = 0; i < msgs.size(); i++) {
      size_t bytes = 0;
      JsonVariantConst c = msgs[i]["content"];
      if (c.is<const char*>()) bytes = std::string(c.as<const char*>()).size();
      std::string first;
      for (JsonObjectConst part : c.as<JsonArrayConst>()) {
        const std::string t = part["text"] | "";
        if (first.empty()) first = t;
        bytes += t.size();
      }
      const std::string role = msgs[i]["role"] | "";
      const bool ack = first.rfind("Noted - continuing", 0) == 0;
      if (i + 1 == msgs.size() || role == "developer" || role == "system") m.prompt += bytes;
      else if (first.find("CONVERSATION SUMMARY") != std::string::npos) m.summary += bytes;
      else if (!ack) {
        std::string one;
        serializeJson(msgs[i], one);
        m.histWire += one.size() + 1;   // and its comma
      }
    }
  }

  Measured measure() {
    LogCapture::install();
    const size_t live = rows.size();
    eng->handleMessage(st["userText"] | "", "Owner", "1001");
    TEST_ASSERT_TRUE_MESSAGE(!wire.http.seen.empty(), head.name);
    Measured m;
    const std::string& body = wire.http.seen[0].body;
    m.body = body.size();
    m.cap = turnBudgetFor(head.host, head.model, true).wireBodyBytes;
    m.refused = LogCapture::contains(" B budget") || LogCapture::contains("refused the request for its size");
    m.leftOut = body.find("LEFT OUT TO FIT THIS REQUEST") != std::string::npos;
    for (size_t i = 0; i < live; i++)
      if (body.find(rows[i].text) != std::string::npos) {
        m.dialogue += rows[i].text.size();
        m.rows++;
      }
    countParts(body, m);
    return m;
  }
};

const Shape kShapes[] = {
    {"17nov-yesterday", 200, false, false},
    {"17nov-today", 200, true, false},
    {"twin-busy-yesterday", 200, false, true},
};

std::string row(const Head& h, const Shape& s, const Measured& m) {
  const ContextBudget c = headContextBudget(h.host, h.model, true, {});
  char line[640];
  std::snprintf(line, sizeof line,
                "head=%s shape=%s cap=%zu body=%zu prompt=%zu tools_wire=%zu summary=%zu room=%ld hist_max=%zu "
                "trigger=%zu floor=%d dialogue=%zu rows=%zu hist_wire=%zu fixed=%zu refused=%d left_out=%d\n",
                h.name, s.name, m.cap, m.body, m.prompt, m.toolsWire, m.summary,
                (long)m.cap - (long)(m.body - m.dialogue), c.historyMaxBytes,
                foldTriggerBytes(c, kCompactAtKbDefault), c.floorBranch ? 1 : 0, m.dialogue, m.rows, m.histWire,
                m.body - m.histWire - m.summary, m.refused ? 1 : 0,
                m.leftOut ? 1 : 0);
  return line;
}

// Every request fits its head's body budget; a measured floor head is never refused at round 0
// on the twin (its overhead constants hold the real request); the constants are no more than
// 2 KB over what the build measures (so a shrinking request gives its room back to the
// conversation); and the routed Anthropic head carries at least 7,500 B of dialogue on every
// shape (v5.1.2: 2,979 B after a shift).
constexpr size_t kV512TodayDialogue = 10955;   // v5.1.2's floor branch, the 17nov-today shape

void test_a_floor_head_carries_its_measured_room() {
  setRoutedBudgetLiftForTest(0);   // the floor, whatever kRoutedBudgetLift says (the lift: its own test)
  size_t measuredHeads = 0;
  for (const Head& h : kHeads)
    for (const Shape& s : kShapes) {
      Rig r(h, s);
      const Measured m = r.measure();
      const std::string what = std::string(h.name) + " " + s.name;
      TEST_ASSERT_TRUE_MESSAGE(m.body <= m.cap, what.c_str());
      const ContextBudget c = headContextBudget(h.host, h.model, true, {});
      if (!c.floorMeasured) continue;
      measuredHeads++;
      TEST_ASSERT_FALSE_MESSAGE(m.refused || m.leftOut, what.c_str());
      const bool anthropic = std::string(h.model).find("claude") != std::string::npos;
      const size_t fixed = m.body - m.histWire - m.summary;
      const size_t constant = anthropic ? kFloorFixedAnthropicBytes : kFloorFixedOpenAIBytes;
      TEST_ASSERT_TRUE_MESSAGE(fixed <= constant, (what + " fixed " + std::to_string(fixed)).c_str());
      if (s.recall) TEST_ASSERT_TRUE_MESSAGE(fixed + 2048 >= constant, (what + " fixed " + std::to_string(fixed)).c_str());
      if (anthropic) TEST_ASSERT_TRUE_MESSAGE(m.dialogue >= 7500, (what + " " + std::to_string(m.dialogue)).c_str());
      // Ruling 81, audit blocker 10: never less of today's conversation than v5.1.2. v5.1.2's floor
      // branch carried 10,955 B of a same-day history on this shape (its day floor; measured on this
      // twin with the floor branch forced, evidence/baseline-floor-dialogue-5edefb72e-v512-floor.txt).
      if (s.today) TEST_ASSERT_TRUE_MESSAGE(m.dialogue >= kV512TodayDialogue, (what + " " + std::to_string(m.dialogue)).c_str());
    }
  TEST_ASSERT_TRUE(measuredHeads > 0);
  setRoutedBudgetLiftForTest(-1);
}

// The floor's overhead constant follows the request-ceiling gate's own golden (RB-Q3b,
// test/golden/e1_request_ceiling.txt: the twin's direct Anthropic turn): the Anthropic constant
// is that request less its history messages plus at most 2 KB (the coverage line a routed head's
// shifted history adds, the summary's framing). A lane that shrinks or grows the old engine's
// request re-blesses that golden and must set kFloorFixed*Bytes from this suite's measure.
void test_the_floor_overhead_follows_the_gate_golden() {
  std::string g;
  TEST_ASSERT_TRUE_MESSAGE(readFile("test/golden/e1_request_ceiling.txt", g), "e1_request_ceiling.txt");
  const size_t at = g.find("\nmeasure ");
  TEST_ASSERT_TRUE(at != std::string::npos);
  const auto field = [&g, at](const char* key) {
    const size_t k = g.find(std::string(" ") + key + "=", at);
    TEST_ASSERT_TRUE_MESSAGE(k != std::string::npos, key);
    return (size_t)std::strtoul(g.c_str() + k + std::strlen(key) + 2, nullptr, 10);
  };
  const size_t gate = field("body") - field("history");
  char what[160];
  std::snprintf(what, sizeof what, "kFloorFixedAnthropicBytes %zu against the gate golden's %zu", kFloorFixedAnthropicBytes,
                gate);
  TEST_ASSERT_TRUE_MESSAGE(kFloorFixedAnthropicBytes >= gate && kFloorFixedAnthropicBytes <= gate + 2048, what);
  // The OpenAI wire carries the same request with its own framing: never under the Anthropic one.
  TEST_ASSERT_TRUE(kFloorFixedOpenAIBytes >= kFloorFixedAnthropicBytes);
}

// Ruling 77: kRoutedBudgetLift is one line, and each state is tested on the real engine whatever
// that line says. Off: a routed head stays on the floor exactly as the compiled-off budget gives
// it. On: it gets its model's budget, carries at least 20,000 B of owner and assistant dialogue on
// the 17 Nov shape inside that budget, and every routed round still counts the wallet's second
// upload (a resend after a 402 clamp on a low balance) in its fit.
void test_the_routed_lift_in_both_states() {
  const Head& h = kHeads[4];   // cumulo/anthropic-sonnet
  TEST_ASSERT_EQUAL_STRING("cumulo", h.host);
  setRoutedBudgetLiftForTest(0);
  {
    Rig r(h, kShapes[0]);
    const Measured m = r.measure();
    const ContextBudget c = headContextBudget(h.host, h.model, true, {});
    const ContextBudget today =
        deriveRequestContext(turnBudgetFor(h.host, h.model, true, BudgetLink::Device, false), {});
    TEST_ASSERT_EQUAL_UINT32(kLegacyWireBodyBytes, m.cap);
    TEST_ASSERT_TRUE(m.body <= m.cap && !m.refused);
    TEST_ASSERT_EQUAL_UINT32(today.historyMaxBytes, c.historyMaxBytes);
    TEST_ASSERT_EQUAL_UINT32(today.historySoftBytes, c.historySoftBytes);
    TEST_ASSERT_EQUAL_UINT32(today.historyKeepBytes, c.historyKeepBytes);
    TEST_ASSERT_EQUAL_UINT32(today.foldTriggerBytes, c.foldTriggerBytes);
    TEST_ASSERT_TRUE(c.floorMeasured);
    TEST_ASSERT_TRUE_MESSAGE(m.dialogue < 20000, std::to_string(m.dialogue).c_str());
  }
  setRoutedBudgetLiftForTest(1);
  {
    Rig r(h, kShapes[0]);
    const Measured m = r.measure();
    const RequestBudget lifted = requestBudgetFor(h.host, h.model, true, BudgetLink::Device, true);
    TEST_ASSERT_TRUE(lifted.wireBodyBytes > kLegacyWireBodyBytes);
    TEST_ASSERT_EQUAL_UINT32(lifted.wireBodyBytes, m.cap);
    TEST_ASSERT_TRUE_MESSAGE(m.body <= m.cap && !m.refused && !m.leftOut, std::to_string(m.body).c_str());
    TEST_ASSERT_TRUE_MESSAGE(m.dialogue >= 20000, std::to_string(m.dialogue).c_str());
    providers::ProviderDeps pd;
    const RouterRoute rr = resolveRouterRoute(h.model);
    const providers::wire::RoundBudget rb = providers::wire::roundBudget(pd, rr.upstream, rr.model, true);
    TEST_ASSERT_EQUAL_UINT32(lifted.wireBodyBytes, rb.budget.wireBodyBytes);
    TEST_ASSERT_EQUAL(2, (int)providers::wire::roundLoad(rb, rb.budget.wireBodyBytes, 0, 0).uploads);
    char line[160];
    std::snprintf(line, sizeof line, "routed lift on: %s cap %zu B, body %zu B, dialogue %zu B in %zu rows", h.name,
                  m.cap, m.body, m.dialogue, m.rows);
    TEST_MESSAGE(line);
  }
  setRoutedBudgetLiftForTest(-1);
}

void test_floor_dialogue_table() {
  setRoutedBudgetLiftForTest(0);   // the floor, whatever kRoutedBudgetLift says (the lift: its own test)
  std::string out =
      "# floor dialogue: generated by test/test_floor_dialogue (GOLDEN_UPDATE=1 rewrites it); do not edit.\n"
      "# One owner text turn of the synthetic twin with 200 corpus-sized exchanges after the summary, per head,\n"
      "# through the real TurnEngine and adapters. cap = the head's body budget; prompt = system blocks plus the\n"
      "# turn's input message; summary = the fold summary's messages; dialogue = history rows' own text carried;\n"
      "# room = cap - (body - dialogue), what the request leaves for dialogue around its fixed parts.\n";
  for (const Head& h : kHeads)
    for (const Shape& s : kShapes) {
      Rig r(h, s);
      out += row(h, s, r.measure());
    }
  // The 20,000 B the plan asked a floor head to carry, against the floor, on the busy twin's
  // routed Anthropic request: its own fixed part, the summary, and 20,000 B of dialogue at the
  // wire's own bytes per dialogue byte on this shape.
  {
    Rig r(kHeads[4], kShapes[2]);
    const Measured m = r.measure();
    const size_t fixed = m.body - m.histWire - m.summary;
    const size_t need = fixed + m.summary + (size_t)(20000ull * m.histWire / m.dialogue);
    char line[256];
    std::snprintf(line, sizeof line,
                  "plan20000 head=%s fixed=%zu summary=%zu wire_per_dialogue_x100=%zu body_needed=%zu over_floor=%ld\n",
                  kHeads[4].name, fixed, m.summary, (size_t)(100ull * m.histWire / m.dialogue), need,
                  (long)need - (long)kLegacyWireBodyBytes);
    out += line;
  }
  TEST_MESSAGE(out.c_str());
  const char* bless = std::getenv("GOLDEN_UPDATE");
  if (bless && std::string(bless) == "1") {
    writeFile(kGolden, out);
    return;
  }
  std::string blessed;
  const bool found = readFile(kGolden, blessed);
  if (found && blessed == out) return;
  writeFile("test/golden/out/floor_dialogue.txt", out);
  TEST_FAIL_MESSAGE(found ? "the build's requests differ from test/golden/floor_dialogue.txt (GOLDEN_UPDATE=1)"
                          : "missing golden test/golden/floor_dialogue.txt");
  setRoutedBudgetLiftForTest(-1);
}


// RB-BGb (c), RB-BG's own note: on a busy day the day floor keeps today whole up to its 16,384 B
// cap (about 4,100 tokens), which on a routed head at the 85,000 B floor can be more than the
// request's measured room, so round 0 goes over the floor into the refit. Measured on the busy
// twin, every day size from 10 to 60 exchanges today, recall on and off, the three routed heads
// (the count is printed; evidence/c-busy-day-measure.log). Every such round is refit to fit,
// never refused to the owner; nothing of the turn's own is there at round 0, so the refit gives
// up the context in the protocol's order, recalled memories before any of today's turns; and
// what it gave up is named in the request that went (the LEFT OUT note, rows and the call to
// read them back). The day floor itself is not shortened: the refit keeps every turn that fits,
// a smaller static cap would carry less (owner, 19 Nov: never less history by default).
void test_a_busy_day_over_the_floor_is_refit_and_named() {
  setRoutedBudgetLiftForTest(0);
  int cases = 0, over = 0;
  std::string which;
  for (int hi = 4; hi < 7; hi++)
    for (int turns = 10; turns <= 60; turns += 2)
      for (int recall = 0; recall < 2; recall++) {
        const Shape s{"busy-today", turns, true, recall == 1};
        Rig r(kHeads[hi], s);
        const Measured m = r.measure();
        cases++;
        const std::string what = std::string(kHeads[hi].name) + " turns=" + std::to_string(turns) +
                                 " recall=" + std::to_string(recall);
        TEST_ASSERT_TRUE_MESSAGE(m.body <= kLegacyWireBodyBytes, what.c_str());
        TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("nothing left to fold"), what.c_str());
        if (!LogCapture::contains("over the 85000 B budget")) continue;
        over++;
        which += (which.empty() ? "" : "; ") + what;
        const std::string& sent = r.wire.http.seen.back().body;
        TEST_ASSERT_TRUE_MESSAGE(m.leftOut, (what + ": the refit names what it left out").c_str());
        const bool today = sent.find("of today's earlier conversation message(s)") != std::string::npos;
        if (today) TEST_ASSERT_TRUE_MESSAGE(sent.find("memory_episodic") != std::string::npos, what.c_str());
        if (today && recall)
          TEST_ASSERT_TRUE_MESSAGE(sent.find("recalled memory item(s)") != std::string::npos,
                                   (what + ": recall goes before today's turns").c_str());
      }
  char line[1536];
  std::snprintf(line, sizeof line, "busy day on routed heads: %d of %d round-0 requests over the floor, refit (%s)", over,
                cases, which.c_str());
  TEST_MESSAGE(line);
  TEST_ASSERT_TRUE_MESSAGE(over > 0, "the sweep reaches the band over the floor");
  setRoutedBudgetLiftForTest(-1);
}


// RB-BGb part 2 (ruling 93 item 3): the same busy day with three tool rounds of about 1,500
// tokens (6,000 B) each before the answer, on the three routed heads at the floor. A later
// round that does not fit gives up the turn's own bulk first (the replayed rounds, then the
// newest round's results cut to a part); the prompt's context (recall, older days, today's
// turns last) goes only in a refusal where the turn's bulk had nothing more to give. Counted
// and printed (evidence/p2-busy-day-later-rounds.log).
void test_a_busy_day_later_round_gives_up_the_turns_bulk_first() {
  setRoutedBudgetLiftForTest(0);
  int runs = 0, laterOver = 0, inTurnOnly = 0, withContext = 0, todayCut = 0;
  for (int hi = 4; hi < 7; hi++)
    for (int turns = 10; turns <= 60; turns += 2)
      for (int recall = 0; recall < 2; recall++) {
        const Shape s{"busy-today", turns, true, recall == 1};
        Rig r(kHeads[hi], s, 3);
        LogCapture::install();
        r.eng->handleMessage(r.st["userText"] | "", "Owner", "1001");
        runs++;
        const std::string what = std::string(kHeads[hi].name) + " turns=" + std::to_string(turns) +
                                 " recall=" + std::to_string(recall);
        for (const auto& sent : r.wire.http.seen)
          TEST_ASSERT_TRUE_MESSAGE(sent.body.size() <= kLegacyWireBodyBytes, what.c_str());
        TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("nothing left to fold"), what.c_str());
        bool over = false, context = false;
        for (const auto& l : LogCapture::lines()) {
          if (l.rfind("headloop: round ", 0) != 0 || l.find(" B over the ") == std::string::npos) continue;
          if (l.rfind("headloop: round 0 ", 0) == 0) continue;
          over = true;
          const bool bulk = l.find("the replayed tool rounds") != std::string::npos ||
                            l.find("the newest round's results") != std::string::npos;
          const bool ctx = l.find("the prompt's context") != std::string::npos ||
                           l.find("the prompt and input") != std::string::npos;
          context = context || ctx;
          if (ctx && bulk)
            TEST_ASSERT_TRUE_MESSAGE(l.find("the prompt") > std::min(l.find("the replayed tool rounds"), l.find("the newest round's results")),
                                     (what + ": " + l).c_str());
          // A refusal that gave up context gave up the turn's bulk first, in that refusal or an
          // earlier one of the same round.
          if (ctx && !bulk) {
            const std::string round = l.substr(0, l.find(" body "));
            bool earlier = false;
            for (const auto& e : LogCapture::lines()) {
              if (&e == &l) break;
              earlier = earlier || (e.rfind(round + " ", 0) == 0 && e.find("tool rounds") != std::string::npos) ||
                        (e.rfind(round + " ", 0) == 0 && e.find("newest round's results") != std::string::npos);
            }
            TEST_ASSERT_TRUE_MESSAGE(earlier || r.wire.http.seen.back().body.find("[cut to fit this request") !=
                                                    std::string::npos,
                                     (what + ": " + l).c_str());
          }
        }
        if (!over) continue;
        laterOver++;
        (context ? withContext : inTurnOnly)++;
        const bool cut = r.wire.http.seen.back().body.find("of today's earlier conversation message(s)") != std::string::npos;
        todayCut += cut;
      }
  char line[256];
  std::snprintf(line, sizeof line,
                "busy day, 3 tool rounds, routed heads: %d of %d turns had a later round over the floor; %d gave up "
                "only the turn's own bulk, %d also context; %d answers left today's turns out",
                laterOver, runs, inTurnOnly, withContext, todayCut);
  TEST_MESSAGE(line);
  setRoutedBudgetLiftForTest(-1);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_floor_overhead_follows_the_gate_golden);
  RUN_TEST(test_a_floor_head_carries_its_measured_room);
  RUN_TEST(test_the_routed_lift_in_both_states);
  RUN_TEST(test_floor_dialogue_table);
  RUN_TEST(test_a_busy_day_over_the_floor_is_refit_and_named);
  RUN_TEST(test_a_busy_day_later_round_gives_up_the_turns_bulk_first);
  return UNITY_END();
}
