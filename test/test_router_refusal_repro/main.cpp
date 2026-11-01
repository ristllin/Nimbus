#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/router_refusal.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/provider_park.h"

// The reproduction of the 31 Oct 2026 Lumi failure, in the base's own API: the router's
// worst-case clamp on a funded wallet parked Cumulo for every seam, and the re-check
// logged "one probe request" with no request behind it. Nothing here names a new API, so
// the same file ran red on the base (evidence/repro-fail-before.log) before the fix.

namespace orch = nimbus::orch;
namespace providers = agent::providers;
namespace rf = agent::router_refusal;
using harness_test::FakePlatform;
using harness_test::FakeConfig;
using orch::PaidSeam;
using orch::ParkKind;
using orch::providerPark;

namespace {

// Byte for byte from docs-internal/router-refusal-contract.md (the cumulo repo).
const char* kClampBody =
    "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\","
    "\"message\":\"This request could cost up to 525 credits and 309 are available. Nothing was charged. "
    "Limit the output to 74904 tokens or fewer, or top up.\",\"charged\":false,\"retryable\":false,"
    "\"balanceCredits\":309,\"availableCredits\":309,\"heldCredits\":0,\"estimatedCredits\":525,"
    "\"maxAffordableOutputTokens\":74904,\"retryHint\":\"lower_max_output\"}";
const char* kEmptyBody =
    "{\"error\":\"insufficient_credits\",\"reason\":\"insufficient_credits\",\"message\":\"Your credit balance is "
    "empty. Top up to continue.\",\"balanceCredits\":0,\"charged\":false,\"retryable\":false}";
const char* kHeldBody =
    "{\"error\":\"credits_held_in_flight\",\"reason\":\"credits_held_in_flight\",\"message\":\"Your other requests "
    "in progress are holding your credits. Nothing was charged. This clears when they finish, so try again in a "
    "moment.\",\"balanceCredits\":100,\"availableCredits\":0,\"heldCredits\":100,\"charged\":false,"
    "\"retryable\":true,\"retryAfterMs\":5000}";
const char* kQuotaBody =
    "{\"error\":\"upstream_quota_exhausted\",\"reason\":\"upstream_quota_exhausted\",\"provider\":\"openai\","
    "\"upstreamStatus\":429,\"message\":\"The platform's openai account is out of quota. This is not your balance, "
    "and you were not charged. Try again later or use another provider.\",\"charged\":false,\"retryable\":true,"
    "\"retryAfterMs\":600000}";

std::string bodyOf(const char* reason) {
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason +
         "\",\"message\":\"refused\",\"charged\":false}";
}

}  // namespace

void setUp() { providers::clearHostParks(); }
void tearDown() { providers::clearHostParks(); }

// Only a truly empty wallet parks as billing. A clamp on a funded wallet parks nothing.
static void test_the_clamp_on_a_funded_wallet_does_not_park() {
  TEST_ASSERT_EQUAL_INT((int)ParkKind::Billing, (int)orch::parkKindFor(402, kEmptyBody));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindFor(402, kClampBody));
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, kClampBody, 1000);
  TEST_ASSERT_TRUE(providerPark().available("cumulo", PaidSeam::Embeddings, 0, 2000).open);
}

// retryable:true never parks, and neither does a 402 or 429 about the platform.
static void test_a_retryable_refusal_does_not_park() {
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindFor(429, kHeldBody));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindFor(429, kQuotaBody));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindFor(402, bodyOf("funding_cap_reached")));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindFor(429, bodyOf("ceiling_reached")));
  TEST_ASSERT_EQUAL_INT((int)orch::StepErr::RateLimited, (int)orch::classifyHttpStatus(429, kHeldBody));
}

// The head's refusal kind: a clamp is not "credits or a plan are missing".
static void test_the_head_does_not_read_the_clamp_as_no_credits() {
  rf::RouterRefusal r;
  r.status = 402;
  r.reason = "insufficient_credits_for_call";
  TEST_ASSERT_TRUE(rf::classifyRefusal(r) != rf::RefusalKind::Account);
  TEST_ASSERT_TRUE(rf::refusalCause(rf::classifyRefusal(r), "x").find("Add credits") == std::string::npos);
}

// A host that is only a failover candidate: reading the candidate list must not spend its
// one re-check. On the base foldHostCandidates hands out the token and logs "one probe
// request"; the fold then runs on the first host, nothing reaches cumulo, and the lease
// runs out into another 30 minute wait.
static void test_reading_the_fold_candidates_does_not_spend_the_recheck() {
  FakePlatform plat;
  FakeConfig cfg;
  cfg.keyed = {"mistral", "cumulo"};
  cfg.keys["cumulo"] = "cumulo_fake";
  cfg.priority = "mistral,cumulo";   // Lumi: Cumulo is last
  agent::TurnEngine::Deps d;
  d.cfg = cfg.contract();
  d.platform = plat.contract();
  providers::ProviderDeps pd;
  pd.nowMs = [&plat] { return plat.ms; };
  d.hosts.add("mistral", [](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                            const agent::HeadTools*, orch::TokenUsage*) {
    out = "{\"reply\":\"sum\"}";
    return true;
  });
  int cumuloCalls = 0;
  d.hosts.add("cumulo", [&cumuloCalls](std::string&, const std::string&, const std::string&, std::string& out,
                                       std::string&, const agent::HeadTools*, orch::TokenUsage*) {
    ++cumuloCalls;
    out = "{\"reply\":\"ok\"}";
    return true;
  });
  d.hosts.parked = [&pd](const std::string& h, providers::HostPark* why) { return providers::hostParked(pd, h, why); };
  agent::TurnEngine eng(std::move(d));
  plat.ms = 1000;
  providerPark().park("cumulo", PaidSeam::Chat, 0, ParkKind::Billing, 402, plat.ms);
  plat.ms += orch::ProviderPark::kRecheckMs + 1;
  eng.canFoldNow();
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, plat.ms);
  TEST_ASSERT_EQUAL_UINT(1, n);
  TEST_ASSERT_FALSE_MESSAGE(e[0].probing, "a read spent the re-check with no request behind it");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_clamp_on_a_funded_wallet_does_not_park);
  RUN_TEST(test_a_retryable_refusal_does_not_park);
  RUN_TEST(test_the_head_does_not_read_the_clamp_as_no_credits);
  RUN_TEST(test_reading_the_fold_candidates_does_not_spend_the_recheck);
  return UNITY_END();
}
