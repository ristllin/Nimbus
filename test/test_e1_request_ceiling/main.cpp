#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/skill_store.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_groups.h"   // toolClassRow: each tool's group in the golden
#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"

// RB-Q3b: the old engine's request on an owner text turn, measured on the real build.
//
// The synthetic twin of a busy owner device (test/support/fixtures/lumi_state_prompt: the
// counts, sizes and classes of the owner's device with invented content) goes through the
// real TurnEngine, and the request it makes goes through the real Anthropic adapter
// (providers::orchTurnAnthropic) onto a fake transport. The tool registrations are the
// device's (device_tools.json, checked against the source by check_prompt_ceiling.py).
// The figures land in test/golden/e1_request_ceiling.txt; this suite fails when the build
// composes anything else (GOLDEN_UPDATE=1 rewrites it), and check_prompt_ceiling.py holds
// the figures under their ceilings.

using namespace nimbus::orch;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;

void setUp() {}
void tearDown() {}

namespace {

const char kFixtureDir[] = "test/support/fixtures/lumi_state_prompt/";
const char kGolden[] = "test/golden/e1_request_ceiling.txt";

constexpr uint64_t kNow = 1795113600ull + 18 * 3600 + 40 * 60;   // Thu 19 Nov 2026 18:40 UTC

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

// The device's tool registrations, as an owner turn's registry lists them.
std::vector<ToolRegistry::Spec> deviceSpecs() {
  const JsonDocument d = loadJson("device_tools.json");
  std::vector<ToolRegistry::Spec> out;
  for (JsonObjectConst t : d["tools"].as<JsonArrayConst>())
    out.push_back({t["name"] | "", t["description"] | "", t["schema"] | ""});
  return out;
}

// An in-memory SkillFs: the owner's stored skills of the fixture.
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

// What one owner text turn sends, through the real engine and the real Anthropic adapter.
struct Measured {
  std::string body;
  size_t system = 0, history = 0, input = 0;
  std::vector<std::string> toolNames;   // wire names, in order
  std::vector<size_t> toolRaw;          // name + description + schema
  size_t toolsWire = 0;                 // the tools array as serialized
};

struct Rig {
  JsonDocument st = loadJson("state.json");
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  InMemoryEpisodicStore store;
  Scratchpad pad;
  MemFs fs;
  std::unique_ptr<agent::SkillStore> skills;
  FixtureEffects effects;
  std::vector<ToolRegistry::Spec> specs = deviceSpecs();
  std::unique_ptr<TurnEngine> eng;
  int seq = 0;
  int budget = 0;   // MemConfig::maxContextBytes as the device passes it (0 = derived from the head)

  std::string addRow(uint32_t hour, const char* role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    std::snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "1001";
    m.tsHours = hour;
    m.role = role;
    m.kind = MsgKind::Message;
    m.text = text;
    m.tags = tags;
    store.addMessage(m);
    return m.id;
  }

