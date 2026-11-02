// test_router_refusal_repro - the 31 Oct 2026 failure on a real Virtual Nimbus, over a real
// socket, in the code's oldest API: nothing here names anything newer than the base of the
// fix, so this same file ran red there (evidence in the lane's failing-first log). The
// router's exact clamp on a funded wallet, and a refusal that only means "wait", must not
// close Cumulo or say "add credits". Only an empty wallet does. No network, no real keys.
#include <ArduinoJson.h>

#include <cstdio>
#include <string>

#include "fake_router.h"
#include "nimbus/orch/provider_park.h"
#include "rig.h"
#include "test_util.h"

using namespace nimbusd;
using ndtest::FakeRouter;
namespace orch = nimbus::orch;

namespace {
const char* kKey = "cumulo_sk_test_REPRO";

// Byte for byte from the router refusal contract (the cloud repo).
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
const char* kServed =
    "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\","
    "\"text\":\"{\\\"reply\\\":\\\"served\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}";

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

bool parked() {
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = orch::providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, NimbusdRig::steadyMs());
  for (size_t i = 0; i < n; i++)
    if (std::string(e[i].provider) == "cumulo") return e[i].kind != orch::ParkKind::None;
  return false;
}

NimbusdRig::Options optFor(const std::string& tag) {
  NimbusdRig::Options opt;
  opt.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.toolLoop = false;
  return opt;
}

FakeRouter::Answer answer(int status, const char* body) {
  FakeRouter::Answer a;
  a.status = status;
  a.body = body;
  return a;
}

TurnRecord turn(const std::string& tag, FakeRouter::Handler h, int* requests) {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "Z_AI_TOKEN"}) unsetenv(k);
  setenv("CUMULO_API_KEY", kKey, 1);
  FakeRouter router(std::move(h));
  ndtest::RouterRedirect http(kCumuloHost, router.port());
  Config cfg;
  NimbusdRig rig(cfg, optFor(tag), &http);
  orch::providerPark().clear();
  const TurnRecord t = rig.say("owner", "hello");
  if (requests) *requests = router.paidRequests();
  unsetenv("CUMULO_API_KEY");
  return t;
}
}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("=== the 31 Oct clamp on a funded wallet, against a local fake router ===\n");
  int requests = 0, calls = 0;

  // The router's real clamp: a request that declares no cap is refused, every time.
  const TurnRecord clamp = turn("rrr-clamp", [](const FakeRouter::Seen& s) {
    return has(s.body, "\"max_output_tokens\"") ? answer(200, kServed) : answer(402, kClampBody);
  }, &requests);
  c.ok(has(clamp.reply, "served"), "the clamp on a funded wallet is served: " + clamp.reply);
  c.eqi(requests, 2, "one capped resend");
  c.ok(!parked(), "nothing closes");
  c.ok(!has(clamp.reply, "dd credits") && !has(clamp.reply, "out of credit"), "and no one is told to add credits");

  const TurnRecord held = turn("rrr-held", [&calls](const FakeRouter::Seen&) {
    return calls++ == 0 ? answer(429, kHeldBody) : answer(200, kServed);
  }, &requests);
  c.ok(has(held.reply, "served"), "a wallet held by other calls is waited out: " + held.reply);
  c.ok(!parked(), "nothing closes");

  const TurnRecord empty = turn("rrr-empty", [](const FakeRouter::Seen&) { return answer(402, kEmptyBody); }, &requests);
  c.ok(parked(), "an empty wallet closes it");
  c.ok(has(empty.reply, "dd credits"), "and says so: " + empty.reply);

  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
