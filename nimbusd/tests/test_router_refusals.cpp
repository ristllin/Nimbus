// test_router_refusals - a real Virtual Nimbus against a local fake router speaking the
// exact JSON of the router refusal contract, over a real socket. The reason decides what
// the instance does: only an empty wallet or no entitlement parks with "add credits", the
// clamp resends once capped, a retryable refusal does not park, a parked provider
// recovers by itself, and every re-check logs its outcome. No network, no real keys.
#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../../test/test_router_refusal_contract/contract_rows.h"
#include "engine_thread.h"
#include "fake_router.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/router_refusals.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;
using ndtest::FakeRouter;
namespace orch = nimbus::orch;

static const char* kKey = "cumulo_sk_test_REFUSALS";

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void clearEnv() {
  for (const char* k : {"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "MISTRAL_API_KEY", "TAVILY_API_KEY", "CUMULO_API_KEY", "Z_AI_TOKEN"})
    unsetenv(k);
}

static NimbusdRig::Options baseOpt(const std::string& tag) {
  NimbusdRig::Options opt;
  opt.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  opt.embeddings = false;
  opt.embedDims = 64;
  opt.toolLoop = false;
  return opt;
}

static std::string contractBody(const char* reason) {
  for (const ContractBody& b : kContractBodies)
    if (std::string(b.reason) == reason) return b.json;
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason + "\",\"message\":\"refused\",\"charged\":false}";
}

static int contractStatus(const char* reason) {
  for (const ContractRow& r : kContractRows)
    if (std::string(r.reason) == reason) return r.http;
  return 400;
}

// A served reply in the wire shape of the path it answered.
static FakeRouter::Answer served(const FakeRouter::Seen& s, const std::string& reply = "served") {
  const std::string turn = "{\\\"reply\\\":\\\"" + reply + "\\\",\\\"memory\\\":\\\"\\\"}";
  FakeRouter::Answer a;
  if (has(s.path, "/messages"))
    a.body = "{\"content\":[{\"type\":\"tool_use\",\"name\":\"orch_turn\",\"input\":{\"reply\":\"" + reply +
             "\",\"memory\":\"\"}}],\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}";
  else if (has(s.path, "/responses"))
    a.body = "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"" +
             turn + "\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}";
  else if (has(s.path, "/conversations"))
    a.body = "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\",\"content\":\"" + turn +
             "\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";
  else if (has(s.path, "/router/models"))
    a.body = "{\"data\":[]}";
  else
    a.body = "{\"choices\":[{\"message\":{\"content\":\"" + turn + "\"}}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";
  return a;
}

static FakeRouter::Answer refuse(const char* reason) {
  FakeRouter::Answer a;
  a.status = contractStatus(reason);
  a.body = contractBody(reason);
  return a;
}

// The router's real clamp, with the wallet of 31 Oct: a request that declares no cap, or
// one above what fits, is refused; a request that declares a cap that fits is served.
static long declaredCap(const std::string& body) {
  JsonDocument d;
  if (deserializeJson(d, body, DeserializationOption::NestingLimit(40))) return -1;
  long best = -1;
  for (const char* k : {"max_tokens", "max_completion_tokens", "max_output_tokens"})
    if (d[k].is<long>() && d[k].as<long>() > best) best = d[k].as<long>();
  return best;
}
static FakeRouter::Answer clampedWallet(const FakeRouter::Seen& s) {
  const long cap = declaredCap(s.body);
  if (cap < 0 || cap > 74904) return refuse("insufficient_credits_for_call");
  return served(s);
}

// Does the park hold a refusal for `provider`? The record, not a seam's answer: a due
// re-check reads open to a reader while the park still stands.
static bool parked(const char* provider) {
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = orch::providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, NimbusdRig::steadyMs());
  for (size_t i = 0; i < n; i++)
    if (std::string(e[i].provider) == provider) return e[i].kind != orch::ParkKind::None;
  return false;
}

// A park whose re-check fell due: the clock on a live rig cannot be moved, so the record
// is written as if it were parked a whole interval ago.
static void parkDue(const char* provider, orch::ParkCause cause) {
  orch::providerPark().park(provider, orch::PaidSeam::Chat, 0, orch::ProviderPark::Refusal{orch::ParkKind::Billing, 402, cause},
                            NimbusdRig::steadyMs() - orch::ProviderPark::kRecheckMs - 1);
}

struct Harness {
  FakeRouter router;
  ndtest::RouterRedirect http;
  Config cfg;
  NimbusdRig rig;
  Harness(const std::string&, FakeRouter::Handler h, NimbusdRig::Options opt)
      : router(std::move(h)), http(kCumuloHost, router.port()), rig(cfg, opt, &http) {
    orch::providerPark().clear();
  }
  std::vector<std::string> logWith(const std::string& needle) {
    std::vector<std::string> out;
    for (const std::string& l : NimbusdRig::harnessLog())
      if (has(l, needle)) out.push_back(l);
    return out;
  }
};

// 31 Oct on Lumi, as a client would meet it: a fold or a turn with a funded wallet under
// 500 credits. The router's exact clamp, one resend capped at what fits, a real reply, no
// park on any seam, and the owner told plainly with no "add credits".
static void testTheClampOnAFundedWallet(ndtest::Ctx& c) {
  std::printf("  -- the clamp resends once capped and serves --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  Harness h("rr-clamp", clampedWallet, baseOpt("rr-clamp"));
  const TurnRecord t = h.rig.say("owner", "hello");
  c.eqi(h.router.paidRequests(), 2, "two requests: the refused one and its capped resend");
  const auto seen = h.router.seen();
  c.ok(seen.size() >= 2 && declaredCap(seen[0].body) < 0, "the first declares no cap, as the head sends it");
  c.ok(seen.size() >= 2 && declaredCap(seen[1].body) == 74904, "the resend is capped at what the body says fits");
  c.ok(has(t.reply, "served"), "the owner gets the reply");
  c.ok(has(t.reply, "larger than the remaining Cumulo credits allow"), "and is told plainly why it is short");
  c.ok(!has(t.reply, "Add credits") && !has(t.reply, "out of credit"), "never told to add credits");
  c.ok(!parked("cumulo"), "nothing parked on any seam");
  // The next turn: the note is not repeated.
  const TurnRecord t2 = h.rig.say("owner", "again");
  c.ok(has(t2.reply, "served") && !has(t2.reply, "larger than"), "the note is not on every turn");
  clearEnv();
}

// Below what any cap could fit: this call fails, plainly, and nothing parks.
static void testNoCapFits(ndtest::Ctx& c) {
  std::printf("  -- no cap fits: the call fails, nothing parks --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  Harness h("rr-nocap", [](const FakeRouter::Seen&) {
    FakeRouter::Answer a;
    a.status = 402;
    a.body = "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\","
             "\"message\":\"This request could cost up to 525 credits and 2 are available.\",\"charged\":false,"
             "\"retryable\":false,\"balanceCredits\":2,\"availableCredits\":2,\"heldCredits\":0,\"estimatedCredits\":525,"
             "\"maxAffordableOutputTokens\":0,\"retryHint\":\"top_up\"}";
    return a;
  }, baseOpt("rr-nocap"));
  const TurnRecord t = h.rig.say("owner", "hello");
  c.ok(has(t.reply, "larger than the remaining credits allow"), "the reply names the real reason");
  c.ok(!has(t.reply, "Add credits") && !has(t.reply, "rejected"), "not an empty wallet, not a key");
  c.ok(!parked("cumulo"), "nothing parked");
  c.eqi(h.router.paidRequests(), 1, "no resend, no same-host retry");
  clearEnv();
}

// The empty wallet, the one refusal that does park: told once, no request while parked,
// and it recovers by itself the moment the wallet is refilled, with the outcome logged.
static void testEmptyWalletParksAndRecovers(ndtest::Ctx& c) {
  std::printf("  -- the empty wallet parks, is told once, and recovers by itself --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  Harness h("rr-empty", [](const FakeRouter::Seen&) { return refuse("insufficient_credits"); }, baseOpt("rr-empty"));
  const TurnRecord t = h.rig.say("owner", "hello");
  c.ok(parked("cumulo"), "the empty wallet parks every seam");
  c.ok(has(t.reply, "Add credits"), "and says so");
  c.eqi(orch::providerPark().causeOf("cumulo") == orch::ParkCause::WalletEmpty, 1, "the park knows it is the wallet");
  // A hosted head is asked on every turn (free when refused), and it keeps its words.
  const TurnRecord t2 = h.rig.say("owner", "again");
  c.ok(has(t2.reply, "Add credits"), "the next message hears it too");
  c.ok(parked("cumulo"), "still parked for the other seams");

  // The re-check due, still empty: one request, refused again, one outcome line, quiet.
  orch::providerPark().clear();
  parkDue("cumulo", orch::ParkCause::WalletEmpty);
  h.router.clearSeen();
  h.rig.recheckTick();
  c.eqi(h.router.paidRequests(), 1, "the idle re-check sends exactly one real request");
  c.ok(parked("cumulo"), "refused again: still parked");
  c.eqi((long)h.logWith("cumulo re-check refused again (billing, HTTP 402)").size(), 1, "its outcome is logged");
  h.router.clearSeen();
  h.rig.recheckTick();
  c.eqi(h.router.paidRequests(), 0, "and the next tick waits a whole interval");

  // The wallet refilled: the seam comes back with nobody touching it.
  h.router.setHandler([](const FakeRouter::Seen& s) { return served(s, "ok"); });
  orch::providerPark().clear();
  parkDue("cumulo", orch::ParkCause::WalletEmpty);
  h.router.clearSeen();
  h.rig.recheckTick();
  c.eqi(h.router.paidRequests(), 1, "one request");
  c.ok(!parked("cumulo"), "recovered by itself");
  c.eqi((long)h.logWith("cumulo re-check answered - open again").size(), 1, "the recovery is logged");
  const TurnRecord t3 = h.rig.say("owner", "back?");
  c.ok(has(t3.reply, "ok"), "turns are served again");

  // A top-up on a hosted instance: the very next message is answered and ends the park.
  orch::providerPark().clear();
  orch::providerPark().park("cumulo", orch::PaidSeam::Chat, 0,
                            orch::ProviderPark::Refusal{orch::ParkKind::Billing, 402, orch::ParkCause::WalletEmpty},
                            NimbusdRig::steadyMs());
  c.ok(parked("cumulo"), "parked, not due");
  const TurnRecord t4 = h.rig.say("owner", "topped up");
  c.ok(has(t4.reply, "ok") && !parked("cumulo"), "the next message is served and ends the park");
  clearEnv();
}

// Lumi's trace: Cumulo parked and last in the list, another head answering every turn. On
// the base the re-check's token went to a read and no request followed. Here reads spend
// nothing, the turn does not touch Cumulo, and the idle tick asks it.
static void testParkedFallbackHostIsReallyAsked(ndtest::Ctx& c) {
  std::printf("  -- a parked provider nothing calls is still asked, once --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  setenv("MISTRAL_API_KEY", "mistral-test-direct", 1);
  auto opt = baseOpt("rr-idle");
  FakeRouter router([](const FakeRouter::Seen& s) { return served(s, "router ok"); });
  // The direct Mistral head goes to the fake as well: its host answers the chat shape.
  struct Both : agent::HttpTransport {
    FakeRouter* r;
    nimbusd::DaemonHttpTransport real;
    bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
      agent::HttpRequest c = req;
      c.host = "127.0.0.1";
      c.port = (uint16_t)r->port();
      c.tls = false;
      return real.exec(c, out, err);
    }
  } both;
  both.r = &router;
  Config cfg;
  NimbusdRig rig(cfg, opt, &both);
  orch::providerPark().clear();
  parkDue("cumulo", orch::ParkCause::WalletEmpty);
  router.clearSeen();
  for (int i = 0; i < 5; i++) {
    rig.engine().canFoldNow();
    rig.engine().headOnFallback();
  }
  c.eqi(router.paidRequests(), 0, "reading the candidates sends nothing");
  c.ok(parked("cumulo"), "and the park stands");
  rig.recheckTick();
  c.eqi(router.paidRequests(), 1, "the idle re-check makes the one request");
  bool toRouter = false;
  for (const auto& s : router.seen()) toRouter = toRouter || has(s.path, "/router/");
  c.ok(toRouter, "and it goes to the router, not the head");
  c.ok(!parked("cumulo"), "recovered");
  clearEnv();
}

// retryable:true never parks. credits_held_in_flight: wait what it says, same request.
static void testRetryableDoesNotPark(ndtest::Ctx& c) {
  std::printf("  -- retryable refusals do not park --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  int refused = 0;
  Harness h("rr-held", [&refused](const FakeRouter::Seen& s) {
    if (refused++ == 0) return refuse("credits_held_in_flight");
    return served(s);
  }, baseOpt("rr-held"));
  const auto t0 = std::chrono::steady_clock::now();
  const TurnRecord t = h.rig.say("owner", "hello");
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  c.ok(has(t.reply, "served"), "served after the wait");
  c.ok(waited >= 5000, "waited at least retryAfterMs (" + std::to_string(waited) + " ms)");
  const auto seen = h.router.seen();
  c.ok(seen.size() >= 2 && seen[0].body == seen[1].body, "the same request, unchanged");
  c.ok(!parked("cumulo"), "not parked");
  clearEnv();
}

// fail_over: the account is fine. Answered on the router default, the pick kept, nothing
// parked, no "add credits", and the upstream not asked again inside its wait.
static void testUpstreamOutOfQuotaFailsOver(ndtest::Ctx& c) {
  std::printf("  -- an upstream out of quota fails over, the account is fine --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  Harness h("rr-quota", [](const FakeRouter::Seen& s) {
    if (has(s.path, "/router/anthropic/")) return refuse("upstream_quota_exhausted");
    return served(s, "on the default");
  }, baseOpt("rr-quota"));
  h.rig.applyOrchModel("cumulo", "anthropic/claude-sonnet-5");
  const TurnRecord t = h.rig.say("owner", "hello");
  c.ok(has(t.reply, "on the default"), "answered on the router default");
  c.ok(!has(t.reply, "Add credits") && !has(t.reply, "out of credit"), "the wallet is not blamed");
  c.eq(h.rig.modelFor("cumulo"), "anthropic/claude-sonnet-5", "the pick is kept");
  c.ok(!parked("cumulo"), "nothing parked");
  h.router.clearSeen();
  const TurnRecord t2 = h.rig.say("owner", "again");
  int toCooled = 0;
  for (const auto& s : h.router.seen()) toCooled += has(s.path, "/router/anthropic/") ? 1 : 0;
  c.eqi(toCooled, 0, "the cooled upstream is not asked inside its wait");
  c.ok(has(t2.reply, "on the default"), "still answered");
  clearEnv();
}

// Every row of the contract through the real instance: the park only on its reasons, the
// wallet words only for the wallet, and always an answer.
static void testEveryReasonEndToEnd(ndtest::Ctx& c) {
  std::printf("  -- every reason in the contract, end to end --\n");
  for (const ContractRow& row : kContractRows) {
    const std::string reason = row.reason;
    if (reason == "credits_held_in_flight" || reason == "rate_limited" || reason == "router_internal_error") continue;
    if (reason == "insufficient_credits_for_call") continue;
    clearEnv();
    setenv("CUMULO_API_KEY", kKey, 1);
    Harness h("rr-all-" + reason, [row](const FakeRouter::Seen&) { return refuse(row.reason); }, baseOpt("rr-all-" + reason));
    const TurnRecord t = h.rig.say("owner", "hello");
    const std::string action = row.action;
    const bool parks = action == "park_billing" || action == "park_auth";
    c.ok(parked("cumulo") == parks, reason + (parks ? " parks" : " does not park"));
    const bool wallet = reason == "insufficient_credits" || reason == "credits_or_subscription_required";
    c.ok(has(t.reply, "dd credits") == wallet, reason + ": \"add credits\" only for the wallet");
    c.ok(!t.reply.empty(), reason + ": the owner gets an answer");
  }
  clearEnv();
}

// An unknown reason, and a bare 402: a per-call failure, never a park.
static void testUnknownReason(ndtest::Ctx& c) {
  std::printf("  -- an unknown reason is a per-call failure --\n");
  for (const char* body : {"{\"error\":\"reason_from_the_future\",\"reason\":\"reason_from_the_future\"}", "{}", ""}) {
    clearEnv();
    setenv("CUMULO_API_KEY", kKey, 1);
    const std::string b = body;
    Harness h("rr-unknown", [b](const FakeRouter::Seen&) {
      FakeRouter::Answer a;
      a.status = 402;
      a.body = b;
      return a;
    }, baseOpt("rr-unknown"));
    const TurnRecord t = h.rig.say("owner", "hello");
    c.ok(!parked("cumulo"), "402 " + b + " does not park");
    c.ok(!has(t.reply, "dd credits"), "and does not say add credits");
  }
  clearEnv();
}

// The embedding seam over the router, on its real path: the token and the class by reason.
static void testEmbeddings(ndtest::Ctx& c) {
  std::printf("  -- embeddings over the router read the reason --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  auto opt = baseOpt("rr-embed");
  opt.embeddings = true;
  opt.embedHost = "mistral";
  opt.embedModel = "mistral-embed";
  opt.embedDims = 8;
  struct Row {
    const char* reason;
    orch::EmbedFail kind;
  };
  const Row rows[] = {
      {"insufficient_credits_for_call", orch::EmbedFail::CallTooLarge},
      {"insufficient_credits", orch::EmbedFail::OutOfCredit},
      {"upstream_quota_exhausted", orch::EmbedFail::RouterHeld},
      {"model_not_priced", orch::EmbedFail::ProviderError},
      {"account_suspended", orch::EmbedFail::AccountHeld},
  };
  Harness h("rr-embed", [](const FakeRouter::Seen&) { return refuse("insufficient_credits"); }, opt);
  for (const Row& r : rows) {
    h.router.setHandler([r](const FakeRouter::Seen&) { return refuse(r.reason); });
    std::string err, detail;
    const auto v = h.rig.embedWith("a note", err, "mistral", "mistral-embed", 8);
    c.ok(v.empty(), std::string(r.reason) + ": no vector");
    c.eqi((int)orch::classifyEmbedFail(err, detail), (int)r.kind, std::string(r.reason) + ": " + err);
  }
  clearEnv();
}

// GET /router/models: an admin suspended the account. The key is not "rejected".
static void testVerifyReadsSuspended(ndtest::Ctx& c) {
  std::printf("  -- the verify badge names a suspension --\n");
  std::string reason;
  c.eqi(probeVerdict("cumulo", 403, reason, contractBody("account_suspended")), -1, "could not verify, not rejected");
  c.eq(reason, "suspended", "the badge reason");
  c.eqi(probeVerdict("cumulo", 402, reason, contractBody("credits_or_subscription_required")), -1, "402");
  c.eq(reason, "nocredits", "needs credits");
  c.eqi(probeVerdict("cumulo", 403, reason, ""), 0, "a bare 403 is still a rejected key");
  c.eqi(probeVerdict("openai", 403, reason, contractBody("account_suspended")), 0, "and only the router says so");
}

// GET /api/orch names the park, its reason and the next check.
static void testOrchShowsThePark(ndtest::Ctx& c) {
  std::printf("  -- /api/orch names the park and its next check --\n");
  clearEnv();
  setenv("CUMULO_API_KEY", kKey, 1);
  Harness h("rr-orch", [](const FakeRouter::Seen&) { return refuse("budget_exceeded"); }, baseOpt("rr-orch"));
  const TurnRecord t = h.rig.say("owner", "hello");
  c.ok(has(t.reply, "monthly limit") && !has(t.reply, "dd credits"), "a key's budget is not an empty wallet: " + t.reply);
  EngineThread eng(&h.rig);
  ReplyBuffer replies;
  WebApi api(&h.rig, &eng, &replies);
  ApiResp out;
  api.handle("GET", "/api/orch", "", out);
  JsonDocument d;
  c.ok(!deserializeJson(d, out.body), "the state parses");
  c.eq(d["parks"][0]["provider"] | "", "cumulo", "the parked provider");
  c.ok(has(d["parks"][0]["why"] | "", "monthly limit"), "its reason in plain words");
  c.ok((d["parks"][0]["recheckInS"] | 0) > 0, "its next check");
  clearEnv();
}

int main() {
  ndtest::Ctx c;
  std::printf("=== Cumulo router refusals against a local fake router (offline) ===\n");
  testTheClampOnAFundedWallet(c);
  testNoCapFits(c);
  testEmptyWalletParksAndRecovers(c);
  testParkedFallbackHostIsReallyAsked(c);
  testRetryableDoesNotPark(c);
  testUpstreamOutOfQuotaFailsOver(c);
  testEveryReasonEndToEnd(c);
  testUnknownReason(c);
  testEmbeddings(c);
  testVerifyReadsSuspended(c);
  testOrchShowsThePark(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
