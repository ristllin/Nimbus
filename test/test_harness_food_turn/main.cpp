#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/orch/body_budget.h"   // bodyBudgetError - a wire's refusal of a body too big to send
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/food_ledger.h"
#include "../support/fake_config.h"
#include "../support/fake_platform.h"

// Lane CTX through the real TurnEngine: the ledger's figures as claim sources, the
// fold's day, the whole day in the history on a 128K head, and the request body guard.
// RB-Q3a: no food playbook and no ledger block in any prompt; the chat's recent actions
// (the effects ring) ride the per-turn block instead.
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

// The effects ring's API behind the engine's recent-actions source (RB-Q3a), per chat.
struct FakeEffects : agent::RecentActionsSource {
  std::map<std::string, std::vector<agent::RecentAction>> byChat;
  void recent(const std::string& chat, size_t n, std::vector<agent::RecentAction>& out) override {
    out.clear();
    const auto it = byChat.find(chat);
    if (it == byChat.end()) return;
    out.assign(it->second.begin() + (it->second.size() > n ? it->second.size() - n : 0), it->second.end());
  }
};

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  InMemoryEpisodicStore store;
  int seq = 0;
  std::string file = kLedgerFile;
  bool fileExists = true;
  std::string skillText;   // the owner's stored food skill ("" = none)
  std::string reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  std::vector<std::string> instructions, inputs, delivered;
  std::vector<std::string> refusals;   // lane H: the host refuses these calls for size first, in order
  std::string turnTag;
  agent::HistoryMarks marks;
  std::string role = "admin";   // the speaker's role the device closure names
  FakeEffects effects;
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
  std::vector<EpisodicMessage> rows(const std::string& chat, size_t limit, const std::string& before,
                                    const std::string& after = std::string()) const {
    MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = MsgKind::Message;
    q.limit = (int)limit;
    q.before = before;
    q.after = after;
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
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Lumi";
      in.speakerRole = role;
      return in;
    };
    d.recentActions = &effects;
    d.skillBody = [this](const std::string& id) {
      return id == "meal-photo-science" ? skillText : agent::builtinSkillBody(id, true);
    };
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
                                  std::string& err, const agent::HeadTools*, TokenUsage*) {
      instructions.push_back(ins);
      inputs.push_back(inp);
      if (!refusals.empty()) {
        err = refusals.front();
        refusals.erase(refusals.begin());
        return false;
      }
      out = reply;
      return true;
    });
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, const agent::HistoryQuery& hq) {
      return rows(c, hq.limit, hq.before, hq.after);
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

// RB-Q3a: the goldens' contract (test_harness_compose): GOLDEN_UPDATE=1 blesses, a missing
// golden fails, drift is written to test/golden/out for the diff.
bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
void checkTextGolden(const char* name, const std::string& current) {
  const std::string path = std::string("test/golden/") + name;
  const char* bless = std::getenv("GOLDEN_UPDATE");
  if (bless && std::string(bless) == "1") {
    writeFile(path, current);
    return;
  }
  std::string blessed;
  const bool found = readFile(path, blessed);
  if (found && blessed == current) return;
  writeFile(std::string("test/golden/out/") + name, current);
  TEST_FAIL_MESSAGE((std::string(found ? "prompt drift vs " : "missing golden ") + path).c_str());
}

// The per-turn block cut at its bracketed headers ("[NAME ..." at a line start), so a
// measurement names each block with its bytes.
void printBlocks(const char* label, const std::string& text) {
  size_t start = 0;
  while (start < text.size()) {
    size_t next = text.find("\n[", start + 1);
    next = next == std::string::npos ? text.size() : next + 1;
    const std::string block = text.substr(start, next - start);
    std::printf("RBQ3A-MEASURE %s %6zu B  %s\n", label, block.size(), block.substr(0, block.find('\n')).substr(0, 72).c_str());
    start = next;
  }
}

}  // namespace

