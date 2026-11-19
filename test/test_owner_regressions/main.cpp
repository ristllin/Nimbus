#include <unity.h>

#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fold_driver.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/turn_fit.h"
#include "../../lib/harness/src/providers/loop_common.h"   // cutRetryNote (RB-Q1a: moved out of reply_guard)
#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/embed_pick.h"
#include "nimbus/orch/failure_chain.h"
#include "nimbus/orch/file_append.h"
#include "nimbus/orch/file_store.h"
#include "nimbus/orch/loop_delivery.h"
#include "nimbus/orch/loops.h"
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/msg_batch.h"
#if __has_include("nimbus/orch/open_questions.h")
#include "nimbus/orch/open_questions.h"
#define OIR_HAVE_OPENQ 1
#endif
#include "nimbus/orch/owner_prefs.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/sub_provenance.h"
#include "nimbus/orch/turn_effects.h"

// test_owner_regressions - the deterministic guards for coverage/OWNER-ISSUES-REGISTER.md
// (lane REGR). One test per register row that has a lib/core seam; every fixture is synthetic,
// shaped like the real case with the same numbers class. No personal text from the evidence.
// The history fix's rows (OIR-51 to OIR-55, lane CTXE, v5.1.2) go through the lib/harness
// seams the fix lives in: the history window, the turn fit and the one fold driver.
//
// Rows whose fix landed while this lane was in flight (OIR-19/OIR-42 lane UPSERT 0cefb9d3,
// OIR-43 lane FSCAP fd75210d) carried a runtime self-flip: while the unfixed behavior was
// detected the test reported EXPECTED-FAIL via TEST_IGNORE_MESSAGE; the train removed the
// ignore when the owning lane landed, so the real assertions run unconditionally.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

struct FakeIO : PrefsStoreIO {
  bool exists = false;
  std::string blob;
  bool load(std::string& out) override {
    if (!exists) return false;
    out = blob;
    return true;
  }
  bool saveChecked(const std::string& b) override {
    exists = true;
    blob = b;
    return true;
  }
};

struct FoldIO : FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

const char* kLedgerHeader = "date,meal,items,kcal,protein_g\n";

// The device's day total of files.read {tail, sum} (file_append.h CsvDayTally) over a whole text.
bool dayTotalOf(const std::string& file, const std::string& col, const std::string& day, double& out) {
  CsvDayTally tally(col, day);
  RowSplitter rows([&](const std::string& r, bool complete) {
    tally.feed(r, complete);
    return true;
  });
  rows.push(file.data(), file.size());
  rows.end();
  out = tally.total().total;
  return tally.ok();
}

// files.append on a whole text: the plan's bytes on the end (file_append.h planAppend).
std::string appended(const std::string& file, const std::string& text) {
  const AppendPlan p = planAppend(text, file.size(), file.empty() ? '\0' : file.back());
  TEST_ASSERT_TRUE_MESSAGE(p.ok, p.err.c_str());
  return file + p.bytes;
}

// OIR-01: a routine's fire request carries the routine's prompt verbatim; nothing
// else is appended to what the owner asked the routine to do.
void test_oir_01_routine_fire_request_carries_prompt() {
  LoopRecord l;
  l.id = "lpe50e9c";
  l.name = "commute brief";
  l.prompt = "Send the commute brief: trains, weather, one calendar line.";
  l.chatId = "owner";
  DeliveryWorld w = deliveryWorld("", "", "owner");
  Delivery d = resolveDelivery(l.chatId, w, true);
  LoopFireRequest fr = buildFireRequest(l, d, 1762000000);
  TEST_ASSERT_EQUAL_STRING(l.prompt.c_str(), fr.prompt.c_str());
}

// OIR-05: the device's day total sums every row of the day (1,611), never the model's 1,461.
void test_oir_05_day_total_receipt() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-28,breakfast,oats,120,8\n"
                         "2026-10-28,snack,bar,131,3\n"
                         "2026-10-28,lunch,plate,355,20\n"
                         "2026-10-28,dinner,plate,1005,40\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(ex, "kcal", "2026-10-28", t));
  TEST_ASSERT_EQUAL_INT(1611, (int)t);
}

