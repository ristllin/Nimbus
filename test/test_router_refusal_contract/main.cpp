#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "../../lib/harness/src/providers/wire.h"   // exchange - the one request seam
#include "contract_rows.h"   // GENERATED from the cloud repo's contract
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/router_refusal.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/head_switch.h"
#include "nimbus/orch/health_rows.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/router_refusals.h"
#include "nimbus/orch/voice_route.h"

// The router refusal contract, over the whole table, on every seam the park closes: the
// reason decides, never the status. Then the clamp's capped resend on the wire, and the
// re-check of a parked provider: claimed only when a request leaves, one outcome line per
// probe, and an idle provider recovers by itself.

namespace orch = nimbus::orch;
namespace providers = agent::providers;
namespace rf = agent::router_refusal;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using orch::PaidSeam;
using orch::ParkCause;
using orch::ParkKind;
using orch::providerPark;
using orch::RouterAction;

namespace {

std::string refusalBody(const char* reason) {
  return std::string("{\"error\":\"") + reason + "\",\"reason\":\"" + reason +
         "\",\"message\":\"refused\",\"charged\":false}";
}

std::string contractBody(const char* reason) {
  for (const ContractBody& b : kContractBodies)
    if (std::strcmp(b.reason, reason) == 0) return b.json;
  return refusalBody(reason);
}

const orch::RouterReasonRow* rowOf(const char* reason) { return orch::routerReasonRow(reason); }

// Does the park hold a refusal? The record, not a seam's answer: a due re-check reads open
// to a reader while the park still stands.
bool anySeamClosed(const char* provider, uint32_t now) {
  orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
  const size_t n = providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, now);
  for (size_t i = 0; i < n; i++)
    if (std::strcmp(e[i].provider, provider) == 0 && e[i].kind != ParkKind::None) return true;
  return false;
}

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int linesWith(const char* needle) {
  int n = 0;
  for (const auto& l : LogCapture::lines()) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

}  // namespace

void setUp() {
  LogCapture::install();
  providers::clearHostParks();
  // The park's outcome lines reach the log once a host was asked about (hostParked wires it).
  providers::ProviderDeps none;
  providers::hostParked(none, "none");
}
void tearDown() {
  agent::hlog::setSink(nullptr);
  providers::clearHostParks();
}

// ---- the table -----------------------------------------------------------------------

// lib/core's table and the contract's agree, both ways: a reason the contract adds with no
// row here, or a row it dropped, fails.
static void test_the_table_is_the_contract() {
  std::set<std::string> seen;
  for (const ContractRow& c : kContractRows) {
    const orch::RouterReasonRow* r = rowOf(c.reason);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, c.reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.http, r->http, c.reason);
    TEST_ASSERT_EQUAL_MESSAGE(c.retryable, r->retryable, c.reason);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.action, orch::routerActionSlug(r->action), c.reason);
    seen.insert(c.reason);
  }
  TEST_ASSERT_TRUE(seen.size() >= 35);
  for (const orch::RouterReasonRow& r : orch::kRouterReasons)
    TEST_ASSERT_TRUE_MESSAGE(seen.count(r.reason) == 1, r.reason);
}

// The class: only park_billing and park_auth close Cumulo, on every seam, and nothing else
// does. retryable:true never parks.
static void test_only_the_park_reasons_close_cumulo_on_every_seam() {
  for (const ContractRow& c : kContractRows) {
    const orch::RouterAction a = rowOf(c.reason)->action;
    const bool billing = a == RouterAction::ParkBilling, auth = a == RouterAction::ParkAuth;
    const std::string body = contractBody(c.reason);
    for (int s = 0; s < orch::kPaidSeamCount; s++) {
      providerPark().clear();
      const ParkKind k = providerPark().noteAnswer("cumulo", (PaidSeam)s, 0, c.http, body, 1000);
      const ParkKind want = billing ? ParkKind::Billing : auth ? ParkKind::Auth : ParkKind::None;
      TEST_ASSERT_EQUAL_INT_MESSAGE((int)want, (int)k, c.reason);
      const bool closed = anySeamClosed("cumulo", 2000);
      TEST_ASSERT_EQUAL_MESSAGE(billing || auth, closed, c.reason);
      if (c.retryable) TEST_ASSERT_FALSE_MESSAGE(closed, c.reason);
    }
    TEST_ASSERT_EQUAL_MESSAGE(billing, orch::isBillingRefusal(c.http, body), c.reason);
    if (c.http == 402 || c.http == 429 || c.http == 400)
      TEST_ASSERT_EQUAL_MESSAGE(billing, orch::classifyHttpStatus(c.http, body) == orch::StepErr::Billing, c.reason);
  }
}

// An unknown reason is a per-call failure. A 402 with no reason on the router is too; the
// same 402 from a provider's own API still means billing (every recorded shape).
static void test_unknown_reason_and_bare_402() {
  const std::string unknown = refusalBody("a_reason_from_the_future");
  for (int status : {400, 401, 402, 403, 429, 500, 502}) {
    providerPark().clear();
    TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, status, unknown, 1));
    TEST_ASSERT_FALSE(anySeamClosed("cumulo", 2));
  }
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindForProvider("cumulo", 402, "{}"));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::None, (int)orch::parkKindForProvider("cumulo", 402, ""));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::Auth, (int)orch::parkKindForProvider("cumulo", 401, ""));   // a lost body
  TEST_ASSERT_EQUAL_INT((int)ParkKind::Billing, (int)orch::parkKindForProvider("mistral", 402, "{\"detail\":\"Payment Required\"}"));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::Billing, (int)orch::parkKindForProvider("openai", 402, "{\"error\":{\"message\":\"Payment Required\"}}"));
  TEST_ASSERT_EQUAL_INT((int)ParkKind::Billing, (int)orch::parkKindFor(402, "{\"error\":\"Payment required.\"}"));   // a sentence, not a reason
}

