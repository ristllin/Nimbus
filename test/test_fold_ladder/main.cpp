// test_fold_ladder - the fold's ladder of rungs (lane F of the history fix, 16 Nov 2026;
// owner ruling 2: a failing summary walks every keyed provider, one rung at a time).
//
// The class, not the instance: every failure kind at every rung index is answered by the
// next rung; no (host, model) is asked twice in a cycle over random configurations; a
// forced-reasoner head (Mistral Large 4) folds on mistral-small-latest at effort "none"
// on the real Mistral wire; the quality gate passes every live fold output we recorded
// (test/support/fixtures/fold_outputs_2026-11/) and refuses a thin answer to a 40 KB
// digest.
#include <unity.h>

#include <dirent.h>

#include <cstdio>
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
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fold_ladder.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/compact.h"

using agent::FoldFailKind;
using agent::FoldResult;
using agent::FoldRung;
using agent::FoldRungKind;
using agent::FoldRungReport;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

namespace {

const char kGoodSummary[] =
    "1. Decisions & preferences\n- Daily targets 1,900 kcal and 120 g protein; label figures win.\n"
    "- Replies stay short; the owner asks for detail when wanted.\n"
    "2. Pending items & next steps\n- 2026-11-16: weigh-in Monday morning, report only when asked.\n"
    "- 2026-11-15: lunch weight still to give (the soup).\n"
    "3. Facts worth remembering long-term\n- Training for a 10 km run in March; long runs on Sundays.\n"
    "- Walks to the market most days for bread and apples.\n"
    "4. Owner intent & active threads\n- Logging every meal with kcal and protein.\n"
    "- Planning the week's meals on market days.\n"
    "5. Device & task state\n- Food ledger in food/food-log.csv since 2026-10-30.\n"
    "- Totals for 2026-11-15: 1,240 kcal from 3 rows, 82 g protein.";

// How one (host, model) answers a fold request.
enum class Answer { Good, Transport, Http4xx, Http5xx, Park, Cut, Empty, Thin, NoUsage };   // NoUsage: a 2xx with no usage object

const Answer kFailures[] = {Answer::Transport, Answer::Http4xx, Answer::Http5xx, Answer::Park,
                            Answer::Cut,       Answer::Empty,   Answer::Thin};

FoldFailKind kindFor(Answer a) {
  switch (a) {
    case Answer::Transport: return FoldFailKind::Transport;
    case Answer::Http4xx: return FoldFailKind::Http4xx;
    case Answer::Http5xx: return FoldFailKind::Http5xx;
    case Answer::Park: return FoldFailKind::Park;
    case Answer::Cut: return FoldFailKind::Cut;
    case Answer::Empty: return FoldFailKind::Empty;
    case Answer::Thin: return FoldFailKind::Thin;
    default: return FoldFailKind::None;
  }
}

// An engine over scripted hosts; each request is recorded with the model it ran on.
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  std::vector<std::pair<std::string, std::string>> asked;   // (host, model) per request
  std::map<std::string, Answer> answers;                    // "host/model" -> answer (default Good)
  std::unique_ptr<TurnEngine> eng;

  Rig() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    for (const char* h : {"openai", "anthropic", "mistral", "custom", "cumulo", "zai"}) d.hosts.add(h, hostFn(h));
    eng.reset(new TurnEngine(std::move(d)));
  }

  std::string modelOn(const std::string& host) const {
    const std::string pin = providers::modelOverrideFor(host.c_str());
    if (!pin.empty()) return pin;
    auto it = cfg.orchModels.find(host);
    return it == cfg.orchModels.end() ? "model-" + host : it->second;
  }

  agent::ProviderTurnFn hostFn(const std::string& host) {
    return [this, host](std::string&, const std::string&, const std::string&, std::string& out, std::string& err,
                        const agent::HeadTools*, orch::TokenUsage* usage) -> bool {
      const std::string model = modelOn(host);
      asked.push_back({host, model});
      const auto it = answers.find(host + "/" + model);
      const Answer a = it == answers.end() ? Answer::Good : it->second;
      if (usage && a != Answer::Transport && a != Answer::NoUsage) usage->responses = 1;
      switch (a) {
        case Answer::Transport: err = "connect failed"; return false;
        case Answer::Http4xx: err = "fold HTTP 400: invalid request"; return false;
        case Answer::Http5xx: err = "fold HTTP 503: service unavailable"; return false;
        case Answer::Park: err = "fold HTTP 402: insufficient credit balance"; return false;
        case Answer::Cut: err = "cut at the output limit (5500 tokens)"; return false;
        case Answer::Empty: out = "{\"reply\":\"\",\"memory\":\"\"}"; return true;
        case Answer::Thin: out = "Noted."; return true;
        default: out = kGoodSummary; return true;
      }
    };
  }
};

