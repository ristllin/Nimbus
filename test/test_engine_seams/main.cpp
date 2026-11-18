// test_engine_seams - the seams lane P0 of the history fix (16 Nov 2026) declares for the
// lanes after it, each with today's behavior: the fold ladder, the history read's
// cursors, the model pin, the listed window, the request budget, the round load and the
// one budget seam over every recorded model. Each later lane changes a seam's body and
// the assertion here that pinned today's figure, in the same commit.
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fold_ladder.h"
#include "nimbus/harness/hooks.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/budget.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/wire_limits.h"

using agent::FoldResult;
using agent::FoldRung;
using agent::FoldRungKind;
using agent::FoldRungReport;
using agent::HistoryQuery;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static const char* kSummaryTurn = "{\"reply\":\"1. Owner intent: testing\",\"memory\":\"\",\"ask\":\"\"}";

// ---- an engine over scripted hosts --------------------------------------------
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  struct Call {
    std::string host;
    std::string pinned;   // modelOverrideFor(host) while the request ran
  };
  std::vector<Call> calls;
  std::map<std::string, bool> failing;   // host -> its request fails
  std::vector<orch::EpisodicMessage> rows;   // the chat's rows, oldest first
  std::vector<HistoryQuery> reads;           // every historyRows read, in order
  std::function<std::vector<orch::EpisodicMessage>(const std::string&, const HistoryQuery&)> readRows;
  agent::HistoryMarks marks;                 // the chat's fold anchor and shift
  uint32_t foldTrigger = 0;       // the fold driver's trigger as the engine reads it (0 = the head's)
  int cutAfterPages = 0;          // after reads the store's budget cuts to an EMPTY page first
  std::string unreadableAfter;    // an after read from this row meets a span the card won't give
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"})
      d.hosts.add(h, hostFn(h));
    d.histV2 = [] { return true; };
    d.captureUserRow = [](const std::string&, const std::string&, const std::string&) { return std::string("m9999"); };
    readRows = [this](const std::string&, const HistoryQuery& q) { return storeRead(q); };
    d.historyRows = readRows;
    d.historyMarks = [this](const std::string&) { return marks; };
    d.foldTriggerBytes = [this] { return foldTrigger; };
    eng.reset(new TurnEngine(std::move(d)));
  }

  // The stores' shape: `before` pages newest first, `after` oldest first (lane H).
  std::vector<orch::EpisodicMessage> storeRead(const HistoryQuery& q) {
    reads.push_back(q);
    if (!q.after.empty()) return afterRead(q);
    std::vector<orch::EpisodicMessage> out;
    for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
      if (q.before.empty() || it->id < q.before) out.push_back(*it);
    return out;
  }
  std::vector<orch::EpisodicMessage> afterRead(const HistoryQuery& q) {
    std::vector<orch::EpisodicMessage> out;
    if (q.info && cutAfterPages > 0) {   // the cold walk ran out of budget
      cutAfterPages--;
      q.info->newerExists = true;
      q.info->nextAfter = q.after;   // nothing of this chat in the bytes it walked
      return out;
    }
    if (q.info && q.after == unreadableAfter) q.info->unreadable = "20591:4096-36864";
    for (auto it = rows.begin(); it != rows.end() && out.size() < q.limit; ++it)
      if (it->id > q.after && (q.before.empty() || it->id < q.before)) out.push_back(*it);
    return out;
  }

  agent::ProviderTurnFn hostFn(const std::string& host) {
    return [this, host](std::string&, const std::string&, const std::string&, std::string& out, std::string& err,
                        const agent::HeadTools*, orch::TokenUsage* usage) -> bool {
      calls.push_back({host, providers::modelOverrideFor(host.c_str())});
      if (usage) {
        usage->promptTokens = 100;
        usage->completionTokens = 20;
      }
      if (failing[host]) {
        err = "network";
        return false;
      }
      out = kSummaryTurn;
      return true;
    };
  }

  // A conversation of `turns` owner/assistant pairs, an hour apart, ids m0001...
  void addTurns(int turns) {
    for (int i = 0; i < 2 * turns; i++) {
      orch::EpisodicMessage m;
      char id[16];
      std::snprintf(id, sizeof(id), "m%04d", (int)rows.size() + 1);
      m.id = id;
      m.sessionId = "1001";
      m.role = i % 2 ? "assistant" : "user";
      m.text = std::string(i % 2 ? "noted, " : "please remember item ") + std::to_string(i / 2) +
               std::string(120, 'x');
      m.tsHours = 490000 + (uint32_t)(i / 2);
      rows.push_back(m);
    }
  }
};

// ---- the fold ladder ------------------------------------------------------------

// Every host failing is a failed fold: Failed, never the paced cycle's Pending (runFold
// is the one-call cycle), with a real attempt on each rung of the ladder (lane F: the
// three keyed hosts, then the head host on its Sub model).
static void test_all_hosts_failing_is_failed_never_pending() {
  Rig r;
  for (const char* h : {"anthropic", "openai", "mistral"}) r.failing[h] = true;
  std::string sum;
  const FoldResult res = r.eng->runFold("1001", "", "- user: hi\n", sum);
  TEST_ASSERT_TRUE(res == FoldResult::Failed);
  TEST_ASSERT_TRUE(res != FoldResult::Pending);
  TEST_ASSERT_EQUAL(4, (int)r.calls.size());
  TEST_ASSERT_EQUAL_STRING("sub-anthropic", r.calls[3].pinned.c_str());
  TEST_ASSERT_EQUAL_STRING("", sum.c_str());
  // The same on one rung.
  FoldRungReport rep;
  r.calls.clear();
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "d", r.eng->foldRungs()[1], sum, &rep) == FoldResult::Failed);
  TEST_ASSERT_TRUE(rep.result == FoldResult::Failed);
  TEST_ASSERT_EQUAL(1, (int)r.calls.size());
  // Pending is appended: the stored values of the three kinds keep their numbers.
  TEST_ASSERT_EQUAL(0, (int)FoldResult::Ok);
  TEST_ASSERT_EQUAL(1, (int)FoldResult::Deferred);
  TEST_ASSERT_EQUAL(2, (int)FoldResult::Failed);
  TEST_ASSERT_EQUAL(3, (int)FoldResult::Pending);
}