// Text a provider echoes never names a reason: the word inside a message does not count.
static void test_a_reason_quoted_in_a_message_is_not_a_reason() {
  const std::string echoed =
      "{\"error\":{\"message\":\"you said: insufficient_credits_for_call\"},\"note\":\"reason\"}";
  TEST_ASSERT_EQUAL_STRING("", orch::readRouterAnswer(echoed).reason.c_str());
  TEST_ASSERT_NULL(orch::readRouterAnswer(echoed).row);
  // A device error line: only a whole word.
  TEST_ASSERT_EQUAL_STRING("insufficient_credits_for_call",
                           orch::readRouterAnswer("HTTP 402 insufficient_credits_for_call").reason.c_str());
  TEST_ASSERT_EQUAL_STRING("insufficient_credits", orch::readRouterAnswer("HTTP 402 insufficient_credits").reason.c_str());
  TEST_ASSERT_EQUAL_STRING("", orch::readRouterAnswer("HTTP 402 not_insufficient_credits_x").reason.c_str());
}

// What a body says beyond its reason, from the contract's own bytes; a cut body still names it.
static void test_the_answer_is_read_whole() {
  orch::RouterAnswer a = orch::readRouterAnswer(contractBody("insufficient_credits_for_call"));
  TEST_ASSERT_EQUAL_INT64(74904, a.maxAffordableOutputTokens);
  TEST_ASSERT_FALSE(a.retryable);
  TEST_ASSERT_EQUAL_INT((int)RouterAction::RetryCapped, (int)a.action());
  a = orch::readRouterAnswer(contractBody("credits_held_in_flight"));
  TEST_ASSERT_TRUE(a.retryable);
  TEST_ASSERT_EQUAL_UINT32(5000, a.retryAfterMs);
  const std::string cut = contractBody("insufficient_credits_for_call").substr(0, 90);
  TEST_ASSERT_EQUAL_STRING("insufficient_credits_for_call", orch::readRouterAnswer(cut).reason.c_str());
  // A raw HTTP answer: headers, then the body.
  a = orch::readRouterAnswer("content-type: application/json\r\n\r\n" + refusalBody("account_suspended"));
  TEST_ASSERT_EQUAL_STRING("account_suspended", a.reason.c_str());
}

// ---- the owner's words ---------------------------------------------------------------

// Only an empty wallet or no entitlement says "add credits". Every cause fits the engine's
// limit whole and is a per-call refusal to the engine, whatever the reason.
static void test_add_credits_only_when_it_is_true() {
  for (const ContractRow& c : kContractRows) {
    rf::RouterRefusal r;
    r.status = c.http;
    r.reason = c.reason;
    r.firstRequest = true;
    const rf::RefusalKind k = rf::classifyRefusal(r);
    const std::string cause = rf::refusalCause(k, "anthropic/claude-sonnet-5");
    const bool emptyWallet = std::strcmp(c.reason, "insufficient_credits") == 0 ||
                             std::strcmp(c.reason, "credits_or_subscription_required") == 0;
    TEST_ASSERT_EQUAL_MESSAGE(emptyWallet, has(cause, "dd credits"), c.reason);
    TEST_ASSERT_TRUE_MESSAGE(cause.size() <= rf::kEngineCauseMax, c.reason);
    if (cause.empty()) continue;
    TEST_ASSERT_TRUE_MESSAGE(rf::isRefusalCause(cause), c.reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)orch::StepErr::Rejected, (int)orch::classifyStepError(cause), c.reason);
  }
  // Voice has its own lines over the same table.
  for (const ContractRow& c : kContractRows) {
    const std::string line = orch::voiceRefusalStatus(c.reason);
    const bool emptyWallet = std::strcmp(c.reason, "insufficient_credits") == 0 ||
                             std::strcmp(c.reason, "credits_or_subscription_required") == 0;
    TEST_ASSERT_EQUAL_MESSAGE(emptyWallet, has(line, "dd credits"), c.reason);
    TEST_ASSERT_FALSE_MESSAGE(has(line, "out of credit"), c.reason);
  }
  rf::RouterRefusal r;
  r.status = 402;
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::Refused, (int)rf::classifyRefusal(r));   // a bare 402 is not the wallet
  r.reason = "a_reason_from_the_future";
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::Refused, (int)rf::classifyRefusal(r));
  r.reason = "insufficient_credits_for_call";
  TEST_ASSERT_TRUE(has(rf::refusalCause(rf::classifyRefusal(r), "x"), "larger than the remaining credits"));
}