std::string bigDigest(size_t bytes) {
  std::string d;
  for (int i = 0; d.size() < bytes; i++)
    d += "[2026-11-15 0" + std::to_string(i % 10) + ":00] user: breakfast " + std::to_string(i) +
         " was two eggs and toast\n- assistant: logged, 1,240 kcal so far\n";
  return d;
}

std::string key(const FoldRung& r) { return r.host + "/" + r.model; }

}  // namespace

// For every failure kind at every rung k, rung k+1 answers: the cycle (runFold) ends Ok on
// exactly that rung, every rung before it asked once, and the failed rung's line names its
// kind.
static void test_every_failure_kind_at_every_rung_is_answered_by_the_next() {
  Rig probe;
  const std::vector<FoldRung> rungs = probe.eng->foldRungs();
  TEST_ASSERT_EQUAL(4, (int)rungs.size());   // anthropic, openai, mistral, anthropic on its Sub model
  for (Answer fail : kFailures) {
    for (size_t k = 0; k + 1 < rungs.size(); k++) {
      Rig r;
      for (size_t j = 0; j <= k; j++) r.answers[key(rungs[j])] = fail;
      std::string sum;
      LogCapture::lines().clear();
      const FoldResult res = r.eng->runFold("1001", "", bigDigest(40 * 1024), sum);
      char what[96];
      std::snprintf(what, sizeof(what), "kind %s at rung %u", agent::foldFailKindName(kindFor(fail)), (unsigned)k);
      TEST_ASSERT_TRUE_MESSAGE(res == FoldResult::Ok, what);
      TEST_ASSERT_EQUAL_MESSAGE((int)k + 2, (int)r.asked.size(), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(rungs[k + 1].host.c_str(), r.asked.back().first.c_str(), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(rungs[k + 1].model.c_str(), r.asked.back().second.c_str(), what);
      TEST_ASSERT_EQUAL_STRING(kGoodSummary, sum.c_str());
      const std::string line = std::string("failed -> ") + agent::foldFailKindName(kindFor(fail));
      TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains(line.c_str()), what);
    }
  }
}

// One rung's report: the kind, the paid requests the daily cap counts (a 2xx or a timeout
// after the upload; a refusal costs nothing) and whether the rung goes last next cycle.
static void test_rung_report_names_kind_paid_and_degraded() {
  struct Case { Answer a; uint32_t paid; bool degraded; };
  const Case cases[] = {{Answer::Transport, 0, false}, {Answer::Http4xx, 1, false}, {Answer::Park, 1, false},
                        {Answer::Empty, 1, true},      {Answer::Thin, 1, true},     {Answer::Good, 1, false},
                        {Answer::NoUsage, 1, false}};
  for (const Case& c : cases) {
    Rig r;
    const FoldRung rung = r.eng->foldRungs()[0];
    r.answers[key(rung)] = c.a;
    FoldRungReport rep;
    std::string sum;
    r.eng->runFoldRung("1001", "", bigDigest(40 * 1024), rung, sum, &rep);
    TEST_ASSERT_TRUE(rep.fail == kindFor(c.a));
    TEST_ASSERT_EQUAL_UINT32(c.paid, rep.paidRequests);
    TEST_ASSERT_EQUAL(c.degraded, rep.degraded);
    TEST_ASSERT_EQUAL_STRING(rung.model.c_str(), rep.model.c_str());
  }
}

// No (host, model) is asked twice in a cycle, over random keyed sets, priority orders,
// pinned heads, routers and Sub models that equal the head model, with every rung
// failing; and the ladder holds every keyed provider once.
// A random device: keyed set, priority, pinned head, router, Sub models and heads.
static void randomConfig(Rig& r, const std::function<uint32_t(uint32_t)>& rnd) {
  static const char* const hosts[] = {"openai", "anthropic", "mistral", "zai", "cumulo", "custom"};
  r.cfg.keyed.clear();
  for (const char* h : hosts)
    if (rnd(2)) r.cfg.keyed.insert(h);
  std::string prio;
  for (int i = 0, n = (int)rnd(5); i < n; i++) prio += std::string(i ? "," : "") + hosts[rnd(6)];
  r.cfg.priority = prio;
  r.cfg.orchHost = rnd(3) == 0 ? hosts[rnd(6)] : "";
  r.cfg.routerHost = rnd(3) == 0 ? (rnd(2) ? "cumulo" : "zai") : "";
  for (const char* h : hosts) {
    if (rnd(4) == 0) r.cfg.subModels[h] = "model-" + std::string(h);   // a Sub model equal to the head's
    if (rnd(5) == 0) r.cfg.orchModels[h] = "mistral-large-4-0";
  }
}

static void test_no_host_model_is_asked_twice_per_cycle() {
  uint32_t seed = 99;
  const std::function<uint32_t(uint32_t)> rnd = [&seed](uint32_t m) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % m;
  };
  for (int run = 0; run < 400; run++) {
    Rig r;
    randomConfig(r, rnd);
    const std::vector<FoldRung> rungs = r.eng->foldRungs();
    for (const FoldRung& g : rungs) r.answers[key(g)] = kFailures[rnd(7)];
    std::string sum;
    const FoldResult res = r.eng->runFold("1001", "", bigDigest(20 * 1024), sum);
    std::set<std::string> seen;
    for (const auto& a : r.asked)
      TEST_ASSERT_TRUE_MESSAGE(seen.insert(a.first + "/" + a.second).second, "a (host, model) was asked twice");
    TEST_ASSERT_TRUE(rungs.empty() ? res == FoldResult::Deferred : res == FoldResult::Failed);
    TEST_ASSERT_EQUAL((int)rungs.size(), (int)r.asked.size());
    for (const std::string& k : r.cfg.keyed) {
      bool there = false;
      for (const FoldRung& g : rungs) there = there || g.host == k;
      TEST_ASSERT_TRUE_MESSAGE(there, ("a keyed provider is missing from the ladder: " + k).c_str());
    }
  }
}