  // `host` "" = the twin's head (Anthropic); "openai" renders the same turn on the OpenAI adapter.
  explicit Rig(const std::string& host = std::string()) {
    plat.epoch = kNow;
    cfg.devName = st["devName"] | "";
    cfg.toolLoop = true;
    const bool oai = host == "openai";
    cfg.orchHost = oai ? host : std::string(st["head"]["host"] | "");
    cfg.priority = oai ? "openai,anthropic,mistral" : "anthropic,openai,mistral";
    cfg.orchModels[cfg.orchHost] = oai ? std::string("gpt-5.6-luna") : std::string(st["head"]["model"] | "");
    if (oai)
      wire.http.script.push_back({"api.openai.com", "/v1/responses", 200,
                                  "{\"id\":\"r2\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
                                  "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"ok\\\"}\"}]}"});
    else
      wire.http.script.push_back({"api.anthropic.com", "/v1/messages", 200,
                                  "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
                                  "\"input\":{\"reply\":\"ok\"}}],\"stop_reason\":\"tool_use\"}"});
    pad.setActiveTask(st["scratchpad"]["active"] | "");
    for (JsonVariantConst v : st["scratchpad"]["short"].as<JsonArrayConst>())
      pad.add(Tier::Short, v.as<const char*>());
    // The owner's stored skills: the fixture's ids, descriptions and body sizes.
    for (JsonObjectConst s : st["skills"].as<JsonArrayConst>()) {
      const std::string id = s["id"] | "";
      fs.files["/mem/skills/" + id + "/SKILL.md"] =
          "---\ntitle: " + std::string(s["title"] | "") + "\ndesc: " + std::string(s["desc"] | "") +
          "\napproved: true\ncreated_by: user\nrev: 1\n---\n" + std::string((size_t)(s["bodyBytes"] | 0), 'b');
    }
    skills.reset(new agent::SkillStore(fs, agent::SkillStoreConfig{}, [] { return std::string("2026-11-19 18:40"); }));
    // History: the recent window as real rows, the fold's summary as the device frames it.
    uint32_t hour = (uint32_t)(kNow / 3600) - 2;
    std::string lastUser;
    for (JsonObjectConst h : st["history"].as<JsonArrayConst>()) {
      if (h["user"] | false) lastUser = addRow(hour, "user", h["text"] | "", "from:Owner");
      else addRow(hour, "assistant", h["text"] | "", "fx:none,out:reply,turn:" + lastUser);
    }
    for (JsonObjectConst a : st["recentActions"].as<JsonArrayConst>())
      effects.all.push_back({(int64_t)kNow - 60 * (int64_t)(a["minutesAgo"] | 0), a["tool"] | "", a["receipt"] | ""});

    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
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
      in.budgetBytes = budget;
      return in;
    };
    d.toolSpecs = [this](const Principal&) { return specs; };
    d.recall = [this](const std::string&, const Principal&) {
      RecallOut r;
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
      return addRow((uint32_t)(kNow / 3600), "user", t, tags);
    };
    d.hosts.add(cfg.orchHost, [this](std::string& conv, const std::string& ins, const std::string& inp,
                                     std::string& out, std::string& err, const agent::HeadTools* tools,
                                     TokenUsage* usage) {
      agent::providers::ProviderDeps pd = wire.contract();
      const std::string model = cfg.orchModels[cfg.orchHost];
      pd.orchModel = [model](const char*) { return model; };
      if (cfg.orchHost == "openai") return agent::providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, usage);
      return agent::providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, usage);
    });
    eng.reset(new TurnEngine(std::move(d)));
  }

  Measured measure() {
    eng->handleMessage(st["userText"] | "", "Owner", "1001");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)wire.http.seen.size(), "one request");
    Measured m;
    m.body = wire.http.seen[0].body;
    JsonDocument doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, m.body, DeserializationOption::NestingLimit(32)) ==
                     DeserializationError::Ok);
    for (JsonObjectConst b : doc["system"].as<JsonArrayConst>()) m.system += std::string(b["text"] | "").size();
    JsonArrayConst msgs = doc["messages"].as<JsonArrayConst>();
    for (size_t i = 0; i < msgs.size(); i++) {
      size_t bytes = 0;
      JsonVariantConst c = msgs[i]["content"];
      if (c.is<const char*>()) bytes = std::string(c.as<const char*>()).size();
      for (JsonObjectConst part : c.as<JsonArrayConst>()) bytes += std::string(part["text"] | "").size();
      (i + 1 == msgs.size() ? m.input : m.history) += bytes;
    }
    std::string tools;
    serializeJson(doc["tools"], tools);
    m.toolsWire = tools.size();
    for (JsonObjectConst t : doc["tools"].as<JsonArrayConst>()) {
      std::string schema;
      serializeJson(t["input_schema"], schema);
      m.toolNames.push_back(t["name"] | "");
      m.toolRaw.push_back(std::string(t["name"] | "").size() + std::string(t["description"] | "").size() +
                          schema.size());
    }
    return m;
  }
};

// The registry name a wire name stands for (the device's registrations).
std::string registryName(const std::vector<ToolRegistry::Spec>& specs, const std::string& wireName) {
  for (const auto& s : specs)
    if (agent::loopToolName(s.name) == wireName) return s.name;
  return wireName;
}

std::string groupOf(const std::string& regName) {
  const ToolClassRow* row = toolClassRow(regName);
  if (!row) return regName == "orch_turn" ? "final" : "unclassed";
  return row->group[0] ? row->group : "always";
}