// A park the router named reads as what it said on every surface that words a park.
static void test_a_named_park_reads_as_what_it_is() {
  struct Row {
    const char* reason;
    int status;
    ParkCause cause;
    const char* says;
    bool emptyWalletWords;
  };
  const Row rows[] = {
      {"insufficient_credits", 402, ParkCause::WalletEmpty, "credit balance is empty", true},
      {"credits_or_subscription_required", 402, ParkCause::NeedsPlan, "credits or a plan", true},
      {"budget_exceeded", 402, ParkCause::KeyBudget, "monthly limit", false},
      {"account_suspended", 403, ParkCause::Suspended, "suspended", false},
  };
  for (const Row& r : rows) {
    providerPark().clear();
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, r.status, refusalBody(r.reason), 1000);
    const orch::ParkVerdict v = providerPark().peek("cumulo", PaidSeam::Embeddings, 0, 2000);
    TEST_ASSERT_FALSE_MESSAGE(v.open, r.reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)r.cause, (int)v.cause, r.reason);
    for (int s = 0; s < orch::kPaidSeamCount; s++)   // an account's refusal holds on every seam
      TEST_ASSERT_FALSE_MESSAGE(providerPark().peek("cumulo", (PaidSeam)s, 0, 2000).open, r.reason);
    const std::string words = orch::parkWords("cumulo", v);
    TEST_ASSERT_TRUE_MESSAGE(has(words, r.says), words.c_str());
    TEST_ASSERT_FALSE_MESSAGE(has(words, "out of credit") || has(words, "refused the key"), words.c_str());
    const std::string shortW = orch::parkShortWords("cumulo", v);
    TEST_ASSERT_TRUE_MESSAGE(shortW.size() <= 48, shortW.c_str());
    const std::string notice = orch::parkCauseNotice("Cumulo Nimbus", v.cause, "openai");
    TEST_ASSERT_TRUE_MESSAGE(has(notice, r.says), notice.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.emptyWalletWords, has(notice, "Add credits"), notice.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(notice, "until then, openai answers."), notice.c_str());
    const orch::HeadPause hp = orch::parkPause("cumulo", PaidSeam::Chat, 2000, false);
    const std::string pause = orch::headPauseNote("Cumulo Nimbus", hp);
    TEST_ASSERT_FALSE_MESSAGE(has(pause, "out of credit") || has(pause, "rejected its key"), pause.c_str());
    const orch::HealthRow row = orch::providersRow(providerPark(), 2000);
    TEST_ASSERT_TRUE_MESSAGE(has(row.detail, r.says), row.detail.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(row.detail, "checked again"), row.detail.c_str());
  }
}

// The cause survives a restart, and a file from before it reads as Unspecified.
static void test_the_cause_survives_a_restart() {
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("budget_exceeded"), 1000);
  const std::string blob = providerPark().serialize(2000);
  providerPark().clear();
  TEST_ASSERT_TRUE(providerPark().deserialize(blob, 3000));
  TEST_ASSERT_EQUAL_INT((int)ParkCause::KeyBudget, (int)providerPark().causeOf("cumulo"));
  providerPark().clear();
  TEST_ASSERT_TRUE(providerPark().deserialize("PK1\ncumulo 1 402 255 60 0 0 1 0 00000000\n", 3000));
  TEST_ASSERT_EQUAL_INT((int)ParkCause::Unspecified, (int)providerPark().causeOf("cumulo"));
  TEST_ASSERT_FALSE(providerPark().peek("cumulo", PaidSeam::Chat, 0, 3100).open);
}

// The embedding seam: its token and its class from the reason, a router 403 included.
static void test_embedding_refusals_by_reason() {
  struct Row {
    int status;
    const char* reason;
    orch::EmbedFail kind;
  };
  const Row rows[] = {
      {402, "insufficient_credits", orch::EmbedFail::OutOfCredit},
      {402, "credits_or_subscription_required", orch::EmbedFail::OutOfCredit},
      {402, "insufficient_credits_for_call", orch::EmbedFail::CallTooLarge},
      {402, "funding_cap_reached", orch::EmbedFail::RouterHeld},
      {429, "ceiling_reached", orch::EmbedFail::RouterHeld},
      {429, "upstream_quota_exhausted", orch::EmbedFail::RouterHeld},
      {429, "credits_held_in_flight", orch::EmbedFail::RateLimited},
      {403, "account_suspended", orch::EmbedFail::AccountHeld},
      {402, "budget_exceeded", orch::EmbedFail::AccountHeld},
      {403, "endpoint_not_allowed", orch::EmbedFail::RouteNotAllowed},
      {401, "invalid_router_key", orch::EmbedFail::KeyRejected},
      {403, "model_not_priced", orch::EmbedFail::ProviderError},
      {402, "a_reason_from_the_future", orch::EmbedFail::ProviderError},
  };
  for (const Row& r : rows) {
    std::string detail;
    const std::string token = orch::embedRefusalToken(r.status, refusalBody(r.reason), r.reason, /*viaRouter=*/true);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)r.kind, (int)orch::classifyEmbedFail(token, detail), token.c_str());
  }
  std::string detail;
  // Direct, a key refusal is the key and a 402 is billing, whatever its code says.
  TEST_ASSERT_EQUAL_STRING("key rejected", orch::embedRefusalToken(403, "{}", "", false).c_str());
  TEST_ASSERT_EQUAL_INT((int)orch::EmbedFail::OutOfCredit,
                        (int)orch::classifyEmbedFail(orch::embedRefusalToken(402, "{}", "something_new", false), detail));
}

// ---- the capped resend's body edit ---------------------------------------------------

static long capOf(const std::string& body, const char* key) {
  JsonDocument d;
  const DeserializationError e = deserializeJson(d, body, DeserializationOption::NestingLimit(40));
  TEST_ASSERT_FALSE_MESSAGE((bool)e, body.c_str());
  return d[key] | -1L;
}

