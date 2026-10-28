#include <unity.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/food_ledger.h"
#include "../support/fake_config.h"
#include "../support/fake_platform.h"

// Lane CTX through the real TurnEngine: today's food ledger in the per-turn block
// of every turn that has rows today (and of a food turn), the food playbook on a
// food turn and its follow-ups, the ledger's figures as claim sources, the fold's
// day, the whole day in the history on a 128K head, and the request body guard.
// Replays the shape of the owner's 26 Oct evening (Mistral large head, embeddings
// paused, a day of meals).

using namespace nimbus::orch;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kDay = 1792972800u;                 // 2026-10-26 00:00 UTC
constexpr uint64_t kNow = kDay + 18 * 3600 + 35 * 60;   // 18:35, the "you estimated earlier" turn
const LogDay kToday{"2026-10-26", "GMT"};

const char kLedgerFile[] =
    "date,meal,items,kcal,protein_g\n"
    "2026-10-25,dinner,pasta,800,30\n"
    "2026-10-26,breakfast,skyr + berries + granola,450,30\n"
    "2026-10-26,lunch,brunch plate: 2 eggs + 250 g chicken + veg,865,70\n";

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  InMemoryEpisodicStore store;
  int seq = 0;
  std::string file = kLedgerFile;
  bool fileExists = true;
  std::string reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  std::vector<std::string> instructions, inputs, delivered;
  std::string turnTag;
  agent::HistoryMarks marks;
  std::unique_ptr<TurnEngine> eng;

  std::string add(uint32_t hour, const std::string& role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
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
  void turn(uint32_t hour, const std::string& owner, const std::string& answer) {
    const std::string u = add(hour, "user", owner, "from:Roy");
    add(hour, "assistant", answer, "fx:none,out:reply,turn:" + u);
  }
  std::vector<EpisodicMessage> rows(const std::string& chat, size_t limit, const std::string& before) const {
    MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = MsgKind::Message;
    q.limit = (int)limit;
    q.before = before;
    return store.query(q);
  }

  Rig() {
    plat.epoch = kNow;
    cfg.toolLoop = false;
    cfg.orchHost = "mistral";   // the paused-provider path of 26 Oct: a 128K head
    cfg.priority = "mistral,anthropic,openai";
    cfg.orchModels["mistral"] = "mistral-large-latest";
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Lumi";
      return in;
    };
    d.skillBody = [](const std::string& id) { return agent::builtinSkillBody(id, true); };
    d.foodLedger = [this](const std::string&) {
      return scanFoodLedger(file, fileExists, kToday, kFoodLedgerProject, kFoodLedgerOwnerName);
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      EpisodicMessage m;
      char id[16];
      snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
      m.id = id;
      m.sessionId = c;
      m.tsHours = (uint32_t)(kNow / 3600);
      m.role = "assistant";
      m.kind = MsgKind::Message;
      m.text = t;
      m.tags = eng->assistantRowTags() + ",turn:" + turnTag;
      store.addMessage(m);
    };
    d.hosts.add("mistral", [this](std::string&, const std::string& ins, const std::string& inp, std::string& out,
                                  std::string&, const agent::HeadTools*, TokenUsage*) {
      instructions.push_back(ins);
      inputs.push_back(inp);
      out = reply;
      return true;
    });
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, size_t limit, const std::string& before) {
      return rows(c, limit, before);
    };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string& tags) {
      turnTag = add((uint32_t)(kNow / 3600), "user", t, tags);
      return turnTag;
    };
    d.historyMarks = [this](const std::string&) { return marks; };
    d.noteHistoryShift = [this](const std::string&, const std::string& s) { marks.softStart = s; };
    eng.reset(new TurnEngine(std::move(d)));
  }

  agent::layout::PromptParts last() const { return agent::layout::split(instructions.back()); }
  std::string history() const {
    std::string out;
    for (const auto& m : last().history) out += m.text + "\n";
    return out;
  }
};

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

