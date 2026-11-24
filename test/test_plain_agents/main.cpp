// test_plain_agents - sub-agents on engine 2 (lane E2-P-SUB, plain/agents.h) over the kept job
// fabric: the device's one-chat binding (chat_binding.h) with a scripted Claude head on the real
// Anthropic wire and a fake card, and the real JobEngine over a fake adapter and a journal whose
// store outlives a restart. Pins:
//   - agent_start inside a turn starts the job on the binding's backend (its id and provider job
//     id in the result), with the task and never the conversation in the brief;
//   - the report is saved whole as agents/<id>-<name>.md, arrives as its own turn (one agent device
//     note, stored and rendered as outside content) and the head's answer reaches the chat; the
//     job's spend is counted once and the report turn's usage is handed over;
//   - agent_list names the chat's jobs; agent_stop stops one; a stranger's id is not stopped;
//   - a report that arrives while a turn runs is kept and runs after it, never dropped;
//   - a job open at a restart is polled again by the new engine and its report arrives then.
#include <unity.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_fabric.h"
#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/plain/agents.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/journal.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC
constexpr char kChat[] = "1001";

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A card that keeps files and folders.
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool present() override { return true; }
  bool mayWrite() override { return true; }
  uint32_t largestInternal() override { return 64 * 1024; }
  uint32_t nowMs() override { return 0; }
  void lock() override {}
  void unlock() override {}
  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool isDir(const std::string& p) override { return dirs.count(p) > 0; }
  bool mkdir(const std::string& p) override { return dirs.insert(p).second; }
  size_t append(const std::string& p, const char* d, size_t n) override {
    files[p].append(d, n);
    return n;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    got = 0;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off < it->second.size()) got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& a, const std::string& b) override {
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (!dirs.count(dir)) return false;
    const std::string pre = dir == "/" ? std::string("/") : dir + "/";
    auto child = [&](const std::string& p) {
      if (p.size() > pre.size() && p.rfind(pre, 0) == 0 && p.find('/', pre.size()) == std::string::npos)
        names.push_back(p.substr(pre.size()));
    };
    for (const auto& kv : files) child(kv.first);
    for (const std::string& d : dirs) child(d);
    return true;
  }
};

// The journal's store (the device's flash file): kept across a restart.
struct FakeStore : orch::JournalStore {
  std::map<int, std::string> slots;
  std::string get(int slot) override {
    auto it = slots.find(slot);
    return it == slots.end() ? std::string() : it->second;
  }
  bool put(int slot, const std::string& v) override {
    slots[slot] = v;
    return true;
  }
  bool remove(int slot) override {
    slots.erase(slot);
    return true;
  }
  void clearNs() override { slots.clear(); }
};

// What lasts across a restart: the card, the journal's store, the provider's side.
struct Kept {
  Card card;
  FakeStore store;
  harness_test::FakeAdapter adapter;
  std::map<std::string, std::string> saved;   // the reports the file store kept (path -> text)
};

