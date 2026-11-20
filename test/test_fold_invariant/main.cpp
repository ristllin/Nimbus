// test_fold_invariant - the fold's one data invariant, through the real engine, fold store
// and fold driver over a store with the device's failure shapes (lane CTXR1, v5.1.2 review).
//
// The invariant: every conversation row after the chat's newest /clear is in the request, or
// covered by the summary, or named as unread; the anchor (thru) never passes a row no landed
// summary took; and no summary ever holds a row from before the /clear. A slice that only
// WAITED (the cache empty after a restart, rows below a truncated index, a failing card, a
// span the card would not give back) once came back empty, the driver read that as "nothing
// to fold" and moved thru to the shift: on the production device that dropped about 1,200
// rows on the first pass after the update.
//
// The class, not the instance: random sequences of turns (in this chat and others, so the
// three-chat conversation cache evicts), restarts with a truncated index, pump passes with
// the backfill, folds, shifts, card outages, unreadable spans, /clear (with an anchor and the
// v5.1.1 way, without), and manual requests, on a head with room and on the floor branch.
// Then the production device's shape: its v5.1.1 blob (thru far behind, paused, softStart
// ahead), 1,213 hidden rows on the real SD log store with the index not complete at boot.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fold_driver.h"
#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/row_overlay.h"

using agent::FoldDriver;
using agent::FoldDriverHooks;
using agent::FoldTick;
using agent::HistoryQuery;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

namespace {

const uint32_t kT0 = 1794787200;   // 2026-11-16 00:00 UTC
const char kMain[] = "1001";

std::string idOf(uint32_t sfx) {
  char b[16];
  std::snprintf(b, sizeof(b), "m%08x", (unsigned)sfx);
  return b;
}

std::string goodSummary(const std::string& digest) {
  std::string s = "1. Decisions & preferences\n- Daily targets 1,900 kcal.\n";
  s += "2. Pending items & next steps\n- Weigh-in on Monday.\n3. Facts worth remembering long-term\n- ";
  s += std::to_string(digest.size()) + " B read.\n";
  return s;
}

// The tokens ("T<n>.") a text holds: each conversation row carries one at its start.
std::set<std::string> tokensIn(const std::string& text) {
  std::set<std::string> out;
  for (size_t at = text.find('T'); at != std::string::npos; at = text.find('T', at + 1)) {
    size_t e = at + 1;
    while (e < text.size() && text[e] >= '0' && text[e] <= '9') e++;
    if (e > at + 1 && e < text.size() && text[e] == '.') out.insert(text.substr(at, e - at + 1));
  }
  return out;
}

struct BlobIO : orch::FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

// ---- a store with the device's failure shapes -------------------------------------------
// Rows of every chat in id order. The index holds the rows from `indexFloor` up (a boot scan
// of the newest rows, the backfill lowering the floor); a `before` read serves indexed rows
// only. An `after` read walks the unindexed rows under a budget (a cut page says newerExists
// with a byte-style cursor, empty or not), skips and names a span the card will not give back,
// and with a failing card only the RAM ring answers (an empty page that says olderExists when
// the ring does not reach the cursor).
struct FaultStore {
  std::vector<orch::EpisodicMessage> rows;
  uint32_t seq = 0;
  uint32_t indexFloor = 0;            // suffix of the oldest indexed row (0 = all)
  size_t coldBudget = 40;             // unindexed rows one after read may walk
  bool failing = false;
  size_t ringRows = 64;
  std::pair<uint32_t, uint32_t> unreadable{0, 0};   // [from, to] suffixes (0,0 = none)

  std::string add(const std::string& chat, const std::string& role, const std::string& text,
                  const std::string& tags = std::string()) {
    orch::EpisodicMessage m;
    m.id = idOf(++seq);
    m.sessionId = chat;
    m.role = role;
    m.kind = orch::MsgKind::Message;
    m.text = text;
    m.tags = tags;
    m.tsHours = (uint32_t)(kT0 / 3600) + seq / 40;
    rows.push_back(m);
    return m.id;
  }
  uint32_t ringFloor() const { return rows.size() > ringRows ? orch::epiIdSuffix(rows[rows.size() - ringRows].id) : 0; }
  bool unreadableAt(uint32_t sfx) const { return unreadable.second && sfx >= unreadable.first && sfx <= unreadable.second; }

  std::vector<orch::EpisodicMessage> read(const std::string& chat, const HistoryQuery& q) {
    std::vector<orch::EpisodicMessage> out;
    if (!q.after.empty()) return after(chat, q);
    const uint32_t floor = failing ? std::max(indexFloor, ringFloor()) : indexFloor;
    const uint32_t before = q.before.empty() ? UINT32_MAX : orch::epiIdSuffix(q.before);
    for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it) {
      const uint32_t sfx = orch::epiIdSuffix(it->id);
      if (sfx < floor) break;
      if (sfx < before && it->sessionId == chat) out.push_back(*it);
    }
    return out;
  }
  std::vector<orch::EpisodicMessage> after(const std::string& chat, const HistoryQuery& q) {
    std::vector<orch::EpisodicMessage> out;
    orch::EpiQueryInfo scratch;
    orch::EpiQueryInfo& info = q.info ? *q.info : scratch;
    const bool byteCur = q.after.find(':') != std::string::npos;
    const uint32_t from = cursorSfx(q.after);
    // The real store (orch_episodic_log.cpp queryAfter): a byte cursor on a store whose index is
    // complete is answered with an empty page and nothing more; while the index is still
    // truncated, the walk past the cold region reads the index from its start.
    if (byteCur && indexFloor == 0 && !failing) {
      info.cursorStale = true;
      return out;
    }
    if (failing && ringFloor() > from + 1) {
      info.olderExists = true;   // the card is failing and the ring does not reach the cursor
      return out;
    }
    Walk w{0, from};
    for (const auto& m : rows) {
      const uint32_t sfx = orch::epiIdSuffix(m.id);
      if (sfx <= from && !(byteCur && sfx >= indexFloor)) continue;   // a byte cursor: the index from its start
      const int cold = coldStep(sfx, w, info);
      if (cold < 0) return out;   // the walk's budget is spent
      if (cold > 0) continue;     // an unreadable row: skipped and named
      w.last = sfx;
      if (m.sessionId != chat) continue;
      out.push_back(m);
      if (out.size() >= q.limit) return out;
    }
    return out;
  }
  static uint32_t cursorSfx(const std::string& after) {
    const size_t colon = after.find(':');
    if (colon == std::string::npos) return orch::epiIdSuffix(after);
    return (uint32_t)std::strtoul(after.c_str() + colon + 1, nullptr, 16);
  }
  struct Walk {
    size_t walked;
    uint32_t last;
  };
  // A row of the cold region: -1 when the walk's budget stops before it, 1 when it is skipped
  // as unreadable, 0 when it is read.
  int coldStep(uint32_t sfx, Walk& w, orch::EpiQueryInfo& info) const {
    if (sfx >= indexFloor || failing) return 0;
    if (++w.walked > coldBudget) {
      info.newerExists = true;
      char c[24];
      std::snprintf(c, sizeof(c), "7:%08x", (unsigned)w.last);
      info.nextAfter = c;
      return -1;
    }
    if (!unreadableAt(sfx)) return 0;
    info.unreadable = "7:0-4096";
    w.last = sfx;
    return 1;
  }
};