// foldRungs() (lane F, fold_ladder.h): today's candidates in order, each on its own
// model, then the head host on its Sub model, over every configuration that shapes it:
// the priority order, a pinned head, a capped or unkeyed host, and the router fallback
// when no BYOK key is set. test_fold_ladder holds the ladder's class properties.
static void test_fold_rungs_are_todays_candidates() {
  struct Case {
    std::string orchHost, priority;
    std::set<std::string> keyed, capped;
    std::string routerHost;
  };
  const Case cases[] = {
      {"", "anthropic,openai,mistral", {"openai", "anthropic", "mistral"}, {}, ""},
      {"mistral", "anthropic,openai,mistral", {"openai", "anthropic", "mistral"}, {}, ""},
      {"", "openai, mistral ,anthropic", {"openai", "mistral"}, {}, ""},
      {"", "anthropic,openai,mistral", {"openai", "anthropic", "mistral"}, {"anthropic"}, ""},
      {"", "anthropic,openai", {}, {}, "cumulo"},
      {"", "", {}, {}, ""},
  };
  for (const Case& c : cases) {
    Rig r;
    r.cfg.orchHost = c.orchHost;
    r.cfg.priority = c.priority;
    r.cfg.keyed = c.keyed;
    r.cfg.overBudgetHosts = c.capped;
    r.cfg.routerHost = c.routerHost;
    const std::vector<std::string> cands = r.eng->foldHostCandidates();
    const std::vector<FoldRung> rungs = r.eng->foldRungs();
    const bool sub = !rungs.empty() && rungs.back().kind == FoldRungKind::SubModel;
    TEST_ASSERT_EQUAL_MESSAGE((int)cands.size() + (sub ? 1 : 0), (int)rungs.size(), c.priority.c_str());
    for (size_t i = 0; i < cands.size(); i++) {
      TEST_ASSERT_EQUAL_STRING(cands[i].c_str(), rungs[i].host.c_str());
      TEST_ASSERT_EQUAL_STRING(("model-" + cands[i]).c_str(), rungs[i].model.c_str());   // its own model
    }
    if (sub) TEST_ASSERT_EQUAL_STRING(("sub-" + rungs[0].host).c_str(), rungs.back().model.c_str());
    if (!rungs.empty() && rungs[0].host == r.eng->headHost() && rungs[0].host != c.routerHost)
      TEST_ASSERT_TRUE(rungs[0].kind == FoldRungKind::HeadHost);
  }
  // The router fallback is the ladder's only rung on a router-only device, and the head.
  Rig r;
  r.cfg.keyed.clear();
  r.cfg.routerHost = "cumulo";
  const std::vector<FoldRung> rungs = r.eng->foldRungs();
  TEST_ASSERT_EQUAL(1, (int)rungs.size());
  TEST_ASSERT_EQUAL_STRING("cumulo", rungs[0].host.c_str());
  TEST_ASSERT_TRUE(rungs[0].kind == FoldRungKind::RouterFallback);
}

// One rung is one host: the request goes to that host alone, on the rung's model when
// it names one (pinned only while the request runs), and its spend is filed under it.
static void test_one_rung_asks_its_host_on_its_model() {
  Rig r;
  std::string sum;
  FoldRung rung;
  rung.host = "openai";
  rung.model = "gpt-5.6-luna";
  rung.kind = FoldRungKind::SubModel;
  FoldRungReport rep;
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "- user: hi\n", rung, sum, &rep) == FoldResult::Ok);
  TEST_ASSERT_EQUAL(1, (int)r.calls.size());
  TEST_ASSERT_EQUAL_STRING("openai", r.calls[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", r.calls[0].pinned.c_str());
  TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("openai").c_str());   // released
  TEST_ASSERT_EQUAL_STRING("1. Owner intent: testing", sum.c_str());
  TEST_ASSERT_TRUE(rep.result == FoldResult::Ok);
  TEST_ASSERT_EQUAL_STRING("openai", rep.host.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", rep.model.c_str());
  TEST_ASSERT_TRUE(rep.producedBytes > 0);
  TEST_ASSERT_EQUAL(1, (int)r.cfg.recorded.size());
  TEST_ASSERT_EQUAL_STRING("compact", r.cfg.recorded[0].tag.c_str());
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", r.cfg.recorded[0].model.c_str());
  // On the host's own model nothing is pinned, and the spend is filed under its model.
  r.calls.clear();
  r.cfg.recorded.clear();
  rung.model.clear();
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "- user: hi\n", rung, sum) == FoldResult::Ok);
  TEST_ASSERT_EQUAL_STRING("", r.calls[0].pinned.c_str());
  TEST_ASSERT_EQUAL_STRING("model-openai", r.cfg.recorded[0].model.c_str());
  // The whole ladder (runFold) pins nothing.
  r.calls.clear();
  TEST_ASSERT_TRUE(r.eng->runFold("1001", "", "- user: hi\n", sum) == FoldResult::Ok);
  TEST_ASSERT_EQUAL_STRING("anthropic", r.calls[0].host.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.calls[0].pinned.c_str());
  // A rung on a host the ladder does not hold now defers, nothing sent: no host, an
  // unkeyed host, a host over its budget.
  r.calls.clear();
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "d", FoldRung(), sum) == FoldResult::Deferred);
  FoldRung off;
  off.host = "zai";
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "d", off, sum) == FoldResult::Deferred);
  r.cfg.overBudgetHosts.insert("openai");
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "d", rung, sum) == FoldResult::Deferred);
  r.cfg.overBudgetHosts.clear();
  TEST_ASSERT_EQUAL(0, (int)r.calls.size());
  // A rung keeps runFold's gates: a turn-time heap floor defers it.
  r.plat.heap = 13000;
  TEST_ASSERT_TRUE(r.eng->runFoldRung("1001", "", "d", rung, sum) == FoldResult::Deferred);
  TEST_ASSERT_EQUAL(0, (int)r.calls.size());
}