static void test_cap_edit_every_spelling_and_path() {
  orch::CapEdit e[orch::kCapEditMax];
  size_t n = 0;
  struct Row {
    const char* path;
    const char* body;
    const char* key;
  };
  const Row added[] = {
      {"/router/openai/v1/responses", "{\"model\":\"m\",\"input\":\"hi\"}", "max_output_tokens"},
      {"/v1/responses", "{\"model\":\"m\"}", "max_output_tokens"},
      {"/router/openai/v1/chat/completions", "{\"model\":\"m\"}", "max_completion_tokens"},
      {"/router/mistral/v1/chat/completions", "{\"model\":\"m\"}", "max_tokens"},
      {"/router/zai/chat/completions", "{\"model\":\"m\"}", "max_tokens"},
      {"/router/anthropic/v1/messages", "{\"model\":\"m\"}", "max_tokens"},
  };
  for (const Row& r : added) {
    std::string b = r.body;
    TEST_ASSERT_TRUE_MESSAGE(orch::capOutputTokens(b, r.path, 1234, e, n), r.path);
    TEST_ASSERT_EQUAL_MESSAGE(1234, capOf(b, r.key), r.path);
  }
  // Declared under several spellings: every one comes down (the router reads the largest).
  std::string b = "{\"max_tokens\":90000,\"model\":\"m\",\"max_completion_tokens\":70000,\"max_output_tokens\":800}";
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/openai/v1/chat/completions", 1000, e, n));
  TEST_ASSERT_EQUAL(1000, capOf(b, "max_tokens"));
  TEST_ASSERT_EQUAL(1000, capOf(b, "max_completion_tokens"));
  TEST_ASSERT_EQUAL(800, capOf(b, "max_output_tokens"));   // already under: it stays
  // Only top-level keys: a cap named inside a string or a tool's schema is not a cap.
  b = "{\"model\":\"m\",\"input\":\"max_tokens\\\":99\",\"tools\":[{\"max_tokens\":7}],\"max_tokens\":5000}";
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/anthropic/v1/messages", 300, e, n));
  JsonDocument d;
  TEST_ASSERT_FALSE((bool)deserializeJson(d, b));
  TEST_ASSERT_EQUAL(300, d["max_tokens"].as<int>());
  TEST_ASSERT_EQUAL(7, d["tools"][0]["max_tokens"].as<int>());
  TEST_ASSERT_EQUAL_STRING("max_tokens\":99", d["input"].as<const char*>());
  // A Mistral conversation carries its cap in completion_args.
  b = "{\"model\":\"m\",\"completion_args\":{\"response_format\":{\"type\":\"json_object\"}}}";
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/mistral/v1/conversations", 400, e, n));
  TEST_ASSERT_FALSE((bool)deserializeJson(d, b));
  TEST_ASSERT_EQUAL(400, d["completion_args"]["max_tokens"].as<int>());
  TEST_ASSERT_EQUAL_STRING("json_object", d["completion_args"]["response_format"]["type"].as<const char*>());
  b = "{\"model\":\"m\"}";
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/mistral/v1/conversations", 400, e, n));
  TEST_ASSERT_FALSE((bool)deserializeJson(d, b));
  TEST_ASSERT_EQUAL(400, d["completion_args"]["max_tokens"].as<int>());
  // A path with no output cap, or a body that is not an object: no resend.
  for (const char* p : {"/router/openai/v1/embeddings", "/router/openai/v1/audio/speech", "/router/openai/v1/images/generations"}) {
    b = "{\"model\":\"m\"}";
    TEST_ASSERT_FALSE_MESSAGE(orch::capOutputTokens(b, p, 400, e, n), p);
    TEST_ASSERT_EQUAL_STRING("{\"model\":\"m\"}", b.c_str());
  }
  b = "not json";
  TEST_ASSERT_FALSE(orch::capOutputTokens(b, "/v1/responses", 400, e, n));
}

// A photo's splice holds an offset into the body: it moves with the edit, wherever it is.
static void test_cap_edit_moves_splice_offsets() {
  orch::CapEdit e[orch::kCapEditMax];
  size_t n = 0;
  const std::string marker = "PHOTO";
  std::string b = "{\"max_tokens\":90000,\"content\":\"" + marker + "\",\"max_completion_tokens\":123456}";
  const size_t at = b.find(marker);
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/openai/v1/chat/completions", 500, e, n));
  TEST_ASSERT_EQUAL_UINT((unsigned)b.find(marker), (unsigned)orch::applyCapEdits(at, e, n));
  b = "{\"content\":\"" + marker + "\"}";
  const size_t at2 = b.find(marker);
  TEST_ASSERT_TRUE(orch::capOutputTokens(b, "/router/openai/v1/chat/completions", 500, e, n));
  TEST_ASSERT_EQUAL_UINT((unsigned)b.find(marker), (unsigned)orch::applyCapEdits(at2, e, n));
}

// ---- the wire: a head over the router ------------------------------------------------

namespace {
struct RouterHead {
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  std::vector<uint32_t> delays;
  RouterHead() {
    pd = wire.contract();
    pd.routerBase = [] { return std::string("router.test"); };
    pd.routerKey = [] { return std::string("cumulo_fake"); };
    pd.nowMs = [this] { return wire.now; };
    pd.delayMs = [this](uint32_t ms) { delays.push_back(ms); };
  }
  void script(int status, const std::string& body) { wire.http.script.push_back({"", "", status, body}); }
  bool turn(std::string& out, std::string& err) {
    std::string conv;
    return providers::orchTurnOpenAI(pd, conv, "system", "hello", out, err, nullptr, nullptr);
  }
};
std::string finalResponses(const char* reply) {
  return std::string("{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
                     "\"output_text\",\"text\":\"{\\\"reply\\\":\\\"") + reply +
         "\\\",\\\"memory\\\":\\\"\\\"}\"}]}],\"usage\":{\"input_tokens\":3,\"output_tokens\":1}}";
}
}  // namespace