// The rung order: the head host first on its fold model, the other keyed providers in the
// owner's order, the router only with no key set, the head's Sub model last.
static void test_rung_order_and_kinds() {
  Rig r;
  r.cfg.keyed = {"openai", "anthropic", "mistral", "cumulo"};
  r.cfg.priority = "mistral,anthropic";
  r.cfg.orchModels["mistral"] = "mistral-large-4-0";
  r.cfg.subModels["mistral"] = "mistral-medium-latest";
  const std::vector<FoldRung> rungs = r.eng->foldRungs();
  const char* const want[][2] = {{"mistral", "mistral-small-latest"},
                                 {"anthropic", "model-anthropic"},
                                 {"cumulo", "model-cumulo"},
                                 {"openai", "model-openai"},
                                 {"mistral", "mistral-medium-latest"}};
  TEST_ASSERT_EQUAL(5, (int)rungs.size());
  for (size_t i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_STRING(want[i][0], rungs[i].host.c_str());
    TEST_ASSERT_EQUAL_STRING(want[i][1], rungs[i].model.c_str());
  }
  TEST_ASSERT_TRUE(rungs[0].kind == FoldRungKind::HeadHost);
  TEST_ASSERT_TRUE(rungs[4].kind == FoldRungKind::SubModel);
  // A parked or capped provider is not a rung; with no key the router is the only rung.
  r.cfg.overBudgetHosts.insert("anthropic");
  for (const FoldRung& g : r.eng->foldRungs()) TEST_ASSERT_TRUE(g.host != "anthropic");
  Rig router;
  router.cfg.keyed.clear();
  router.cfg.routerHost = "zai";
  TEST_ASSERT_EQUAL(1, (int)router.eng->foldRungs().size());
  TEST_ASSERT_TRUE(router.eng->foldRungs()[0].kind == FoldRungKind::RouterFallback);
}

// A Large 4 head's first rung runs mistral-small-latest at reasoning effort "none" on the
// real Mistral wire (orchestrator ruling of 16 Nov), pinned only for that request.
static void test_large4_head_folds_on_small_at_effort_none() {
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", agent::foldModelForHead("mistral", "mistral-large-4-0").c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest",
                           agent::foldModelForHead("mistral", "mistral/mistral-large-4-0").c_str());
  TEST_ASSERT_EQUAL_STRING("", agent::foldModelForHead("mistral", "mistral-medium-latest").c_str());
  TEST_ASSERT_EQUAL_STRING("", agent::foldModelForHead("anthropic", "claude-sonnet-5-5").c_str());
  TEST_ASSERT_EQUAL_STRING("none", providers::mistralHeadEffort("mistral-small-latest"));

  harness_test::FakeProviderDeps wire;
  wire.http.script.push_back({"api.mistral.ai", "/v1/chat/completions", 200,
                              "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"" +
                                  std::string("1. Decisions & preferences\\n- x\\n2. Pending items\\n- y") +
                                  "\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}"});
  auto pd = wire.contract();
  pd.orchModel = [](const char* h) {
    const std::string pin = providers::modelOverrideFor(h);
    return pin.empty() ? std::string("mistral-large-4-0") : pin;
  };
  FakePlatform plat;
  FakeConfig cfg;
  cfg.keyed = {"mistral"};
  cfg.priority = "mistral";
  cfg.orchModels["mistral"] = "mistral-large-4-0";
  TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  d.hosts.add("mistral", [&pd](std::string& conv, const std::string& ins, const std::string& in, std::string& out,
                              std::string& err, const agent::HeadTools*, orch::TokenUsage* u) {
    return providers::orchTurnMistral(pd, conv, ins, in, out, err, nullptr, u);
  });
  TurnEngine eng(std::move(d));
  const FoldRung first = eng.foldRungs()[0];
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", first.model.c_str());
  std::string sum;
  FoldRungReport rep;
  TEST_ASSERT_TRUE(eng.runFoldRung("owner", "", "- user: hi\n", first, sum, &rep) == FoldResult::Ok);
  TEST_ASSERT_EQUAL(1, (int)wire.http.seen.size());
  TEST_ASSERT_TRUE(harness_test::bodyHas(wire, 0, "\"model\":\"mistral-small-latest\""));
  TEST_ASSERT_TRUE(harness_test::bodyHas(wire, 0, "\"reasoning_effort\":\"none\""));
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", rep.model.c_str());
  TEST_ASSERT_EQUAL_STRING("", providers::modelOverrideFor("mistral").c_str());   // released
}