// Every turn whose chat has rows today carries them, a plain question too; the
// playbook rides only a food turn.
static void test_ledger_rides_every_turn_with_rows_today() {
  Rig r;
  r.eng->handleMessage("is it raining at the office?", "Roy", "1001");
  std::string dyn = r.last().dyn;
  TEST_ASSERT_TRUE_MESSAGE(has(dyn, "[TODAY'S FOOD LEDGER - food/food-log.csv, 2026-10-26 (GMT)."), dyn.c_str());
  TEST_ASSERT_TRUE(has(dyn, "Today so far (2026-10-26): 1315 kcal from 2 rows, 100 g protein."));
  TEST_ASSERT_FALSE(has(dyn, "pasta"));                 // yesterday's dinner is not today's
  TEST_ASSERT_FALSE(has(dyn, "[FOOD PLAYBOOK"));
  TEST_ASSERT_FALSE(has(r.last().stat, "FOOD LEDGER"));   // never the cached blocks
  TEST_ASSERT_FALSE(has(r.last().shelf, "FOOD LEDGER"));
  r.eng->handleMessage("drop chinese. give me the full list by meal, main components and kcal for each", "Roy", "1001");
  dyn = r.last().dyn;
  TEST_ASSERT_TRUE(has(dyn, "[TODAY'S FOOD LEDGER"));
  TEST_ASSERT_TRUE(has(dyn, "[FOOD PLAYBOOK - attached because this turn is about food"));
  // A request: the coach, without the logging procedure, and the hint last.
  TEST_ASSERT_TRUE(has(dyn, "# Food coach: answers and plans"));
  TEST_ASSERT_FALSE(has(dyn, "# Food log: logging a meal"));
  TEST_ASSERT_TRUE(has(dyn, "[THIS MESSAGE] asks for an answer or a plan, not a meal eaten"));
  // Device context, not a skill read: the reply's row carries no skills: tag for it
  // (a weaker head copied the history's "(skills: ...)" suffix into its replies).
  const auto rows = r.rows("1001", 1, "");
  TEST_ASSERT_FALSE_MESSAGE(has(rows[0].tags, "skills:"), rows[0].tags.c_str());
}

// No rows today: a food turn gets the one call that starts today's rows; any other
// turn gets nothing. No file at all: the call creates it.
static void test_no_rows_today() {
  Rig r;
  r.file = "date,meal,items,kcal,protein_g\n2026-10-25,dinner,pasta,800,30\n";
  r.eng->handleMessage("is it raining at the office?", "Roy", "1001");
  TEST_ASSERT_FALSE(has(r.last().dyn, "FOOD LEDGER"));
  r.eng->handleMessage("Breakfast: 2 eggs and toast", "Roy", "1001");
  TEST_ASSERT_TRUE(has(r.last().dyn, "no rows yet today. The first meal the owner says they ate is logged with files.append"));
  // A report of food eaten: the logging playbook, not the coach, and the hint last.
  TEST_ASSERT_TRUE(has(r.last().dyn, "# Food log: logging a meal"));
  TEST_ASSERT_FALSE(has(r.last().dyn, "# Food coach"));
  TEST_ASSERT_TRUE(has(r.last().dyn, "[THIS MESSAGE] reports food eaten"));
  r.fileExists = false;
  r.file.clear();
  r.eng->handleMessage("Lunch was a chicken salad", "Roy", "1001");
  TEST_ASSERT_TRUE(has(r.last().dyn, "(this first call creates the file)"));
}

// A short follow-up to a food exchange is a food turn ("brine" answered "was the
// tuna in brine or oil?"), and so is a question about the ledger while it has rows.
static void test_follow_ups_get_the_playbook() {
  Rig r;
  r.file = "date,meal,items,kcal,protein_g\n";   // no rows today: the ledger words alone do not count
  r.turn((uint32_t)(kNow / 3600) - 1, "tuna 80 g", "Logged. Was the tuna in brine or oil?");
  r.eng->handleMessage("brine", "Roy", "1001");
  TEST_ASSERT_TRUE(has(r.last().dyn, "[FOOD PLAYBOOK"));
  // The words say neither: both playbooks, no hint.
  TEST_ASSERT_TRUE(has(r.last().dyn, "# Food log: logging a meal") && has(r.last().dyn, "# Food coach"));
  TEST_ASSERT_FALSE(has(r.last().dyn, "\n[THIS MESSAGE] "));   // the hint line (the playbook names it mid-line)
  Rig q;
  q.turn((uint32_t)(kNow / 3600) - 1, "how long should I rest between sets?", "2 to 3 minutes.");
  q.eng->handleMessage("I think your calc is off", "Roy", "1001");
  TEST_ASSERT_TRUE_MESSAGE(has(q.last().dyn, "[FOOD PLAYBOOK"), q.last().dyn.c_str());   // rows today + "calc"
  Rig n;
  n.turn((uint32_t)(kNow / 3600) - 1, "how long should I rest between sets?", "2 to 3 minutes.");
  n.eng->handleMessage("and on rest days?", "Roy", "1001");
  TEST_ASSERT_FALSE(has(n.last().dyn, "[FOOD PLAYBOOK"));
}

