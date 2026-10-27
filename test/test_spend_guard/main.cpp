#include <unity.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "nimbus/harness/fabric.h"   // kDeviceBackends - every provider a helper can bill
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/spend_guard.h"
#include "nimbus/orch/usage_ledger.h"

// Lane QW4: no paid call escapes the monthly spend ceiling. The portable rules
// (spend_guard.h, the custom endpoint's estimate in model_price_defaults.h) and the
// device seams that must call them (read from the source, the test_spend_seams way):
//   - a custom endpoint on a paid host prices every token above zero, so its spend
//     reaches the ceiling (the class: every backend a turn or helper can bill);
//   - a message the pause leaves unanswered gets the short reply, and the notice pump
//     and that reply are wired where the device handles a message;
//   - counts made without a clock are enforced at once and reconciled when it is set;
//   - a count read back is floored at the ledger's estimate;
//   - the verify probe is gated and counted.

using namespace nimbus::orch;

void setUp() { setCustomEndpointPaid(true); }
void tearDown() { setCustomEndpointPaid(true); }

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The body of the function whose definition starts at `signature` ("" when absent). A
// signature may end with its "{" to skip a forward declaration of the same name.
std::string bodyOf(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = src.find('{', at + signature.size() - (signature.back() == '{' ? 1 : 0));
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    if (src[i] == '}' && --depth == 0) return src.substr(open, i - open + 1);
  }
  return std::string();
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

TokenDelta tokens(uint64_t in, uint64_t out) {
  TokenDelta d;
  d.in = in;
  d.out = out;
  return d;
}

}  // namespace

// ---- (1) the custom endpoint ---------------------------------------------------------

static void test_custom_endpoint_local_is_only_the_devices_own_network() {
  const char* local[] = {"http://192.168.1.20:11434/v1", "http://10.0.0.5:1234", "http://172.16.4.2",
                         "http://127.0.0.1:8080", "localhost:11434", "http://localhost",
                         "http://mac-mini.local:11434/v1", "192.168.50.10:1234",
                         "http://169.254.10.10", "http://0x7f000001", "https://LOCALHOST:8443/v1"};
  for (const char* b : local) TEST_ASSERT_TRUE_MESSAGE(customEndpointIsLocal(b), b);
  // Paid (or unknown): a public host in any spelling, an empty or unparsable base, and
  // every base the classifier and the adapter's parser could read differently (userinfo,
  // a query or fragment marker, an IPv6 literal, a second ':' or a non-numeric port): the
  // adapter dials "evil.com" for "http://evil.com:80@192.168.1.5".
  const char* paid[] = {"https://openrouter.ai/api/v1", "openrouter.ai", "api.together.xyz:443",
                        "https://api.openai.com", "http://8.8.8.8:8080", "https://example.com/v1",
                        "", "https://", "http://:8080", "ftp://192.168.1.2", "https://user@/x",
                        "http://evil.com:80@192.168.1.5", "http://192.168.1.5#.evil.com",
                        "https://localhost?.evil.com", "https://[evil.com/v1", "http://[::1]:8080",
                        "http://[fd00::12]:8000", "2001:db8::10.0.0.1", "http://192.168.1.5:80x",
                        "http://192.168.1.5 .evil.com", "foo://192.168.1.5"};
  for (const char* b : paid) TEST_ASSERT_FALSE_MESSAGE(customEndpointIsLocal(b), b);
}

// Free only when local AND keyless: a key on a LAN endpoint is most likely a metered
// proxy in front of a paid provider.
static void test_a_keyed_endpoint_is_paid_even_on_the_lan() {
  TEST_ASSERT_TRUE(customEndpointIsFree("http://192.168.1.20:11434/v1", false));
  TEST_ASSERT_FALSE(customEndpointIsFree("http://192.168.1.20:11434/v1", true));
  TEST_ASSERT_FALSE(customEndpointIsFree("https://openrouter.ai/api/v1", false));
  TEST_ASSERT_FALSE(customEndpointIsFree("https://openrouter.ai/api/v1", true));
}

