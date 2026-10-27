#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/served_model.h"
#include "nimbus/orch/token_usage_json.h"
#include "nimbus/orch/turn_spend.h"
#include "nimbus/orch/usage_ledger.h"

// test_orch_turn_spend - the class rule for a turn's spend: every billed response is
// filed exactly once, under the provider and model that billed it, whatever order the
// hosts ran in, however many responses each billed, and nothing current at record time
// (a later model change) relabels it. The usage RCA (18 Oct): switched turns booked
// every earlier host's rounds and cache writes under the final host.
//
// Each generated response carries its provider's NATIVE usage shape and goes through
// the real tokenUsageFromJson, the way the adapters fill a host's counter.

using nimbus::orch::SpendSlot;
using nimbus::orch::TokenDelta;
using nimbus::orch::TokenUsage;
using nimbus::orch::TurnSpend;
using nimbus::orch::ledgerDelta;
using nimbus::orch::tokenUsageFromJson;

void setUp() {}
void tearDown() {}

static uint32_t g_seed = 0x7ea5u;
static uint32_t rnd() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

static const char* kHosts[3] = {"anthropic", "openai", "mistral"};
static const char* kModels[3][2] = {{"claude-haiku-5-5", "claude-sonnet-5-5"},
                                    {"gpt-5.6-sol", "gpt-5.6-luna"},
                                    {"mistral-small-latest", "mistral-large-latest"}};

struct Resp {
  int host;
  uint32_t in, out, cr, cw;
  std::string echo;   // the served model the response echoed ("" = none)
};

// The response's usage object in its own provider's shape, parsed as the adapters do.
static TokenUsage parsed(const Resp& r) {
  char js[400];
  if (r.host == 0)
    std::snprintf(js, sizeof js,
                  "{\"usage\":{\"input_tokens\":%u,\"output_tokens\":%u,"
                  "\"cache_read_input_tokens\":%u,\"cache_creation_input_tokens\":%u}}",
                  r.in, r.out, r.cr, r.cw);
  else if (r.host == 1)
    std::snprintf(js, sizeof js,
                  "{\"usage\":{\"input_tokens\":%u,\"output_tokens\":%u,"
                  "\"input_tokens_details\":{\"cached_tokens\":%u,\"cache_write_tokens\":%u}}}",
                  r.in, r.out, r.cr, r.cw);
  else
    std::snprintf(js, sizeof js,
                  "{\"usage\":{\"prompt_tokens\":%u,\"completion_tokens\":%u,"
                  "\"prompt_tokens_details\":{\"cached_tokens\":%u}}}",
                  r.in, r.out, r.cr);
  JsonDocument d;
  deserializeJson(d, js);
  TokenUsage u = tokenUsageFromJson(d["usage"].as<JsonObjectConst>());
  u.servedModel = r.echo;
  return u;
}

static Resp genResp(int host) {
  Resp r;
  r.host = host;
  r.in = 1 + rnd() % 40000;
  r.out = rnd() % 4000;
  // OpenAI's cache counts are PART of its input; Anthropic's are siblings.
  r.cr = host == 1 ? rnd() % (r.in + 1) : rnd() % 30000;
  r.cw = host == 0 ? rnd() % 20000 : host == 1 ? rnd() % (r.in - r.cr + 1) : 0;
  r.echo = (rnd() % 4 == 0) ? kModels[host][1] : "";   // a real substitution, sometimes
  return r;
}

struct Expect {
  uint64_t in = 0, out = 0, cr = 0, cw = 0, n = 0;
};