// ---- the model pin --------------------------------------------------------------

static void test_model_pin_is_its_hosts_and_ends_with_its_scope() {
  TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("mistral").c_str());
  {
    const providers::ScopedModelOverride pin("mistral", "mistral-medium-latest");
    TEST_ASSERT_TRUE(pin.active());
    TEST_ASSERT_EQUAL_STRING("mistral-medium-latest", providers::modelOverrideFor("mistral").c_str());
    for (const char* other : {"openai", "anthropic", "zai", "cumulo", "custom", "", "mistral "})
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", providers::modelOverrideFor(other).c_str(), other);
    TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor(nullptr).c_str());
    {
      // Nested: logged, and the outer pin stays (for its host and for the inner one's).
      const providers::ScopedModelOverride inner("openai", "gpt-5.6-luna");
      TEST_ASSERT_FALSE(inner.active());
      TEST_ASSERT_TRUE(LogCapture::contains("model pin: gpt-5.6-luna on openai not taken"));
      TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("openai").c_str());
      TEST_ASSERT_EQUAL_STRING("mistral-medium-latest", providers::modelOverrideFor("mistral").c_str());
    }
    TEST_ASSERT_EQUAL_STRING("mistral-medium-latest", providers::modelOverrideFor("mistral").c_str());
  }
  TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("mistral").c_str());
  // An empty host or model pins nothing and says nothing.
  LogCapture::install();
  {
    const providers::ScopedModelOverride none("mistral", "");
    const providers::ScopedModelOverride none2("", "x");
    TEST_ASSERT_FALSE(none.active() || none2.active());
    TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("mistral").c_str());
  }
  TEST_ASSERT_EQUAL(0, (int)LogCapture::lines().size());
  // Only a direct host's model, or the Cumulo router head's (its pick reads the pin), can
  // be pinned: Z.ai and the custom endpoint read theirs elsewhere, so a pin there would
  // bill one model and send another.
  {
    const providers::ScopedModelOverride pin("cumulo", "mistral/mistral-small-latest");
    TEST_ASSERT_TRUE(pin.active());
    TEST_ASSERT_EQUAL_STRING("mistral/mistral-small-latest", providers::modelOverrideFor("cumulo").c_str());
  }
  for (const char* h : {"zai", "custom"}) {
    const providers::ScopedModelOverride pin(h, "some-model");
    TEST_ASSERT_FALSE_MESSAGE(pin.active(), h);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", providers::modelOverrideFor(h).c_str(), h);
  }
  TEST_ASSERT_TRUE(LogCapture::contains("model pin: some-model on zai not taken, only a direct host"));
  // The pin is the pinning thread's own: a request on another task never sees it, and
  // another task's pin never shows here.
  {
    const providers::ScopedModelOverride pin("openai", "gpt-5.6-luna");
    std::string seen = "unset";
    bool otherPinned = false;
    std::thread([&] {
      seen = providers::modelOverrideFor("openai");
      const providers::ScopedModelOverride own("anthropic", "claude-haiku-5-5");
      otherPinned = own.active();
    }).join();
    TEST_ASSERT_EQUAL_STRING("", seen.c_str());
    TEST_ASSERT_TRUE(otherPinned);   // its own pin, not nested under this thread's
    TEST_ASSERT_EQUAL_STRING("gpt-5.6-luna", providers::modelOverrideFor("openai").c_str());
    TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("anthropic").c_str());
  }
}

// ---- the history read's cursors -------------------------------------------------

// The fold's slice reads FORWARD from thru (lane H): the after cursor, 48 rows a page,
// oldest first, each page strictly after the last one's newest row, until an empty page (a
// store under its read budget answers a short page with more after it) - never the newest
// rows first (v5.1.1 read 400 rows newest first and could anchor past rows it never read).
static void test_the_fold_slice_reads_forward_from_thru() {
  Rig r;
  r.addTurns(30);   // 60 rows: pages of 48 and 12
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, true);
  TEST_ASSERT_FALSE(slice.digest.empty());
  TEST_ASSERT_EQUAL_STRING("m0060", slice.thru.c_str());
  std::vector<HistoryQuery> fwd;
  for (const auto& q : r.reads)
    if (!q.after.empty()) fwd.push_back(q);
  TEST_ASSERT_EQUAL(3, (int)fwd.size());   // the last page is empty: only an empty page is the end
  const char* afters[] = {"m00000000", "m0048", "m0060"};
  for (size_t i = 0; i < fwd.size(); i++) {
    TEST_ASSERT_EQUAL(48, (int)fwd[i].limit);
    TEST_ASSERT_EQUAL_STRING(afters[i], fwd[i].after.c_str());
    TEST_ASSERT_EQUAL_STRING("", fwd[i].before.c_str());
  }
  // Every row the chat holds was read, oldest first, and the slice took them all.
  TEST_ASSERT_EQUAL(30, (int)slice.turns);
}

// The store's cold walk spent its read budget before it met a row of this chat and answered
// an EMPTY page that says more follows (newerExists, nextAfter): the fold read goes on from
// where the store stopped, never takes the empty page for the end (it would fold nothing, or
// anchor short of rows it never saw).
static void test_the_fold_read_goes_on_past_a_page_the_store_cut_short() {
  Rig r;
  r.addTurns(30);
  r.cutAfterPages = 2;
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, true);
  TEST_ASSERT_EQUAL(30, (int)slice.turns);
  TEST_ASSERT_EQUAL_STRING("m0060", slice.thru.c_str());
}