static void test_paid_custom_endpoint_prices_at_the_highest_row_never_zero() {
  uint32_t hiIn = 0, hiOut = 0;
  for (const BuiltinPrice& p : kBuiltinPrices) {
    if (p.microIn > hiIn) hiIn = p.microIn;
    if (p.microOut > hiOut) hiOut = p.microOut;
  }
  UsageLedger L;
  const TokenDelta m = tokens(1000000, 1000000);
  // A model with a name, and spend whose model was not recorded, both fail closed.
  TEST_ASSERT_EQUAL_UINT64((uint64_t)hiIn + hiOut, L.priceDeltaMicro("custom", "llama-405b", m));
  TEST_ASSERT_EQUAL_UINT64((uint64_t)hiIn + hiOut, L.priceDeltaMicro("custom", "", m));
  const EffRate r = L.effectiveRate("custom", "openrouter/some-model");
  TEST_ASSERT_TRUE(r.approx());   // the Usage page shows it as an estimate
  TEST_ASSERT_EQUAL_STRING("est", rateSrcName(r.srcIn));
  // The owner's price wins over the estimate.
  L.models().setOwnerRate("custom", "llama-405b", 900000, 900000);   // $0.90 / $0.90
  TEST_ASSERT_EQUAL_UINT64(1800000ULL, L.priceDeltaMicro("custom", "llama-405b", m));
  // Spend with no model recorded reads the highest price the owner set for the endpoint.
  setCustomEndpointPaid(false);
  TEST_ASSERT_EQUAL_UINT64(1800000ULL, L.priceDeltaMicro("custom", "", m));
  // A local model costs nothing until priced.
  UsageLedger lan;
  TEST_ASSERT_EQUAL_UINT64(0, lan.priceDeltaMicro("custom", "qwen3:8b", m));
  TEST_ASSERT_EQUAL_UINT64(0, lan.priceDeltaMicro("custom", "", m));
}

// The class: every backend a turn or a helper can bill (the device's sub-agent backends,
// which are also every head host) prices an unpriced model above zero, so none of them
// can spend without reaching the ceiling.
static void test_every_billing_backend_prices_an_unknown_model_above_zero() {
  UsageLedger L;
  const TokenDelta m = tokens(100000, 10000);
  for (int i = 0; i < agent::kDeviceBackendCount; ++i) {
    const char* b = agent::kDeviceBackends[i];
    TEST_ASSERT_TRUE_MESSAGE(L.priceDeltaMicro(b, "a-model-nobody-priced", m) > 0, b);
    TEST_ASSERT_TRUE_MESSAGE(L.priceDeltaMicro(b, "", m) > 0, b);
  }
  for (size_t i = 0; i < kProviderSlotCount; ++i) {
    const char* slot = kProviderSlots[i].slug;
    TEST_ASSERT_TRUE_MESSAGE(L.priceDeltaMicro(slot, "zz-unknown", m) > 0, slot);
  }
}

// The DoD's seam: a paid custom endpoint's reported usage, priced by the ledger and
// committed to the ceiling, reaches the 20 USD default and freezes paid calls.
static void test_custom_host_spend_reaches_the_ceiling() {
  UsageLedger L;
  SpendCeiling c = SpendCeiling::firstBoot(202610, false, 0, false);
  TEST_ASSERT_TRUE(c.active());
  int turns = 0;
  while (!c.frozen && turns < 1000) {
    // One turn of 16K input and 500 output tokens: about $0.19 at the estimate.
    c.add(L.priceDeltaMicro("custom", "openrouter-model", tokens(16000, 500)), 202610);
    ++turns;
  }
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_TRUE(c.frozenWith(0));
  TEST_ASSERT_TRUE(turns > 50 && turns < 200);
  // Before the fix the same turns priced at 0 and never froze: a local model still does.
  setCustomEndpointPaid(false);
  SpendCeiling free = SpendCeiling::firstBoot(202610, false, 0, false);
  for (int i = 0; i < 1000; ++i) free.add(L.priceDeltaMicro("custom", "qwen3:8b", tokens(16000, 500)), 202610);
  TEST_ASSERT_FALSE(free.frozen);
}

// The owner-facing page states the estimate's numbers: they are the code's.
static void test_the_doc_states_the_custom_estimate_the_code_uses() {
  const BuiltinPrice* row = builtinSlotDefault("custom");
  TEST_ASSERT_NOT_NULL(row);
  std::string doc;   // the page with its line breaks read as spaces
  for (char ch : slurp("docs/spend-ceiling.md")) {
    const bool space = ch == '\n' || ch == ' ';
    if (!(space && !doc.empty() && doc.back() == ' ')) doc += space ? ' ' : ch;
  }
  char want[160];
  snprintf(want, sizeof(want), "(%u USD per million input tokens and %u USD per million output",
           (unsigned)(row->microIn / 1000000u), (unsigned)(row->microOut / 1000000u));
  TEST_ASSERT_TRUE_MESSAGE(has(doc, want), want);
  setCustomEndpointPaid(false);
  TEST_ASSERT_NULL(builtinSlotDefault("custom"));
}