// OIR-08/OIR-34/OIR-39: an open item is raised once, re-opened when the owner returns
// to it, and dropped when the owner pairs a drop cue with its subject.
#ifdef OIR_HAVE_OPENQ
void test_oir_08_open_question_raise_once_and_drop() {
  FakeIO io;
  OpenQuestions q;
  q.begin(&io);
  std::string id;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(q.add("Photograph the skyr pot label", 5, &id)));
  // First mention in a reply marks it raised (a change is logged).
  const std::string first = observeOpenQ(q, "", "Did you photograph the skyr pot label yet?");
  TEST_ASSERT_FALSE(first.empty());
  TEST_ASSERT_TRUE(q.find(1)->raised);
  // The same question again changes nothing (raised once, not nagged).
  TEST_ASSERT_TRUE(observeOpenQ(q, "", "Did you photograph the skyr pot label yet?").empty());
  // The owner returns to it: re-opened.
  observeOpenQ(q, "What about the skyr pot label?", "");
  TEST_ASSERT_FALSE(q.find(1)->raised);
  // The owner drops it: removed.
  TEST_ASSERT_TRUE(openQOwnerDrops("Duolingo lesson", "Forget duolingo, no reason to ask"));
  observeOpenQ(q, "Forget the skyr pot label, I will send it", "");
  TEST_ASSERT_EQUAL_INT(0, (int)q.count());
}
#endif  // OIR_HAVE_OPENQ

// OIR-09: the day total counts the morning cappuccino row too (534, not 486).
void test_oir_09_total_includes_every_row() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-29,breakfast,plate,486,20\n"
                         "2026-10-29,drink,cappuccino,48,3\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(ex, "kcal", "2026-10-29", t));
  TEST_ASSERT_EQUAL_INT(534, (int)t);
}

// OIR-14: yesterday's rows never enter today's total (465, not 2,989).
void test_oir_14_day_rollover() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-29,dinner,plate,2524,120\n";
  const std::string nw = "2026-10-30,snack,skyr,342,20\n"
                         "2026-10-30,coffee,black coffee,75,2\n"
                         "2026-10-30,drink,cappuccino,48,3\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(appended(ex, nw), "kcal", "2026-10-30", t));
  TEST_ASSERT_EQUAL_INT(465, (int)t);
}

// OIR-17: a sub-agent result with no live task and no connector call is unverified,
// and the note says so.
void test_oir_17_subagent_invented_items() {
  TEST_ASSERT_TRUE(subResultUnverified(true, 0, false, 0));
  TEST_ASSERT_TRUE(subResultUnverified(false, 0, true, 0));
  TEST_ASSERT_FALSE(liveDataNotCheckedNote().empty());
}

// OIR-18: the device's day total gives 4,006 for the rows the reply itself listed as 3,989.
void test_oir_18_receipt_vs_reply_rows() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-10-30,lunch,chicken sandwich,900,40\n"
                         "2026-10-30,snack,black beans,300,15\n"
                         "2026-10-30,drink,cappuccino,150,6\n"
                         "2026-10-30,snack,raspberries,50,1\n"
                         "2026-10-30,dinner,plate,2606,90\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(ex, "kcal", "2026-10-30", t));
  TEST_ASSERT_EQUAL_INT(4006, (int)t);
}

// OIR-19: "1 avocado" was logged over half an avocado because a keyed write replaced a row
// describing a different item. Since v5.2.0 files.append only appends (owner ruling 41): the
// stored row is never touched, the new row lands after it, and the day counts both (457 + 445).
void test_oir_19_silent_replace_fixed() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,dinner,smoked brisket 150 g and bread 58 g,457,40.2\n";
  const std::string nw = "2026-11-04,dinner,Frive Smoky Baharat Mango Chicken 70% of 406 g pack,445,26.0\n";
  const std::string after = appended(ex, nw);
  TEST_ASSERT_EQUAL_STRING((ex + nw).c_str(), after.c_str());
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(after, "kcal", "2026-11-04", t));
  TEST_ASSERT_EQUAL_INT(902, (int)t);
}

// OIR-23: an output cut carries a retry note naming the cut, never silence.
void test_oir_23_cut_retry_note() {
  const std::string note = agent::providers::cutRetryNote("output cut", "partial answer");
  TEST_ASSERT_FALSE(note.empty());
}

// OIR-24: a fold that does not shrink the chat cannot re-send the identical request in
// a loop. Since lane F (ruling 2: never a permanent pause) the refill right after a
// completed fold is due once and schedules its next try 15 min out; a second
// evaluateDue before then is No, and it is due again at that time, never paused.
void test_oir_24_compact_thrash_pause() {
  FoldIO io;
  FoldStore st;
  st.begin(&io);
  const uint32_t t0 = 1700000000;
  for (int i = 0; i < 5; i++) st.noteMessage("c1", 2000);   // over the 1000-byte threshold
  st.applyFold("c1", "summary", t0);                         // a completed fold
  st.noteMessage("c1", 2000);                                // refills within kThrashTurns
  TEST_ASSERT_EQUAL(int(FoldDue::Yes), int(st.evaluateDue("c1", 1000, 200, t0 + 30)));
  TEST_ASSERT_EQUAL_UINT32(t0 + 30 + 15 * 60, st.get("c1").nextTry);
  TEST_ASSERT_EQUAL(int(FoldDue::No), int(st.evaluateDue("c1", 1000, 200, t0 + 31)));
  TEST_ASSERT_EQUAL(int(FoldDue::No), int(st.evaluateDue("c1", 1000, 200, t0 + 29 + 15 * 60)));
  TEST_ASSERT_EQUAL(int(FoldDue::Yes), int(st.evaluateDue("c1", 1000, 200, t0 + 30 + 15 * 60)));
}