// ---- the device around the shared engine, store and driver ------------------------------
struct Device {
  FakePlatform plat;
  FakeConfig cfg;
  FaultStore store;
  BlobIO blob;
  std::string ctl;
  orch::FoldStore folds;
  std::unique_ptr<TurnEngine> eng;
  std::unique_ptr<FoldDriver> drv;
  std::string host, model;
  uint32_t token = 0;
  std::map<std::string, std::string> tokenRow;      // token -> row id
  std::map<std::string, std::set<std::string>> covered;   // chat -> tokens a landed summary took
  std::map<std::string, uint32_t> clearMark;        // chat -> suffix of its newest /clear
  std::vector<std::string> violations;
  int landed = 0, restartsWithHidden = 0, maxHidden = 0, unreadLooks = 0;
  bool quiet = true;
  size_t replyBytes = 2400;
  size_t bootIndexRows = 60;
  size_t backfillRows = 30;
  // Candidate 4, F5: the DMA pool stays fragmented. Every round goes at the send floor, as the
  // wire does it (counted, and a head request over 85,000 B refused with the budget's error).
  bool floorPool = false;
  bool lastTrimmed = false;     // the last turn left history out (TurnEndEv::trimmedForSize)
  std::string lastRequest;      // the last head request a host took: instructions and input
  size_t foldRequestMax = 0;    // the largest fold request sent
  size_t promptPad = 0;         // working notes in the prompt (a device's prompt is 30-56 KB)

  Device(const std::string& h, const std::string& m, bool toolLoop = false) : host(h), model(m) {
    cfg.toolLoop = toolLoop;
    cfg.orchHost = h;
    cfg.orchModels[h] = m;
    cfg.keyed.insert(h);
    cfg.priority = h;
    plat.epoch = kT0;
    boot();
  }

  // The device's headRequestContext: the head's turn budget in the form its turn goes out in.
  orch::ContextBudget headCtx() const { return orch::deriveRequestContext(orch::turnBudgetFor(host, model, cfg.toolLoop), {}); }
  uint32_t trigger() const { return (uint32_t)orch::foldTriggerBytes(headCtx(), orch::kCompactAtKbDefault); }

  void boot() {
    drv.reset();
    eng.reset();
    folds.begin(&blob);
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      for (size_t i = 0; in.runningMemory.size() < promptPad; i++)
        in.runningMemory += "- note " + std::to_string(i) + ": the owner tracks meals, walks and the garden.\n";
      return in;
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& c) { return orch::principalForRole(c, orch::Role::Admin); };
    d.apply.captureAssistant = [this](const std::string& c, const std::string& t) {
      store.add(c, "assistant", t, "out:reply");
      noteTokens(t, store.rows.back().id);
      if (c != "system") folds.noteMessage(c, t.size());
    };
    for (const char* hh : {"openai", "anthropic", "mistral", "cumulo", "zai", "custom"})
      d.hosts.add(hh, [this](std::string&, const std::string& ins, const std::string& in, std::string& out,
                             std::string& err, const agent::HeadTools*, orch::TokenUsage* u) {
        const bool fold = ins == orch::ORCH_COMPACT_PROMPT;
        orch::noteRoundBudget(floorPool);   // the wire counts every round (wire_budget.cpp)
        if (floorPool) {
          const size_t size = ins.size() + in.size();
          if (fold) foldRequestMax = std::max(foldRequestMax, size);
          if (!fold && size > orch::kLegacyWireBodyBytes) {
            err = orch::bodyBudgetError(orch::kLegacyWireBodyBytes, size);
            return false;
          }
        }
        if (u) u->responses = 1;
        if (!fold) lastRequest = ins + "\n" + in;
        if (fold) {
          out = goodSummary(in);
          return true;
        }
        std::string reply = "T" + std::to_string(++token) + ". ";
        while (reply.size() < replyBytes) reply += "Noted, and here is a longer answer about the day. ";
        out = "{\"reply\":\"" + reply + "\",\"memory\":\"\",\"ask\":\"\"}";
        return true;
      });
    d.histV2 = [] { return true; };
    d.captureUserRow = [this](const std::string& c, const std::string& t, const std::string& tags) {
      const std::string id = store.add(c, "user", t, tags);
      noteTokens(t, id);
      if (c != "system") folds.noteOwnerMessage(c, t);
      return id;
    };
    d.historyRows = [this](const std::string& c, const HistoryQuery& q) { return store.read(c, q); };
    d.historyMarks = [this](const std::string& c) {
      const orch::ChatFold f = folds.get(c);
      return agent::HistoryMarks{f.thru, f.softStart, 0, f.lastFoldEpoch};
    };
    d.noteHistoryShift = [this](const std::string& c, const std::string& soft) { folds.setSoftStart(c, soft); };
    d.noteHistoryBytes = [this](const std::string& c, size_t bytes) { folds.noteHistoryBytes(c, bytes); };
    d.foldTriggerBytes = [this] { return trigger(); };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {   // the device's reactive fold
      lastTrimmed = ev.trimmedForSize;
      if (ev.trimmedForSize) folds.noteHistoryTrimmed(ev.chatId);
    };
    eng.reset(new TurnEngine(std::move(d)));
    FoldDriverHooks h;
    h.quiet = [this](bool) { return quiet; };
    h.triggerBytes = [this] { return trigger(); };
    h.foldCycleBytes = [this] { return orch::foldCycleBytes(headCtx()); };
    h.folded = [this](const agent::FoldDone& done) { onLanded(done); };
    h.tell = [this](const std::string& c, const std::string& text) { told.push_back({c, text}); };
    h.firmware = "5.1.2";
    orch::FoldCtlIO io;
    io.load = [this] { return ctl; };
    io.save = [this](const std::string& t) {
      ctl = t;
      return true;
    };
    drv.reset(new FoldDriver(*eng, folds, io, h));
    drv->begin();
  }
  // A restart: the RAM state goes, the store's index is the boot scan's newest rows.
  void restart() {
    if (hiddenRows(kMain) > 0) restartsWithHidden++;
    indexBoot();
    boot();
  }
  void indexBoot() {
    store.indexFloor = store.rows.size() > bootIndexRows ? orch::epiIdSuffix(store.rows[store.rows.size() - bootIndexRows].id) : 0;
  }

  void noteTokens(const std::string& text, const std::string& id) {
    for (const std::string& t : tokensIn(text.substr(0, 16))) tokenRow[t] = id;
  }
  std::map<std::string, std::string> thruSeen;   // chat -> thru after the last step
  std::vector<std::pair<std::string, std::string>> told;   // (chat, owner notice)
  // RB-BGb (a): what each landed fold kept after its new anchor, and whether it was manual.
  struct Landing {
    bool manual;
    bool hidden;   // the fold took rows a shift had hidden (its anchor is the soft start)
    uint32_t keptBytes;
  };
  std::vector<Landing> landings;

  void onLanded(const agent::FoldDone& done) {
    landed++;
    const orch::ChatFold now = folds.get(done.chat);
    landings.push_back({done.manual, !now.softStart.empty() && !agent::rowIdAfter(now.softStart, done.slice.thru),
                        done.slice.keptBytes});
    const uint32_t mark = clearMark[done.chat];
    const auto seen = thruSeen.find(done.chat);
    if (seen != thruSeen.end() && !seen->second.empty() && !done.slice.firstId.empty() &&
        !agent::rowIdAfter(done.slice.firstId, seen->second))
      violations.push_back("a summary starts at " + done.slice.firstId + ", at or before the anchor " + seen->second);
    for (const std::string& t : tokensIn(done.slice.digest)) {
      const auto r = tokenRow.find(t);
      if (r == tokenRow.end()) continue;
      if (orch::epiIdSuffix(r->second) <= mark) violations.push_back("a summary holds " + r->second + ", before the /clear");
      covered[done.chat].insert(t);
    }
  }

  void turn(const std::string& chat) {
    const std::string text = "T" + std::to_string(++token) + ". what about lunch, about " + std::string(150, 'x');
    const size_t before = store.rows.size();
    plat.ms += 1000;
    eng->handleMessage(text, "Roy", chat);
    folds.noteTurn(chat);
    drv->noteTurnChat(chat);
    checkWindow(chat, before);
  }
  void clear(const std::string& chat, bool anchored) {
    const std::string id = store.add(chat, "system", agent::kClearBoundaryText, agent::kClearBoundaryTag);
    clearMark[chat] = orch::epiIdSuffix(id);
    covered[chat].clear();
    eng->clearChatConv(chat);
    folds.applyFold(chat, std::string(), 0, anchored ? id : std::string());
    drv->noteCleared(chat);
  }
  void pump(uint32_t passes, uint32_t stepS) {
    for (uint32_t i = 0; i < passes; i++) {
      plat.epoch += stepS;
      plat.ms += stepS * 1000u;
      drv->tick(plat.ms, (uint32_t)plat.epoch);
      eng->historyIdleStep();
      if (!store.failing && store.indexFloor)   // the backfill indexes the card downward
        store.indexFloor = store.indexFloor > backfillRows ? store.indexFloor - (uint32_t)backfillRows : 0;
      checkCovered();
    }
  }

  // Conversation rows of `chat` after its newest /clear.
  std::vector<const orch::EpisodicMessage*> live(const std::string& chat) const {
    std::vector<const orch::EpisodicMessage*> out;
    const auto cm = clearMark.find(chat);
    const uint32_t mark = cm == clearMark.end() ? 0 : cm->second;
    for (const auto& m : store.rows)
      if (m.sessionId == chat && orch::epiIdSuffix(m.id) > mark && (m.role == "user" || m.role == "assistant")) out.push_back(&m);
    return out;
  }
  int hiddenRows(const std::string& chat) const {
    const orch::ChatFold f = folds.get(chat);
    if (f.softStart.empty() || !(f.thru.empty() || agent::rowIdAfter(f.softStart, f.thru))) return 0;
    int n = 0;
    for (const auto* m : live(chat))
      if (agent::rowIdAfter(m->id, f.thru.empty() ? std::string("m00000000") : f.thru) && !agent::rowIdAfter(m->id, f.softStart)) n++;
    return n;
  }
  // thru never passes a row no landed summary took.
  void checkCovered() {
    for (const std::string& chat : folds.chatIds()) {
      const orch::ChatFold f = folds.get(chat);
      thruSeen[chat] = f.thru;
      if (f.thru.empty()) continue;
      for (const auto* m : live(chat)) {
        if (agent::rowIdAfter(m->id, f.thru)) break;
        const std::set<std::string> t = tokensIn(m->text.substr(0, 16));
        if (t.empty() || covered[chat].count(*t.begin())) continue;
        violations.push_back("thru " + f.thru + " passed " + m->id + " which no summary took");
        return;
      }
    }
    maxHidden = std::max(maxHidden, hiddenRows(kMain));
  }
  // Every row after thru that existed before the turn is in the window or in its left-out span.
  static bool inSpan(const agent::ConvSpan& s, const std::string& id) {
    return !s.empty() && (s.afterId.empty() || agent::rowIdAfter(id, s.afterId)) &&
           (s.beforeId.empty() || agent::rowIdAfter(s.beforeId, id));
  }
  // A row the request must carry or name: after thru, before the kept turns.
  static bool owedToRequest(const std::string& id, const std::string& thru, const std::string& firstKept) {
    if (!thru.empty() && !agent::rowIdAfter(id, thru)) return false;
    return firstKept.empty() || agent::rowIdAfter(firstKept, id);
  }
  void dumpAround(const std::string& id) const {
    const uint32_t at = orch::epiIdSuffix(id);
    for (const auto& x : store.rows) {
      const uint32_t sx = orch::epiIdSuffix(x.id);
      if (sx + 4 >= at && sx <= at + 14)
        std::printf("  row %s %s %s [%s] %.12s\n", x.id.c_str(), x.sessionId.c_str(), x.role.c_str(), x.tags.c_str(),
                    x.text.c_str());
    }
    std::printf("  floor %u failing %d unread %u-%u\n", (unsigned)store.indexFloor, (int)store.failing,
                (unsigned)store.unreadable.first, (unsigned)store.unreadable.second);
  }
  // Every row after thru that existed before the turn is in the window or in its left-out span.
  void checkWindow(const std::string& chat, size_t rowsBefore) {
    checkCovered();
    const agent::HistoryWindow& w = eng->lastHistoryWindow();
    const orch::ChatFold f = folds.get(chat);
    const std::string firstKept = w.kept.empty() ? std::string() : w.kept.front().firstId;
    for (const auto* m : live(chat)) {
      if ((size_t)(m - store.rows.data()) >= rowsBefore) break;
      if (!owedToRequest(m->id, f.thru, firstKept) || inSpan(w.leftOut, m->id)) continue;
      violations.push_back("row " + m->id + " is in neither the request, the summary nor a named span (thru " +
                           f.thru + " soft " + f.softStart + " kept " + firstKept + ")");
      dumpAround(m->id);
      return;
    }
  }
};

struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed * 2654435761u + 1) {}
  uint32_t next(uint32_t n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  }
};

// One random step of the sequence.
void randomStep(Device& d, Lcg& r) {
  static const char* const others[] = {"2002", "3003", "4004"};
  const uint32_t p = r.next(100);
  if (p < 42) return d.turn(kMain);
  if (p < 52) return d.turn(others[r.next(3)]);
  if (p < 78) return d.pump(1 + r.next(4), 30 + r.next(600));
  if (p < 82) return d.restart();
  if (p < 83) {   // the backfill completes between two passes
    d.store.indexFloor = 0;
    return;
  }
  if (p < 87) {
    d.store.failing = !d.store.failing;
    return;
  }
  if (p < 91) {   // a span the card will not give back, right after the anchor
    const orch::ChatFold f = d.folds.get(kMain);
    const uint32_t at = f.thru.empty() ? 1 : orch::epiIdSuffix(f.thru) + 1;
    d.store.unreadable = d.store.unreadable.second ? std::make_pair(0u, 0u) : std::make_pair(at, at + 30);
    return;
  }
  if (p < 94) return d.clear(kMain, p < 93);
  if (p < 97) {
    d.drv->requestManual(kMain);
    return;
  }
  d.quiet = !d.quiet;
}

struct Totals {
  int landed = 0, restartsHidden = 0, hidden = 0;
};

void runSequence(Device& d, uint32_t seed, int steps, Totals& t) {
  Lcg r(seed);
  for (int step = 0; step < steps && d.violations.empty(); step++) randomStep(d, r);
  // Heal and drain: the card comes back and the chat folds forward. Half the seeds leave the
  // backfill stalled for good (a capped or stalled index): liveness must not need it.
  d.store.failing = false;
  d.store.unreadable = {0, 0};
  d.quiet = true;
  if (seed % 2) d.backfillRows = 0;
  d.pump(400, 900);
  // Liveness (fix review, candidate 2): with a readable store, the rows a shift hid are folded
  // and a manual request has ended within those passes.
  if (d.violations.empty() && d.hiddenRows(kMain) > 0) d.violations.push_back("hidden rows still unfolded after the drain");
  if (d.violations.empty() && !d.drv->ctl().manualPendingChat.empty()) d.violations.push_back("a manual request never ended");
  if (!d.violations.empty()) {
    std::printf("seed %u: %s\n", (unsigned)seed, d.violations.front().c_str());
    const auto& L = LogCapture::lines();
    for (size_t i = L.size() > 40 ? L.size() - 40 : 0; i < L.size(); i++) std::printf("  log: %s\n", L[i].c_str());
    TEST_FAIL_MESSAGE(d.violations.front().c_str());
  }
  t.landed += d.landed;
  t.restartsHidden += d.restartsWithHidden;
  t.hidden += d.maxHidden;
}

void runProperty(const std::string& host, const std::string& model, size_t replyBytes, bool wantFloor) {
  TEST_ASSERT_EQUAL_MESSAGE(wantFloor, Device(host, model).headCtx().floorBranch, (host + "/" + model).c_str());
  Totals t;
  for (uint32_t seed = 1; seed <= 12; seed++) {
    Device d(host, model);
    d.replyBytes = replyBytes;
    runSequence(d, seed, 260, t);
  }
  std::printf("%s/%s: %d folds landed, %d restarts with hidden rows, %d hidden rows seen\n", host.c_str(),
              model.c_str(), t.landed, t.restartsHidden, t.hidden);
  // Not vacuous: the sequences fold, and restart with rows a shift hid.
  TEST_ASSERT_TRUE(t.landed > 20);
  TEST_ASSERT_TRUE(t.restartsHidden > 0);
  TEST_ASSERT_TRUE(t.hidden > 0);
}

}  // namespace