// The device keeps the switch in step with the configured base URL before every paid
// request (frozen() runs first) and classifies it with the one rule.
static void test_the_device_refreshes_the_custom_switch_before_paid_calls() {
  const std::string src = slurp("src/agent/spend_ceiling_store.cpp");
  const std::string refresh = bodyOf(src, "void refreshCustomPricing() {");
  TEST_ASSERT_TRUE(has(refresh, "customEndpointIsFree("));
  TEST_ASSERT_TRUE(has(refresh, "store::customKey()"));
  TEST_ASSERT_TRUE(has(refresh, "setCustomEndpointPaid("));
  TEST_ASSERT_TRUE(has(bodyOf(src, "bool frozen()"), "refreshCustomPricing();"));
  // ...and before the first load, whose recount or seed prices the ledger's history.
  const std::string load = bodyOf(src, "bool loadedLocked()");
  TEST_ASSERT_TRUE(load.find("refreshCustomPricing();") < load.find("readStateLocked()"));
}

// ---- (3) the freeze notice and the unanswered message --------------------------------

static void test_frozen_reply_text_is_short_plain_and_names_the_way_out() {
  CeilingFacts f;
  f.limitMicro = 20000000;
  f.spentMicro = 20400000;
  f.monthKey = 202610;
  const std::string t = frozenReplyText(f);
  TEST_ASSERT_TRUE(has(t, "Not answered: paid AI calls are paused"));
  TEST_ASSERT_TRUE(has(t, "$20.00"));
  TEST_ASSERT_TRUE(has(t, "Assistant > Usage"));
  TEST_ASSERT_TRUE(has(t, "1 November"));
  TEST_ASSERT_TRUE(t.size() < 320);
  for (unsigned char ch : t) TEST_ASSERT_TRUE_MESSAGE(ch >= 0x20 && ch < 0x7f, "printable ASCII only");
  // Round 3 (merge review major 5): with no clock the month cannot roll on its own, so
  // the reply never promises "next month": it says the clock must be set first.
  f.monthKey = 0;
  TEST_ASSERT_FALSE(has(frozenReplyText(f), "next month"));
  TEST_ASSERT_TRUE(has(frozenReplyText(f), "once the device's clock is set"));
}

// Round 3 (merge review 7): the paused reply is throttled. The owner's chat gets the full
// reply at most once per 30 minutes (and again when a new freeze starts); any other chat
// (a member, a guest, a group) one neutral line per freeze, with no amount and no steps.
static void test_the_paused_reply_is_throttled_and_others_get_one_neutral_line() {
  FrozenReplyThrottle th;
  const uint32_t ep = 202610;
  TEST_ASSERT_TRUE(th.allow("owner", true, ep, 1000));
  TEST_ASSERT_FALSE(th.allow("owner", true, ep, 1000 + 60000));
  TEST_ASSERT_FALSE(th.allow("owner", true, ep, 1000 + kFrozenReplyEveryMs - 1));
  TEST_ASSERT_TRUE(th.allow("owner", true, ep, 1000 + kFrozenReplyEveryMs));
  TEST_ASSERT_TRUE(th.allow("group", false, ep, 5000));
  TEST_ASSERT_FALSE(th.allow("group", false, ep, 5000 + 10u * kFrozenReplyEveryMs));   // once per freeze
  TEST_ASSERT_TRUE(th.allow("group", false, ep + 1, 9000));                           // a new freeze
  TEST_ASSERT_TRUE(th.allow("owner", true, ep + 1, 1000 + kFrozenReplyEveryMs + 1));  // a new freeze
  for (int i = 0; i < 40; ++i) th.allow("chat" + std::to_string(i), false, ep, 20000);   // more chats than slots
  const std::string other = frozenReplyTextOther();
  TEST_ASSERT_FALSE(has(other, "$"));
  TEST_ASSERT_FALSE(has(other, "Usage"));
  TEST_ASSERT_TRUE(has(other, "paused"));
  for (unsigned char ch : other) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7f);
  // The store asks the throttle, and main says which chat is the owner's.
  const std::string store = bodyOf(slurp("src/agent/spend_ceiling_store.cpp"),
                                   "std::string frozenReplyFor(const std::string& chatId, bool answered, bool owner)");
  TEST_ASSERT_TRUE(has(store, ".allow(chatId, owner,"));
  TEST_ASSERT_TRUE(has(store, "frozenReplyTextOther()"));
  TEST_ASSERT_TRUE(has(bodyOf(slurp("src/main.cpp"), "static void answerIfPaused("), "agent::telegram::ownerChat()"));
}

