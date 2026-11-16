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
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"})
      d.hosts.add(h, hostFn(h));
    d.histV2 = [] { return true; };
    d.captureUserRow = [](const std::string&, const std::string&, const std::string&) { return std::string("m9999"); };
    // The bindings' shape (orchestrator.cpp, rig.h): an after-cursor read is refused.
    readRows = [this](const std::string&, const HistoryQuery& q) {
      reads.push_back(q);
      std::vector<orch::EpisodicMessage> out;
      if (agent::historyAfterUnserved(q)) return out;
      for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
        if (q.before.empty() || it->id < q.before) out.push_back(*it);
      return out;
    };
    d.historyRows = readRows;
    eng.reset(new TurnEngine(std::move(d)));
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

// The history reads it pages exactly as before: 24 rows a page, each page strictly
// older than the last one's oldest row, `before` only.
static void test_before_only_reads_return_todays_rows() {
  Rig r;
  r.addTurns(30);   // 60 rows: pages of 24, 24 and 12
  const agent::FoldSlice slice = r.eng->historyFoldSlice("1001", 65536, true);
  TEST_ASSERT_FALSE(slice.digest.empty());
  TEST_ASSERT_EQUAL_STRING("m0060", slice.thru.c_str());
  TEST_ASSERT_EQUAL(3, (int)r.reads.size());
  const char* befores[] = {"", "m0037", "m0013"};
  for (size_t i = 0; i < r.reads.size(); i++) {
    TEST_ASSERT_EQUAL(24, (int)r.reads[i].limit);
    TEST_ASSERT_EQUAL_STRING(befores[i], r.reads[i].before.c_str());
    TEST_ASSERT_EQUAL_STRING("", r.reads[i].after.c_str());
    TEST_ASSERT_FALSE(agent::historyAfterUnserved(r.reads[i]));
  }
  // Every row the chat holds was read, newest first, and the slice took them all.
  TEST_ASSERT_EQUAL(30, (int)slice.turns);
}

// A read after a cursor is not served yet: no rows, said once a boot.
static void test_an_after_cursor_reads_no_rows_and_says_so_once() {
  HistoryQuery q(24);
  TEST_ASSERT_FALSE(agent::historyAfterUnserved(q));
  q.after = "m0005";
  TEST_ASSERT_TRUE(agent::historyAfterUnserved(q));
  TEST_ASSERT_TRUE(agent::historyAfterUnserved(q));
  int said = 0;
  for (const auto& l : LogCapture::lines()) said += l.find("history: a read after m0005") != std::string::npos;
  TEST_ASSERT_TRUE(said <= 1);   // once per task (an earlier test on this thread may have said it)
  Rig r;
  r.addTurns(3);
  TEST_ASSERT_EQUAL(0, (int)r.readRows("1001", q).size());
  // The query's own cursor field rides MsgQuery for the stores that will serve it.
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
  // Nothing reads it yet: the window table is unchanged by it.
  TEST_ASSERT_EQUAL_UINT32(128000, orch::modelCtxTokens("mistral", "mistral-large-4-0"));
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

static void test_head_budget_is_todays_for_every_recorded_model() {
  const std::vector<Recorded> models = recordedModels();
  TEST_ASSERT_TRUE(models.size() > 200);
  for (const Recorded& m : models) {
    orch::BudgetOverrides ov;
    const orch::ContextBudget a = orch::headContextBudget(m.provider, m.id, ov);
    const orch::ContextBudget b = orch::deriveBudget(orch::modelCtxTokens(m.provider, m.id), ov);
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
  const orch::ContextBudget b = orch::deriveBudget(orch::modelCtxTokens(m.provider, m.id), {});
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

// Today's window table and caps are exactly v5.1.1's, row for row. Lane H re-pins this
// when it switches modelCtxTokens; the fixture itself never changes.
static void test_todays_table_is_v5_1_1() {
  const std::string have = readFixture(kBudgetsFixture);
  TEST_ASSERT_TRUE_MESSAGE(have == budgetsFixtureText(), "today's budgets or caps differ from budgets_v5_1_1.json");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_all_hosts_failing_is_failed_never_pending);
  RUN_TEST(test_fold_rungs_are_todays_candidates);
  RUN_TEST(test_one_rung_asks_its_host_on_its_model);
  RUN_TEST(test_model_pin_is_its_hosts_and_ends_with_its_scope);
  RUN_TEST(test_before_only_reads_return_todays_rows);
  RUN_TEST(test_an_after_cursor_reads_no_rows_and_says_so_once);
  RUN_TEST(test_listed_window_from_the_stored_list);
  RUN_TEST(test_listed_window_lookup_install);
  RUN_TEST(test_request_budget_is_lane_bs);
  RUN_TEST(test_round_load_reads_the_load_within_the_table);
  RUN_TEST(test_declared_fields_start_empty);
  RUN_TEST(test_head_budget_is_todays_for_every_recorded_model);
  RUN_TEST(test_budgets_v5_1_1_fixture_holds_every_recorded_model);
  RUN_TEST(test_todays_table_is_v5_1_1);
  return UNITY_END();
}