// One generated turn, filed the way the fabric does it: each host's responses add
// into that host's own counter, and each host's counter is filed once when the turn
// ends (success or failure). `configured` is the host's model at bill time.
static void genTurn(TurnSpend& sp, std::map<std::string, Expect>& want, std::vector<int>& order) {
  sp.clear();
  want.clear();
  order.clear();
  const int hosts = 1 + (int)(rnd() % 3);
  int perm[3] = {0, 1, 2};
  for (int i = 2; i > 0; --i) std::swap(perm[i], perm[rnd() % (i + 1)]);
  for (int k = 0; k < hosts; ++k) {
    const int h = perm[k];
    order.push_back(h);
    TokenUsage hostUse;
    const int responses = (int)(rnd() % 7);   // 0 = the host billed nothing (a 400)
    for (int i = 0; i < responses; ++i) {
      const Resp r = genResp(h);
      hostUse += parsed(r);
      Expect& e = want[kHosts[h]];
      e.in += r.in; e.out += r.out; e.cr += r.cr; e.cw += r.cw; e.n += 1;
    }
    sp.add(kHosts[h], kModels[h][0], hostUse);
  }
}

static void test_property_every_response_filed_once_under_its_host() {
  g_seed = 0x7ea5u;
  for (int c = 0; c < 2000; ++c) {
    TurnSpend sp;
    std::map<std::string, Expect> want;
    std::vector<int> order;
    genTurn(sp, want, order);
    // (a) each host's slots add up to exactly what it billed; (c) no slot for a host
    // that billed nothing.
    for (int h = 0; h < 3; ++h) {
      const SpendSlot got = sp.hostTotal(kHosts[h]);
      const auto it = want.find(kHosts[h]);
      const Expect e = it == want.end() ? Expect() : it->second;
      TEST_ASSERT_EQUAL_UINT64(e.in, got.in);
      TEST_ASSERT_EQUAL_UINT64(e.out, got.out);
      // Mistral's chat usage has no cache-write field; its reads ride like the rest.
      TEST_ASSERT_EQUAL_UINT64(e.cr, got.cacheR);
      TEST_ASSERT_EQUAL_UINT64(e.cw, got.cacheW);
      TEST_ASSERT_EQUAL_UINT64(e.n, got.billed);
    }
    for (size_t i = 0; i < sp.n; ++i) {
      TEST_ASSERT_FALSE(sp.slot[i].empty());
      // (d) a cache write appears only on a host whose wire reports one.
      if (std::strcmp(sp.slot[i].host, "mistral") == 0) TEST_ASSERT_EQUAL_UINT32(0, sp.slot[i].cacheW);
      // (e) the ledger delta never wraps and keeps every token of the slot.
      const TokenDelta d = ledgerDelta(sp.slot[i]);
      if (std::strcmp(sp.slot[i].host, "openai") == 0) {
        TEST_ASSERT_TRUE(d.in <= sp.slot[i].in);
        TEST_ASSERT_EQUAL_UINT64(sp.slot[i].in, d.in + d.cacheR + d.cacheW);
      } else if (std::strcmp(sp.slot[i].host, "anthropic") == 0) {
        TEST_ASSERT_EQUAL_UINT64(sp.slot[i].in, d.in);
        TEST_ASSERT_EQUAL_UINT64(sp.slot[i].cacheW, d.cacheW);
      } else {
        TEST_ASSERT_EQUAL_UINT64(sp.slot[i].in, d.in);   // not split: billed as input
        TEST_ASSERT_EQUAL_UINT64(0, d.cacheR + d.cacheW);
      }
    }
    // (b) the slots add up to the display total.
    const TokenUsage t = sp.total();
    uint64_t in = 0, out = 0;
    for (const auto& kv : want) { in += kv.second.in; out += kv.second.out; }
    TEST_ASSERT_EQUAL_UINT64(in, t.promptTokens);
    TEST_ASSERT_EQUAL_UINT64(out, t.completionTokens);
  }
}

// (f) More (host, model) pairs than slots: every token kept, never merged across hosts.
static void test_overflow_keeps_every_token_never_across_hosts() {
  TurnSpend sp;
  uint64_t perHost[3] = {0, 0, 0};
  for (int round = 0; round < 3; ++round)
    for (int h = 0; h < 3; ++h)
      for (int m = 0; m < 2; ++m) {
        TokenUsage u;
        u.promptTokens = 100u * (uint32_t)(h + 1) + (uint32_t)m;
        u.responses = 1;
        u.servedModel = kModels[h][m];
        sp.add(kHosts[h], kModels[h][0], u);
        perHost[h] += u.promptTokens;
      }
  TEST_ASSERT_EQUAL(TurnSpend::kSlots, (int)sp.n);
  for (int h = 0; h < 3; ++h) TEST_ASSERT_EQUAL_UINT64(perHost[h], sp.hostTotal(kHosts[h]).in);
}