// The ledger's figures are sources: a total quoted from it is not corrected; a
// figure from nowhere still is (counter-test). Rebase (HON, 28 Oct): the quoted
// total now credits the ledger itself - lane HON's source rule asks for a source
// this turn's texts name, and no label was seen this turn; the counter keeps a
// source noun so the figure check still fires on a figure from nowhere.
static void test_ledger_figures_are_claim_sources() {
  Rig r;
  r.reply = "{\"reply\":\"The ledger says 1315 kcal for today.\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("what's my total?", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("The ledger says 1315 kcal for today.", r.delivered.back().c_str());
  Rig c;
  c.reply = "{\"reply\":\"The label says 2402 kcal for today.\",\"memory\":\"\",\"ask\":\"\"}";
  c.eng->handleMessage("what's my total?", "Roy", "1001");
  TEST_ASSERT_TRUE(c.delivered.back() != "The label says 2402 kcal for today.");
}

// The fold's input closes with the date and today's ledger, so the summary dates
// its facts and carries the day's totals.
static void test_the_fold_carries_the_day() {
  Rig r;
  std::string summary;
  TEST_ASSERT_EQUAL(int(TurnEngine::FoldResult::Ok),
                    int(r.eng->runFold("1001", "", "- user: breakfast\n- assistant: logged\n", summary)));
  const std::string& in = r.inputs.back();
  TEST_ASSERT_TRUE_MESSAGE(has(in, "[TODAY] 2026-10-26 (GMT)"), in.c_str());
  TEST_ASSERT_TRUE(has(in, "[FOOD LEDGER - food/food-log.csv on 2026-10-26 (GMT)"));
  TEST_ASSERT_TRUE(has(in, "Totals for 2026-10-26: 1315 kcal from 2 rows, 100 g protein."));
  TEST_ASSERT_TRUE(has(r.instructions.back(), "a [FOOD LEDGER] block is the device's record"));
}

// On the 128K head the whole day is in the history at 18:35, the 12:55 estimate
// included; yesterday's turns are not.
static void test_the_whole_day_is_in_the_history() {
  Rig r;
  const uint32_t h0 = kDay / 3600;
  for (int i = 0; i < 4; i++) r.turn(h0 - 5, "yesterday " + std::to_string(i), std::string(500, 'y'));
  r.turn(h0 + 12, "breakfast and lunch estimates please", "Breakfast about 450 kcal, lunch about 865 kcal.");
  for (int i = 0; i < 14; i++) r.turn(h0 + 13 + (uint32_t)i / 3, "evening " + std::to_string(i), std::string(450, 'e'));
  r.eng->handleMessage("you estimated earlier, use those", "Roy", "1001");
  const std::string h = r.history();
  TEST_ASSERT_TRUE_MESSAGE(has(h, "Breakfast about 450 kcal, lunch about 865 kcal."), h.c_str());
  TEST_ASSERT_TRUE(has(h, "evening 13"));
  TEST_ASSERT_FALSE(has(h, "yesterday 0"));
}

// A request that would pass the body budget leaves out the oldest history turns
// (never the newest), and says so in the log.
static void test_the_request_stays_under_the_body_budget() {
  Rig r;
  const uint32_t h0 = kDay / 3600;
  for (int i = 0; i < 12; i++) r.turn(h0 + 10 + (uint32_t)i / 2, "turn " + std::to_string(i), std::string(900, 'z'));
  r.eng->handleMessage("here is a long document: " + std::string(52000, 'd'), "Roy", "1001");
  const std::string h = r.history();
  TEST_ASSERT_FALSE_MESSAGE(has(h, "turn 0\n"), "the oldest turn should have been left out");
  TEST_ASSERT_TRUE(has(h, "turn 11"));   // the newest turn stays
  // Counter-test: the same history with a short message is sent whole.
  Rig s;
  for (int i = 0; i < 12; i++) s.turn(h0 + 10 + (uint32_t)i / 2, "turn " + std::to_string(i), std::string(900, 'z'));
  s.eng->handleMessage("short", "Roy", "1001");
  TEST_ASSERT_TRUE(has(s.history(), "turn 0\n"));
}