// Since v5.2.0 (lane RB-Q1b) a reply's figures are not checked against the turn's sources:
// a total quoted from the ledger and one from nowhere are both delivered as written, once.
static void test_ledger_figures_are_claim_sources() {
  Rig r;
  r.reply = "{\"reply\":\"The ledger says 1315 kcal for today.\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("what's my total?", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("The ledger says 1315 kcal for today.", r.delivered.back().c_str());
  Rig c;
  c.reply = "{\"reply\":\"The label says 2402 kcal for today.\",\"memory\":\"\",\"ask\":\"\"}";
  c.eng->handleMessage("what's my total?", "Roy", "1001");
  TEST_ASSERT_EQUAL_STRING("The label says 2402 kcal for today.", c.delivered.back().c_str());
}

// The food figure check (a target nothing holds, a day total off the ledger) went with the
// claim check (RB-Q1b): the reply goes out as written, once, with no "(Correction" line; since
// RB-Q3a the turn carries neither the ledger's figures nor the [FOOD TARGETS] pointer.
static void test_a_made_up_target_reaches_the_owner_marked() {
  Rig r;
  r.reply = "{\"reply\":\"You have 38.3 g protein left from your 250 g daily protein target.\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("how much protein is left?", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("You have 38.3 g protein left from your 250 g daily protein target.", r.delivered.back().c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(r.last().dyn, "[FOOD TARGETS]"), r.last().dyn.c_str());   // RB-Q3a: no ledger block
}

static void test_the_day_total_in_a_reply_is_checked_against_the_ledger() {
  Rig r;
  r.reply = "{\"reply\":\"Today's total is now 3,989 kcal.\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("so what are we at for today?", "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("Today's total is now 3,989 kcal.", r.delivered.back().c_str());
  TEST_ASSERT_FALSE_MESSAGE(has(r.last().dyn, "1315"), r.last().dyn.c_str());   // RB-Q3a: the ledger does not ride the turn
}

// The fold's input closes with the date and today's ledger, so the summary dates
// its facts and carries the day's totals (a side prompt, not the per-turn block).
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

// Lane H: no history leaves a request before a real refusal of its body. A long message on a
// day of history is sent whole (v5.1.1's wire guard cut the oldest turns from an estimate,
// on 121 of 123 owner turns); when the wire does refuse the body for its size, the oldest of
// today's turns go (never the newest exchange), and the request says what went, with the
// rows, the time range and the call that reads them back.
static void test_history_leaves_only_after_a_real_refusal() {
  const uint32_t h0 = kDay / 3600;
  Rig r;
  for (int i = 0; i < 12; i++) r.turn(h0 + 10 + (uint32_t)i / 2, "turn " + std::to_string(i), std::string(900, 'z'));
  r.eng->handleMessage("here is a long document: " + std::string(52000, 'd'), "Roy", "1001");
  TEST_ASSERT_EQUAL(1, (int)r.instructions.size());
  TEST_ASSERT_TRUE_MESSAGE(has(r.history(), "turn 0\n"), "nothing is cut before a refusal");
  TEST_ASSERT_FALSE(has(r.inputs.back(), "[LEFT OUT"));
  // The wire refuses the body: the turn gives up history, oldest first, and says so.
  Rig q;
  for (int i = 0; i < 12; i++) q.turn(h0 + 10 + (uint32_t)i / 2, "turn " + std::to_string(i), std::string(900, 'z'));
  q.refusals.push_back(nimbus::orch::bodyBudgetError(85000, 89000));
  q.eng->handleMessage("here is a long document: " + std::string(52000, 'd'), "Roy", "1001");
  TEST_ASSERT_EQUAL(2, (int)q.instructions.size());
  const std::string h = q.history();
  TEST_ASSERT_FALSE_MESSAGE(has(h, "turn 0\n"), h.c_str());
  TEST_ASSERT_TRUE(has(h, "turn 11"));   // the newest exchange stays
  const std::string in = q.inputs.back();
  TEST_ASSERT_TRUE_MESSAGE(has(in, "[LEFT OUT TO FIT THIS REQUEST]"), in.c_str());
  TEST_ASSERT_TRUE(has(in, "of today's earlier conversation message(s) from Mon 26 Oct 10:00"));
  TEST_ASSERT_TRUE(has(in, "(rows m00000001 to "));
  TEST_ASSERT_TRUE(has(in, "memory_episodic {\"kind\":\"message\",\"before\":\"m0000"));
}