// Every row of `chat` from before row index `before`, after thru, is in the last request.
static void assertRowsInRequest(const Device& d, const std::string& chat, size_t before) {
  const orch::ChatFold f = d.folds.get(chat);
  for (const auto* m : d.live(chat)) {
    if ((size_t)(m - d.store.rows.data()) >= before) break;
    if (!f.thru.empty() && !agent::rowIdAfter(m->id, f.thru)) continue;   // covered by the summary
    const std::set<std::string> t = tokensIn(m->text.substr(0, 16));
    if (t.empty()) continue;
    TEST_ASSERT_TRUE_MESSAGE(d.lastRequest.find(*t.begin()) != std::string::npos,
                             ("row " + m->id + " (" + *t.begin() + ") is in neither the request nor the summary, thru " +
                              f.thru).c_str());
  }
}

// The id a request's LEFT OUT note sends the model back to: memory_episodic "before" (rows
// before it are read back with the tool). "" when the request left no history out.
static std::string leftOutBefore(const std::string& request) {
  size_t at = request.find("[LEFT OUT TO FIT THIS REQUEST]");
  if (at == std::string::npos) return std::string();
  at = request.find("\"before\":\"", at);
  if (at == std::string::npos) return std::string();
  at += 10;
  return request.substr(at, request.find('"', at) - at);
}

// Every live row after thru that existed before the last turn is in its request, or named by
// its LEFT OUT note (a row before the note's "before" id), or in the window's own left-out span.
static void assertRowsReachable(const Device& d, const std::string& chat, size_t before) {
  const orch::ChatFold f = d.folds.get(chat);
  const std::string named = leftOutBefore(d.lastRequest);
  const agent::HistoryWindow& w = d.eng->lastHistoryWindow();
  for (const auto* m : d.live(chat)) {
    if ((size_t)(m - d.store.rows.data()) >= before) break;
    if (!f.thru.empty() && !agent::rowIdAfter(m->id, f.thru)) continue;   // covered by the summary
    if (!named.empty() && agent::rowIdAfter(named, m->id)) continue;     // named by the LEFT OUT note
    if (Device::inSpan(w.leftOut, m->id)) continue;                       // named by the window's span
    const std::set<std::string> t = tokensIn(m->text.substr(0, 16));
    if (t.empty()) continue;
    TEST_ASSERT_TRUE_MESSAGE(d.lastRequest.find(*t.begin()) != std::string::npos,
                             ("row " + m->id + " (" + *t.begin() + ") is in neither the request, its LEFT OUT note "
                              "nor the summary, thru " + f.thru).c_str());
  }
}

// Every automatic fold that took visible rows kept the head's full keep (less one turn, the
// tail keeps whole turns) or the day floor, never v5.1.1's 4,096 B (about 1,600 tokens).
static void assertFullKeep(const Device& d, size_t from) {
  const size_t keep = d.headCtx().historyKeepBytes;
  for (size_t i = from; i < d.landings.size(); i++) {
    const Device::Landing& l = d.landings[i];
    if (l.manual || l.hidden) continue;
    TEST_ASSERT_TRUE_MESSAGE(l.keptBytes + 8192 >= keep,
                             ("an automatic fold kept " + std::to_string(l.keptBytes) + " B of a " +
                              std::to_string(keep) + " B keep").c_str());
  }
}

// RB-BGb (a), ruling 83 item 9: on a pool that stays fragmented every round goes at the send
// floor (85,000 B). After any number of floor sends the automatic fold waits for a request above
// the floor: no automatic fold lands with a shortened tail, each floor request names what it
// leaves out, and no row is in neither the request nor the summary. When the pool recovers the
// next fold keeps the head's full keep (59,040 B, about 23,600 tokens on claude-sonnet-5-5).
static void runFloorProperty(size_t promptPad, int healthyTurns, int floorTurns) {
  Device d("anthropic", "claude-sonnet-5-5");
  d.promptPad = promptPad;
  d.boot();
  for (int i = 0; i < healthyTurns; i++) {   // a healthy head builds its full-size history first
    d.turn(kMain);
    d.pump(1, 60);
  }
  const size_t floorFrom = d.landings.size();
  d.floorPool = true;
  int trimmed = 0;
  for (int i = 0; i < floorTurns; i++) {
    const size_t before = d.store.rows.size();
    d.turn(kMain);
    trimmed += d.lastTrimmed;
    assertRowsReachable(d, kMain, before);
    d.pump(1, 60);
  }
  std::printf("  floor turns that left history out: %d of %d, folds landed at the floor: %u\n", trimmed, floorTurns,
              (unsigned)(d.landings.size() - floorFrom));
  assertFullKeep(d, 0);
  TEST_ASSERT_TRUE_MESSAGE(trimmed > 0, "the floor cuts the history");
  TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("deferred"), "the deferral is logged");
  // The pool recovers: the next request goes above the floor, and the fold that follows keeps
  // the full keep.
  d.floorPool = false;
  const size_t at = d.landings.size();
  d.turn(kMain);
  // A busy day at the floor may have spent the daily fold cap: the next fold may be tomorrow's.
  for (int p = 0; p < 200 && d.landings.size() == at; p++) d.pump(1, 600);
  TEST_ASSERT_TRUE_MESSAGE(d.landings.size() > at, "a fold lands once a request goes above the floor");
  assertFullKeep(d, at);
  const size_t before = d.store.rows.size();
  d.turn(kMain);
  assertRowsInRequest(d, kMain, before);
  TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
}

static void test_floor_sends_defer_the_fold_and_the_next_fold_keeps_the_full_keep() { runFloorProperty(24000, 0, 160); }

// The Lumi case (candidate 5, G6): the pool fragments after a healthy head built its full-size
// history, with a device-sized prompt (about 20 KB of history fits the floor).
static void test_a_pool_that_fragments_late_defers_the_fold_too() { runFloorProperty(52000, 120, 40); }

// The deferred chat is looked at once a minute, not on every driver pass (one log line each).
static void test_a_deferred_fold_is_looked_at_once_a_minute() {
  Device d("anthropic", "claude-sonnet-5-5");
  d.floorPool = true;
  d.promptPad = 24000;
  d.boot();
  for (int i = 0; i < 60; i++) {
    d.turn(kMain);
    d.pump(1, 60);
  }
  LogCapture::lines().clear();
  d.pump(60, 5);   // five minutes of passes 5 s apart
  int looks = 0;
  for (const auto& l : LogCapture::lines()) looks += l.find("deferred - the last request went at the send floor") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(looks >= 1 && looks <= 6, std::to_string(looks).c_str());
}

// Counter-test: a Summarize now on the fragmented pool is never deferred. It lands, its request
// fits the floor, and the next request holds every row after the new anchor.
static void test_a_summarize_now_at_the_floor_still_lands() {
  Device d("anthropic", "claude-sonnet-5-5");
  d.floorPool = true;
  d.promptPad = 24000;
  d.boot();
  for (int i = 0; i < 400 && !d.lastTrimmed; i++) d.turn(kMain);
  TEST_ASSERT_TRUE_MESSAGE(d.lastTrimmed, "a turn is cut at the floor");
  const int landed = d.landed;
  d.drv->requestManual(kMain);
  for (int p = 0; p < 40 && d.landed == landed; p++) d.pump(1, 60);
  TEST_ASSERT_TRUE_MESSAGE(d.landed > landed, "the manual fold lands");
  TEST_ASSERT_TRUE_MESSAGE(d.landings.back().manual, "the landing is the manual one");
  const size_t before = d.store.rows.size();
  d.turn(kMain);
  assertRowsInRequest(d, kMain, before);
  TEST_ASSERT_TRUE_MESSAGE(d.foldRequestMax > 0 && d.foldRequestMax <= orch::kLegacyWireBodyBytes,
                           std::to_string(d.foldRequestMax).c_str());
  TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
}