// The guard's arithmetic on the device's real shape: a loop turn advertises about
// 42 tools (test_loop_body_budget's deviceShapedSpecs) and keeps room for its tool
// rounds, so it leaves out more history than a single-shot turn with the same text;
// the summary pair and the newest turn always stay, and what is left fits.
static void test_the_wire_guard_on_a_device_shaped_loop_turn() {
  std::vector<nimbus::orch::ToolRegistry::Spec> specs;
  const std::string schema = R"({"type":"object","properties":{"project":{"type":"string"},"q":{"type":"string"}}})";
  for (int i = 0; i < 42; i++) specs.push_back({"tool_" + std::to_string(i), std::string(309, 'd'), schema});
  const size_t loopTools = agent::wireToolBytes(specs, true), shotTools = agent::wireToolBytes(specs, false);
  TEST_ASSERT_TRUE(loopTools > shotTools + 42 * 309);   // a single-shot turn sends the answer schema alone
  const auto chat = [] {
    std::vector<agent::HistMsg> h = {{true, std::string(agent::kSummaryUserHead) + "earlier"}, {false, agent::kSummaryAck}};
    for (int i = 0; i < 30; i++) {
      h.push_back({true, "turn " + std::to_string(i) + " " + std::string(900, 'u')});
      h.push_back({false, "reply " + std::to_string(i) + " " + std::string(900, 'a')});
    }
    return h;
  };
  // The instructions carry the history (compose renders it in), as the engine passes them.
  const auto histBytes = [](const std::vector<agent::HistMsg>& h) {
    size_t n = 0;
    for (const auto& m : h) n += m.text.size();
    return n;
  };
  std::vector<agent::HistMsg> loop = chat(), shot = chat();
  const size_t base = 20000, inp = 20000;
  const size_t droppedLoop = agent::fitHistoryToWire(loop, base + histBytes(loop), loopTools, inp, true);
  const size_t droppedShot = agent::fitHistoryToWire(shot, base + histBytes(shot), shotTools, inp, false);
  TEST_ASSERT_TRUE(droppedLoop > 0 && droppedLoop % 2 == 0);   // whole exchanges only
  TEST_ASSERT_TRUE_MESSAGE(droppedLoop > droppedShot, (std::to_string(droppedLoop) + " vs " + std::to_string(droppedShot)).c_str());
  for (const auto* h : {&loop, &shot}) {
    TEST_ASSERT_TRUE(h->front().text.rfind(agent::kSummaryUserHead, 0) == 0);
    TEST_ASSERT_TRUE(h->back().text.rfind("reply 29 ", 0) == 0);
  }
  // What is left fits: the guard run again on it leaves out nothing more (its
  // accounting of what a dropped message saves matches what the request costs).
  TEST_ASSERT_EQUAL(0, (int)agent::fitHistoryToWire(loop, base + histBytes(loop), loopTools, inp, true));
  TEST_ASSERT_EQUAL(0, (int)agent::fitHistoryToWire(shot, base + histBytes(shot), shotTools, inp, false));
  // Nothing to do when it already fits.
  std::vector<agent::HistMsg> small = chat();
  small.resize(6);
  TEST_ASSERT_EQUAL(0, (int)agent::fitHistoryToWire(small, 4000 + histBytes(small), loopTools, 2000, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ledger_rides_every_turn_with_rows_today);
  RUN_TEST(test_no_rows_today);
  RUN_TEST(test_follow_ups_get_the_playbook);
  RUN_TEST(test_ledger_figures_are_claim_sources);
  RUN_TEST(test_the_fold_carries_the_day);
  RUN_TEST(test_the_whole_day_is_in_the_history);
  RUN_TEST(test_the_request_stays_under_the_body_budget);
  RUN_TEST(test_the_wire_guard_on_a_device_shaped_loop_turn);
  return UNITY_END();
}