// One boot of the device: platform, head, binding, registry, journal and job engine, wired the way
// orchestrator_plain.cpp wires them.
struct Device {
  explicit Device(Kept& k) : kept(k), files(k.card) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    kept.adapter.backend = "anthropic";
    fabric.registerAdapter(&kept.adapter);
    journal.begin(&kept.store);
    agent::JobEngine::Deps j;
    j.platform = platform;
    j.fabric = &fabric;
    j.journal = &journal;
    j.principalFor = [](const std::string& c) { return orch::principalForRole(c, orch::Role::Admin); };
    j.deliver = [this](const std::string& c, const std::string& t) { delivered.push_back(c + "|" + t); };
    j.subPriority = [] { return std::string("anthropic"); };
    j.providerHasKey = [](const std::string& p) { return p == "anthropic"; };
    j.subModel = [](const std::string&) { return std::string("claude-haiku-5-5"); };
    j.modelIsValid = [](const std::string&, const std::string& m) { return m == "claude-haiku-5-5"; };
    j.recordSpawnUsage = [this](const std::string& b, const std::string& m, const orch::TokenUsage& u) {
      spawnSpend.push_back(b + "/" + m + " in=" + std::to_string(u.promptTokens) + " out=" + std::to_string(u.completionTokens));
    };
    j.persistResult = [this](const std::string& prj, const std::string& name, const std::string& tag,
                             const std::string& text, const std::string&) {
      std::string path;
      if (prj != plain::kAgentsProject || !plain::agentReportPath(tag, name, path).ok()) return std::string();
      kept.saved[path] = text;
      return "[saved: " + path + "]";
    };
    j.turnInFlight = [this] { return inTurn; };
    j.synthesize = [this](const std::string& chat) {
      if (inTurn) return;   // the device's gate: a report never boots a chat inside a turn
      int ran = 0;
      (void)plain::deliverReports(*jobs, chat, [this](const std::string&, const std::string& note) {
        report = plain::TurnOutcome{};
        return binding->reportTurn(note, report);
      }, ran);
      reportsRun += ran;
    };
    jobs.reset(new agent::JobEngine(std::move(j)));
    plain::AgentsDeps a;
    a.jobs = jobs.get();
    a.chat = [this] { return inTurn ? std::string(kChat) : std::string(); };
    a.dispatchNow = [this] { (void)jobs->pump(); };
    TEST_ASSERT_TRUE(plain::registerAgentTools(reg, a).ok());
  }

  plain::Status boot() {
    plain::ChatBindingConfig c;
    c.chatKey = kChat;
    c.model = "claude-haiku-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-binding"); };
    d.usage = [this](const plain::TurnOutcome&) { ++usages; };
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }

  plain::Status say(const std::string& text) {
    plain::ChatMessage m;
    m.text = text;
    out = plain::TurnOutcome{};
    inTurn = true;
    const plain::Status st = binding->turn(m, out);
    inTurn = false;
    return st;
  }

  // Pumps with the clock moving, as the device's poll loop does between turns.
  void pumps(int n, uint32_t stepMs = 20000) {
    for (int i = 0; i < n; ++i) {
      ms += stepMs;
      (void)jobs->pump();
    }
  }

  // A tool called by the model outside the scripted head (inside a turn of this chat).
  orch::ToolResult call(const std::string& name, const std::string& argsJson) {
    ArduinoJson::JsonDocument args;
    (void)deserializeJson(args, argsJson);
    inTurn = true;
    const orch::ToolResult r = reg.dispatch(name, args.as<ArduinoJson::JsonObjectConst>(),
                                            orch::principalForRole(kChat, orch::Role::Admin));
    inTurn = false;
    return r;
  }

  std::string firstTag() const {
    orch::JobRecord r;
    return journal.count() > 0 && journal.get(0, r) ? std::string(r.tag) : std::string();
  }

  Kept& kept;
  uint32_t ms = 1000;
  agent::Platform platform;
  sp::ScriptedProvider model;
  tx::CardTxFiles files;
  orch::ToolRegistry reg;
  agent::HeavyFabric fabric;
  orch::Journal journal;
  std::unique_ptr<agent::JobEngine> jobs;
  std::unique_ptr<plain::ChatBinding> binding;
  bool inTurn = false;
  std::vector<std::string> sent, delivered, spawnSpend;
  int usages = 0, reportsRun = 0;
  plain::TurnOutcome out, report;
};

// Every byte of the chat's stored transcript.
std::string transcript(const Card& card) {
  std::string all;
  for (const auto& kv : card.files)
    if (kv.first.rfind(std::string("/mem/tx/") + kChat + "/", 0) == 0) all += kv.second;
  return all;
}

}  // namespace

void setUp() {}
void tearDown() {}

static void test_the_report_path_is_agents_id_name() {
  std::string p;
  TEST_ASSERT_TRUE(plain::agentReportPath("job0007", "Trip prices!", p).ok());
  TEST_ASSERT_EQUAL_STRING("agents/job0007-trip-prices.md", p.c_str());
  TEST_ASSERT_TRUE(plain::agentReportPath("job0007", "", p).ok());
  TEST_ASSERT_EQUAL_STRING("agents/job0007-report.md", p.c_str());
  TEST_ASSERT_FALSE(plain::agentReportPath("", "x", p).ok());
  TEST_ASSERT_FALSE(plain::agentReportPath("../a", "x", p).ok());
}