// Counter-test: on a healthy pool nothing is deferred; the automatic folds land and keep the
// full keep.
static void test_a_healthy_pool_folds_with_the_full_keep() {
  Device d("anthropic", "claude-sonnet-5-5");
  d.promptPad = 24000;
  d.boot();
  for (int i = 0; i < 120; i++) {
    d.turn(kMain);
    d.pump(1, 60);
  }
  TEST_ASSERT_TRUE_MESSAGE(d.landings.size() >= 2, "the automatic folds land");
  assertFullKeep(d, 0);
  TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("deferred"), "nothing is deferred");
}

// The property on a head with room (lane H's derived marks: Sonnet 5.5, 118 KB window).
static void test_invariant_holds_on_a_head_with_room() { runProperty("anthropic", "claude-sonnet-5-5", 6000, false); }

// The property on the floor branch (v5.1.1's window: a shift every few turns).
static void test_invariant_holds_on_the_floor_branch() {
  nimbus::orch::setRoutedBudgetLiftForTest(0);   // RB-BG: the routed floor, whatever kRoutedBudgetLift says
  runProperty("cumulo", "gpt-5.6", 900, true);
  nimbus::orch::setRoutedBudgetLiftForTest(-1);
}

// The restart case alone, deterministic: rows a shift hid, then a restart (the cache empty, the
// index the boot scan's newest rows). The first pass folds the hidden rows, oldest first, and
// never moves thru past one it did not summarize.
static void test_a_restart_with_hidden_rows_folds_them_first() {
  Device d("anthropic", "claude-sonnet-5-5");
  for (int i = 0; i < 60; i++) d.turn(kMain);
  const orch::ChatFold before = d.folds.get(kMain);
  TEST_ASSERT_FALSE_MESSAGE(before.softStart.empty(), "a shift hid rows");
  TEST_ASSERT_TRUE(d.hiddenRows(kMain) > 0);
  d.restart();
  LogCapture::lines().clear();
  d.pump(3, 60);
  if (d.landed < 1)
    for (const auto& l : LogCapture::lines()) std::printf("  log: %s\n", l.c_str());
  const orch::ChatFold after = d.folds.get(kMain);
  TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
  TEST_ASSERT_TRUE_MESSAGE(d.landed >= 1, "the first passes fold the hidden rows");
  d.pump(60, 300);
  TEST_ASSERT_TRUE(d.violations.empty());
  TEST_ASSERT_EQUAL(0, d.hiddenRows(kMain));
}

// /clear bounds the fold again: a long conversation, /clear (the v5.1.1 way: no anchor, and the
// way the bindings clear now: anchored at the boundary row), new turns until a shift, then an
// automatic and a manual fold. No row from before the /clear is read into a summary.
static void test_clear_bounds_the_fold() {
  for (int anchored = 0; anchored < 2; anchored++) {
    Device d("anthropic", "claude-sonnet-5-5");
    d.replyBytes = 1200;
    for (int i = 0; i < 140; i++) d.turn(kMain);   // about 190 KB before the /clear
    d.clear(kMain, anchored == 1);
    d.restart();   // the cache cannot say where the /clear is
    for (int i = 0; i < 70; i++) d.turn(kMain);
    d.pump(30, 300);
    d.drv->requestManual(kMain);
    d.pump(30, 300);
    TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
    TEST_ASSERT_TRUE_MESSAGE(d.landed > 0, "the post-clear conversation folds");
    const orch::ChatFold f = d.folds.get(kMain);
    TEST_ASSERT_TRUE(orch::epiIdSuffix(f.thru) > d.clearMark[kMain]);
  }
}

// A forward read that cannot reach the rows never gives them up and never stops the chat
// folding for good (finding: an unreadable span, a read budget, a failing card). With rows a
// shift hid and the anchor right before them:
//  - a span the card will not give back: the pass is Deferred, thru stays, the next look waits
//    1 minute, then 2, 4 ... (no card read in between), and once the card gives the rows back
//    the fold takes them from the row after thru;
//  - a failing card (the RAM ring does not reach thru): the same, never "nothing to fold";
//  - a cold walk cut every two rows with the backfill stalled: the read goes on from where
//    the store stopped, and the fold lands.
static void test_a_read_that_cannot_reach_the_rows_waits_and_retries() {
  for (int shape = 0; shape < 3; shape++) {
    Device d("anthropic", "claude-sonnet-5-5");
    for (int i = 0; i < 60; i++) d.turn(kMain);
    d.pump(20, 300);   // the first folds: thru set
    for (int i = 0; i < 40; i++) d.turn(kMain);
    const orch::ChatFold f0 = d.folds.get(kMain);
    TEST_ASSERT_FALSE(f0.thru.empty());
    TEST_ASSERT_TRUE_MESSAGE(d.hiddenRows(kMain) > 0, "rows hidden after thru");
    const uint32_t from = orch::epiIdSuffix(f0.thru) + 1;
    d.bootIndexRows = 8;
    d.restart();
    if (shape == 0) d.store.unreadable = {from, from + 6};
    if (shape == 1) d.store.failing = true;
    if (shape == 2) {   // a cold walk cut every two rows, the backfill stalled
      d.store.coldBudget = 2;
      d.backfillRows = 0;
    }
    const int landed0 = d.landed;
    int deferred = 0;
    for (int pass = 0; pass < 6; pass++) {
      d.plat.ms += 20000;
      d.plat.epoch += 20;
      const FoldTick t = d.drv->tick(d.plat.ms, (uint32_t)d.plat.epoch);
      if (t == FoldTick::Deferred) deferred++;
      TEST_ASSERT_TRUE_MESSAGE(t != FoldTick::Skipped, "never nothing to fold");
      d.checkCovered();
      if (shape == 2 && d.landed > landed0) break;   // the walk got there: a landed fold moved it
      TEST_ASSERT_EQUAL_STRING_MESSAGE(f0.thru.c_str(), d.folds.get(kMain).thru.c_str(),
                                       ("the anchor stays, shape " + std::to_string(shape)).c_str());
    }
    if (shape < 2) {
      TEST_ASSERT_EQUAL_MESSAGE(landed0, d.landed, "no fold while the rows cannot be read");
      TEST_ASSERT_TRUE(deferred > 0);
      TEST_ASSERT_TRUE(LogCapture::contains("next look in 60 s"));
      d.store.unreadable = {0, 0};
      d.store.failing = false;
    }
    d.pump(40, 300);
    TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
    TEST_ASSERT_TRUE_MESSAGE(d.landed > landed0, "the rows fold once they can be read");
    TEST_ASSERT_EQUAL(0, d.hiddenRows(kMain));
  }
}

// Steady state on every head with room the rig can drive (lane H's "fold before shift"): the
// trigger counts the bytes after thru, so after a fold it fires again at the soft mark, before
// the window reaches its maximum. N consecutive folds with no overflow shift between them. The
// counter used to restart at 0 while the kept tail stayed after thru, so every fold after the
// first came only once a shift had hidden the oldest fifth of the window.
static void test_steady_state_folds_come_before_any_shift() {
  const char* const heads[][2] = {{"anthropic", "claude-sonnet-5-5"}, {"openai", "gpt-5.6"},
                                  {"mistral", "mistral-medium-latest"}};
  for (const auto& hm : heads) {
    Device d(hm[0], hm[1], /*toolLoop=*/true);
    TEST_ASSERT_FALSE_MESSAGE(d.headCtx().floorBranch, hm[1]);
    d.replyBytes = 3000;
    int shiftsAfterFirst = 0;
    for (int i = 0; i < 600 && d.landed < 8; i++) {
      LogCapture::lines().clear();
      d.turn(kMain);
      if (d.landed > 0 && LogCapture::contains("overflow shift")) shiftsAfterFirst++;
      d.pump(1, 120);
    }
    std::printf("%s/%s: %d folds, %d shifts after the first fold\n", hm[0], hm[1], d.landed, shiftsAfterFirst);
    TEST_ASSERT_TRUE_MESSAGE(d.landed >= 8, hm[1]);
    TEST_ASSERT_EQUAL_MESSAGE(0, shiftsAfterFirst, hm[1]);
    TEST_ASSERT_TRUE(d.violations.empty());
  }
}