// (g) The model key is fixed when the spend is filed: a model changed later in the
// turn (a device action applied before turn end) cannot relabel it.
static void test_model_key_fixed_at_bill_time() {
  TurnSpend sp;
  TokenUsage u;
  u.promptTokens = 10;
  u.responses = 1;
  std::string configured = "claude-sonnet-5";
  sp.add("anthropic", configured.c_str(), u);
  configured = "claude-opus-5";   // the turn's own device action changed the model
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", sp.slot[0].model);
  TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", sp.total().servedModel.c_str());
  // A served echo that is a REAL substitution files under the model that answered.
  TokenUsage v = u;
  v.servedModel = "claude-haiku-5-5";
  sp.add("anthropic", "claude-sonnet-5", v);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", sp.slot[1].model);
}

// The live switched turn (sdlog 9714-9746): Anthropic billed rounds with cache writes,
// then OpenAI finished. Each host's cache counts stay on its own slot, and the sums the
// old single counter made (in=92,834 vs cacheR=150,100) can never reach ledgerDelta.
static void test_live_switched_shape_never_underflows() {
  TurnSpend sp;
  TokenUsage ant;
  for (int i = 0; i < 2; ++i) {
    Resp r{0, 3472, 3000, 0, 21757, ""};
    ant += parsed(r);
  }
  sp.add("anthropic", "claude-sonnet-5", ant);
  Resp o{1, 30000, 1200, 20000, 0, ""};
  sp.add("openai", "gpt-5.6-sol", parsed(o));
  TEST_ASSERT_EQUAL_UINT32(43514, sp.hostTotal("anthropic").cacheW);
  TEST_ASSERT_EQUAL_UINT32(0, sp.hostTotal("openai").cacheW);
  TEST_ASSERT_EQUAL_UINT32(20000, sp.hostTotal("openai").cacheR);
  TEST_ASSERT_EQUAL_UINT32(0, sp.hostTotal("anthropic").cacheR);
  const TokenDelta od = ledgerDelta(sp.slot[1]);
  TEST_ASSERT_EQUAL_UINT64(10000, od.in);
  TEST_ASSERT_EQUAL_UINT64(20000, od.cacheR);
  // A malformed OpenAI usage (cache above input) is clamped, never wrapped.
  SpendSlot bad;
  std::snprintf(bad.host, sizeof bad.host, "openai");
  bad.in = 92834;
  bad.cacheR = 150100;
  bad.cacheW = 21757;
  const TokenDelta bd = ledgerDelta(bad);
  TEST_ASSERT_EQUAL_UINT64(0, bd.in);
  TEST_ASSERT_EQUAL_UINT64(92834, bd.cacheR);
  TEST_ASSERT_EQUAL_UINT64(0, bd.cacheW);
}