std::string render(const Measured& m, const Measured& demand, const std::vector<ToolRegistry::Spec>& specs) {
  std::string out =
      "# e1 request ceiling: generated by test/test_e1_request_ceiling (GOLDEN_UPDATE=1 rewrites it); do not edit.\n"
      "# One owner text turn of the synthetic twin (test/support/fixtures/lumi_state_prompt) through the real\n"
      "# TurnEngine and the real Anthropic adapter. Gate: tools/release_gate/check_prompt_ceiling.py.\n"
      "# prompt = the system prompt the device composes: the system blocks plus the per-turn input message (the\n"
      "# conversation history rides apart: history); text = prompt + history. tools: name +\n"
      "# description + schema of every registry tool in tools[] (the final-answer tool orch_turn is the reply\n"
      "# contract, counted in tools_raw); tools_wire = the tools array as serialized. prompt_demand = the prompt\n"
      "# when the owner sets the largest context budget (56,000 B): nothing is clipped, so text added to the\n"
      "# budget-bound static block shows here even where the head's budget hides it in prompt.\n";
  size_t raw = 0, regRaw = 0;
  std::map<std::string, std::pair<int, size_t>> groups;
  std::set<std::string> sent;
  for (size_t i = 0; i < m.toolNames.size(); i++) {
    raw += m.toolRaw[i];
    const std::string reg = registryName(specs, m.toolNames[i]);
    sent.insert(reg);
    if (reg != "orch_turn") regRaw += m.toolRaw[i];
    auto& g = groups[groupOf(reg)];
    g.first++;
    g.second += m.toolRaw[i];
  }
  char line[512];
  std::snprintf(line, sizeof line,
                "measure turn=owner_text prompt=%zu system=%zu input=%zu history=%zu text=%zu tools=%zu "
                "registry_tools=%zu tools_raw=%zu tools_wire=%zu body=%zu prompt_demand=%zu\n",
                m.system + m.input, m.system, m.input, m.history, m.system + m.input + m.history, regRaw,
                m.toolNames.size() - 1, raw, m.toolsWire, m.body.size(), demand.system + demand.input);
  out += line;
  for (const auto& g : groups) {
    std::snprintf(line, sizeof line, "group name=%s tools=%d raw=%zu\n", g.first.c_str(), g.second.first,
                  g.second.second);
    out += line;
  }
  for (size_t i = 0; i < m.toolNames.size(); i++) {
    const std::string reg = registryName(specs, m.toolNames[i]);
    std::snprintf(line, sizeof line, "tool name=%s group=%s raw=%zu sent=1\n", reg.c_str(), groupOf(reg).c_str(),
                  m.toolRaw[i]);
    out += line;
  }
  for (const auto& s : specs) {
    if (sent.count(s.name)) continue;
    std::snprintf(line, sizeof line, "tool name=%s group=%s raw=%zu sent=0\n", s.name.c_str(),
                  groupOf(s.name).c_str(), s.name.size() + s.description.size() + s.schemaJson.size());
    out += line;
  }
  return out;
}

void test_the_owner_text_turn_matches_its_golden() {
  Rig r;
  const Measured m = r.measure();
  Rig wide;
  wide.budget = 56000;   // MemConfig::kContextMax: the largest budget an owner can set
  const Measured demand = wide.measure();
  const std::string current = render(m, demand, r.specs);
  if (std::getenv("E1_DUMP")) {   // for a breakdown
    writeFile("test/golden/out/e1_request_body.json", m.body);
    writeFile("test/golden/out/e1_request_body_wide.json", demand.body);
  }
  TEST_MESSAGE(current.substr(0, current.find("\ngroup")).c_str());
  const char* bless = std::getenv("GOLDEN_UPDATE");
  if (bless && std::string(bless) == "1") {
    writeFile(kGolden, current);
    return;
  }
  std::string blessed;
  const bool found = readFile(kGolden, blessed);
  if (found && blessed == current) return;
  writeFile("test/golden/out/e1_request_ceiling.txt", current);
  TEST_FAIL_MESSAGE(found ? "the build's request differs from test/golden/e1_request_ceiling.txt "
                            "(GOLDEN_UPDATE=1 rewrites it; check_prompt_ceiling.py then holds the figures)"
                          : "missing golden test/golden/e1_request_ceiling.txt");
}

// The rig is the device's shape, not an empty request: every fixture class reaches the request.
void test_every_fixture_class_reaches_the_request() {
  Rig r;
  const Measured m = r.measure();
  const auto has = [&m](const char* part) { return m.body.find(part) != std::string::npos; };
  TEST_ASSERT_TRUE(has("[OWNER PREFERENCES"));
  TEST_ASSERT_TRUE(has("[STANDING RULES"));
  TEST_ASSERT_TRUE(has("trip-planner"));
  TEST_ASSERT_TRUE(has("[RUNNING MEMORY]"));
  TEST_ASSERT_TRUE(has("[RECENT ACTIONS"));
  TEST_ASSERT_TRUE(has("CONVERSATION SUMMARY"));
  TEST_ASSERT_TRUE(has("Owner fact 9"));
  TEST_ASSERT_TRUE(has("Notion"));
  TEST_ASSERT_TRUE(m.history > 4000);
  TEST_ASSERT_TRUE(m.toolNames.size() > 20);
}

// E1_DUMP only: the same turn on the OpenAI adapter, for the live first-call runs (not pinned).
void test_the_openai_body_for_the_live_runs() {
  if (!std::getenv("E1_DUMP")) return;
  Rig r("openai");
  r.eng->handleMessage(r.st["userText"] | "", "Owner", "1001");
  TEST_ASSERT_EQUAL_INT(1, (int)r.wire.http.seen.size());
  writeFile("test/golden/out/e1_request_body_openai.json", r.wire.http.seen[0].body);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_owner_text_turn_matches_its_golden);
  RUN_TEST(test_every_fixture_class_reaches_the_request);
  RUN_TEST(test_the_openai_body_for_the_live_runs);
  return UNITY_END();
}