// One Summarize now summarizes everything up to the newest message, hidden rows or not (lane
// CTXR1: the step that makes a rollback to v5.1.1 safe, docs/compaction.md). A long window
// with nothing hidden used to fold one digest cap per press.
static void test_one_summarize_now_reaches_the_newest_row() {
  Device d("anthropic", "claude-sonnet-5-5");
  d.replyBytes = 1500;
  d.quiet = false;   // no automatic fold meanwhile
  for (int i = 0; i < 70; i++) d.turn(kMain);
  TEST_ASSERT_EQUAL(0, d.hiddenRows(kMain));
  d.drv->requestManual(kMain);
  for (int i = 0; i < 200 && !d.drv->ctl().manualPendingChat.empty(); i++) d.pump(1, 130);
  TEST_ASSERT_TRUE(d.violations.empty());
  TEST_ASSERT_TRUE_MESSAGE(d.landed > 1, "more than one digest cap");
  const std::vector<const orch::EpisodicMessage*> rows = d.live(kMain);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(rows.back()->id.c_str(), d.folds.get(kMain).thru.c_str(), "thru at the newest row");
}

// Review (lane CTXR1): a chat with no anchor on a failing card. The read gets nowhere, so it is
// not progress: the chat backs off, and a manual request is answered instead of retried on
// every pass (which would starve every other chat's fold).
static void test_no_anchor_on_a_failing_card_backs_off_and_answers() {
  Device d("anthropic", "claude-sonnet-5-5");
  d.quiet = false;
  for (int i = 0; i < 30; i++) d.turn(kMain);
  d.bootIndexRows = 4;
  d.restart();
  d.store.failing = true;
  d.store.ringRows = 2;
  d.drv->requestManual(kMain);
  int deferred = 0;
  for (int i = 0; i < 10 && !d.drv->ctl().manualPendingChat.empty(); i++)
    deferred += d.drv->tick(d.plat.ms += 1000, (uint32_t)(d.plat.epoch += 1)) == FoldTick::Deferred;
  TEST_ASSERT_TRUE_MESSAGE(d.drv->ctl().manualPendingChat.empty(), "the manual request is answered");
  TEST_ASSERT_EQUAL_MESSAGE(1, deferred, "on the first look: a read that got nowhere is not progress");
  TEST_ASSERT_TRUE(LogCapture::contains("next look in 60 s"));
  TEST_ASSERT_TRUE(d.violations.empty());
}

// v5.1.2 fix review A: a byte cursor ("day:off") a cold walk handed back outlives the store
// state it was issued under. Between two passes the backfill completes; the store then answers
// the stored cursor with an empty page and nothing more. That is never the end of the read:
// the hidden rows behind the anchor are folded, not given up, and no row before a /clear (for
// a chat with no anchor, whose /clear scan held such a cursor) reaches a summary.
namespace {
void otherChatRows(Device& d, int n) {
  for (int i = 0; i < n; i++) d.store.add("9999", i % 2 ? "assistant" : "user", "other chat " + std::to_string(i));
}
}  // namespace

static void test_a_backfill_completing_between_passes_never_ends_the_read() {
  for (int shape = 0; shape < 2; shape++) {   // 0: hidden rows behind an anchor; 1: no anchor, an old /clear
    Device d("anthropic", "claude-sonnet-5-5");
    d.quiet = false;
    for (int i = 0; i < 20; i++) d.turn(kMain);
    if (shape == 0) {
      d.quiet = true;
      d.drv->requestManual(kMain);
      d.pump(10, 300);   // a first fold: an anchor
      d.quiet = false;
      TEST_ASSERT_FALSE(d.folds.get(kMain).thru.empty());
    } else {
      d.clear(kMain, false);   // the v5.1.1 way: no anchor
    }
    otherChatRows(d, 700);   // a cold region with none of this chat's rows
    for (int i = 0; i < 60; i++) d.turn(kMain);
    TEST_ASSERT_TRUE_MESSAGE(shape == 1 || d.hiddenRows(kMain) > 0, "rows hidden after thru");
    d.bootIndexRows = 30;
    d.backfillRows = 0;   // the backfill stalls, then completes at once below
    d.store.coldBudget = 10;
    d.restart();
    d.quiet = true;
    d.drv->requestManual(kMain);
    d.pump(1, 60);   // a pass that walks cold rows of the other chat only: a byte cursor is kept
    const bool cursorKept = LogCapture::contains("waits for its rows (progress)");
    d.store.indexFloor = 0;   // the backfill completes
    d.pump(80, 300);
    TEST_ASSERT_TRUE_MESSAGE(cursorKept, "the first pass kept a byte cursor");
    TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
    TEST_ASSERT_TRUE_MESSAGE(d.landed > 0, "the chat folds once the cursor is dropped");
    TEST_ASSERT_EQUAL(0, d.hiddenRows(kMain));
  }
}

// Fix review, candidate 2: on a truncated index whose backfill never completes, the real end of
// a read reached through a byte cursor is the end, not a stale cursor. A Summarize now on a chat
// whose rows all lie in the cold region, far before its end (the web chat of an owner who uses
// Telegram), is answered and the slice lands or reaches Nothing; and an automatic fold of
// another chat runs meanwhile. For a chat with an anchor and for a no-anchor /clear scan.
static void test_a_manual_read_on_cold_rows_ends_and_never_starves_the_others() {
  for (int shape = 0; shape < 2; shape++) {
    Device d("anthropic", "claude-sonnet-5-5");
    d.quiet = false;
    const std::string web = "5005";
    for (int i = 0; i < 12; i++) d.turn(web);
    if (shape == 0) {   // an anchor at its fourth exchange (the summary took those rows)
      const std::vector<const orch::EpisodicMessage*> rows = d.live(web);
      for (size_t i = 0; i <= 7; i++)
        for (const std::string& t : tokensIn(rows[i]->text.substr(0, 16))) d.covered[web].insert(t);
      d.folds.applyFold(web, "1. Decisions & preferences\n- web notes.\n", kT0, rows[7]->id);
    }
    otherChatRows(d, 600);                     // more than two day files of the cold walk: none of web's
    for (int i = 0; i < 60; i++) d.turn(kMain);   // the main chat, rows hidden by a shift
    TEST_ASSERT_TRUE(d.hiddenRows(kMain) > 0);
    d.bootIndexRows = 200;   // the main chat's rows indexed, web's cold
    d.backfillRows = 0;      // the backfill stalls for good
    d.store.coldBudget = 30;
    d.restart();
    d.quiet = true;
    d.drv->requestManual(web);
    const int landed0 = d.landed;
    d.pump(200, 60);
    TEST_ASSERT_TRUE_MESSAGE(d.violations.empty(), d.violations.empty() ? "" : d.violations.front().c_str());
    TEST_ASSERT_TRUE_MESSAGE(d.drv->ctl().manualPendingChat.empty(), "the manual request ended");
    bool answered = false;
    for (const auto& t : d.told) answered = answered || t.first == web;
    TEST_ASSERT_TRUE_MESSAGE(answered, "and was answered");
    TEST_ASSERT_TRUE_MESSAGE(d.landed > landed0, "the automatic folds ran");
    TEST_ASSERT_EQUAL_MESSAGE(0, d.hiddenRows(kMain), "the main chat's hidden rows are folded");
  }
}

// ---- the production device's shape on the real SD log store ----------------------------
namespace {
struct FakeFs : orch::EpiFs {
  std::map<std::string, std::string> files;
  long size(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  long append(const std::string& p, const std::string& b) override {
    std::string& f = files[p];
    const long off = (long)f.size();
    f += b;
    return off;
  }
  std::string readAll(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& p, long off, long len) const override {
    auto it = files.find(p);
    if (it == files.end() || off < 0 || len < 0 || (size_t)(off + len) > it->second.size()) return std::string();
    return it->second.substr((size_t)off, (size_t)len);
  }
  void forEachName(const std::string& dir, const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      if (std::strchr(it->first.c_str() + prefix.size(), '/') == nullptr) fn(it->first.c_str() + prefix.size());
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.push_back(n); });
    return out;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool exists(const std::string& p) const override { return files.count(p) > 0; }
};
}  // namespace