// A host that answered with an error billed nothing: no slot, and the failed turn's
// total is exactly what the other hosts billed (the 18 Oct dream: three Anthropic
// rounds billed, every later request a 400).
static void test_failed_turn_files_only_what_billed() {
  TurnSpend sp;
  TokenUsage ant;
  for (int i = 0; i < 3; ++i) {
    Resp r{0, 5000, 120, 9000, 0, ""};
    ant += parsed(r);
  }
  sp.add("anthropic", "claude-opus-5", ant);
  sp.add("openai", "gpt-5.6-sol", TokenUsage());
  sp.add("mistral", "mistral-large-latest", TokenUsage());
  TEST_ASSERT_EQUAL(1, (int)sp.n);
  TEST_ASSERT_EQUAL_STRING("claude-opus-5", sp.lastBilled()->model);
  TEST_ASSERT_EQUAL_UINT16(3, sp.slot[0].billed);
  char line[200];
  nimbus::orch::spendLine(sp, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5 in=15000 out=360 cR=27000 cW=0 n=3", line);
  TurnSpend none;
  nimbus::orch::spendLine(none, line, sizeof line);
  TEST_ASSERT_EQUAL_STRING("(none)", line);
  TEST_ASSERT_NULL(none.lastBilled());
}

// The recorded GPT-5.6 Responses usage shape (18 Oct 2026): cache_write_tokens is read.
static void test_openai_cache_write_tokens_parsed() {
  JsonDocument d;
  deserializeJson(d,
                  "{\"usage\":{\"input_tokens\":13,\"input_tokens_details\":{\"cache_write_tokens\":4,"
                  "\"cached_tokens\":6},\"output_tokens\":5,\"total_tokens\":18}}");
  const TokenUsage u = tokenUsageFromJson(d["usage"].as<JsonObjectConst>());
  TEST_ASSERT_EQUAL_UINT32(13, u.promptTokens);
  TEST_ASSERT_EQUAL_UINT32(6, u.cacheReadTokens);
  TEST_ASSERT_EQUAL_UINT32(4, u.cacheWriteTokens);
  TEST_ASSERT_EQUAL_UINT16(1, u.responses);
  TEST_ASSERT_EQUAL_UINT32(13, u.maxRequestIn);   // OpenAI's input already holds its cache
  // Anthropic's whole prompt adds its cache siblings.
  JsonDocument a;
  deserializeJson(a, "{\"usage\":{\"input_tokens\":816,\"cache_read_input_tokens\":9743,"
                     "\"cache_creation_input_tokens\":10,\"output_tokens\":250}}");
  TEST_ASSERT_EQUAL_UINT32(10569, tokenUsageFromJson(a["usage"].as<JsonObjectConst>()).maxRequestIn);
}

// ---- the concrete model each host answered with (HFM, 26 Oct 2026) -----------------

static TokenUsage answered(const char* model, uint32_t in = 100, uint32_t out = 10) {
  TokenUsage u;
  u.promptTokens = in;
  u.completionTokens = out;
  u.responses = 1;
  u.servedModel = model ? model : "";
  return u;
}

// What a device with a fetched Mistral list resolves (the model store's own lookup is
// tested in test_model_catalog): a "-latest" echo becomes the model behind it.
static void lumiResolver(const char* host, const char* echo, char* out, size_t cap) {
  if (!std::strcmp(host, "mistral") && !std::strcmp(echo, "mistral-large-latest"))
    std::snprintf(out, cap, "%s", "mistral-large-2512");
}

// Lumi's turns on 26 Oct, recorded the way they now are: every slot names the model the
// provider answered with, resolved when it echoed an alias; the spend line carries it
// as " id=" after the counts (the log parsers read up to n=); a host that named no
// model files no id; the next turn's prompt can ask what each pick really runs.
static void test_each_slot_names_the_model_that_answered() {
  nimbus::orch::clearServedModels();
  nimbus::orch::installServedModelResolver(&lumiResolver);
  TurnSpend s;
  s.add("mistral", "mistral-large-latest", answered("mistral-large-latest", 34494, 63));
  s.add("mistral", "mistral-large-latest", answered("mistral-large-latest", 100, 1));   // a second round
  s.add("mistral", "mistral-small-latest", answered("mistral-small-latest", 1293, 320));   // a helper, unresolved
  s.add("anthropic", "claude-haiku-5-5", answered(""));                                   // named none
  TEST_ASSERT_EQUAL(3, (int)s.n);
  TEST_ASSERT_EQUAL_STRING("mistral-large-2512", s.slot[0].served);
  TEST_ASSERT_EQUAL_UINT16(2, s.slot[0].billed);
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", s.slot[1].served);   // the echo, as returned
  TEST_ASSERT_EQUAL_STRING("", s.slot[2].served);
  char line[400];
  nimbus::orch::spendLine(s, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING(
      "mistral/mistral-large in=34594 out=64 cR=0 cW=0 n=2 id=mistral-large-2512 | "
      "mistral/mistral-small in=1293 out=320 cR=0 cW=0 n=1 id=mistral-small-latest | "
      "anthropic/claude-haiku-5-5 in=100 out=10 cR=0 cW=0 n=1",
      line);
  TEST_ASSERT_EQUAL_STRING("mistral-large-2512",
                           nimbus::orch::servedModelFor("mistral", "mistral-large-latest").c_str());
  TEST_ASSERT_EQUAL_STRING("", nimbus::orch::servedModelFor("mistral", "mistral-medium-latest").c_str());
  TEST_ASSERT_EQUAL_STRING("", nimbus::orch::servedModelFor("anthropic", "claude-haiku-5-5").c_str());
  const nimbus::orch::ServedModelNote last = nimbus::orch::lastServedModel();
  TEST_ASSERT_EQUAL_STRING("mistral", last.host.c_str());
  TEST_ASSERT_EQUAL_STRING("mistral-small-latest", last.asked.c_str());
  nimbus::orch::installServedModelResolver(nullptr);
  nimbus::orch::clearServedModels();
}

// Unresolved (no resolver, or it has no answer): the echo stands; a later answer of the
// same (host, asked) replaces the note; the notes keep the newest kServedNotes pairs; an
// echo longer than a model id is cut, never overflowing.
static void test_served_notes_are_bounded_and_newest_wins() {
  nimbus::orch::clearServedModels();
  TurnSpend s;
  s.add("openai", "gpt-5.6", answered("gpt-5.6-sol"));
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-sol", s.slot[0].served);
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-sol", nimbus::orch::servedModelFor("openai", "gpt-5.6").c_str());
  nimbus::orch::noteServedModel("openai", "gpt-5.6", "gpt-5.6-sol-2");
  TEST_ASSERT_EQUAL_STRING("gpt-5.6-sol-2", nimbus::orch::servedModelFor("openai", "gpt-5.6").c_str());
  for (size_t i = 0; i < nimbus::orch::kServedNotes; ++i) {
    const std::string asked = "m" + std::to_string(i);
    nimbus::orch::noteServedModel("zai", asked.c_str(), ("served-" + asked).c_str());
  }
  TEST_ASSERT_EQUAL_STRING("", nimbus::orch::servedModelFor("openai", "gpt-5.6").c_str());   // the oldest went
  TEST_ASSERT_EQUAL_STRING("served-m0", nimbus::orch::servedModelFor("zai", "m0").c_str());
  // Provider text: a clean id longer than the slot is cut, never overflowing; an echo
  // that is no clean model id (too long, a newline that would forge a log or page line)
  // is not recorded at all.
  const std::string longEcho(60, 'x');
  TurnSpend t;
  t.add("zai", "glm-5.3", answered(longEcho.c_str()));
  TEST_ASSERT_EQUAL_UINT(nimbus::orch::kModelKeyCap - 1, std::strlen(t.slot[0].served));
  for (const char* hostile : {"glm-5.3\nresult: ok", "a/b in=1 out=1 cR=1 cW=1 n=1", "<b>x</b>"}) {
    TurnSpend h;
    h.add("zai", "glm-5.3", answered(hostile));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", h.slot[0].served, hostile);
  }
  TurnSpend huge;
  huge.add("zai", "glm-5.3", answered(std::string(200, 'x').c_str()));
  TEST_ASSERT_EQUAL_STRING("", huge.slot[0].served);
  nimbus::orch::noteServedModel("", "a", "b");   // nothing to note
  nimbus::orch::noteServedModel("zai", "", "b");
  nimbus::orch::clearServedModels();
  TEST_ASSERT_EQUAL_STRING("", nimbus::orch::lastServedModel().host.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_property_every_response_filed_once_under_its_host);
  RUN_TEST(test_overflow_keeps_every_token_never_across_hosts);
  RUN_TEST(test_model_key_fixed_at_bill_time);
  RUN_TEST(test_live_switched_shape_never_underflows);
  RUN_TEST(test_failed_turn_files_only_what_billed);
  RUN_TEST(test_openai_cache_write_tokens_parsed);
  RUN_TEST(test_each_slot_names_the_model_that_answered);
  RUN_TEST(test_served_notes_are_bounded_and_newest_wins);
  return UNITY_END();
}
