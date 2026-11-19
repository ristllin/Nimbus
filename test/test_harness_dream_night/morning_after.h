#pragma once
// The morning after a scripted night (owner ruling 81; lane RB-Q2b).
//
// One fixture, two trees. The same evening of conversation, the same night and the same
// morning message run on rb-main f5044ee1b (where the night still runs the dream's model
// turn) and on this tree (where it does not). What the morning's head request carries of
// earlier conversation is dumped by dumpMorning(); the rb-main dump is the golden
// test/golden/dream_morning_rb_main.txt, and this tree's request must carry every one of
// its lines except the named model-written dream outputs (kDreamWritten).
//
// The night is bound by scriptedNight(), the same code on both trees: it binds every seam
// the tree's NightDeps offers, so on rb-main the reflection turn runs (and its wake note
// fires as the self-wakeup at the end of quiet hours) and here it cannot.
#include <ArduinoJson.h>

#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/dream.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/self_wakeup.h"

namespace morning {

using nimbus::orch::EpisodicMessage;
using nimbus::orch::MsgKind;

constexpr uint32_t kDay = 1792972800u;                       // 2026-10-26 00:00 UTC
constexpr uint64_t kEvening = kDay + 18 * 3600;              // the owner's evening
constexpr uint64_t kNight = kDay + 24 * 3600 + 210 * 60;     // 03:30 the next day
constexpr uint64_t kMorning = kDay + 24 * 3600 + 8 * 3600;   // 08:00
constexpr const char* kChat = "1001";

// What the dream's model turn writes on a tree that still has it (scripted): one durable
// fact, a running-memory refresh, a memory search of its own and a reply worth telling.
// These are the named model-written dream outputs (PROGRESS.md note 2, items 6, 8, 10, 11).
constexpr const char* kDreamFact = "DREAMFACT the owner plans a city trip in March";
constexpr const char* kDreamMemory = "DREAMMEMO trip planning is in progress";
constexpr const char* kDreamReply = "DREAMREPLY the budget question is still open";
constexpr const char* kDreamQuery = "DREAMQUERY trip";
// A line of the morning request that holds any of these is a dream output, not earlier
// conversation: the fact, the running memory, the reply, the dream's own memory search,
// the dream's trigger row and the self-wakeup that delivered its reply (item 10).
inline const std::vector<std::string>& dreamWritten() {
  static const std::vector<std::string> v{"DREAMFACT", "DREAMMEMO", "DREAMREPLY", "DREAMQUERY",
                                          "[routine \"Dream\" ran]", "Self-wakeup: upkeep"};
  return v;
}

struct Rig {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  nimbus::orch::InMemoryEpisodicStore store;
  std::unique_ptr<agent::JobEngine> jobs;
  std::unique_ptr<agent::TurnEngine> eng;
  agent::HistoryMarks marks;
  int seq = 0;
  std::string turnTag;
  std::string runningMemory;        // OrchMemory::model on the device
  std::vector<std::string> facts;   // the vector store's contents, in write order
  std::string reply = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
  std::vector<std::string> instructions, inputs;
  int providerCalls = 0;
  uint32_t billedTokens = 0;

  std::string add(const std::string& role, const std::string& text, const std::string& tags) {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = kChat;
    m.tsHours = (uint32_t)(plat.epoch / 3600);
    m.role = role;
    m.kind = MsgKind::Message;
    m.text = text;
    m.tags = tags;
    store.addMessage(m);
    return m.id;
  }
  std::vector<EpisodicMessage> rows(const std::string& chat, size_t limit, const std::string& before,
                                    const std::string& after) const {
    nimbus::orch::MsgQuery q;
    q.sessionId = chat;
    q.haveKind = true;
    q.kind = MsgKind::Message;
    q.limit = (int)limit;
    q.before = before;
    q.after = after;
    return store.query(q);
  }