// Round 3 (merge review major 5): a network that blocks NTP leaves the clock unset for
// good. The newest server date the device has seen (an HTTPS Date header) plus the uptime
// since stands in, so the month still rolls.
static void test_a_server_date_stands_in_for_an_unset_clock() {
  int64_t e = 0;
  TEST_ASSERT_TRUE(parseHttpDate("Tue, 27 Oct 2026 03:10:00 GMT", e));
  TEST_ASSERT_EQUAL_INT64(1793070600LL, e);
  TEST_ASSERT_TRUE(parseHttpDate("Sun, 01 Nov 2026 00:00:00 GMT", e));
  TEST_ASSERT_EQUAL_INT64(1793491200LL, e);
  TEST_ASSERT_FALSE(parseHttpDate("not a date", e));
  TEST_ASSERT_FALSE(parseHttpDate("Tue, 27 Foo 2026 03:10:00 GMT", e));
  TEST_ASSERT_FALSE(parseHttpDate("Tue, 27 Oct 1999 03:10:00 GMT", e));   // before any valid clock
  ServerClock c;
  TEST_ASSERT_EQUAL_INT64(0, c.epoch(0, 5000));                 // no clock, no server date
  TEST_ASSERT_EQUAL_INT64(1793491200LL, c.epoch(1793491200LL, 5000));   // a set clock wins
  c.note(1793070600LL, 10000);
  TEST_ASSERT_EQUAL_INT64(1793070600LL + 50, c.epoch(0, 60000));        // the server date + the uptime since
  c.note(1793070000LL, 70000);                                  // an older date never moves it back
  TEST_ASSERT_EQUAL_INT64(1793070600LL + 60, c.epoch(0, 70000));
  TEST_ASSERT_EQUAL_UINT32(202610u, ceilingMonthKey((time_t)c.epoch(0, 70000)));
  // Five days later (no NTP all the while) it is November: the month rolls.
  TEST_ASSERT_EQUAL_UINT32(202611u, ceilingMonthKey((time_t)c.epoch(0, 70000u + 5u * 86400000u)));
  // The device feeds it and the store reads it.
  const std::string st = slurp("src/agent/spend_ceiling_store.cpp");
  TEST_ASSERT_TRUE(has(bodyOf(st, "uint32_t monthNow()"), "serverClock().epoch("));
  TEST_ASSERT_TRUE(has(slurp("src/agent/transport_tls.cpp"), "noteHttpDateHeader("));
  TEST_ASSERT_TRUE(has(slurp("src/agent/telegram.cpp"), "noteHttpDateHeader("));
  // Round 3 review: the usage ledger reads the same time base, so the month-to-date the
  // store settles with is never 0 while the store's month is already set.
  const std::string ledger = slurp("src/agent/store.cpp");
  TEST_ASSERT_TRUE(has(bodyOf(ledger, "static time_t ledgerNow()"), "serverClock().epoch("));
  for (const char* fn : {"uint32_t currentPeriodKey(uint8_t resetDay) {", "uint32_t currentDayKey() {",
                         "uint64_t monthToDateMicro() {"}) {
    TEST_ASSERT_TRUE_MESSAGE(has(bodyOf(ledger, fn), "ledgerNow()"), fn);
    TEST_ASSERT_FALSE_MESSAGE(has(bodyOf(ledger, fn), "time(nullptr)"), fn);
  }
}

// Round 3 review: only a date from a provider or Telegram over TLS is trusted, never one
// that jumps years ahead, and the clock re-anchors on every sample (no drift, no wrap).
static void test_the_server_clock_trusts_only_sane_provider_dates() {
  TEST_ASSERT_TRUE(trustedDateHost("api.openai.com"));
  TEST_ASSERT_TRUE(trustedDateHost("api.telegram.org"));
  TEST_ASSERT_TRUE(trustedDateHost("app.cumulo-nimbus.ai"));
  TEST_ASSERT_FALSE(trustedDateHost("my-pi.local"));
  TEST_ASSERT_FALSE(trustedDateHost("evil-openai.com.example"));
  TEST_ASSERT_FALSE(trustedDateHost(""));
  ServerClock c;
  c.note(4102444800LL, 1000);   // 2100: not a date this device can be in
  TEST_ASSERT_EQUAL_INT64(0, c.epoch(0, 2000));
  c.note(1793070600LL, 1000);
  c.note(1793070600LL + 40LL * 86400, 2000);   // a 40-day jump one second later: refused
  TEST_ASSERT_EQUAL_INT64(1793070601LL, c.epoch(0, 2000));
  c.note(1793070600LL + 5, 3000);              // a sample ahead of the extrapolation re-anchors
  TEST_ASSERT_EQUAL_INT64(1793070605LL, c.epoch(0, 3000));
  c.note(1793070600LL, 4000000);               // a sample behind it re-anchors at the extrapolation
  TEST_ASSERT_EQUAL_INT64(1793070605LL + 3997, c.epoch(0, 4000000));
  TEST_ASSERT_TRUE(has(bodyOf(slurp("src/agent/transport_tls.cpp"), "bool readHeaders(int code)"),
                       "trustedDateHost("));
}

static void test_frozen_reply_is_due_only_for_an_unanswered_telegram_message_during_a_pause() {
  for (int tg = 0; tg < 2; ++tg)
    for (int fr = 0; fr < 2; ++fr)
      for (int an = 0; an < 2; ++an)
        TEST_ASSERT_EQUAL(tg && fr && !an, frozenReplyDue(tg, fr, an));
}