// A span the card would not give back stops the fold's read before it: the fold anchors on
// the rows it read, so the rows past the span are folded once the card reads them, never
// skipped by an anchor moved past them. With nothing read before the span the slice says
// Unread (lane CTXR1): the driver keeps the anchor and tries again later.
static void test_the_fold_read_stops_before_rows_the_card_will_not_give_back() {
  Rig r;
  r.addTurns(30);
  r.marks.thru = "m0002";
  r.unreadableAfter = "m0050";   // the second page
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, true);
  // The read stopped short of the end, so the slice holds whole turns only (its last turn
  // waits for the next fold too): rows m0003..m0048, never past the span.
  TEST_ASSERT_EQUAL_STRING("m0048", slice.thru.c_str());
  TEST_ASSERT_EQUAL(23, (int)slice.turns);
  TEST_ASSERT_TRUE(slice.state == agent::SliceState::Ready);
  r.marks.thru = "m0047";
  r.unreadableAfter = "m0047";
  const agent::FoldSlice none = r.eng->historyFoldSlice("1001", 65536, true);
  TEST_ASSERT_TRUE(none.digest.empty());
  TEST_ASSERT_TRUE_MESSAGE(none.state == agent::SliceState::Unread, agent::sliceStateName(none.state));
}

// After a restart a shift's hidden rows make an automatic fold due before the conversation
// cache has read anything. The turns the fold keeps verbatim are not known yet, but a shift
// never hides one of them: the slice takes the hidden rows alone (lane CTXR1; it used to come
// back empty, which the driver took for "nothing to fold" and moved the anchor past them).
// Once the fill holds the newest rows the tail is known and the slice reaches it.
static void test_an_automatic_fold_waits_for_the_tail_after_a_restart() {
  Rig r;
  r.addTurns(30);
  r.marks.softStart = "m0020";   // a shift hid rows m0001..m0020 before the restart
  const agent::FoldSlice hiddenOnly = r.eng->historyFoldSlice("1001", 65536, false);
  TEST_ASSERT_TRUE(hiddenOnly.state == agent::SliceState::Ready);
  TEST_ASSERT_EQUAL_STRING("m0020", hiddenOnly.thru.c_str());   // the hidden rows, never past them
  TEST_ASSERT_TRUE(r.eng->historyIdleStep());   // the fill's first page: the newest rows
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, false);
  TEST_ASSERT_FALSE(slice.digest.empty());
  TEST_ASSERT_TRUE_MESSAGE(slice.thru != "m0060", slice.thru.c_str());   // the newest exchange stays
  // With no rows hidden and nothing held yet, the tail is unknown: the slice waits.
  Rig w;
  w.addTurns(30);
  const agent::FoldSlice waits = w.eng->historyFoldSlice("1001", 65536, false);
  TEST_ASSERT_TRUE(waits.digest.empty());
  TEST_ASSERT_TRUE_MESSAGE(waits.state == agent::SliceState::Wait, agent::sliceStateName(waits.state));
}

// An owner's own Compact threshold below the head's keep mark (24 KB on a Sonnet head, which
// keeps 59 KB of its 94 KB soft mark since the switch-over): the fold driver finds the chat
// due at 24 KB, so the slice keeps a tail in proportion (about 15 KB) and folds the rest. With
// the head's own trigger the same 30 KB conversation is all within the keep mark: no fold.
static void test_an_owner_trigger_below_the_keep_mark_still_folds() {
  Rig r;
  r.cfg.orchModels["anthropic"] = "claude-sonnet-5-5";   // the rig's head (top of its priority)
  r.addTurns(100);   // 200 rows, about 30 KB after thru
  r.eng->historyFoldSlice("1001", 65536, false);   // opens the chat's cache
  while (r.eng->historyIdleStep()) {
  }
  TEST_ASSERT_TRUE_MESSAGE(r.eng->historyFoldSlice("1001", 65536, false).digest.empty(), "within the head's keep mark");
  r.foldTrigger = 24 * 1024;
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, false);
  TEST_ASSERT_FALSE_MESSAGE(slice.digest.empty(), "due at the owner's trigger: something to fold");
  // The kept tail: 59,040 B x 24,576 / 94,464 = 15,360 B of the newest turns stay verbatim.
  size_t kept = 0;
  for (const auto& m : r.rows)
    if (m.id > slice.thru) kept += m.text.size();
  TEST_ASSERT_TRUE_MESSAGE(kept > 8000 && kept < 16000, std::to_string(kept).c_str());
}

// How often `needle` appears in a source file's code (paths from the repo root, where pio
// runs; // comments do not count).
static int countIn(const char* path, const std::string& needle) {
  std::ifstream f(path);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), path);
  std::string text, line;
  while (std::getline(f, line)) text += line.substr(0, line.find("//")) + "\n";
  int n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) n++;
  return n;
}

// Every store serves the after cursor (lane H): each binding hands the query's cursor to
// its store with the page info (an empty page past the store's read budget is not the
// end), and a read after a cursor returns the rows newer than it, oldest first.
static void test_an_after_cursor_is_served() {
  for (const char* f : {"src/agent/orchestrator.cpp", "nimbusd/src/rig.h", "tools/harness-lab/src/lab_rig.h"}) {
    TEST_ASSERT_EQUAL_MESSAGE(1, countIn(f, "q.after = hq.after;"), f);
    TEST_ASSERT_EQUAL_MESSAGE(1, countIn(f, "query(q, hq.info)"), f);
  }
  HistoryQuery q(24);
  q.after = "m0005";
  Rig r;
  r.addTurns(3);
  const auto rows = r.readRows("1001", q);
  TEST_ASSERT_EQUAL(1, (int)rows.size());
  TEST_ASSERT_EQUAL_STRING("m0006", rows[0].id.c_str());
  // The query's own cursor field rides MsgQuery to the store.
  orch::MsgQuery mq;
  TEST_ASSERT_EQUAL_STRING("", mq.after.c_str());
}