  Rig() {
    plat.epoch = kEvening;
    cfg.toolLoop = false;
    cfg.orchHost = "anthropic";
    cfg.priority = "anthropic";
    cfg.spendBatch = true;   // every billed turn lands in cfg.spendCalls with its attribution
    agent::JobEngine::Deps jd;
    jd.platform = plat.contract();
    jobs.reset(new agent::JobEngine(std::move(jd)));
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = [](const std::string&, const std::string&) {};
    d.firstAllowedChat = [] { return std::string(kChat); };
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.runningMemory = runningMemory;
      return in;
    };
    d.recall = [this](const std::string&, const nimbus::orch::Principal&) {
      nimbus::orch::RecallOut o;
      for (const std::string& f : facts) o.hits.push_back("[90%] " + f);
      return o;
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) {
      return nimbus::orch::principalForRole(chat, nimbus::orch::Role::Admin);
    };
    d.apply.setModelMemory = [this](const std::string&, const std::string& v) {
      runningMemory = v;
      return true;
    };
    d.apply.memDispatch = [this](const char* name, ArduinoJson::JsonObjectConst a,
                                 const nimbus::orch::Principal&) {
      if (std::string(name) == "memory.search")
        return nimbus::orch::ToolResult::ok(std::string("found: ") + (a["query"] | ""));
      facts.push_back(a["content"] | "");
      return nimbus::orch::ToolResult::ok("stored memory m1");
    };
    d.apply.captureAssistant = [this](const std::string&, const std::string& t) {
      add("assistant", t, eng->assistantRowTags() + ",turn:" + turnTag);
    };
    d.captureTrigger = [this](const std::string&, const std::string& t, const std::string& tags) {
      add("system", t, tags);
    };
    d.hosts.add("anthropic", [this](std::string& conv, const std::string& ins, const std::string& inp,
                                    std::string& out, std::string&, const agent::HeadTools*,
                                    nimbus::orch::TokenUsage* usage) {
      ++providerCalls;
      billedTokens += 1200;
      if (usage) usage->add(1000, 200);
      instructions.push_back(ins);
      inputs.push_back(inp);
      conv = "c";
      out = inp.find("[DREAM]") != std::string::npos ? dreamReply() : reply;
      return true;
    });
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, const agent::HistoryQuery& hq) {
      return rows(c, hq.limit, hq.before, hq.after);
    };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string& tags) {
      turnTag = add("user", t, tags);
      return turnTag;
    };
    d.historyMarks = [this](const std::string&) { return marks; };
    d.noteHistoryShift = [this](const std::string&, const std::string& s) { marks.softStart = s; };
    eng.reset(new agent::TurnEngine(std::move(d)));
  }

  static std::string dreamReply() {
    return std::string("{\"reply\":\"") + kDreamReply + "\",\"memory\":\"" + kDreamMemory +
           "\",\"ask\":\"\",\"mem_write\":[{\"content\":\"" + kDreamFact +
           "\"}],\"mem_query\":[\"" + kDreamQuery + "\"]}";
  }

  void say(uint32_t hourOfEvening, const std::string& text, const std::string& answer) {
    plat.epoch = kEvening + hourOfEvening * 3600;
    reply = answer;
    eng->handleMessage(text, "Roy", kChat);
  }
};

// The evening: four owner turns with distinctive content, the last one asks the memory and
// leaves its results parked for the chat's next turn, and a refused spawn leaves its note.
inline void evening(Rig& r) {
  r.say(0, "Plan the city trip: flights on 12 March, the hotel near the old town.",
        "{\"reply\":\"Noted: flights on 12 March, hotel near the old town.\",\"memory\":\"trip: 12 March\",\"ask\":\"\"}");
  r.say(1, "My sister Ana arrives on the 14th and stays two nights.",
        "{\"reply\":\"Ana: 14th, two nights. I will plan a room for her.\",\"memory\":\"trip: 12 March; Ana 14th\",\"ask\":\"\"}");
  r.say(2, "Here is the itinerary draft:\nDay 1 museum\nDay 2 river walk\nDay 3 market",
        "{\"reply\":\"Draft kept: museum, river walk, market (days 1 to 3).\",\"memory\":\"trip: 12 March; Ana 14th; 3-day draft\",\"ask\":\"\"}");
  r.say(3, "What budget did I set last time?",
        "{\"reply\":\"Let me look that up.\",\"memory\":\"trip: 12 March; Ana 14th; 3-day draft\",\"ask\":\"\",\"mem_query\":[\"trip budget\"]}");
  r.jobs->noteSpawnIntentRefused(kChat, "price-check", "compare hotel prices for 12 March",
                                 "the sub-agent limit was reached");
}

// The morning: one owner message; the request it sends is the one compared.
inline void morningTurn(Rig& r) {
  r.plat.epoch = kMorning;
  r.reply = "{\"reply\":\"Good morning.\",\"memory\":\"\",\"ask\":\"\"}";
  r.eng->handleMessage("Good morning. Where did we leave the trip?", "Roy", kChat);
}

// ---- the scripted night, bound the same way on both trees ----------------------------

template <class T, class = void> struct HasTurnSeam : std::false_type {};
template <class T>
struct HasTurnSeam<T, std::void_t<decltype(std::declval<T&>().injectTurn)>> : std::true_type {};

struct NightOut {
  int providerCalls = 0;       // provider requests the night made
  size_t spendRows = 0;        // usage-ledger batches the night recorded
  size_t dreamSpendRows = 0;   // ... attributed to the dream ("loop:dream")
  bool maintained = false;     // decay, prune and dedup ran
  std::string row;             // the night's timeline row
};