// Lumi's 31 Oct call, the contract's exact body: one resend, capped at what fits, served,
// nothing parked, and every seam still open.
static void test_the_clamp_resends_once_capped_and_serves() {
  RouterHead h;
  h.script(402, contractBody("insufficient_credits_for_call"));
  h.script(200, finalResponses("done"));
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(h.turn(out, err), err.c_str());
  TEST_ASSERT_EQUAL_UINT(2, h.wire.http.seen.size());
  TEST_ASSERT_TRUE(h.wire.http.seen[0].body.find("max_output_tokens") == std::string::npos);
  TEST_ASSERT_EQUAL(74904, capOf(h.wire.http.seen[1].body, "max_output_tokens"));
  TEST_ASSERT_TRUE(h.wire.http.seen[1].path.find("/router/openai/") == 0);
  TEST_ASSERT_FALSE(anySeamClosed("cumulo", h.wire.now));
  TEST_ASSERT_TRUE(LogCapture::contains("resending once with the output capped at 74904"));
  TEST_ASSERT_TRUE(h.delays.empty());
}

// A second refusal fails the call. It never loops and it never parks.
static void test_a_second_clamp_fails_the_call_without_a_loop() {
  RouterHead h;
  h.script(402, contractBody("insufficient_credits_for_call"));
  h.script(402, contractBody("insufficient_credits_for_call"));
  std::string out, err;
  TEST_ASSERT_FALSE(h.turn(out, err));
  TEST_ASSERT_EQUAL_UINT(2, h.wire.http.seen.size());
  TEST_ASSERT_FALSE(anySeamClosed("cumulo", h.wire.now));
}

// No cap that fits (the input alone is too expensive), or under the seam's floor: no resend.
static void test_the_clamp_below_the_floor_fails_at_once() {
  for (int maxOut : {0, 255}) {
    RouterHead h;
    h.script(402, "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\","
                  "\"charged\":false,\"retryable\":false,\"maxAffordableOutputTokens\":" + std::to_string(maxOut) +
                  ",\"retryHint\":\"top_up\"}");
    std::string out, err;
    TEST_ASSERT_FALSE(h.turn(out, err));
    TEST_ASSERT_EQUAL_UINT(1, h.wire.http.seen.size());
    TEST_ASSERT_FALSE(anySeamClosed("cumulo", h.wire.now));
  }
  // At the floor exactly it does resend.
  RouterHead h;
  h.script(402, "{\"error\":\"insufficient_credits_for_call\",\"reason\":\"insufficient_credits_for_call\","
                "\"maxAffordableOutputTokens\":256}");
  h.script(200, finalResponses("short"));
  std::string out, err;
  TEST_ASSERT_TRUE(h.turn(out, err));
  TEST_ASSERT_EQUAL(256, capOf(h.wire.http.seen[1].body, "max_output_tokens"));
}

// retry_later: the same request, unchanged, after retryAfterMs when that is short. Longer
// fails the call now. Neither parks.
static void test_retry_later_waits_then_resends_unchanged() {
  RouterHead h;
  h.script(429, contractBody("credits_held_in_flight"));
  h.script(200, finalResponses("done"));
  std::string out, err;
  TEST_ASSERT_TRUE_MESSAGE(h.turn(out, err), err.c_str());
  TEST_ASSERT_EQUAL_UINT(2, h.wire.http.seen.size());
  TEST_ASSERT_EQUAL_STRING(h.wire.http.seen[0].body.c_str(), h.wire.http.seen[1].body.c_str());
  TEST_ASSERT_EQUAL_UINT(1, h.delays.size());
  TEST_ASSERT_EQUAL_UINT32(5000, h.delays[0]);
  TEST_ASSERT_FALSE(anySeamClosed("cumulo", h.wire.now));

  RouterHead longWait;
  longWait.script(429, "{\"error\":\"rate_limited\",\"reason\":\"rate_limited\",\"retryable\":true,\"retryAfterMs\":60000}");
  TEST_ASSERT_FALSE(longWait.turn(out, err));
  TEST_ASSERT_EQUAL_UINT(1, longWait.wire.http.seen.size());
  TEST_ASSERT_TRUE(longWait.delays.empty());
  TEST_ASSERT_FALSE(anySeamClosed("cumulo", longWait.wire.now));
}

// Every other refusal on the wire: no resend, unless the table says so, and the park only
// on its reasons, from the head's own narrow filter (the wire widens it).
static void test_every_reason_on_the_wire() {
  for (const ContractRow& c : kContractRows) {
    const RouterAction a = rowOf(c.reason)->action;
    if (a == RouterAction::RetryLater || a == RouterAction::RetryCapped) continue;
    providers::clearHostParks();
    RouterHead h;
    h.script(c.http, contractBody(c.reason));
    std::string out, err;
    TEST_ASSERT_FALSE_MESSAGE(h.turn(out, err), c.reason);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, h.wire.http.seen.size(), c.reason);
    TEST_ASSERT_EQUAL_MESSAGE(a == RouterAction::ParkBilling || a == RouterAction::ParkAuth,
                              anySeamClosed("cumulo", h.wire.now), c.reason);
  }
}

// ---- the head ladder -----------------------------------------------------------------

// A tap over the fake transport, as a head holds it.
static uint32_t g_tapNow = 1000;
static uint32_t tapNow() { return g_tapNow; }