// ---- the listed window ------------------------------------------------------------

static std::string readFixture(const std::string& path) {
  std::ifstream f(path);
  TEST_ASSERT_TRUE_MESSAGE(f.good(), path.c_str());
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string storeTextOf(const char* provider, const std::string& fixture) {
  orch::ModelHarvest h;
  h.provider = provider;
  h.now = 1794000000u;   // 2026-11-07, before any recorded retire date that matters here
  const std::string body = readFixture(fixture);
  orch::harvestBody(h, body.data(), body.size());
  orch::finishHarvest(h);
  return orch::serializeModelStore(orch::ModelStoreMeta(), h.rows);
}

static void test_listed_window_from_the_stored_list() {
  const std::string mistral = storeTextOf("mistral", "test/support/fixtures/models_2026-11/mistral.json");
  // An alias reads its row's window (mistral-large-4 lists mistral-large-4-0).
  TEST_ASSERT_EQUAL_UINT32(1048576, orch::listedCtxTokens("mistral", mistral, "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(1048576, orch::listedCtxTokens("mistral", mistral, "mistral-large-4"));
  TEST_ASSERT_EQUAL_UINT32(1048576, orch::listedCtxTokens("mistral", mistral, "mistral-large-latest"));
  // A row's own id wins over a sibling that lists it.
  TEST_ASSERT_EQUAL_UINT32(262144, orch::listedCtxTokens("mistral", mistral, "mistral-large-2512"));
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedCtxTokens("mistral", mistral, "mistral-huge-9"));
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedCtxTokens("mistral", std::string(), "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedCtxTokens("mistral", mistral, ""));
  // An OpenAI row's window is the id heuristic's, never the provider's: 0.
  const std::string openai = storeTextOf("openai", "test/support/fixtures/models_2026-10/openai.json");
  TEST_ASSERT_TRUE(openai.find("\ngpt-5.6-luna\t") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedCtxTokens("openai", openai, "gpt-5.6-luna"));
  // The class: every Mistral row the store holds answers its API window, every OpenAI
  // row 0.
  size_t rows = 0;
  orch::forEachStoreRow("mistral", mistral, [&](const orch::ModelInfo& m) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(m.apiCaps ? m.ctxTokens : 0, orch::listedCtxTokens("mistral", mistral, m.id),
                                     m.id.c_str());
    rows++;
    return true;
  });
  TEST_ASSERT_TRUE(rows > 20);
  orch::forEachStoreRow("openai", openai, [&](const orch::ModelInfo& m) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, orch::listedCtxTokens("openai", openai, m.id), m.id.c_str());
    return true;
  });
}

static uint32_t fakeListed(const char* host, const char* model) {
  return std::string(host) == "mistral" && std::string(model) == "mistral-large-4-0" ? 1048576 : 0;
}

// The installed lookup answers; with none installed (host tests, a fresh process) 0.
static void test_listed_window_lookup_install() {
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedWindowTokens("mistral", "mistral-large-4-0"));
  orch::installListedCtxLookup(&fakeListed);
  TEST_ASSERT_EQUAL_UINT32(1048576, orch::listedWindowTokens("mistral", "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedWindowTokens("openai", "gpt-5.6"));
  // Since lane H's switch-over the window table reads it; v5.1.1's frozen table does not.
  TEST_ASSERT_EQUAL_UINT32(1048576, orch::modelCtxTokens("mistral", "mistral-large-4-0"));
  TEST_ASSERT_EQUAL_UINT32(128000, orch::legacyCtxTokens("mistral", "mistral-large-4-0"));
  orch::installListedCtxLookup(nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, orch::listedWindowTokens("mistral", "mistral-large-4-0"));
}

// ---- the request budget and the round load -----------------------------------------

// Lane B filled the request budget (its own suite, test_request_budget, holds the class):
// the pins here keep the seam's shape for its consumers - the round deadlines are still
// the wires' socket ceilings (a routed one at most 90 s), the custom endpoint is the only
// flat wire, a routed head sits on the v5.1.1 floor until kRoutedBudgetLift flips, and the
// head budgets are the plan's figures.
static void test_request_budget_is_lane_bs() {
  TEST_ASSERT_EQUAL_UINT32(providers::kWireBodyMax, orch::kLegacyWireBodyBytes);
  TEST_ASSERT_EQUAL_UINT32(55904, providers::flatPromptBudget());
  TEST_ASSERT_EQUAL_UINT32(orch::kJsonStrGuard - 4096, providers::flatPromptBudget());
  struct Row { const char* host; const char* model; uint32_t roundMs; bool flat; uint32_t bytes; };
  const Row rows[] = {
      {"anthropic", "claude-sonnet-5-5", providers::kHeadRoundCeilingMs, false, 250000},
      {"openai", "gpt-5.6", providers::kHeadRoundCeilingMs, false, 360000},
      {"mistral", "mistral-large-4-0", providers::kMistralRoundMs, false, 360000},
      {"zai", "glm-5.3", providers::kZaiHeadTimeoutMs, false, 0},
      {"cumulo", "openai/gpt-5.6", 90000, false, 85000},
      {"custom", "gpt-5.6-luna", providers::kHeadRoundCeilingMs, true, 85000},
  };
  for (const Row& r : rows) {
    for (bool routed : {false, true}) {
      const orch::RequestBudget b = orch::requestBudgetFor(r.host, r.model, routed);
      const bool viaRouter = routed || std::string(r.host) == "cumulo";
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(viaRouter ? std::min<uint32_t>(r.roundMs, 90000) : r.roundMs, b.roundMs, r.host);
      TEST_ASSERT_EQUAL_MESSAGE(viaRouter, b.routed, r.host);
      TEST_ASSERT_EQUAL_MESSAGE(r.flat, b.flat, r.host);
      TEST_ASSERT_TRUE_MESSAGE(b.prefillTokPerS > 0 && b.decodeTokPerS > 0, r.host);
      const uint32_t want = viaRouter && !orch::kRoutedBudgetLift ? 85000 : r.bytes;
      if (want) TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, b.wireBodyBytes, r.host);
      TEST_ASSERT_TRUE_MESSAGE(b.wireBodyBytes >= orch::kLegacyWireBodyBytes, r.host);
    }
  }
}