// Device scale, the tool loop on (plan v3 lane H): a day of food chat on Lumi's head (Mistral
// Large 4, its derived history of 206 KB since the switch-over) keeps the morning's estimate in
// the request at 18:35, with nothing left out (v5.1.1's 128K window for the same head kept
// 5,242 B and the 16 KB day: test_history_day's counter-tests).
static void test_device_scale_loop_turn_keeps_the_mornings_estimate() {
  Rig r;
  r.cfg.toolLoop = true;
  r.cfg.orchModels["mistral"] = "mistral-large-4-0";
  const uint32_t h0 = kDay / 3600;
  for (int i = 0; i < 4; i++) r.turn(h0 - 5, "yesterday " + std::to_string(i), std::string(500, 'y'));
  r.turn(h0 + 8, "breakfast photo: skyr, berries, granola", "Breakfast about 450 kcal / 30 g protein.");
  for (int i = 0; i < 30; i++) r.turn(h0 + 9 + (uint32_t)i / 4, "meal chat " + std::to_string(i), std::string(1400, 'e'));
  r.eng->handleMessage("you estimated breakfast earlier, use that number", "Roy", "1001");
  const std::string h = r.history();
  TEST_ASSERT_TRUE_MESSAGE(has(h, "Breakfast about 450 kcal"), "the morning's estimate rides the request");
  TEST_ASSERT_TRUE(has(h, "meal chat 29"));
  TEST_ASSERT_TRUE(has(h, "yesterday 0"));   // nothing moved at midnight either
  TEST_ASSERT_FALSE(has(r.inputs.back(), "[LEFT OUT"));
}

// Lane QFIX (OIR-36): the [MEMORY ROUTING] hint names where a remember-request lands, on
// the owner's own message only. A scheduled routine whose prompt happens to say "remember"
// is not an owner asking for a save, so it never carries the hint.
static void test_memory_routing_hint_rides_only_a_real_owner_message() {
  Rig r;
  r.eng->handleMessage("Also remember: always double-check my coffee orders before placing them.", "Roy", "1001");
  TEST_ASSERT_EQUAL_INT(1, (int)r.instructions.size());
  TEST_ASSERT_TRUE_MESSAGE(has(r.last().dyn, "[MEMORY ROUTING]"), r.last().dyn.c_str());
  TEST_ASSERT_TRUE(has(r.last().dyn, "prefs.edit"));
  TEST_ASSERT_FALSE(has(r.last().stat, "[MEMORY ROUTING]"));   // never the cached blocks
  TEST_ASSERT_FALSE(has(r.last().shelf, "[MEMORY ROUTING]"));
  r.eng->handleMessage("is it raining at the office?", "Roy", "1001");
  TEST_ASSERT_FALSE(has(r.last().dyn, "[MEMORY ROUTING]"));
  // Counter-test: the same words as a scheduled routine's prompt (nobody behind the turn).
  Rig s;
  s.eng->injectScheduledTurn("1001", "remember to check the weather and tell Roy", "morning", "L1");
  TEST_ASSERT_TRUE_MESSAGE(!s.instructions.empty(), "the scheduled turn never reached the host");
  TEST_ASSERT_FALSE_MESSAGE(has(s.last().dyn, "[MEMORY ROUTING]"), s.last().dyn.c_str());
}