// OIR-25/OIR-33: the body-budget error round-trips (85,000 B budget, 90,519 B body).
void test_oir_25_body_budget_roundtrip() {
  const std::string err = bodyBudgetError(85000, 90519);
  size_t budget = 0, size = 0;
  TEST_ASSERT_TRUE(parseBodyBudgetError(err, budget, size));
  TEST_ASSERT_EQUAL_size_t(85000, budget);
  TEST_ASSERT_EQUAL_size_t(90519, size);
}

// OIR-27: the built-in day sum over a five-row day is the hand sum (1,878), never 256.
void test_oir_27_day_sum_receipt_multirow() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,breakfast,skyr and granola,533,32.5\n"
                         "2026-11-04,drink,cappuccino,66,3.3\n"
                         "2026-11-04,lunch,shoyu chicken thighs,557,39.7\n"
                         "2026-11-04,snack,waffles,277,5.7\n"
                         "2026-11-04,dinner,brisket and bread,445,26.0\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(ex, "kcal", "2026-11-04", t));
  TEST_ASSERT_EQUAL_INT(1878, (int)t);
}

// OIR-33: a raw findings dump is capped at the budget, never delivered whole.
void test_oir_33_raw_dump_capped() {
  const std::string big(200000, 'x');
  const std::string out = sanitizeFailText(big.c_str(), big.size(), 85000);
  TEST_ASSERT_TRUE(out.size() <= 85000);
}

// OIR-36: when the standing-rules list is full, a new active rule makes room
// (evicts) instead of being refused into a memory write.
void test_oir_36_rules_full_makes_room() {
  FakeIO io;
  OwnerPrefs p;
  p.begin(&io);
  for (int i = 0; i < 12; i++) {
    std::string id;
    TEST_ASSERT_EQUAL(int(PrefErr::Ok),
                      int(p.add("rule number " + std::to_string(i), PrefOrigin::Web, 1, false, &id)));
  }
  std::vector<PrefEntry> evicted;
  std::string id;
  TEST_ASSERT_EQUAL(int(PrefErr::Ok),
                    int(p.add("Always assume the whole pack unless I say otherwise", PrefOrigin::Chat, 2,
                              false, &id, &evicted)));
  TEST_ASSERT_FALSE(evicted.empty());
}

// OIR-39: a standing rule with a stop marker ("never ask about duolingo") blocks the
// routine whose name or prompt raises that topic, and never blocks an unrelated one.
void test_oir_39_never_ask_blocks_the_routine() {
  const std::vector<std::string> rules = {"never ask about duolingo"};
  const StandingBlock hit = standingBlocksFire(rules, "check the Duolingo streak", "Duolingo streak check");
  TEST_ASSERT_TRUE(hit.blocked);
  TEST_ASSERT_EQUAL_STRING("never ask about duolingo", hit.rule.c_str());
  TEST_ASSERT_FALSE(standingBlocksFire(rules, "check the gym streak", "gym check").blocked);
  TEST_ASSERT_FALSE(standingBlocksFire({"always log meals"}, "log today's meals").blocked);
}

// OIR-40: a message arriving past the batch caps is Deferred (re-served), never
// silently dropped.
void test_oir_40_queued_message_deferred_not_dropped() {
  MessageBatcher b;
  bool sawDeferred = false;
  for (int i = 0; i < 200; i++) {
    const BatchAdd r = b.add("c1", "owner", "message " + std::to_string(i), i + 1);
    if (r == BatchAdd::Deferred) {
      sawDeferred = true;
      break;
    }
  }
  TEST_ASSERT_TRUE(sawDeferred);
}

// OIR-41: a Mistral Studio connector is never promised to a Mistral head turn;
// it is SubsessionsOnly (the call rides a spawned sub-agent).
void test_oir_41_mistral_connector_scope() {
  ConnectorInfo c;
  c.name = "gmail";
  c.prov = "mistral";
  c.kind = "connector";
  c.enabled = true;
  c.auth = 1;
  ProviderState ps;
  ps.mistralKeyed = true;
  ps.currentHost = "mistral";
  ps.mistralHeadCarriesStudio = false;
  TEST_ASSERT_EQUAL(int(CapScope::SubsessionsOnly), int(connectorScope(c, ps)));
}