// Lane B made the load-aware overload read the load (test_output_caps holds the class).
// The pins here keep the seam's contract for the wires that switch to it: it never asks
// past the table, OpenAI and custom stay uncapped whatever the load, a capped host always
// sends a cap, a heavier load never gets a bigger first attempt, and the legacy overload
// is still v5.1.1's table (test_todays_table_is_v5_1_1).
static void test_round_load_reads_the_load_within_the_table() {
  providers::RoundLoad light;
  light.bodyBytes = 20000;
  providers::RoundLoad heavy;
  heavy.bodyBytes = 340000;
  heavy.imageBytes = 250000;
  heavy.promptTokens = 90000;
  heavy.fragmented = true;
  const char* hosts[][2] = {{"anthropic", "claude-sonnet-5-5"}, {"anthropic", "claude-haiku-4-5"},
                            {"openai", "gpt-5.6"},              {"mistral", "mistral-large-4-0"},
                            {"mistral", "mistral-small-latest"}, {"zai", "glm-5.3"},
                            {"custom", "x"}};
  for (const auto& hm : hosts)
    for (int k = 0; k < providers::kRoundKindCount; k++)
      for (uint32_t ms : {0u, 8000u, 50000u, 65000u, 120000u}) {
        const providers::OutputCap table = providers::outputCapFor(hm[0], hm[1], (providers::RoundKind)k, 0);
        const providers::OutputCap a = providers::outputCapFor(hm[0], hm[1], (providers::RoundKind)k, ms, light);
        const providers::OutputCap b = providers::outputCapFor(hm[0], hm[1], (providers::RoundKind)k, ms, heavy);
        TEST_ASSERT_TRUE_MESSAGE(a.maxTokens <= table.maxTokens && b.maxTokens <= table.maxTokens, hm[1]);
        TEST_ASSERT_TRUE_MESSAGE(b.maxTokens <= a.maxTokens, hm[1]);
        TEST_ASSERT_EQUAL_MESSAGE(table.maxTokens == 0, a.maxTokens == 0, hm[1]);
        TEST_ASSERT_EQUAL_MESSAGE(table.maxTokens == 0, b.maxTokens == 0, hm[1]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(table.effort, b.effort, hm[1]);
      }
}

// ---- the declared fields hold today's values ----------------------------------------

static void test_declared_fields_start_empty() {
  const agent::TurnEndEv ev;
  TEST_ASSERT_EQUAL(0, (int)(ev.historyBytes + ev.historyMaxBytes + ev.historySoftBytes + ev.leftOutRows));
  const agent::HistoryMarks marks;
  TEST_ASSERT_EQUAL(0, (int)marks.foldEpoch);
  // A message's ids are never rendered: the history renders as before.
  const std::vector<agent::HistMsg> pair = agent::renderHistory({}, "the summary", 0);
  TEST_ASSERT_EQUAL(2, (int)pair.size());
  for (const agent::HistMsg& m : pair) {
    TEST_ASSERT_TRUE(m.firstId.empty() && m.lastId.empty());
    TEST_ASSERT_EQUAL(0, (int)(m.localDay + m.atUnix));
  }
  const agent::HistMsg brace{true, "hi"};
  TEST_ASSERT_TRUE(brace.user);
  TEST_ASSERT_EQUAL_STRING("hi", brace.text.c_str());
  // The fold's device-wide storage reads nothing and stores nothing by default.
  const orch::FoldCtlIO io;
  TEST_ASSERT_EQUAL_STRING("", io.load().c_str());
  TEST_ASSERT_TRUE(io.save("paidDay=1"));
  TEST_ASSERT_EQUAL_STRING("", io.load().c_str());
}

// ---- every recorded model: the one seam and the v5.1.1 budgets fixture -------------

struct Recorded {
  std::string provider;
  std::string id;
};

static void addIds(std::vector<Recorded>& out, std::map<std::string, bool>& seen, const char* provider,
                   const std::string& path, bool aliases) {
  JsonDocument doc;
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, readFixture(path)), path.c_str());
  for (JsonObjectConst m : doc["data"].as<JsonArrayConst>()) {
    std::vector<std::string> ids{m["id"] | ""};
    if (aliases)
      for (JsonVariantConst a : m["aliases"].as<JsonArrayConst>()) ids.push_back(a | "");
    for (const std::string& id : ids) {
      const std::string key = std::string(provider) + "\t" + id;
      if (id.empty() || seen[key]) continue;
      seen[key] = true;
      out.push_back({provider, id});
    }
  }
}

// Every model the recorded provider lists name (models_2026-10, models_2026-11, Mistral's
// aliases included), in a stable order. The router's selectors are not here: a Cumulo
// head stays at the floor in v5.1.2 (rulings v3), so it has no budget to hold.
static std::vector<Recorded> recordedModels() {
  std::vector<Recorded> out;
  std::map<std::string, bool> seen;
  const std::string f = "test/support/fixtures/";
  for (const char* p : {"anthropic", "openai", "mistral", "zai"})
    addIds(out, seen, p, f + "models_2026-10/" + p + ".json", true);
  addIds(out, seen, "mistral", f + "models_2026-11/mistral.json", true);
  return out;
}