// The quality gate: only digests of 8 KB or more are judged; the floor is min(800 B, 30%
// of the previous summary) (none for a first summary); 2 of 5 sections named, leniently.
static void test_quality_gate_bounds() {
  TEST_ASSERT_FALSE(agent::foldAnswerThin("ok", 8 * 1024 - 1, 0));   // a small digest may fold small
  TEST_ASSERT_TRUE(agent::foldAnswerThin("ok", 8 * 1024, 0));
  TEST_ASSERT_TRUE(agent::foldAnswerThin("Noted.", 40 * 1024, 4000));
  std::string padded = "1. Decisions\n- a\n2. Pending\n- b\n";
  while (padded.size() + 29 < 800) padded += "- more detail about the week\n";
  TEST_ASSERT_TRUE(padded.size() < 800);
  TEST_ASSERT_TRUE(agent::foldAnswerThin(padded, 40 * 1024, 4000));    // under min(800 B, 30% of 4,000 B)
  TEST_ASSERT_FALSE(agent::foldAnswerThin(padded, 40 * 1024, 1000));   // 30% of 1,000 B is 300 B
  TEST_ASSERT_FALSE(agent::foldAnswerThin(padded, 40 * 1024, 0));      // a first summary: no size floor
  std::string oneSection(3000, 'x');
  oneSection = "## Decisions\n" + oneSection;
  TEST_ASSERT_TRUE(agent::foldAnswerThin(oneSection, 40 * 1024, 0));   // one section named
  TEST_ASSERT_EQUAL(5, agent::foldSectionsNamed(kGoodSummary));
  TEST_ASSERT_EQUAL(5, agent::foldSectionsNamed("**DECISIONS AND PREFERENCES**\n**Pending items**\n**Key facts**\n"
                                                 "**Owner intent**\n**Device and task state**\n"));
}

// Every live fold output recorded on the PL legs passes the gate against its own digest
// size; a thin answer to a 40 KB digest is refused.
static void test_matcher_passes_every_recorded_fold_output() {
  const std::string dir = "test/support/fixtures/fold_outputs_2026-11";
  DIR* dp = opendir(dir.c_str());
  TEST_ASSERT_NOT_NULL_MESSAGE(dp, "fold_outputs_2026-11");
  int checked = 0;
  for (dirent* e = readdir(dp); e; e = readdir(dp)) {
    const std::string name = e->d_name;
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".txt") != 0) continue;
    std::ifstream f(dir + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    // The file name carries the digest it answered: <leg>_<digestBytes>.txt.
    const size_t us = name.rfind('_');
    const size_t digest = (size_t)std::strtoul(name.substr(us + 1).c_str(), nullptr, 10);
    TEST_ASSERT_TRUE_MESSAGE(digest >= agent::kFoldQualityDigestMin, name.c_str());
    TEST_ASSERT_FALSE_MESSAGE(agent::foldAnswerThin(text, digest, 0), name.c_str());
    checked++;
  }
  closedir(dp);
  TEST_ASSERT_TRUE_MESSAGE(checked >= 4, "fewer than four recorded fold outputs");
  TEST_ASSERT_TRUE(agent::foldAnswerThin("Here is the summary you asked for.", 40 * 1024, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_failure_kind_at_every_rung_is_answered_by_the_next);
  RUN_TEST(test_rung_report_names_kind_paid_and_degraded);
  RUN_TEST(test_no_host_model_is_asked_twice_per_cycle);
  RUN_TEST(test_rung_order_and_kinds);
  RUN_TEST(test_large4_head_folds_on_small_at_effort_none);
  RUN_TEST(test_quality_gate_bounds);
  RUN_TEST(test_matcher_passes_every_recorded_fold_output);
  return UNITY_END();
}