// Where the device handles a message: the sink counts Telegram sends to the chat being
// handled (no other chat's), every message is checked after it was handled, and the pump
// goes to the owner chat of every notice.
static void test_main_wires_the_unanswered_reply_and_the_owner_notice() {
  const std::string src = slurp("src/main.cpp");
  const std::string sink = bodyOf(src, "static bool orchSendSink(const String& chatId, const String& text)");
  const size_t count = sink.find("if (chatHash(chatId) == g_turnChatHash.load()) g_turnChatSends.fetch_add(1);");
  TEST_ASSERT_TRUE(count != std::string::npos && count < sink.rfind("agent::telegram::send(chatId, text)"));
  const std::string onMsg = bodyOf(src, "static void orchOnMessage(");
  const size_t handled = onMsg.find("agent::orchestrator::handleMessage(");
  TEST_ASSERT_TRUE(handled != std::string::npos);
  TEST_ASSERT_TRUE(onMsg.find("g_turnChatHash.store(chatHash(chatId));") < handled);
  TEST_ASSERT_TRUE(onMsg.find("g_turnChatSends.load() != sentBefore") > handled);
  TEST_ASSERT_TRUE(onMsg.find("g_turnChatHash.store(0);") > handled);
  TEST_ASSERT_TRUE(onMsg.find("answerIfPaused(chatId,") > handled);
  TEST_ASSERT_TRUE(has(bodyOf(src, "static void answerIfPaused("), "agent::spend::frozenReplyFor("));
  const std::string pump = bodyOf(src, "static void spendNoticePump()");
  TEST_ASSERT_TRUE(has(pump, "agent::telegram::ownerChat()"));
  TEST_ASSERT_FALSE(has(pump, "firstAllowedChat()"));
  // The store's answer: Telegram chats only, the overall freeze, the one decision.
  const std::string store = bodyOf(slurp("src/agent/spend_ceiling_store.cpp"),
                                   "std::string frozenReplyFor(const std::string& chatId, bool answered, bool owner)");
  TEST_ASSERT_TRUE(has(store, "DeliveryChannel::Telegram"));
  TEST_ASSERT_TRUE(has(store, "frozenReplyDue("));
  TEST_ASSERT_TRUE(has(store, "frozenWith(g_st.pending)"));
}

// ---- (4) no clock: a provisional month, enforced at once -----------------------------

static void test_counts_without_a_clock_are_enforced_at_once() {
  SpendCeiling c = SpendCeiling::firstBoot(0, false, 0, false);   // a new device, no clock
  TEST_ASSERT_EQUAL_UINT32(0, c.monthKey);
  TEST_ASSERT_TRUE(c.active());
  c.add(19000000, 0);
  TEST_ASSERT_FALSE(c.frozen);
  TEST_ASSERT_TRUE(c.frozenWith(1000000));   // the turn in flight crosses it
  c.add(1000000, 0);
  TEST_ASSERT_TRUE(c.frozen);   // no clock, still frozen at the ceiling
  // The clock is set: the provisional count is this month's, the freeze holds.
  TEST_ASSERT_TRUE(reconcileProvisional(c, 202610, false, 0));
  TEST_ASSERT_EQUAL_UINT32(202610, c.monthKey);
  TEST_ASSERT_EQUAL_UINT64(20000000ULL, c.spentMicro);
  TEST_ASSERT_TRUE(c.frozen);
}

// A restart with no clock after the month changed: the count is still filed under the
// earlier month, so the clock's first read starts the new month - keeping the spend
// made while the clock was unset, never dropping it with the old month's count.
static void test_spend_made_without_a_clock_survives_a_month_that_changed_meanwhile() {
  SpendCeiling c = SpendCeiling::firstBoot(202609, false, 0, false);
  c.add(15000000, 202609);                 // September's count, read back from the file
  c.add(2000000, 0);                       // October, before the clock: added on top
  TEST_ASSERT_EQUAL_UINT64(17000000ULL, c.spentMicro);
  TEST_ASSERT_TRUE(reconcileProvisional(c, 202610, false, 0, 2000000));
  TEST_ASSERT_EQUAL_UINT32(202610, c.monthKey);
  TEST_ASSERT_EQUAL_UINT64(2000000ULL, c.spentMicro);   // October keeps its own spend
  // The same month: the provisional spend is already in the count, never added twice.
  SpendCeiling d = SpendCeiling::firstBoot(202610, false, 0, false);
  d.add(1000000, 0);
  TEST_ASSERT_FALSE(reconcileProvisional(d, 202610, false, 0, 1000000));
  TEST_ASSERT_EQUAL_UINT64(1000000ULL, d.spentMicro);
}