// Lane H's switch-over: the seam's body is the request budget's derivation for every
// recorded model (deriveRequestContext over requestBudgetFor), every field of it. Lane L:
// that is the seam with the tool loop on (the form requestBudgetFor answers for).
static void test_head_budget_is_the_request_budget_for_every_recorded_model() {
  const std::vector<Recorded> models = recordedModels();
  TEST_ASSERT_TRUE(models.size() > 200);
  for (const Recorded& m : models) {
    orch::BudgetOverrides ov;
    const orch::ContextBudget a = orch::headContextBudget(m.provider, m.id, true, ov);
    const orch::ContextBudget b = orch::deriveRequestContext(orch::requestBudgetFor(m.provider, m.id, false), ov);
    const std::string what = m.provider + "/" + m.id;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.ctxTokens, a.ctxTokens, what.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(b.systemPromptBytes, a.systemPromptBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.chatSummaryBytes, a.chatSummaryBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.recentConvBytes, a.recentConvBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.toolResultBytes, a.toolResultBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.toolTotalBytes, a.toolTotalBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.briefBytes, a.briefBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.foldSliceBytes, a.foldSliceBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.historyMaxBytes, a.historyMaxBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.recallBytes, a.recallBytes, what.c_str());
  }
}

// budgets_v5_1_1.json: every recorded model's v5.1.1 ContextBudget and output caps, the
// no-shrink baseline lane B's request budget is held to. READ-ONLY: it records v5.1.1,
// so it is never regenerated after lane P0 wrote it. NIMBUS_GEN_BUDGETS_V5_1_1=1 writes
// it only when the file is absent (GOLDEN_UPDATE does not touch it).
static const char* kBudgetsFixture = "test/support/fixtures/budgets_v5_1_1.json";

static uint32_t capRoundMs(const std::string& host) {
  if (host == "mistral") return providers::kMistralRoundMs;
  if (host == "zai") return providers::kZaiHeadTimeoutMs;
  return providers::kHeadRoundCeilingMs;
}

static std::string budgetRow(const Recorded& m) {
  const orch::ContextBudget b = orch::deriveBudget(orch::legacyCtxTokens(m.provider, m.id), {});
  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "{\"provider\":\"%s\",\"id\":\"%s\",\"ctxTokens\":%u,\"systemPromptBytes\":%d,"
                "\"chatSummaryBytes\":%u,\"recentConvBytes\":%u,\"toolResultBytes\":%u,\"toolTotalBytes\":%u,"
                "\"briefBytes\":%u,\"foldSliceBytes\":%u,\"historyMaxBytes\":%u,\"historySoftBytes\":%u,"
                "\"historyKeepBytes\":%u,\"historyRows\":%u,\"replayBytes\":%u,\"foldStepBytes\":%u,"
                "\"perTurnFloorBytes\":%u,\"historyMessageCap\":%u,\"recallBytes\":%u",
                m.provider.c_str(), m.id.c_str(), (unsigned)b.ctxTokens, b.systemPromptBytes,
                (unsigned)b.chatSummaryBytes, (unsigned)b.recentConvBytes, (unsigned)b.toolResultBytes,
                (unsigned)b.toolTotalBytes, (unsigned)b.briefBytes, (unsigned)b.foldSliceBytes,
                (unsigned)b.historyMaxBytes, (unsigned)b.historySoftBytes, (unsigned)b.historyKeepBytes,
                (unsigned)b.historyRows, (unsigned)b.replayBytes, (unsigned)b.foldStepBytes,
                (unsigned)b.perTurnFloorBytes, (unsigned)b.historyMessageCap, (unsigned)b.recallBytes);
  std::string row = buf;
  const uint32_t ms = capRoundMs(m.provider);
  row += ",\"capsRoundMs\":" + std::to_string(ms) + ",\"caps\":{";
  for (int k = 0; k < providers::kRoundKindCount; k++) {
    const providers::OutputCap c = providers::outputCapFor(m.provider, m.id, (providers::RoundKind)k, ms);
    std::snprintf(buf, sizeof(buf), "%s\"%s\":[%u,\"%s\",%u,\"%s\"]", k ? "," : "",
                  providers::roundKindSlug((providers::RoundKind)k), (unsigned)c.maxTokens, c.effort,
                  (unsigned)c.retryTokens, c.retryEffort);
    row += buf;
  }
  return row + "}}";
}

static std::string budgetsFixtureText() {
  std::string out =
      "{\"about\":\"Every recorded model's v5.1.1 head budget (deriveBudget over modelCtxTokens) and output "
      "caps (outputCapFor at the adapter's round ceiling; tool, final, single, fold = [maxTokens, effort, "
      "retryTokens, retryEffort]). The no-shrink baseline of the history fix. Written once by lane P0 "
      "(test_engine_seams, NIMBUS_GEN_BUDGETS_V5_1_1=1); read-only.\",\n\"models\":[\n";
  const std::vector<Recorded> models = recordedModels();
  for (size_t i = 0; i < models.size(); i++) out += budgetRow(models[i]) + (i + 1 < models.size() ? ",\n" : "\n");
  return out + "]}\n";
}