// fail_over: answered on the router default this turn, the pick kept, and the upstream
// cooled: the next turn does not ask it again until retryAfterMs passes.
static void test_fail_over_cools_the_upstream() {
  FakeProviderDeps wire;
  rf::RouterTap tap(&wire.http, "router.test");
  tap.setClock(tapNow);
  g_tapNow = 1000;
  wire.http.script.push_back({"", "", 429, contractBody("upstream_quota_exhausted")});
  wire.http.script.push_back({"", "", 200, "{}"});
  auto attempt = [&](const std::string& sel, bool) {
    const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(sel);
    agent::HttpRequest req;
    req.host = "router.test";
    req.path = "/router/" + rr.upstream + "/v1/responses";
    agent::HttpResponse resp;
    std::string e;
    tap.exec(req, resp, e);
    return resp.status == 200;
  };
  rf::LadderResult r = rf::runLadder(tap, "openai/gpt-6", "", "anthropic/claude-haiku-5-5", attempt);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-5-5", r.used.c_str());
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::Upstream, (int)r.kind);
  TEST_ASSERT_EQUAL_UINT(2, wire.http.seen.size());
  // Within the upstream's wait: the pick is not asked, the turn starts on the default.
  g_tapNow += 60000;
  wire.http.script.push_back({"", "", 200, "{}"});
  r = rf::runLadder(tap, "openai/gpt-6", "", "anthropic/claude-haiku-5-5", attempt);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-haiku-5-5", r.used.c_str());
  TEST_ASSERT_EQUAL_UINT(3, wire.http.seen.size());
  TEST_ASSERT_TRUE(wire.http.seen[2].path.find("/router/anthropic/") == 0);
  // Past it: the pick is asked again.
  g_tapNow += 600000;
  wire.http.script.push_back({"", "", 200, "{}"});
  r = rf::runLadder(tap, "openai/gpt-6", "", "anthropic/claude-haiku-5-5", attempt);
  TEST_ASSERT_EQUAL_STRING("openai/gpt-6", r.used.c_str());
  TEST_ASSERT_EQUAL_INT((int)rf::RefusalKind::None, (int)r.kind);
}

// The owner hears about the clamp plainly, not on every turn of a low wallet.
static void test_the_clamp_note_is_not_on_every_turn() {
  rf::RouterRefusal first;
  first.reason = "insufficient_credits_for_call";
  uint32_t last = 0;
  bool noted = false;
  TEST_ASSERT_TRUE(rf::clampNoteDue(first, 1000, last, noted));
  TEST_ASSERT_FALSE(rf::clampNoteDue(first, 2000, last, noted));
  TEST_ASSERT_TRUE(rf::clampNoteDue(first, 1000 + rf::kClampNoteEveryMs, last, noted));
  first.reason = "insufficient_credits";
  TEST_ASSERT_FALSE(rf::clampNoteDue(first, 1000 + 2 * rf::kClampNoteEveryMs, last, noted));
  TEST_ASSERT_FALSE(has(rf::clampNote(), "dd credits"));
}

// ---- the re-check --------------------------------------------------------------------

namespace {
// Lumi: the head on Mistral answers every turn, Cumulo is last in the list and parked.
struct IdleRig {
  FakePlatform plat;
  FakeConfig cfg;
  providers::ProviderDeps pd;
  std::vector<std::pair<std::string, std::string>> asked;
  bool cumuloOk = true;
  std::string cumuloErr;
  std::function<void()> onCumulo;
  std::unique_ptr<agent::TurnEngine> eng;
  IdleRig() {
    cfg.keyed = {"mistral", "cumulo"};
    cfg.keys["cumulo"] = "cumulo_fake";
    cfg.priority = "mistral,cumulo";
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [this](const char* h) { return cfg.keys[h]; };
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.hosts.add("mistral", host("mistral"));
    d.hosts.add("cumulo", host("cumulo"));
    d.hosts.parked = [this](const std::string& h, providers::HostPark* why) { return providers::hostParked(pd, h, why); };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) { providers::noteHostFailed(pd, h, k, status); };
    d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
    eng.reset(new agent::TurnEngine(std::move(d)));
    plat.ms = 1000;
  }
  agent::ProviderTurnFn host(const std::string& h) {
    return [this, h](std::string&, const std::string& ins, const std::string&, std::string& out, std::string& err,
                     const agent::HeadTools*, orch::TokenUsage*) {
      asked.push_back({h, ins});
      if (h == "cumulo" && onCumulo) onCumulo();
      if (h == "cumulo" && !cumuloOk) {
        err = cumuloErr;
        return false;
      }
      out = "{\"reply\":\"ok\"}";
      return true;
    };
  }
  int calls(const char* h) const {
    int n = 0;
    for (const auto& a : asked) n += a.first == h ? 1 : 0;
    return n;
  }
  bool probing() {
    orch::ParkEntry e[orch::ProviderPark::kMaxProviders];
    const size_t n = providerPark().snapshot(e, orch::ProviderPark::kMaxProviders, plat.ms);
    for (size_t i = 0; i < n; i++)
      if (std::strcmp(e[i].provider, "cumulo") == 0) return e[i].probing;
    return false;
  }
  void parkCumulo() {
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), plat.ms);
    plat.ms += orch::ProviderPark::kRecheckMs + 1;
  }
};
}  // namespace