static void test_reconcile_adds_the_owed_seed_once_even_after_an_early_roll() {
  // A device that spent before (ledger: $12 this month) started without a clock.
  SpendCeiling c = SpendCeiling::firstBoot(0, false, 0, false);
  c.add(3000000, 0);                          // $3 provisional
  TEST_ASSERT_FALSE(reconcileProvisional(c, 0, true, 12000000));   // still no clock
  TEST_ASSERT_TRUE(reconcileProvisional(c, 202610, true, 12000000));
  TEST_ASSERT_EQUAL_UINT64(15000000ULL, c.spentMicro);   // fail-closed: both counted
  // A commit took the count into the month before the seed landed: the seed still adds.
  SpendCeiling d = SpendCeiling::firstBoot(0, false, 0, false);
  d.add(18000000, 0);
  d.add(1000000, 202610);   // rolled by the commit (month 0 -> 202610, kept)
  TEST_ASSERT_EQUAL_UINT32(202610, d.monthKey);
  TEST_ASSERT_TRUE(reconcileProvisional(d, 202610, true, 5000000));
  TEST_ASSERT_EQUAL_UINT64(24000000ULL, d.spentMicro);
  TEST_ASSERT_TRUE(d.frozen);
}

// The clock debt across restarts, the sequences the device store runs (each restart
// is a serialize / deserialize of the marker, the ceiling file kept as the count).
static void test_clock_debt_survives_restarts_and_settles_once() {
  // A new device, no clock: spend counts at once; a restart before the clock keeps the
  // provisional part; the clock (same month) adds nothing twice.
  SpendCeiling c = SpendCeiling::firstBoot(0, false, 0, false);
  ClockDebt debt;
  debt.floor = true;
  c.add(3000000, 0);
  debt.noteCommit(3000000, false);
  ClockDebt again;
  TEST_ASSERT_TRUE(again.deserialize(debt.serialize()));   // restart
  TEST_ASSERT_EQUAL_UINT64(3000000ULL, again.provisional);
  c.add(2000000, 0);
  again.noteCommit(2000000, false);
  const ClockDebt::Settled s = again.settle(c, 202610, 0);
  TEST_ASSERT_TRUE(s.changed);
  TEST_ASSERT_FALSE(s.seeded);
  TEST_ASSERT_EQUAL_UINT64(5000000ULL, c.spentMicro);
  TEST_ASSERT_FALSE(again.owed());
  TEST_ASSERT_EQUAL_STRING("", again.serialize().c_str());   // the marker goes
  TEST_ASSERT_FALSE(again.settle(c, 202610, 0).changed);       // settled once
  // A count filed under September, two restarts with no clock in October: October keeps
  // all of its own spend (the review's lost-12-dollars case).
  SpendCeiling sep = SpendCeiling::firstBoot(202609, false, 0, false);
  sep.add(5000000, 202609);
  ClockDebt d1;
  sep.add(12000000, 0);
  d1.noteCommit(12000000, false);
  ClockDebt d2;
  d2.deserialize(d1.serialize());   // second clockless restart
  sep.add(1000000, 0);
  d2.noteCommit(1000000, false);
  d2.settle(sep, 202610, 0);
  TEST_ASSERT_EQUAL_UINT32(202610, sep.monthKey);
  TEST_ASSERT_EQUAL_UINT64(13000000ULL, sep.spentMicro);
  // An upgraded device with no clock: the seed and the first-boot notice, once.
  SpendCeiling up = SpendCeiling::firstBoot(0, false, 0, false);
  ClockDebt seed;
  seed.seed = 2;
  up.add(1000000, 0);
  seed.noteCommit(1000000, false);
  ClockDebt seed2;
  seed2.deserialize(seed.serialize());
  TEST_ASSERT_TRUE(seed2.needsLedger());
  const ClockDebt::Settled done = seed2.settle(up, 202610, 9000000);
  TEST_ASSERT_TRUE(done.seeded && done.migration);
  TEST_ASSERT_EQUAL_UINT64(10000000ULL, up.spentMicro);
  // A plain clockless load is floored at the ledger once the clock is set.
  SpendCeiling fl = SpendCeiling::firstBoot(202610, false, 0, false);
  fl.add(4000000, 202610);
  ClockDebt f;
  f.floor = true;
  TEST_ASSERT_EQUAL_STRING("", f.serialize().c_str());   // RAM only: each load decides
  f.settle(fl, 202610, 7000000);
  TEST_ASSERT_EQUAL_UINT64(7000000ULL, fl.spentMicro);
  // Nothing happens without a clock.
  ClockDebt none;
  none.seed = 1;
  SpendCeiling z = SpendCeiling::firstBoot(0, false, 0, false);
  TEST_ASSERT_FALSE(none.settle(z, 0, 1000).changed);
  TEST_ASSERT_TRUE(none.owed());
}