// RB-Q3a: no keyword playbook and no per-turn ledger block in any prompt the head receives,
// for any role and any trigger: whatever the ledger's own renderers (food_ledger.h) produce
// for this day, and every playbook or skill attachment the food path ever made, is absent
// from the instructions and the input of every request. The markers come from the
// renderers themselves, so a renamed header is still caught.
static void test_no_food_playbook_or_ledger_block_in_any_prompt() {
  const FoodLedger day = scanFoodLedger(kLedgerFile, true, kToday, kFoodLedgerProject, kFoodLedgerOwnerName);
  std::vector<std::string> markers = {"[FOOD PLAYBOOK", "# Food log", "# Food coach", "[SKILL meal-photo-science"};
  auto firstLines = [&markers](const std::string& block) {
    size_t at = 0;
    while (at < block.size()) {   // every bracketed header line of the block
      const size_t nl = block.find('\n', at);
      const std::string line = block.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      if (!line.empty() && line[0] == '[') markers.push_back(line.substr(0, 40));
      at = nl == std::string::npos ? block.size() : nl + 1;
    }
  };
  firstLines(renderLedgerBlock(day, true));
  firstLines(renderLedgerBlock(day, false));
  firstLines(renderLedgerDispute(day));
  firstLines(renderFoodRecall(kLedgerFile, kToday.date, "I had some pasta for dinner"));
  for (FoodIntent i : {FoodIntent::Unclear, FoodIntent::Eaten, FoodIntent::Ask, FoodIntent::Complaint})
    firstLines(renderFoodIntentHint(i));
  TEST_ASSERT_TRUE(markers.size() >= 10);   // the renderers still render: the check has teeth
  const char* triggers[] = {"Lunch was a chicken salad, about 150 g chicken",
                            "how many calories are left for today?",
                            "brine",
                            "your total is wrong, I never had the brunch plate",
                            "I had some pasta for dinner",
                            "what's my total?",
                            "is it raining at the office?"};
  for (const char* role : {"admin", "user", "guest"}) {
    for (const char* text : triggers) {
      for (bool loop : {false, true}) {
        Rig r;
        r.role = role;
        r.cfg.toolLoop = loop;
        r.skillText = "Blinded estimate first, then the label.";
        r.turn((uint32_t)(kNow / 3600) - 1, "tuna 80 g", "Logged. Was the tuna in brine or oil?");
        r.eng->handleMessage(text, "Roy", "1001");
        TEST_ASSERT_FALSE_MESSAGE(r.instructions.empty(), text);
        for (size_t k = 0; k < r.instructions.size(); k++)
          for (const std::string& m : markers) {
            const std::string where = std::string(role) + " / " + text + ": " + m;
            TEST_ASSERT_FALSE_MESSAGE(has(r.instructions[k], m), where.c_str());
            TEST_ASSERT_FALSE_MESSAGE(has(r.inputs[k], m), where.c_str());
          }
      }
    }
  }
  // A scheduled routine about food carries none of it either.
  Rig s;
  s.eng->injectScheduledTurn("1001", "tell Roy what he ate today and how many calories are left", "evening", "L1");
  TEST_ASSERT_FALSE(s.instructions.empty());
  for (const std::string& m : markers) TEST_ASSERT_FALSE_MESSAGE(has(s.instructions.back() + s.inputs.back(), m), m.c_str());
  // The playbooks are gone from the built-ins too: nothing can be attached or read as one.
  TEST_ASSERT_FALSE(agent::isBuiltinSkill("food-log"));
  TEST_ASSERT_FALSE(agent::isBuiltinSkill("food-coach"));
}

// RB-Q3a: this chat's newest effect receipts ride the per-turn block of every turn, food or
// not; another chat's never do, and a chat with none gets no block.
static void test_recent_actions_ride_the_turn_for_this_chat_only() {
  Rig r;
  const int64_t t = (int64_t)kNow - 600;   // 18:25
  r.effects.byChat["1001"].push_back({t, "ledger_write", "added L3K9QZ; day 2026-10-26: 3 rows, 1,765 kcal"});
  r.effects.byChat["-2002"].push_back({t, "ledger_write", "added LMEMBR; day 2026-10-26: 1 row, 90 kcal"});
  r.eng->handleMessage("is it raining at the office?", "Roy", "1001");
  std::string dyn = r.last().dyn;
  TEST_ASSERT_TRUE_MESSAGE(has(dyn, "[RECENT ACTIONS - "), dyn.c_str());
  TEST_ASSERT_TRUE(has(dyn, "Mon 26 Oct 2026 18:25 ledger_write: added L3K9QZ; day 2026-10-26: 3 rows, 1,765 kcal\n"));
  TEST_ASSERT_FALSE(has(dyn, "LMEMBR"));
  TEST_ASSERT_FALSE(has(r.last().stat + r.last().shelf, "[RECENT ACTIONS"));   // never the cached blocks
  r.eng->handleMessage("make that 200 g", "Roy", "-2002");
  dyn = r.last().dyn;
  TEST_ASSERT_TRUE(has(dyn, "added LMEMBR"));
  TEST_ASSERT_FALSE(has(dyn, "L3K9QZ"));
  r.eng->handleMessage("hello", "Roy", "3003");
  TEST_ASSERT_FALSE(has(r.last().dyn, "[RECENT ACTIONS"));
}