static void test_budgets_v5_1_1_fixture_holds_every_recorded_model() {
  std::ifstream probe(kBudgetsFixture);
  const char* gen = std::getenv("NIMBUS_GEN_BUDGETS_V5_1_1");
  if (!probe.good() && gen && std::string(gen) == "1") {
    std::ofstream(kBudgetsFixture) << budgetsFixtureText();
    TEST_IGNORE_MESSAGE("budgets_v5_1_1.json written: review it, commit it, run again");
  }
  const std::string have = readFixture(kBudgetsFixture);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, have));
  std::map<std::string, JsonObjectConst> rows;
  for (JsonObjectConst r : doc["models"].as<JsonArrayConst>())
    rows[std::string(r["provider"] | "") + "/" + (r["id"] | "")] = r;
  // Every recorded model has its row, and each row is v5.1.1's derivation of its own
  // window: deriveBudget is never changed, only what the seam reads.
  const std::vector<Recorded> models = recordedModels();
  TEST_ASSERT_EQUAL((int)models.size(), (int)rows.size());
  for (const Recorded& m : models) {
    const std::string key = m.provider + "/" + m.id;
    TEST_ASSERT_TRUE_MESSAGE(rows.count(key) == 1, key.c_str());
    const JsonObjectConst r = rows[key];
    const orch::ContextBudget b = orch::deriveBudget(r["ctxTokens"] | 0u, {});
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.recentConvBytes, r["recentConvBytes"] | 0u, key.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.toolResultBytes, r["toolResultBytes"] | 0u, key.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.foldSliceBytes, r["foldSliceBytes"] | 0u, key.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(b.systemPromptBytes, r["systemPromptBytes"] | 0, key.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.historyMaxBytes, r["historyMaxBytes"] | 0u, key.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.recallBytes, r["recallBytes"] | 0u, key.c_str());
  }
}

// v5.1.1's frozen window table and caps still derive budgets_v5_1_1.json row for row: the
// no-shrink baseline a request budget is held to (lane H re-pinned it to legacyCtxTokens at
// its switch-over; the fixture itself never changes). One kind of row may differ, in its caps
// only: an alias Mistral moved to another model since the fixture was written. Its v5.1.1 caps
// were the old model's; today it gets the caps of the model it names (release review v5.1.2:
// mistral-large-latest was mistral-large-2512's alias on the 2026-10 list and is Large 4's on
// the 14 Nov list, nimbus/orch/model_family.h), which never ask for fewer tokens.
static void test_todays_table_is_v5_1_1() {
  struct Moved { const char* id; const char* now; };
  static const Moved kMoved[] = {{"mistral-large-latest", "mistral-large-4-0"}};
  auto lines = [](const std::string& t) {
    std::vector<std::string> out;
    size_t at = 0;
    for (size_t nl; (nl = t.find('\n', at)) != std::string::npos; at = nl + 1) out.push_back(t.substr(at, nl - at));
    return out;
  };
  auto rowOf = [](std::string line, JsonDocument& doc) {
    if (!line.empty() && line.back() == ',') line.pop_back();
    return !deserializeJson(doc, line);
  };
  const std::vector<std::string> have = lines(readFixture(kBudgetsFixture));
  const std::vector<std::string> want = lines(budgetsFixtureText());
  TEST_ASSERT_EQUAL_MESSAGE((int)have.size(), (int)want.size(), "v5.1.1's budgets differ from budgets_v5_1_1.json");
  std::map<std::string, std::string> capsNow;
  for (const std::string& l : want) {
    JsonDocument d;
    if (rowOf(l, d)) {
      std::string caps;
      serializeJson(d["caps"], caps);
      capsNow[std::string(d["provider"] | "") + "/" + (d["id"] | "")] = caps;
    }
  }
  int moved = 0;
  for (size_t i = 0; i < have.size(); i++) {
    if (have[i] == want[i]) continue;
    JsonDocument h, w;
    TEST_ASSERT_TRUE_MESSAGE(rowOf(have[i], h) && rowOf(want[i], w), want[i].c_str());
    const std::string id = h["id"] | "";
    const Moved* m = nullptr;
    for (const Moved& x : kMoved)
      if (id == x.id && std::string(h["provider"] | "") == "mistral") m = &x;
    TEST_ASSERT_NOT_NULL_MESSAGE(m, ("v5.1.1's budgets or caps differ from budgets_v5_1_1.json: " + want[i]).c_str());
    std::string wantCaps;
    serializeJson(w["caps"], wantCaps);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(capsNow[std::string("mistral/") + m->now].c_str(), wantCaps.c_str(), id.c_str());
    h.remove("caps");
    w.remove("caps");
    std::string hb, wb;
    serializeJson(h, hb);
    serializeJson(w, wb);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hb.c_str(), wb.c_str(), id.c_str());   // only the caps moved
    for (const char* kind : {"tool", "final", "single", "fold"})
      TEST_ASSERT_TRUE_MESSAGE((w["caps"][kind][0] | 0u) >= (h["caps"][kind][0] | 0u), id.c_str());
    moved++;
  }
  TEST_ASSERT_EQUAL_INT((int)(sizeof(kMoved) / sizeof(kMoved[0])), moved);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_all_hosts_failing_is_failed_never_pending);
  RUN_TEST(test_fold_rungs_are_todays_candidates);
  RUN_TEST(test_one_rung_asks_its_host_on_its_model);
  RUN_TEST(test_model_pin_is_its_hosts_and_ends_with_its_scope);
  RUN_TEST(test_the_fold_slice_reads_forward_from_thru);
  RUN_TEST(test_an_after_cursor_is_served);
  RUN_TEST(test_the_fold_read_goes_on_past_a_page_the_store_cut_short);
  RUN_TEST(test_the_fold_read_stops_before_rows_the_card_will_not_give_back);
  RUN_TEST(test_an_automatic_fold_waits_for_the_tail_after_a_restart);
  RUN_TEST(test_an_owner_trigger_below_the_keep_mark_still_folds);
  RUN_TEST(test_listed_window_from_the_stored_list);
  RUN_TEST(test_listed_window_lookup_install);
  RUN_TEST(test_request_budget_is_lane_bs);
  RUN_TEST(test_round_load_reads_the_load_within_the_table);
  RUN_TEST(test_declared_fields_start_empty);
  RUN_TEST(test_head_budget_is_the_request_budget_for_every_recorded_model);
  RUN_TEST(test_budgets_v5_1_1_fixture_holds_every_recorded_model);
  RUN_TEST(test_todays_table_is_v5_1_1);
  return UNITY_END();
}