static void test_clock_debt_marker_reads_old_and_bad_markers_fail_closed() {
  ClockDebt d;
  TEST_ASSERT_TRUE(d.deserialize("2"));   // the earlier marker
  TEST_ASSERT_EQUAL(2, d.seed);
  TEST_ASSERT_TRUE(d.deserialize("1|250000"));
  TEST_ASSERT_EQUAL(1, d.seed);
  TEST_ASSERT_EQUAL_UINT64(250000ULL, d.provisional);
  for (const char* bad : {"", "x", "3|1", "1|abc", "|5", "1|99999999999999999999"}) {
    TEST_ASSERT_FALSE_MESSAGE(d.deserialize(bad), bad);
    TEST_ASSERT_EQUAL_MESSAGE(1, d.seed, bad);   // unreadable: the seed is owed
  }
}

// The device store loads with no clock (it used to wait, counting nothing and freezing
// nothing until the first time sync), keeps the debt in its marker, and settles it at
// the first chance.
static void test_the_store_loads_and_enforces_without_a_clock() {
  const std::string src = slurp("src/agent/spend_ceiling_store.cpp");
  const std::string load = bodyOf(src, "bool loadedLocked()");
  TEST_ASSERT_FALSE_MESSAGE(has(load, "monthNow() == 0) return false"), "no clock must not mean no ceiling");
  TEST_ASSERT_TRUE(load.find("loadDebtLocked();") < load.find("readStateLocked()"));
  TEST_ASSERT_TRUE(has(load, "reconcileLocked()"));
  TEST_ASSERT_TRUE(has(bodyOf(src, "void firstLoadLocked()"), "oweSeedLocked("));
  TEST_ASSERT_TRUE(has(bodyOf(src, "bool reconcileLocked()"), "g_st.debt.settle("));
  // Whatever a start without a clock owes lands at the first chance: a freeze check,
  // the tick, or a commit (before it can roll the count into the month without it).
  for (const char* fn : {"void tick()", "bool frozen()", "void commit(const Part* parts, size_t n, bool turn)"})
    TEST_ASSERT_TRUE_MESSAGE(has(bodyOf(src, fn), "reconcileLocked()"), fn);
  const std::string commit = bodyOf(src, "void commit(const Part* parts, size_t n, bool turn)");
  TEST_ASSERT_TRUE(commit.find("reconcileLocked()") < commit.find("g_st.c.add(total"));
  TEST_ASSERT_TRUE(has(commit, "g_st.debt.noteCommit(total"));
  // The marker is written before the ceiling file and removed only after a save.
  const std::string persist = bodyOf(src, "bool persistLocked()");
  TEST_ASSERT_TRUE(persist.find("saveDebtLocked(/*afterSave=*/false)") < persist.find("writeFileAtomic("));
  TEST_ASSERT_TRUE(persist.find("saveDebtLocked(/*afterSave=*/true)") > persist.find("writeFileAtomic("));
}

// ---- (5) the count read back, the park, the probe ------------------------------------

static void test_a_count_read_back_is_floored_at_the_ledger() {
  SpendCeiling c = SpendCeiling::firstBoot(202610, false, 0, false);
  c.add(5000000, 202610);
  TEST_ASSERT_TRUE(floorFromLedger(c, 202610, 21000000));   // a lost write: the ledger has more
  TEST_ASSERT_EQUAL_UINT64(21000000ULL, c.spentMicro);
  TEST_ASSERT_TRUE(c.frozen);
  TEST_ASSERT_FALSE(floorFromLedger(c, 202610, 1000000));   // never lowered
  TEST_ASSERT_EQUAL_UINT64(21000000ULL, c.spentMicro);
  TEST_ASSERT_FALSE(floorFromLedger(c, 202611, 99000000));  // another month: untouched
  TEST_ASSERT_FALSE(floorFromLedger(c, 0, 99000000));       // no clock: untouched
  const std::string src = slurp("src/agent/spend_ceiling_store.cpp");
  TEST_ASSERT_TRUE(has(bodyOf(src, "bool loadedLocked()"), "floorLocked()"));
  // A file that will not open is asked again, never read as torn at the first failure.
  const std::string read = bodyOf(src, "FileRead readFile(std::string& out)");
  TEST_ASSERT_TRUE(has(read, "FileRead::Missing"));
  TEST_ASSERT_TRUE(has(read, "attempt < 5"));
  TEST_ASSERT_TRUE(has(bodyOf(src, "bool readStateLocked()"), "FileRead::Unreadable"));
}