// D1 and D3: start inside the turn, the report as its own stored and rendered turn, spend counted.
static void test_a_started_job_reports_back_as_its_own_turn() {
  Kept kept;
  kept.adapter.pollScript = {agent::JobState::Running, agent::JobState::Done};
  kept.adapter.doneReply = "Fares: London to York 42 GBP.";
  kept.adapter.usageIn = 1200;
  kept.adapter.usageOut = 300;
  kept.adapter.toolRuns = kept.adapter.webRuns = 2;   // it looked the fares up
  Device dev(kept);
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::call("agent_start", R"({"name":"trip prices","task":"find the train fares to York"})"));
  dev.model.push(sp::say("I started a job for the fares."));
  TEST_ASSERT_TRUE(dev.say("my secret plan: look up the fares").ok());
  const std::string tag = dev.firstTag();
  TEST_ASSERT_EQUAL_STRING("job0000", tag.c_str());
  TEST_ASSERT_EQUAL(2, (int)dev.model.requests().size());
  TEST_ASSERT_TRUE_MESSAGE(has(dev.model.last().body, "started id=job0000 job=anthropic:job-1"), dev.model.last().body.c_str());
  TEST_ASSERT_EQUAL(1, (int)kept.adapter.dispatched.size());
  TEST_ASSERT_TRUE(has(kept.adapter.dispatched[0].task, "find the train fares to York"));
  TEST_ASSERT_FALSE(has(kept.adapter.dispatched[0].instruction, "secret plan"));   // never the transcript
  TEST_ASSERT_TRUE(dev.delivered.empty());   // quiet: no "On it." beside the model's own reply

  dev.model.push(sp::say("The fare to York is 42 GBP."));
  dev.pumps(4);
  TEST_ASSERT_EQUAL(1, dev.reportsRun);
  TEST_ASSERT_EQUAL(1, (int)kept.saved.count("agents/job0000-trip-prices.md"));
  TEST_ASSERT_EQUAL_STRING("Fares: London to York 42 GBP.", kept.saved["agents/job0000-trip-prices.md"].c_str());
  const std::string body = dev.model.last().body;
  TEST_ASSERT_TRUE_MESSAGE(has(body, "[agent report] trip prices id=job0000 saved whole as agents/job0000-trip-prices.md"),
                           body.c_str());
  TEST_ASSERT_TRUE(has(body, "Fares: London to York 42 GBP."));
  TEST_ASSERT_TRUE(dev.report.delivered);
  TEST_ASSERT_EQUAL_STRING("1001|The fare to York is 42 GBP.", dev.sent.back().c_str());
  TEST_ASSERT_TRUE(has(transcript(kept.card), "[agent report] trip prices"));   // stored in the conversation
  TEST_ASSERT_EQUAL(1, (int)dev.spawnSpend.size());
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-5-5 in=1200 out=300", dev.spawnSpend[0].c_str());
  TEST_ASSERT_EQUAL(2, dev.usages);   // the person's turn and the report turn
  dev.pumps(3);
  TEST_ASSERT_EQUAL(1, dev.reportsRun);   // once
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(dev.model.verify(why), why.c_str());
}

// D1 and D3: list and stop; a job of another chat is not stopped from here.
static void test_list_and_stop() {
  Kept kept;
  kept.adapter.pollScript = {agent::JobState::Running};
  Device dev(kept);
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_EQUAL_STRING(plain::texts::tool_results::kAgentNone, dev.call("agent_list", "{}").output.c_str());
  const orch::ToolResult started = dev.call("agent_start", R"({"name":"scan","task":"scan the news"})");
  TEST_ASSERT_TRUE_MESSAGE(started.success, started.error.c_str());
  const orch::ToolResult listed = dev.call("agent_list", "{}");
  TEST_ASSERT_TRUE(listed.success);
  TEST_ASSERT_TRUE_MESSAGE(has(listed.output, "scan (job0000, anthropic/claude-haiku-5-5): running"), listed.output.c_str());
  TEST_ASSERT_FALSE(has(listed.output, "[SPAWN LEDGER]"));
  TEST_ASSERT_FALSE(dev.call("agent_stop", R"({"id":"job0099"})").success);
  TEST_ASSERT_FALSE(dev.call("agent_start", R"({"name":"x","task":"y","priority":"high"})").success);   // unknown argument
  const orch::ToolResult stopped = dev.call("agent_stop", R"({"id":"job0000"})");
  TEST_ASSERT_TRUE_MESSAGE(stopped.success, stopped.error.c_str());
  TEST_ASSERT_EQUAL_STRING("stopping id=job0000", stopped.output.c_str());
  TEST_ASSERT_EQUAL(1, kept.adapter.cancels);
  TEST_ASSERT_FALSE(dev.call("agent_start", "{}").success);
}