// Lumi on the first boot of v5.1.2: the recorded v5.1.1 blob (thru m000017d9, softStart
// m00001c96, fails=3 paused=1, a 13-day-old summary) over its card: rows m000017da..m00001ca0
// of the chat across 13 day files, the boot index holding only the newest 300 rows, the
// backfill not run. The first passes fold forward from the row right after thru, every
// landed slice starting exactly where the last one ended, until the shift is covered; thru
// never passes a row no summary took.
namespace {
// Lumi's card: rows m00001700..m00001ca0 (every seventh another chat's) across 13 day files.
void writeLumiCard(orch::RowTextOverlay& overlay, const std::string& chat) {
  const uint32_t hour0 = (uint32_t)(kT0 / 3600) - 14 * 24;
  for (uint32_t sfx = 0x1700; sfx <= 0x1ca0; sfx++) {
    orch::EpisodicMessage m;
    m.id = idOf(sfx);
    m.sessionId = sfx % 7 == 0 ? std::string("web") : chat;
    m.role = sfx % 2 ? "assistant" : "user";
    m.kind = orch::MsgKind::Message;
    m.text = "T" + std::to_string(sfx) + ". " + (sfx % 2 ? "Logged it: " + std::string(300, 'k') : "Breakfast two eggs");
    m.tsHours = hour0 + (sfx - 0x1700) / 4;   // about 13 days of rows
    overlay.addMessage(m);
  }
}

// The device's engine over the store, the blob's fold state and a Sonnet head.
TurnEngine::Deps lumiDeps(FakePlatform& plat, FakeConfig& cfg, orch::RowTextOverlay& overlay, orch::FoldStore& folds) {
  cfg.orchHost = "anthropic";
  cfg.orchModels["anthropic"] = "claude-sonnet-5-5";
  TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  for (const char* h : {"openai", "anthropic", "mistral"})
    d.hosts.add(h, [](std::string&, const std::string& ins, const std::string& in, std::string& out, std::string&,
                      const agent::HeadTools*, orch::TokenUsage* u) {
      if (u) u->responses = 1;
      out = ins == orch::ORCH_COMPACT_PROMPT ? goodSummary(in) : "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
  d.histV2 = [] { return true; };
  d.captureUserRow = [](const std::string&, const std::string&, const std::string&) { return std::string("m00001ca1"); };
  d.historyRows = [&overlay](const std::string& c, const HistoryQuery& hq) {
    orch::MsgQuery q;
    q.sessionId = c;
    q.haveKind = true;
    q.kind = orch::MsgKind::Message;
    q.alsoKinds = {orch::MsgKind::Image, orch::MsgKind::File, orch::MsgKind::Audio};
    q.limit = (int)hq.limit;
    q.before = hq.before;
    q.after = hq.after;
    return overlay.query(q, hq.info);
  };
  d.historyMarks = [&folds](const std::string& c) {
    const orch::ChatFold f = folds.get(c);
    return agent::HistoryMarks{f.thru, f.softStart, 0, f.lastFoldEpoch};
  };
  d.noteHistoryShift = [&folds](const std::string& c, const std::string& soft) { folds.setSoftStart(c, soft); };
  return d;
}

// No gap: each slice starts at the chat's first row after the previous anchor.
void assertLumiSlicesHaveNoGap(const std::vector<agent::FoldDone>& done) {
  std::string prevThru = "m000017d9";
  for (const agent::FoldDone& fd : done) {
    uint32_t next = orch::epiIdSuffix(prevThru) + 1;
    while (next % 7 == 0) next++;   // the web chat's rows are not this chat's
    const std::string first = "T" + std::to_string(next) + ".";
    TEST_ASSERT_TRUE_MESSAGE(fd.slice.digest.find(first) != std::string::npos &&
                                 fd.slice.digest.find(first) == fd.slice.digest.find('T'),
                             ("an id gap after " + prevThru).c_str());
    prevThru = fd.slice.thru;
  }
}
}  // namespace

// Lumi on the first boot of v5.1.2: the recorded v5.1.1 blob (thru m000017d9, softStart
// m00001c96, fails=3 paused=1, a 13-day-old summary) over its card: rows m000017da..m00001ca0
// of the chat across 13 day files, the boot index holding only the newest 300 rows, the
// backfill not run. The first passes fold forward from the row right after thru, every
// landed slice starting exactly where the last one ended, until the shift is covered; thru
// never passes a row no summary took.
static void test_production_device_shape_folds_forward_with_no_gap() {
  FakeFs fs;
  orch::AppendLogEpisodicStore log(fs, "/mem/episodic", 512);
  orch::RowTextOverlay overlay;
  overlay.bind(&log);
  std::ifstream bf("test/support/fixtures/fold_blob_v5_1_1_lumi.txt", std::ios::binary);
  std::stringstream ss;
  ss << bf.rdbuf();
  BlobIO blob;
  blob.blob = ss.str();
  orch::FoldStore folds;
  folds.begin(&blob);
  std::string chat;
  for (const std::string& c : folds.chatIds())
    if (folds.get(c).thru == "m000017d9") chat = c;
  TEST_ASSERT_FALSE(chat.empty());
  TEST_ASSERT_EQUAL_STRING("m00001c96", folds.get(chat).softStart.c_str());
  writeLumiCard(overlay, chat);
  log.hydrate(300, 0);   // the boot scan: the newest rows only, the backfill not run
  FakePlatform plat;
  plat.epoch = kT0;
  FakeConfig cfg;
  TurnEngine eng(lumiDeps(plat, cfg, overlay, folds));
  std::vector<agent::FoldDone> done;
  FoldDriverHooks h;
  h.triggerBytes = [] { return 94464u; };
  h.foldCycleBytes = [] { return (size_t)35424; };   // Sonnet 5.5's step
  h.folded = [&](const agent::FoldDone& fd) { done.push_back(fd); };
  std::string ctl;
  orch::FoldCtlIO io;
  io.load = [&] { return ctl; };
  io.save = [&](const std::string& t) {
    ctl = t;
    return true;
  };
  FoldDriver drv(eng, folds, io, h);
  drv.begin();
  uint32_t ms = 1000, epoch = kT0;
  for (int pass = 0; pass < 2000 && agent::rowIdAfter("m00001c96", folds.get(chat).thru); pass++) {
    ms += 60000;
    epoch += 60;
    drv.tick(ms, epoch);
    eng.historyIdleStep();
    // thru never passes a row no landed digest took.
    TEST_ASSERT_FALSE_MESSAGE(agent::rowIdAfter(folds.get(chat).thru, done.empty() ? std::string("m000017d9") : done.back().slice.thru),
                              "the anchor moved with no summary");
  }
  TEST_ASSERT_FALSE_MESSAGE(agent::rowIdAfter("m00001c96", folds.get(chat).thru), "the backlog is folded");
  TEST_ASSERT_TRUE(done.size() > 3);
  assertLumiSlicesHaveNoGap(done);
  // The log names each slice's range (bench B11 reads it): the first starts right after the blob's thru.
  TEST_ASSERT_TRUE(LogCapture::contains(("fold(" + chat + "): slice of the rows after m000017d9 through ").c_str()));
  TEST_ASSERT_FALSE(LogCapture::contains("nothing new beyond the kept history - skipped"));
  std::printf("production shape: %u folds to %s\n", (unsigned)done.size(), folds.get(chat).thru.c_str());
}

// Bench B11 on candidate 2 (d5afa30a): the serial chat, a v5.1.1 plant with about 1,200 rows
// hidden behind its anchor, an index that stops at its 20,000-row cap (it never completes), and
// six paid folds already counted that local day. The automatic fold never started: the binding
// counted the serial console as a guest, and the guests' share of the cap (6) was spent. With
// the chat counted as the owner's (the device's own surfaces are Admin), the fold starts and
// works the backlog off over the capped index, each slice starting where the last one ended,
// the anchor never passing a row no summary took. As a guest, each pass now says why it does
// not start.
namespace {
struct CappedBench {
  FakeFs fs;
  orch::AppendLogEpisodicStore log{fs, "/mem/episodic", 512};
  orch::RowTextOverlay overlay;
  BlobIO blob;
  orch::FoldStore folds;
  std::string ctl;
  std::vector<agent::FoldDone> done;
  static constexpr uint32_t kThru = 3000, kSoft = 4200, kRows = 24500;
  CappedBench() {
    overlay.bind(&log);
    const uint32_t hour0 = (uint32_t)(kT0 / 3600) - 20 * 24;
    for (uint32_t sfx = 1; sfx <= kRows; sfx++) {
      orch::EpisodicMessage m;
      m.id = idOf(sfx);
      m.sessionId = sfx >= 2500 && sfx <= 4300 ? std::string("serial") : std::string("6098532481");
      m.role = sfx % 2 ? "assistant" : "user";
      m.kind = orch::MsgKind::Message;
      m.text = "T" + std::to_string(sfx) + ". " + std::string(sfx % 2 ? 160 : 40, 'x');
      m.tsHours = hour0 + sfx / 60;   // about 17 days of rows
      overlay.addMessage(m);
    }
    log.hydrate(300, 0);
    for (int i = 0; i < 100000 && log.backfillStep(); i++) {
    }
    TEST_ASSERT_TRUE_MESSAGE(log.hydrateTruncated(), "the index stopped at its cap");
    folds.begin(&blob);
    folds.applyFold("serial", "1. Decisions & preferences\n- notes.\n", kT0 - 86400, idOf(kThru));
    folds.setSoftStart("serial", idOf(kSoft));
    folds.noteTurn("serial");
    const uint32_t day = orch::foldLocalDay(kT0, 0);
    ctl = "paidDay=" + std::to_string(day) + "\npaidCount=6\nguestPaid=6\n";   // six paid folds that day
  }
  int run(bool serialIsOwners) {
    FakePlatform plat;
    plat.epoch = kT0;
    FakeConfig cfg;
    TurnEngine eng(lumiDeps(plat, cfg, overlay, folds));
    FoldDriverHooks h;
    h.triggerBytes = [] { return 94464u; };
    h.foldCycleBytes = [] { return (size_t)35424; };
    h.folded = [this](const agent::FoldDone& fd) { done.push_back(fd); };
    h.ownerChat = [serialIsOwners](const std::string& c) { return c != "serial" || serialIsOwners; };
    orch::FoldCtlIO io;
    io.load = [this] { return ctl; };
    io.save = [this](const std::string& t) {
      ctl = t;
      return true;
    };
    FoldDriver drv(eng, folds, io, h);
    drv.begin();
    uint32_t ms = 1000;
    for (int pass = 0; pass < 240; pass++) {
      ms += 60000;
      drv.tick(ms, kT0 + (uint32_t)pass * 60);
      eng.historyIdleStep();
      const std::string thru = folds.get("serial").thru;
      const std::string landedThru = done.empty() ? idOf(kThru) : done.back().slice.thru;
      TEST_ASSERT_FALSE_MESSAGE(agent::rowIdAfter(thru, landedThru), "the anchor moved with no summary");
    }
    return (int)done.size();
  }
};
}  // namespace

static void test_a_capped_index_still_folds_the_serial_backlog() {
  {
    CappedBench b;   // the bench's classification: the serial console as a guest
    TEST_ASSERT_EQUAL_MESSAGE(0, b.run(false), "a guest past its share does not fold");
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("fold(serial): due, not started - the daily cap's share"),
                             "and says why");
  }
  CappedBench b;
  const int folds = b.run(true);
  TEST_ASSERT_TRUE_MESSAGE(folds >= 3, std::to_string(folds).c_str());
  TEST_ASSERT_FALSE_MESSAGE(agent::rowIdAfter(idOf(CappedBench::kSoft), b.folds.get("serial").thru),
                            "the backlog is folded over the capped index");
  std::string prev = idOf(CappedBench::kThru);
  for (const agent::FoldDone& fd : b.done) {   // each slice starts at the row after the last anchor
    TEST_ASSERT_EQUAL_STRING_MESSAGE(idOf(orch::epiIdSuffix(prev) + 1).c_str(), fd.slice.firstId.c_str(), "an id gap");
    prev = fd.slice.thru;
  }
}

// The body of the function in src/agent/orchestrator_e1.cpp that starts with `head`.
static std::string deviceFunctionBody(const char* head) {
  std::ifstream f("src/agent/orchestrator_e1.cpp");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  const size_t at = src.find(head);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, head);
  return src.substr(at, src.find("\n}\n", at) - at);
}