static void test_reports_land_in_state_order_and_park_checks_ask_the_ceiling_first() {
  const std::string src = slurp("src/agent/spend_ceiling_store.cpp");
  const std::string report = bodyOf(src, "void report(bool global, const ProviderFreezes& providers)");
  TEST_ASSERT_TRUE(has(report, "providers.seq - g_reportedSeq"));
  TEST_ASSERT_TRUE(has(bodyOf(src, "bool frozen()"), "p.seq = ++g_st.reportSeq;"));
  const std::string gate = slurp("src/agent/provider_gate.cpp");
  const std::string check = bodyOf(gate, "orch::ParkVerdict check(const char* provider, orch::PaidSeam seam, const String& key)");
  const size_t frozen = check.find("nimbus::orch::spendFrozen()");
  TEST_ASSERT_TRUE(frozen != std::string::npos && frozen < check.find("providerPark().available("));
  // The park file: missing is "none yet", an open failure is "ask again" (never overwritten).
  const std::string load = bodyOf(gate, "int loadPark(std::string& blob)");
  TEST_ASSERT_TRUE(load.find("LittleFS.exists(kParkPath)") < load.find("LittleFS.open(kParkPath"));
  TEST_ASSERT_TRUE(has(load, "return -1;\n  }"));
}

static void test_verify_probe_policy_and_its_metering() {
  TEST_ASSERT_TRUE(verifyProbeAllowed(true, true, false));     // the owner's Verify
  TEST_ASSERT_TRUE(verifyProbeAllowed(false, false, true));    // nothing holds it back
  TEST_ASSERT_FALSE(verifyProbeAllowed(false, true, true));    // paused
  TEST_ASSERT_FALSE(verifyProbeAllowed(false, false, false));  // parked
  TEST_ASSERT_EQUAL_STRING("{\"usage\":1}", httpBodyOf("Content-Type: x\r\nA: b\r\n\r\n{\"usage\":1}").c_str());
  TEST_ASSERT_EQUAL_STRING("", httpBodyOf("Content-Type: x\r\n").c_str());
  const std::string src = slurp("src/agent/provider_verify.cpp");
  const std::string probe = bodyOf(src, "static nimbus::orch::ProbeVerdict probeModel(const String& provider, const String& model)");
  TEST_ASSERT_TRUE(has(probe, "meterProbe("));
  TEST_ASSERT_TRUE(has(probe, "noteProbeAnswer("));
  TEST_ASSERT_TRUE(has(bodyOf(src, "static void meterProbe("), "meterSeamAnswerBody("));
  const std::string sel = bodyOf(src, "static void probeSelectedModels(const String& provider)");
  TEST_ASSERT_TRUE(sel.find("paidProbeAllowed(provider)") < sel.find("probeModel("));
  const std::string run = bodyOf(src, "static void runOne()");
  TEST_ASSERT_TRUE(has(run, "store::recordProviderCall(\"tavily\")"));
  TEST_ASSERT_TRUE(has(run, "verifyProbeAllowed(g_ownerAsked"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_custom_endpoint_local_is_only_the_devices_own_network);
  RUN_TEST(test_a_keyed_endpoint_is_paid_even_on_the_lan);
  RUN_TEST(test_paid_custom_endpoint_prices_at_the_highest_row_never_zero);
  RUN_TEST(test_every_billing_backend_prices_an_unknown_model_above_zero);
  RUN_TEST(test_custom_host_spend_reaches_the_ceiling);
  RUN_TEST(test_the_doc_states_the_custom_estimate_the_code_uses);
  RUN_TEST(test_the_device_refreshes_the_custom_switch_before_paid_calls);
  RUN_TEST(test_frozen_reply_text_is_short_plain_and_names_the_way_out);
  RUN_TEST(test_frozen_reply_is_due_only_for_an_unanswered_telegram_message_during_a_pause);
  RUN_TEST(test_the_paused_reply_is_throttled_and_others_get_one_neutral_line);
  RUN_TEST(test_a_server_date_stands_in_for_an_unset_clock);
  RUN_TEST(test_the_server_clock_trusts_only_sane_provider_dates);
  RUN_TEST(test_main_wires_the_unanswered_reply_and_the_owner_notice);
  RUN_TEST(test_counts_without_a_clock_are_enforced_at_once);
  RUN_TEST(test_spend_made_without_a_clock_survives_a_month_that_changed_meanwhile);
  RUN_TEST(test_reconcile_adds_the_owed_seed_once_even_after_an_early_roll);
  RUN_TEST(test_clock_debt_survives_restarts_and_settles_once);
  RUN_TEST(test_clock_debt_marker_reads_old_and_bad_markers_fail_closed);
  RUN_TEST(test_the_store_loads_and_enforces_without_a_clock);
  RUN_TEST(test_a_count_read_back_is_floored_at_the_ledger);
  RUN_TEST(test_reports_land_in_state_order_and_park_checks_ask_the_ceiling_first);
  RUN_TEST(test_verify_probe_policy_and_its_metering);
  return UNITY_END();
}