// OIR-42: after a second dinner is logged, the day total must be 2,335 (both rows), not 1,878
// (the replaced row lost). Since v5.2.0 the second row is a plain append (owner ruling 41).
void test_oir_42_upsert_total_fixed() {
  const std::string ex = std::string(kLedgerHeader) +
                         "2026-11-04,breakfast,skyr and granola,533,32.5\n"
                         "2026-11-04,drink,cappuccino,66,3.3\n"
                         "2026-11-04,lunch,shoyu chicken thighs,557,39.7\n"
                         "2026-11-04,snack,waffles,277,5.7\n"
                         "2026-11-04,dinner,smoked brisket 150 g and bread 58 g,457,40.2\n";
  const std::string nw = "2026-11-04,dinner,Frive Smoky Baharat Mango Chicken 70% of 406 g pack,445,26.0\n";
  double t = -1;
  TEST_ASSERT_TRUE(dayTotalOf(appended(ex, nw), "kcal", "2026-11-04", t));
  TEST_ASSERT_EQUAL_INT(2335, (int)t);
}

// OIR-43 (fixed, lane FSCAP fd75210d): the store's 256-entry count cap must not be a limit
// an owner can hit while the card has room. The 257th entry is accepted.
void test_oir_43_store_count_cap_fixed() {
  FileStore st;
  for (int i = 0; i < 256; i++) {
    FileEntry e;
    e.project = "mem";
    e.name = "file" + std::to_string(i) + ".txt";
    e.bytes = 10;
    std::string err;
    TEST_ASSERT_TRUE_MESSAGE(st.add(e, err), err.c_str());
  }
  FileEntry e;
  e.project = "mem";
  e.name = "file256.txt";
  e.bytes = 10;
  std::string err;
  const bool ok = st.add(e, err);
  TEST_ASSERT_TRUE(ok);
}

// OIR-46: a parked embedding key is never picked.
void test_oir_46_embed_parked_not_used() {
  TEST_ASSERT_FALSE(embedKeyServes(true, 1, "", true, "openai"));
  TEST_ASSERT_TRUE(embedKeyServes(true, 1, "", false, "openai"));
}

// OIR-47: an HTTP 402 parks as billing, with words a person can act on.
void test_oir_47_park_402_billing() {
  TEST_ASSERT_EQUAL(int(ParkKind::Billing), int(parkKindFor(402, "insufficient credits for call")));
  ParkVerdict v;
  v.open = false;
  v.kind = ParkKind::Billing;
  v.httpStatus = 402;
  TEST_ASSERT_FALSE(parkWords("mistral", v).empty());
  TEST_ASSERT_EQUAL_STRING("billing", parkKindSlug(ParkKind::Billing));
}

}  // namespace

// ---- OIR-51 to OIR-55: the history and summary fix (v5.1.2) ---------------------------------