// The night at 03:30 as the device binds it (dream_subsystem.cpp): maintenance, the row,
// and - on a tree whose NightDeps still has the turn seam - the reflection turn in the
// owner's default chat (the reserved record's chat is empty) and its wake note, fired as
// the self-wakeup at the end of quiet hours (07:00).
template <class Deps = agent::dream::NightDeps>
NightOut scriptedNight(Rig& r) {
  namespace dream = agent::dream;
  NightOut out;
  r.plat.epoch = kNight;
  const int calls0 = r.providerCalls;
  const size_t spend0 = r.cfg.spendCalls.size();
  dream::DreamMemo memo;
  Deps d;
  d.memo = &memo;
  d.loadState = [](dream::DreamState&) { return false; };
  d.saveState = [](const dream::DreamState&) { return true; };
  d.storeReady = [] { return true; };
  d.maintain = [&out] {
    out.maintained = true;
    dream::MaintResult m;
    m.ran = true;
    m.persisted = true;
    return m;
  };
  d.emitRow = [&out](const agent::DreamEndEv& ev) {
    if (ev.report) out.row = dream::renderDreamRow(*ev.report);
  };
  dream::NightClock c;
  c.epoch = kNight;
  c.localDay = (uint32_t)(kNight / 86400);
  c.minuteOfDay = 210;
  c.synced = true;
  if constexpr (HasTurnSeam<Deps>::value) {
    d.yesterday = [&r] {
      return dream::readYesterday(r.store, (uint32_t)(r.plat.epoch / 3600), {kChat, "system"});
    };
    d.scratchBlock = [] { return std::string(); };
    d.scratchHash = [] { return (uint64_t)1; };
    d.stats = [] { return dream::MemStats{}; };
    d.embeddingsUp = [] { return true; };
    d.memOps = [] { return nimbus::orch::MemOpCounters{}; };
    d.injectTurn = [&r](const std::string& in) {
      return r.eng->injectScheduledTurn("", in, "Dream", dream::kLoopId, /*quietOk=*/true);
    };
    const auto res = dream::runNight(d, c, false);
    if (!res.report.wake.text.empty()) {
      r.plat.epoch = kDay + 24 * 3600 + 7 * 3600;
      r.reply = std::string("{\"reply\":\"") + kDreamReply + "\",\"memory\":\"\",\"ask\":\"\"}";
      r.eng->injectScheduledTurn(kChat, nimbus::orch::dumpWakeNote(res.report.wake), "Self-wakeup: upkeep",
                                 "sw-upkeep", false, /*once=*/true, false, /*selfWakeup=*/true);
    }
  } else {
    dream::runNight(d, c);
  }
  out.providerCalls = r.providerCalls - calls0;
  for (size_t i = spend0; i < r.cfg.spendCalls.size(); ++i) {
    ++out.spendRows;
    if (r.cfg.spendCalls[i].tag.find("dream") != std::string::npos) ++out.dreamSpendRows;
  }
  return out;
}

// The earlier-conversation content of the last head request, one entry per line: the
// history messages (tagged with their role and their index, so a line keeps its exchange),
// then the per-turn block's and the input's lines. Fixed clock, so both trees render the
// same stamps.
inline void addLines(std::vector<std::string>& out, const std::string& tag, const std::string& text) {
  size_t s = 0;
  while (s <= text.size()) {
    size_t e = text.find('\n', s);
    if (e == std::string::npos) e = text.size();
    const std::string l = text.substr(s, e - s);
    if (l.find_first_not_of(" \t") != std::string::npos) out.push_back(tag + l);
    s = e + 1;
  }
}
inline std::vector<std::string> dumpMorning(const Rig& r) {
  std::vector<std::string> out;
  if (r.instructions.empty()) return out;
  const agent::layout::PromptParts p = agent::layout::split(r.instructions.back());
  for (const auto& m : p.history) addLines(out, m.user ? "history|user|" : "history|assistant|", m.text);
  addLines(out, "dynamic|", p.dyn);
  addLines(out, "input|", r.inputs.back());
  return out;
}

inline bool isDreamWritten(const std::string& line) {
  for (const std::string& w : dreamWritten())
    if (line.find(w) != std::string::npos) return true;
  return false;
}

// The lines of a dump that are earlier conversation: every line except a dream output, and
// except the assistant lines that answer a dream-written owner-side history line (the reply
// row of the dream's own trigger or self-wakeup exchange).
inline std::vector<std::string> conversationLines(const std::vector<std::string>& dump) {
  std::vector<std::string> out;
  bool answeringDream = false;
  for (const std::string& l : dump) {
    const bool user = l.rfind("history|user|", 0) == 0;
    const bool assistant = l.rfind("history|assistant|", 0) == 0;
    if (user) answeringDream = isDreamWritten(l);
    else if (!assistant) answeringDream = false;
    const bool dream = isDreamWritten(l) || (assistant && answeringDream);
    if (!dream) out.push_back(l);
  }
  return out;
}

}  // namespace morning