// RB-Q3a: the owner's food turn as the head receives it (the owner's role, the tool loop on,
// rows today, the owner's own food skill stored), pinned byte for byte, with the bytes of
// every block printed for the lane's measurement.
static void test_owner_food_turn_prompt_golden() {
  Rig r;
  r.cfg.toolLoop = true;
  r.skillText = "Blinded estimate first, then the label.";
  // The day's two earlier writes as the ledger tools' receipt sink words them (RB-L1b sinkLine).
  r.effects.byChat["1001"] = {{(int64_t)kDay + 8 * 3600 + 40 * 60, "ledger_write", "added L00A3; day 2026-10-26: 1 row, 450 kcal"},
                              {(int64_t)kDay + 12 * 3600 + 55 * 60, "ledger_write", "added L00A4; day 2026-10-26: 2 rows, 1,315 kcal"}};
  r.eng->handleMessage("Lunch was a chicken salad, about 150 g chicken", "Roy", "1001");
  TEST_ASSERT_EQUAL_INT(1, (int)r.instructions.size());
  const std::string& ins = r.instructions.back();
  const agent::layout::PromptParts p = r.last();
  std::printf("RBQ3A-MEASURE total-instructions %zu B; static %zu, shelf %zu, history %zu msgs, per-turn %zu; input %zu\n",
              ins.size(), p.stat.size(), p.shelf.size(), p.history.size(), p.dyn.size(), r.inputs.back().size());
  printBlocks("per-turn", p.dyn);
  checkTextGolden("orch_prompt_owner_food_turn.txt", agent::layout::display(ins));
  // The other food shapes, measured only: a question (the coach's shape) and a short
  // follow-up to a food exchange (both playbooks' shape).
  const struct {
    const char* label;
    const char* text;
  } shapes[] = {{"question", "how many calories are left for today?"}, {"follow-up", "brine"}};
  for (const auto& sh : shapes) {
    Rig q;
    q.cfg.toolLoop = true;
    q.skillText = r.skillText;
    q.turn((uint32_t)(kNow / 3600) - 1, "tuna 80 g", "Logged. Was the tuna in brine or oil?");
    q.eng->handleMessage(sh.text, "Roy", "1001");
    std::printf("RBQ3A-MEASURE %s: per-turn %zu B, instructions %zu B\n", sh.label, q.last().dyn.size(),
                q.instructions.back().size());
    printBlocks(sh.label, q.last().dyn);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ledger_figures_are_claim_sources);
  RUN_TEST(test_a_made_up_target_reaches_the_owner_marked);
  RUN_TEST(test_the_day_total_in_a_reply_is_checked_against_the_ledger);
  RUN_TEST(test_the_fold_carries_the_day);
  RUN_TEST(test_the_whole_day_is_in_the_history);
  RUN_TEST(test_history_leaves_only_after_a_real_refusal);
  RUN_TEST(test_device_scale_loop_turn_keeps_the_mornings_estimate);
  RUN_TEST(test_memory_routing_hint_rides_only_a_real_owner_message);
  RUN_TEST(test_no_food_playbook_or_ledger_block_in_any_prompt);
  RUN_TEST(test_recent_actions_ride_the_turn_for_this_chat_only);
  RUN_TEST(test_owner_food_turn_prompt_golden);
  return UNITY_END();
}