namespace {

constexpr uint32_t kOirDay = 1794787200u;   // 2026-11-16 00:00 UTC (Mon)

// The window a head's turn reads (TurnEngine::historyBudgetOf, engine_history.cpp): the derived
// marks on a head with room, v5.1.1's window with its day floor on a head at the floor. The tool
// loop is on (the default, Lumi's setting); with it off a one-string wire keeps v5.1.1's window
// by lane CTXL's rule, checked by check_budget_table's loop-off table.
agent::HistoryBudget oirWindow(const std::string& host, const std::string& model, uint32_t dayStart) {
  const ContextBudget cb = headContextBudget(host, model, /*loopOn=*/true, {});
  agent::HistoryBudget b;
  b.highBytes = cb.recentConvBytes;
  b.messageCap = cb.historyMessageCap;
  b.dayStartUnix = dayStart;
  if (cb.floorBranch) {
    b.dayBytes = agent::historyDayBytes(legacyCtxTokens(host, model));
    return b;
  }
  b.derived = true;
  b.maxBytes = cb.historyMaxBytes;
  b.softBytes = cb.historySoftBytes;
  b.keepBytes = cb.historyKeepBytes;
  b.highRows = cb.historyRows;
  return b;
}

// v5.1.1's window for the same head (deriveBudget over its window table, the day floor).
agent::HistoryBudget oirWindowV511(const std::string& host, const std::string& model, uint32_t dayStart) {
  agent::HistoryBudget b;
  b.highBytes = deriveBudget(legacyCtxTokens(host, model), {}).recentConvBytes;
  b.dayStartUnix = dayStart;
  b.dayBytes = agent::historyDayBytes(legacyCtxTokens(host, model));
  return b;
}

// A chat row by row, each stamped with its hour (the history reader's input).
struct OirChat {
  std::vector<EpisodicMessage> rows;   // oldest first
  void turn(uint32_t hour, const std::string& owner, const std::string& reply) {
    for (const auto& half : {std::make_pair("user", owner), std::make_pair("assistant", reply)}) {
      EpisodicMessage m;
      char id[16];
      std::snprintf(id, sizeof(id), "m%08x", (unsigned)(rows.size() + 1));
      m.id = id;
      m.sessionId = "123";
      m.tsHours = hour;
      m.role = half.first;
      m.kind = MsgKind::Message;
      m.text = half.second;
      m.tags = std::string(half.first) == "user" ? "from:Owner" : "fx:none,out:reply,turn:" + rows.back().id;
      rows.push_back(m);
    }
  }
  std::vector<agent::HistTurn> turns() const {
    return agent::historyTurns(std::vector<EpisodicMessage>(rows.rbegin(), rows.rend()), agent::HistoryOptions());
  }
};

const char* const kOirEstimate = "Estimate, not logged: blueberries about 190 g, 108 kcal";

// The 15 Nov shape: an unlogged estimate early in a busy food day (20 more exchanges of about
// 850 B, 17 KB in all), and the same evening seen from just after midnight.
OirChat oirFoodDay() {
  OirChat c;
  const uint32_t h = kOirDay / 3600u;
  c.turn(h + 8, "Breakfast was porridge and a banana, log it.", "Logged as breakfast: 456 kcal, 19 g protein.");
  c.turn(h + 10, "A bowl of blueberries with skyr, estimate it, don't log yet.", kOirEstimate);
  for (int i = 0; i < 20; i++)
    c.turn(h + 11 + (uint32_t)(i / 2), "Log entry " + std::to_string(i) + ", please.",
           "Logged entry " + std::to_string(i) + ": " + std::string(800, 'k'));
  return c;
}

bool oirHas(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// One device for the fold rows: an engine over three scripted hosts, its fold store and
// control file, and the fold driver over them (the device's compactTick and the hosted
// foldTick are bindings of this one driver). Every host answers a fold with a thin 2xx (a
// paid request) while `failing`, else with a sectioned summary.
struct OirFoldDevice {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  FoldIO blobIO;
  std::string ctlText;
  FoldStore folds;
  std::unique_ptr<agent::TurnEngine> eng;
  std::unique_ptr<agent::FoldDriver> drv;
  bool failing = true;
  std::vector<std::string> asked;                          // "host/model" per fold request
  std::vector<std::string> told;                           // every owner notice and alert
  std::map<std::string, std::vector<std::string>> rows;    // chat -> row ids, oldest first

  OirFoldDevice() {
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    for (const char* h : {"openai", "anthropic", "mistral"}) {
      const std::string host = h;
      d.hosts.add(h, [this, host](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                                  const agent::HeadTools*, TokenUsage* usage) -> bool {
        if (usage) usage->responses = 1;
        const std::string model = agent::providers::modelOverrideFor(host.c_str());
        asked.push_back(host + "/" + (model.empty() ? "head" : model));
        out = failing ? "Noted."
                      : "1. Decisions & preferences\n- Daily targets 1,900 kcal.\n2. Pending items & next steps\n"
                        "- Weigh-in on Monday.\n3. Facts worth remembering long-term\n- 10 km in March.\n";
        return true;
      });
    }
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
  void boot() {
    folds.begin(&blobIO);
    agent::FoldDriverHooks h;
    h.triggerBytes = [] { return 48u * 1024u; };
    h.foldCycleBytes = [] { return (size_t)53900; };
    h.slice = [this](const std::string& chat, size_t cap, bool) {
      agent::FoldSlice s;
      const std::string thru = folds.get(chat).thru;
      for (const std::string& id : rows[chat]) {
        if (!thru.empty() && !rowIdAfter(id, thru)) continue;
        if (s.digest.size() + 1024 > cap) break;
        s.digest += "row " + id + std::string(1016, '.') + "\n";
        s.thru = id;
        s.rows++;
        s.turns++;
      }
      return s;
    };
    h.tell = [this](const std::string&, const std::string& text) { told.push_back(text); };
    h.alert = [this](const std::string&, const std::string& text) {
      told.push_back(text);
      return true;   // delivered (lane CTXR1: the day's alert is stamped only then)
    };
    h.firmware = "5.1.2";
    FoldCtlIO io;
    io.load = [this] { return ctlText; };
    io.save = [this](const std::string& t) {
      ctlText = t;
      return true;
    };
    drv.reset(new agent::FoldDriver(*eng, folds, io, h));
    drv->begin();
  }
  // `n` rows on the chat with a span hidden behind its soft start: a fold is due.
  void backlog(const std::string& chat, int n, int firstId) {
    for (int i = 0; i < n; i++) {
      char id[16];
      std::snprintf(id, sizeof(id), "m%08x", (unsigned)(firstId + i));
      rows[chat].push_back(id);
    }
    folds.setSoftStart(chat, rows[chat][rows[chat].size() - 3]);
    folds.noteTurn(chat);
  }
  agent::FoldTick tick(uint32_t epoch) {
    plat.ms += 1000;
    return drv->tick(plat.ms, epoch);
  }
  bool toldAbout(const char* part) const {
    for (const auto& t : told)
      if (t.find(part) != std::string::npos) return true;
    return false;
  }
};

}  // namespace

// OIR-51 (15 Nov 2026): asked why its estimate was off, the head no longer had its own
// estimate. The class: on every head with room, an unlogged estimate from earlier in a busy
// day, or from the evening before, is in the window the turn reads, and nothing is shifted
// out to make room. Every cold-start head choice of the four direct providers is walked; a
// head at the floor keeps v5.1.1's window (RULINGS-v3 item 3) and must be named here, so a new
// floor head fails this test instead of quietly losing the day. v5.1.1's window for the same
// head loses the estimate both times (its 16 KB day cap, and midnight).
void test_oir_51_own_estimate_stays_in_the_request() {
  const std::vector<agent::HistTurn> day = oirFoodDay().turns();
  std::string floorHeads;
  int heads = 0;
  for (const char* host : {"mistral", "openai", "anthropic", "zai"}) {
    for (const std::string& model : choiceItems(staticModelChoices(host))) {
      const std::string name = std::string(host) + "/" + model;
      if (headContextBudget(host, model, /*loopOn=*/true, {}).floorBranch) {
        floorHeads += name + " ";
        continue;
      }
      heads++;
      for (const uint32_t dayStart : {kOirDay, kOirDay + 86400u}) {   // 21:00 the same day; 00:05 the next
        const agent::HistoryWindow w = agent::fitHistory(day, "", oirWindow(host, model, dayStart), 0);
        TEST_ASSERT_TRUE_MESSAGE(oirHas(w.block, kOirEstimate), name.c_str());
        TEST_ASSERT_FALSE_MESSAGE(w.shifted, name.c_str());
      }
    }
  }
  TEST_ASSERT_TRUE(heads >= 15);
  // The one cold-start choice without room on d0dea039: its family's measured rates put the
  // latency bound under the 85,000 B floor (docs/reference/request-budgets.md, bound "floor").
  // The release review (finding: Large 4 under the id mistral-large-latest) gives it Large 4's
  // budget in lane CTXR2; then no head is at the floor. Any other floor head fails here.
  const bool known = floorHeads.empty() || floorHeads == "mistral/mistral-large-latest ";
  TEST_ASSERT_TRUE_MESSAGE(known, floorHeads.c_str());
  // v5.1.1 (Lumi's head): gone the same evening, and gone just after midnight.
  for (const uint32_t dayStart : {kOirDay, kOirDay + 86400u}) {
    const agent::HistoryWindow old =
        agent::fitHistory(day, "", oirWindowV511("mistral", "mistral-large-4-0", dayStart), 0);
    TEST_ASSERT_TRUE(old.shifted);
    TEST_ASSERT_FALSE(oirHas(old.block, kOirEstimate));
  }
}

// OIR-52 (2 to 15 Nov 2026): three failed folds paused automatic summaries and nothing lifted
// the pause for 13 days. Every pause an older release can have stored (paused 1 or 2, any
// failure count, with or without hidden rows) loads as a chat that is due now, and the blob
// written back reads as neither paused nor failing on v5.1.1. Then the schedule itself: a
// chat that fails forever is due again within 12 hours 15 minutes, every time.
void test_oir_52_no_summary_pause_survives() {
  const uint32_t now = kOirDay + 13u * 86400u;
  for (const char* paused : {"1", "2"}) {
    for (const char* fails : {"0", "1", "3", "200"}) {
      for (const bool hidden : {false, true}) {
        FoldIO io;
        io.blob = std::string("c1\x1F") + "61234\x1F" "187\x1F" + std::to_string(kOirDay) + ";m000017d9\x1F" +
                  (hidden ? "158;m00001c96" : "158") + "\x1F" + fails + "\x1F" + paused + "\x1F" "the 2 Nov summary\x1E";
        FoldStore st;
        st.begin(&io);
        const std::string what = std::string("paused=") + paused + " fails=" + fails;
        TEST_ASSERT_EQUAL_MESSAGE(int(FoldDue::Yes), int(st.evaluateDue("c1", 48 * 1024, 200, now)), what.c_str());
        TEST_ASSERT_EQUAL_STRING("the 2 Nov summary", st.get("c1").summary.c_str());
        st.noteTurn("c1");   // persists: what a rollback to v5.1.1 would read
        std::vector<std::string> f;
        size_t at = 0;
        for (size_t us; (us = io.blob.find('\x1F', at)) != std::string::npos; at = us + 1) f.push_back(io.blob.substr(at, us - at));
        TEST_ASSERT_TRUE(f.size() >= 7);
        TEST_ASSERT_EQUAL_MESSAGE(0, std::atoi(f[5].c_str()), what.c_str());   // failures, as v5.1.1 reads the field
        TEST_ASSERT_EQUAL_STRING_MESSAGE("0", f[6].c_str(), what.c_str());      // pause
      }
    }
  }
  FoldIO io;
  FoldStore st;
  st.begin(&io);
  st.setSoftStart("c1", "m00000100");
  uint32_t t = now;
  for (int cycle = 0; cycle < 60; cycle++) {
    TEST_ASSERT_EQUAL(int(FoldDue::Yes), int(st.evaluateDue("c1", 48 * 1024, 200, t)));
    const uint32_t next = st.noteCycleFailed("c1", t);
    TEST_ASSERT_TRUE(next > t && next - t <= 12u * 3600u + 15u * 60u);
    TEST_ASSERT_EQUAL(int(FoldDue::No), int(st.evaluateDue("c1", 48 * 1024, 200, next - 1)));
    t = next;
  }
}

// OIR-53 (6 to 15 Nov 2026): a turn fit announced "10 recalled memory item(s)" left out when
// recall was never in the request: the body grew by the note's 158 B and nothing was freed.
// A recall rung that renders nothing gives nothing and says nothing; one that does render is
// dropped, counted and named (so the first half cannot pass by the rung being dead).
void test_oir_53_no_phantom_recall_note() {
  for (const bool rendered : {false, true}) {
    agent::ComposeInputs ci;
    ci.devName = "Nimbus";
    for (int i = 0; i < 10; i++) ci.recalled.push_back("[90%] RECALLED " + std::to_string(i) + " " + std::string(300, 'm'));
    if (!rendered) ci.budgetBytes = 4096;   // the 128K head on 15 Nov: recall had no room to render
    const std::string before = agent::composeInstructions(ci);
    TEST_ASSERT_EQUAL(rendered, oirHas(before, "RECALLED 3"));
    agent::TurnFit fit(ci, "[USER]\nDo you understand why?", nullptr);
    const size_t freed = fit.shrink(968, FitScope::NextRung);
    const size_t after = agent::composeInstructions(ci).size() + fit.inputs().size();
    const size_t was = before.size() + std::string("[USER]\nDo you understand why?").size();
    if (rendered) {
      TEST_ASSERT_TRUE(freed > 0 && after < was);
      TEST_ASSERT_TRUE(oirHas(fit.note(), "recalled"));
    } else {
      TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)freed);
      TEST_ASSERT_TRUE_MESSAGE(after <= was, "the request grew");
      TEST_ASSERT_FALSE(oirHas(fit.note(), "recalled"));
      TEST_ASSERT_FALSE(fit.trimmed());
    }
  }
}