// The device binding counts its own surfaces and every Admin chat as the owner's, by role: no
// list of chat names that can leave the serial console or voice out (bench B11, candidate 2).
// Candidate 5, G5: 47d6c0a4's guard restored (roleOfChat reads the tenant table under its lock
// and NVS, so it is not host-reachable without a larger extraction): the fold's owner predicate
// is roleOfChat == Admin, and roleOfChat gives web, serial and voice Admin before any row.
static void test_the_device_owner_predicate_is_by_role() {
  const std::string body = deviceFunctionBody("static bool foldOwnerChat(const std::string& chat) {");
  TEST_ASSERT_TRUE_MESSAGE(body.find("roleOfChat(") != std::string::npos && body.find("Role::Admin") != std::string::npos,
                           "the owner's chats are the Admin ones");
  const std::string role = deviceFunctionBody("nimbus::orch::Role roleOfChat(const String& chatId) {");
  TEST_ASSERT_TRUE_MESSAGE(role.find("chatId == \"web\" || chatId == \"serial\" || chatId == \"voice\") return Role::Admin") !=
                               std::string::npos,
                           "the device's own surfaces are Admin");
}

// Candidate 5, G4: the device's alert send reports delivered only when the reply sink took the
// message (queued); a null sink or a full queue leaves the day unspent. foldSay is device glue
// over the sink, so the guard reads its source; foldAlertSend's own rule is host-tested in
// test_fold_driver.
static void test_the_device_alert_send_counts_only_a_queued_message() {
  const std::string body = deviceFunctionBody("static bool foldSay(const std::string& chat, const std::string& text) {");
  TEST_ASSERT_TRUE_MESSAGE(body.find("&queued") != std::string::npos && body.find("&& queued") != std::string::npos,
                           "a Telegram alert counts only when the reply sink queued it");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_restart_with_hidden_rows_folds_them_first);
  RUN_TEST(test_production_device_shape_folds_forward_with_no_gap);
  RUN_TEST(test_clear_bounds_the_fold);
  RUN_TEST(test_steady_state_folds_come_before_any_shift);
  RUN_TEST(test_one_summarize_now_reaches_the_newest_row);
  RUN_TEST(test_no_anchor_on_a_failing_card_backs_off_and_answers);
  RUN_TEST(test_a_backfill_completing_between_passes_never_ends_the_read);
  RUN_TEST(test_a_manual_read_on_cold_rows_ends_and_never_starves_the_others);
  RUN_TEST(test_a_capped_index_still_folds_the_serial_backlog);
  RUN_TEST(test_a_read_that_cannot_reach_the_rows_waits_and_retries);
  RUN_TEST(test_the_device_owner_predicate_is_by_role);
  RUN_TEST(test_the_device_alert_send_counts_only_a_queued_message);
  RUN_TEST(test_invariant_holds_on_a_head_with_room);
  RUN_TEST(test_invariant_holds_on_the_floor_branch);
  RUN_TEST(test_floor_sends_defer_the_fold_and_the_next_fold_keeps_the_full_keep);
  RUN_TEST(test_a_pool_that_fragments_late_defers_the_fold_too);
  RUN_TEST(test_a_deferred_fold_is_looked_at_once_a_minute);
  RUN_TEST(test_a_summarize_now_at_the_floor_still_lands);
  RUN_TEST(test_a_healthy_pool_folds_with_the_full_keep);
  return UNITY_END();
}