// CUM-889 on a fake clock: reading, folding and asking about candidates spends nothing and
// logs no probe; the fold runs on the first host and reaches no parked one.
static void test_reads_never_spend_the_recheck() {
  IdleRig r;
  r.parkCumulo();
  for (int i = 0; i < 20; i++) {
    TEST_ASSERT_TRUE(r.eng->canFoldNow());
    r.eng->headOnFallback();
    providers::hostParked(r.pd, "cumulo");
    orch::providersRow(providerPark(), r.plat.ms);
    orch::parkPause("cumulo", PaidSeam::Spawn, r.plat.ms, true);
    r.plat.ms += 60000;
  }
  TEST_ASSERT_FALSE(r.probing());
  TEST_ASSERT_EQUAL_INT(0, linesWith("re-check due"));
  std::string summary;
  TEST_ASSERT_EQUAL_INT((int)agent::TurnEngine::FoldResult::Ok, (int)r.eng->runFold("1001", "", "digest", summary));
  TEST_ASSERT_EQUAL_INT(0, r.calls("cumulo"));
  TEST_ASSERT_FALSE(r.probing());
}

// The ask itself: exactly one real request, one outcome line, whatever the answer.
static void test_the_idle_recheck_makes_one_request_with_one_outcome() {
  struct Row {
    const char* name;
    const char* err;       // "" = answered
    int reStatus;          // the wire's own park on this answer, 0 = none
    const char* outcome;
    bool openAfter;
  };
  const Row rows[] = {
      {"answered", "", 0, "re-check answered - open again", true},
      {"refused again", "resp HTTP 402", 402, "re-check refused again (billing, HTTP 402)", false},
      {"a server error", "resp HTTP 503: down", 0, "re-check failed without a verdict", false},
      {"no answer", "network", 0, "re-check failed without a verdict", false},
  };
  for (const Row& row : rows) {
    providers::clearHostParks();
    LogCapture::install();
    IdleRig r;
    r.parkCumulo();
    r.cumuloOk = !*row.err;
    r.cumuloErr = row.err;
    if (row.reStatus)
      r.onCumulo = [&r, &row] {
        providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, row.reStatus, refusalBody("insufficient_credits"), r.plat.ms);
      };
    TEST_ASSERT_TRUE_MESSAGE(r.eng->recheckParkedHost(), row.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.calls("cumulo"), row.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.calls("mistral"), row.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(orch::ORCH_PARK_RECHECK_PROMPT, r.asked[0].second.c_str(), row.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, linesWith("park: cumulo re-check "), row.name);
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains(row.outcome), row.name);
    TEST_ASSERT_EQUAL_MESSAGE(row.openAfter, !anySeamClosed("cumulo", r.plat.ms), row.name);
    // Nothing is due now: the very next tick sends nothing.
    TEST_ASSERT_FALSE_MESSAGE(r.eng->recheckParkedHost(), row.name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.calls("cumulo"), row.name);
    // A fresh wait, whole, when it did not recover.
    if (!row.openAfter) {
      r.plat.ms += orch::ProviderPark::kRecheckMs - 2;
      TEST_ASSERT_FALSE_MESSAGE(r.eng->recheckParkedHost(), row.name);
      r.plat.ms += 2;
      TEST_ASSERT_TRUE_MESSAGE(r.eng->recheckParkedHost(), row.name);
    }
  }
}

// Nothing due, nothing sent; a fresh device with no park file included. A paused provider
// and a park on one other seam are never asked.
static void test_the_idle_recheck_stays_quiet_when_it_should() {
  IdleRig r;
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());   // fresh: no park at all
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), r.plat.ms);
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());   // parked, not due
  r.plat.ms += orch::ProviderPark::kRecheckMs + 1;
  providerPark().pause("cumulo", orch::kPauseOwner);
  TEST_ASSERT_FALSE(r.eng->recheckParkedHost());   // the owner's pause: never probed
  providerPark().resume("cumulo", orch::kPauseOwner);
  TEST_ASSERT_TRUE(r.eng->recheckParkedHost());
  TEST_ASSERT_EQUAL_INT(1, r.calls("cumulo"));

  providers::clearHostParks();
  IdleRig seam;
  providerPark().noteAnswer("cumulo", PaidSeam::Vision, 0, 403, "{\"error\":{\"message\":\"nope\"}}", seam.plat.ms);
  seam.plat.ms += orch::ProviderPark::kRecheckMs + 1;
  TEST_ASSERT_FALSE(seam.eng->recheckParkedHost());   // that seam's own to re-check
  TEST_ASSERT_EQUAL_INT(0, seam.calls("cumulo"));
}

// The token itself: a claim is exactly one, a hand back keeps it due, a reader never takes
// it, and the wire claims it as the request leaves, with the outcome logged after.
static void test_the_probe_token() {
  auto& park = providerPark();
  park.noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), 1000);
  const uint32_t due = 1000 + orch::ProviderPark::kRecheckMs + 1;
  orch::ParkVerdict v = park.peek("cumulo", PaidSeam::Chat, 0, due);
  TEST_ASSERT_TRUE(v.open);
  TEST_ASSERT_TRUE(v.due);
  TEST_ASSERT_FALSE(v.probe);
  v = park.available("cumulo", PaidSeam::Embeddings, 0, due);
  TEST_ASSERT_TRUE(v.probe);
  TEST_ASSERT_FALSE(park.claimProbe("cumulo", PaidSeam::Chat, due));   // already out
  park.releaseProbe("cumulo");
  TEST_ASSERT_EQUAL_INT(1, linesWith("handed back"));
  TEST_ASSERT_TRUE(park.claimProbe("cumulo", PaidSeam::Chat, due + 1));   // still due: not a new wait
  // No answer within the lease: a failed probe, with a line, and a whole new wait.
  v = park.peek("cumulo", PaidSeam::Chat, 0, due + 1 + orch::ProviderPark::kProbeLeaseMs);
  TEST_ASSERT_FALSE(v.open);
  TEST_ASSERT_EQUAL_INT(1, linesWith("got no answer in time"));
  // Nothing claimed when nothing is due, or another seam's park is.
  TEST_ASSERT_FALSE(park.claimProbe("cumulo", PaidSeam::Chat, due + 2 + orch::ProviderPark::kProbeLeaseMs));
}