// OIR-54 (2 Nov 2026): the summary was asked of the same model three times in two and a half
// minutes, then switched off. One failing cycle walks its rungs once each (no host and model
// twice), then nothing is asked until the next try 15 minutes later, the wait after that is an
// hour, the owner is never told summaries are paused, and once a provider answers it lands.
void test_oir_54_fold_never_retried_back_to_back() {
  OirFoldDevice d;
  d.boot();
  d.backlog("c1", 60, 0x100);
  agent::FoldTick last = agent::FoldTick::Idle;
  uint32_t t = kOirDay;
  for (int i = 0; i < 12 && last != agent::FoldTick::CycleFailed; i++) last = d.tick(++t);
  TEST_ASSERT_TRUE(last == agent::FoldTick::CycleFailed);
  TEST_ASSERT_TRUE(d.asked.size() >= 3);   // a ladder across providers, not one host asked again
  TEST_ASSERT_EQUAL(d.asked.size(), std::set<std::string>(d.asked.begin(), d.asked.end()).size());
  const uint32_t next = d.folds.get("c1").nextTry;
  TEST_ASSERT_EQUAL_UINT32(t + 15 * 60, next);
  const size_t askedAfterOne = d.asked.size();
  for (uint32_t at = t + 1; at < next; at += 31) d.tick(at);   // the 31.6 s pump passes of 2 Nov
  TEST_ASSERT_EQUAL(askedAfterOne, d.asked.size());
  last = agent::FoldTick::Idle;
  t = next;
  for (int i = 0; i < 12 && last != agent::FoldTick::CycleFailed; i++) last = d.tick(t++);
  TEST_ASSERT_TRUE(last == agent::FoldTick::CycleFailed);
  TEST_ASSERT_TRUE(d.folds.get("c1").nextTry - t >= 3590);   // the second wait: an hour
  TEST_ASSERT_FALSE(d.toldAbout("paused"));
  d.failing = false;
  last = agent::FoldTick::Idle;
  t = d.folds.get("c1").nextTry;
  for (int i = 0; i < 12 && last != agent::FoldTick::Ok; i++) last = d.tick(t++);
  TEST_ASSERT_TRUE(last == agent::FoldTick::Ok);
  TEST_ASSERT_TRUE(oirHas(d.folds.get("c1").summary, "Decisions & preferences"));
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("c1").failedCycles);
}