// D2: a report that lands while a turn runs is kept, then runs after it; a turn that cannot start
// puts it back.
static void test_a_report_during_a_turn_is_kept_not_lost() {
  Kept kept;
  kept.adapter.pollScript = {agent::JobState::Done};
  kept.adapter.doneReply = "done: 3 items";
  kept.adapter.toolRuns = 1;
  Device dev(kept);
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.call("agent_start", R"({"name":"count","task":"count the items"})").success);
  dev.inTurn = true;   // a person's turn is running (the agent tools pump inside it)
  dev.pumps(5, 1000);
  TEST_ASSERT_EQUAL(0, dev.reportsRun);
  TEST_ASSERT_EQUAL(1, (int)dev.jobs->peekFreshFor(dev.jobs->freshScopeFor(kChat)).size());
  int ran = -1;
  const plain::Status busy = plain::deliverReports(*dev.jobs, kChat, [](const std::string&, const std::string&) {
    return plain::Status::fail(tx::Err::Busy, "test.busy");
  }, ran);
  TEST_ASSERT_TRUE(busy.err == tx::Err::Busy);
  TEST_ASSERT_EQUAL(0, ran);
  TEST_ASSERT_EQUAL(1, (int)dev.jobs->peekFreshFor(dev.jobs->freshScopeFor(kChat)).size());   // put back
  dev.inTurn = false;
  dev.model.push(sp::say("Three items."));
  dev.pumps(1, 1000);
  TEST_ASSERT_EQUAL(1, dev.reportsRun);
  TEST_ASSERT_TRUE(has(dev.model.last().body, "done: 3 items"));
  TEST_ASSERT_TRUE(dev.jobs->peekFreshFor(dev.jobs->freshScopeFor(kChat)).empty());
}

// D2 and D3: a job open at a restart is in the journal; the next boot lists it, polls it and its
// report arrives as a turn of the rebooted chat.
static void test_a_job_open_at_restart_reports_after_it() {
  Kept kept;
  kept.adapter.pollScript = {agent::JobState::Running};
  {
    Device before(kept);
    TEST_ASSERT_TRUE(before.boot().ok());
    before.model.push(sp::call("agent_start", R"({"name":"long read","task":"read the long report"})"));
    before.model.push(sp::say("Started."));
    TEST_ASSERT_TRUE(before.say("read it in the background").ok());
    before.pumps(2);
    TEST_ASSERT_EQUAL(1, before.journal.count());
  }
  kept.adapter.pollScript = {agent::JobState::Done};
  kept.adapter.pollCount = 0;
  kept.adapter.doneReply = "The long report says yes.";
  Device after(kept);
  TEST_ASSERT_TRUE(after.boot().ok());
  TEST_ASSERT_EQUAL(1, after.journal.count());
  const orch::ToolResult listed = after.call("agent_list", "{}");
  TEST_ASSERT_TRUE_MESSAGE(has(listed.output, "long read (job0000, anthropic/claude-haiku-5-5)"), listed.output.c_str());
  after.model.push(sp::say("It says yes."));
  after.pumps(4);
  TEST_ASSERT_EQUAL(1, after.reportsRun);
  TEST_ASSERT_TRUE(has(after.model.last().body, "The long report says yes."));
  TEST_ASSERT_TRUE(has(after.model.last().body, "read it in the background"));   // the same conversation
  TEST_ASSERT_EQUAL_STRING("1001|It says yes.", after.sent.back().c_str());
  TEST_ASSERT_EQUAL(1, (int)kept.saved.count("agents/job0000-long-read.md"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_report_path_is_agents_id_name);
  RUN_TEST(test_a_started_job_reports_back_as_its_own_turn);
  RUN_TEST(test_list_and_stop);
  RUN_TEST(test_a_report_during_a_turn_is_kept_not_lost);
  RUN_TEST(test_a_job_open_at_restart_reports_after_it);
  return UNITY_END();
}