static void test_the_wire_claims_the_probe_when_the_request_leaves() {
  RouterHead h;
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), h.wire.now);
  h.wire.now += orch::ProviderPark::kRecheckMs + 1;
  h.script(402, contractBody("insufficient_credits"));
  std::string out, err;
  TEST_ASSERT_FALSE(h.turn(out, err));
  TEST_ASSERT_EQUAL_UINT(1, h.wire.http.seen.size());
  TEST_ASSERT_EQUAL_INT(1, linesWith("cumulo re-check due - one probe request"));
  TEST_ASSERT_EQUAL_INT(1, linesWith("cumulo re-check refused again (billing, HTTP 402)"));
  TEST_ASSERT_TRUE(anySeamClosed("cumulo", h.wire.now));
}

// A free poll answered 429 while a re-check is due says nothing about the account: the
// re-check stays due, so a job polling a parked provider cannot starve its recovery.
static void test_a_poll_does_not_push_the_recheck_back() {
  RouterHead h;
  providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), h.wire.now);
  h.wire.now += orch::ProviderPark::kRecheckMs + 1;
  h.script(429, "{\"error\":\"rate_limited\",\"reason\":\"rate_limited\"}");
  JsonDocument doc, filter;
  providers::wire::exchange(h.pd, "router.test", 443, true, "GET", "/router/openai/v1/responses/r1", {}, "", 1000, doc, filter);
  TEST_ASSERT_EQUAL_UINT(1, h.wire.http.seen.size());
  TEST_ASSERT_TRUE(providerPark().claimProbe("cumulo", PaidSeam::Chat, h.wire.now));   // still due
}

// A refusal that shows the account stands ends a park at once: after a wallet refill the
// clamp is as good a verdict as a served call, and so is an upstream's failure.
static void test_an_answer_past_the_wallet_ends_the_park() {
  for (const char* reason : {"insufficient_credits_for_call", "credits_held_in_flight", "upstream_quota_exhausted"}) {
    providers::clearHostParks();
    RouterHead h;
    h.wire.now = 1000;
    providerPark().noteAnswer("cumulo", PaidSeam::Chat, 0, 402, refusalBody("insufficient_credits"), h.wire.now);
    h.wire.now += orch::ProviderPark::kRecheckMs + 1;
    const orch::RouterReasonRow* row = rowOf(reason);
    h.script(row->http, contractBody(reason));
    h.script(row->http, contractBody(reason));
    std::string out, err;
    h.turn(out, err);
    TEST_ASSERT_FALSE_MESSAGE(anySeamClosed("cumulo", h.wire.now), reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, linesWith("re-check answered - open again"), reason);
    LogCapture::install();
  }
  // The same answers from a provider's own host mean nothing to its park.
  providers::clearHostParks();
  providerPark().noteAnswer("openai", PaidSeam::Chat, 0, 402, "{}", 1000);
  providerPark().noteAnswer("openai", PaidSeam::Chat, 0, 429, "{\"error\":{\"message\":\"Rate limit\"}}", 1001);
  TEST_ASSERT_TRUE(anySeamClosed("openai", 1002));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_table_is_the_contract);
  RUN_TEST(test_only_the_park_reasons_close_cumulo_on_every_seam);
  RUN_TEST(test_unknown_reason_and_bare_402);
  RUN_TEST(test_a_reason_quoted_in_a_message_is_not_a_reason);
  RUN_TEST(test_the_answer_is_read_whole);
  RUN_TEST(test_add_credits_only_when_it_is_true);
  RUN_TEST(test_a_named_park_reads_as_what_it_is);
  RUN_TEST(test_the_cause_survives_a_restart);
  RUN_TEST(test_embedding_refusals_by_reason);
  RUN_TEST(test_cap_edit_every_spelling_and_path);
  RUN_TEST(test_cap_edit_moves_splice_offsets);
  RUN_TEST(test_the_clamp_resends_once_capped_and_serves);
  RUN_TEST(test_a_second_clamp_fails_the_call_without_a_loop);
  RUN_TEST(test_the_clamp_below_the_floor_fails_at_once);
  RUN_TEST(test_retry_later_waits_then_resends_unchanged);
  RUN_TEST(test_every_reason_on_the_wire);
  RUN_TEST(test_fail_over_cools_the_upstream);
  RUN_TEST(test_the_clamp_note_is_not_on_every_turn);
  RUN_TEST(test_reads_never_spend_the_recheck);
  RUN_TEST(test_the_idle_recheck_makes_one_request_with_one_outcome);
  RUN_TEST(test_the_idle_recheck_stays_quiet_when_it_should);
  RUN_TEST(test_the_probe_token);
  RUN_TEST(test_the_wire_claims_the_probe_when_the_request_leaves);
  RUN_TEST(test_a_poll_does_not_push_the_recheck_back);
  RUN_TEST(test_an_answer_past_the_wallet_ends_the_park);
  return UNITY_END();
}