// OIR-55 (plan review, 16 Nov 2026): the daily bound on paid summary requests was going to be
// counted per chat. Five chats failing all day, a pump pass a minute: at most kFoldPaidPerDay
// paid requests for the whole device that local day, the count survives a restart (it lives in
// the device-wide control file, not in a chat's record), and the next day starts a new count
// (a bound, never a pause).
void test_oir_55_fold_cap_is_device_wide() {
  OirFoldDevice d;
  d.boot();
  for (int c = 0; c < 5; c++) d.backlog("chat" + std::to_string(c), 60, 0x1000 + c * 0x1000);
  for (uint32_t m = 0; m < 12u * 60u; m++) d.tick(kOirDay + m * 60);
  d.drv.reset();   // a restart at noon: the chats' records and the control file are what is left
  d.boot();
  for (uint32_t m = 12u * 60u; m < 24u * 60u; m++) d.tick(kOirDay + m * 60);
  TEST_ASSERT_EQUAL_UINT32(kFoldPaidPerDay, (uint32_t)d.asked.size());
  TEST_ASSERT_EQUAL_UINT32(kFoldPaidPerDay, d.drv->ctl().paidOn(foldLocalDay(kOirDay, 0)));
  for (uint32_t m = 0; m < 24u * 60u; m++) d.tick(kOirDay + 86400u + m * 60);
  TEST_ASSERT_TRUE(d.asked.size() > kFoldPaidPerDay);
  TEST_ASSERT_TRUE(d.asked.size() <= 2 * kFoldPaidPerDay);
  TEST_ASSERT_FALSE(d.toldAbout("paused"));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_oir_01_routine_fire_request_carries_prompt);
  RUN_TEST(test_oir_05_day_total_receipt);
#ifdef OIR_HAVE_OPENQ
  RUN_TEST(test_oir_08_open_question_raise_once_and_drop);
#endif
  RUN_TEST(test_oir_09_total_includes_every_row);
  RUN_TEST(test_oir_14_day_rollover);
  RUN_TEST(test_oir_17_subagent_invented_items);
  RUN_TEST(test_oir_18_receipt_vs_reply_rows);
  RUN_TEST(test_oir_19_silent_replace_fixed);
  RUN_TEST(test_oir_23_cut_retry_note);
  RUN_TEST(test_oir_24_compact_thrash_pause);
  RUN_TEST(test_oir_25_body_budget_roundtrip);
  RUN_TEST(test_oir_27_day_sum_receipt_multirow);
  RUN_TEST(test_oir_33_raw_dump_capped);
  RUN_TEST(test_oir_36_rules_full_makes_room);
  RUN_TEST(test_oir_39_never_ask_blocks_the_routine);
  RUN_TEST(test_oir_40_queued_message_deferred_not_dropped);
  RUN_TEST(test_oir_41_mistral_connector_scope);
  RUN_TEST(test_oir_42_upsert_total_fixed);
  RUN_TEST(test_oir_43_store_count_cap_fixed);
  RUN_TEST(test_oir_46_embed_parked_not_used);
  RUN_TEST(test_oir_47_park_402_billing);
  RUN_TEST(test_oir_51_own_estimate_stays_in_the_request);
  RUN_TEST(test_oir_52_no_summary_pause_survives);
  RUN_TEST(test_oir_53_no_phantom_recall_note);
  RUN_TEST(test_oir_54_fold_never_retried_back_to_back);
  RUN_TEST(test_oir_55_fold_cap_is_device_wide);
  return UNITY_END();
}
