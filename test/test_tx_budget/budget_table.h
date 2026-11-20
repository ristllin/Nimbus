#pragma once
// budget_table.h - the generator of docs/reference/transcript-budgets.md (lane RB-T4): engine 2's
// per-head budget in tokens, the cost of a turn at full and the latency at full, from the code
// (tx::budgetFor) and the measured inputs (tests/evals/measured/transcript_budget.json). The cost and
// latency are REPORTED (owner ruling 82 item 5): nothing here is a pass condition except that a
// measured figure may not contradict its own row by more than 15%.
#include <ArduinoJson.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/recorded_models.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"

namespace budgettable {

namespace tx = nimbus::orch::tx;
using nimbus::orch::BudgetLink;

inline const JsonDocument& measured() {
  static JsonDocument doc;
  static bool loaded = false;
  if (!loaded) {
    TEST_ASSERT_FALSE(deserializeJson(doc, recorded::readFixture("tests/evals/measured/transcript_budget.json")));
    loaded = true;
  }
  return doc;
}

// One head class of the table.
struct Head {
  const char* name;
  const char* host;
  const char* model;
  bool routed;
  bool lift;
  BudgetLink link;
};
inline const std::vector<Head>& heads() {
  static const std::vector<Head> h = {
      {"claude-sonnet-5-5 direct (opus-5-5, fable-5-1 alike)", "anthropic", "claude-sonnet-5-5", false, false, BudgetLink::Device},
      {"claude-haiku-5-5 direct", "anthropic", "claude-haiku-5-5", false, false, BudgetLink::Device},
      {"gpt-5.6 direct", "openai", "gpt-5.6", false, false, BudgetLink::Device},
      {"gpt-5.6-luna direct", "openai", "gpt-5.6-luna", false, false, BudgetLink::Device},
      {"glm-5.3 direct (Z.ai), device", "zai", "glm-5.3", false, false, BudgetLink::Device},
      {"glm-5.3 direct (Z.ai), Virtual Nimbus", "zai", "glm-5.3", false, false, BudgetLink::Server},
      {"routed default (cumulo gpt-5.6), lift off", "cumulo", "gpt-5.6", true, false, BudgetLink::Device},
      {"routed default (cumulo gpt-5.6), lift on", "cumulo", "gpt-5.6", true, true, BudgetLink::Device},
      {"routed Sonnet (cumulo anthropic/claude-sonnet-5-5), lift off", "cumulo", "anthropic/claude-sonnet-5-5", true, false,
       BudgetLink::Device},
      {"routed Sonnet (cumulo anthropic/claude-sonnet-5-5), lift on", "cumulo", "anthropic/claude-sonnet-5-5", true, true,
       BudgetLink::Device},
      {"mistral-large-4-0 direct, ASSUMED (held)", "mistral", "mistral-large-4-0", false, false, BudgetLink::Device},
      {"paid custom endpoint (gpt-5.6-luna id)", "custom", "gpt-5.6-luna", false, false, BudgetLink::Device},
  };
  return h;
}

inline tx::HeadBudget budgetOf(const Head& h) {
  tx::HeadBudget b;
  TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{h.host, h.model, h.routed}, tx::Profile::Full, h.link, h.lift, b).ok());
  return b;
}

// ---- the cost of a turn at full ----------------------------------------------------------------

// Micro-USD per 1M tokens by price class, as a head is billed.
struct Rates {
  double in = 0, read = 0, write = 0, out = 0;
  const char* cache = "";
};
inline Rates ratesOf(const Head& h) {
  const std::string host = h.host;
  const nimbus::orch::BuiltinPrice* p = host == "custom" ? nimbus::orch::builtinCustomPaidRow()
                                                         : nimbus::orch::builtinModelPrice(h.host, h.model);
  if (!p) p = nimbus::orch::builtinDefaultPrice(h.host, h.model);
  TEST_ASSERT_NOT_NULL_MESSAGE(p, h.model);
  Rates r;
  r.in = p->microIn, r.out = p->microOut;
  if (host == "anthropic") {   // cache reads and 5-minute writes, as billed
    r.read = p->microCacheR ? p->microCacheR : r.in / 10;
    r.write = p->microCacheW ? p->microCacheW : r.in * 1.25;
    r.cache = "reads and 5-minute writes billed";
  } else if (host == "openai" || host == "zai") {   // automatic caching: reads discounted, no write premium
    r.read = p->microCacheR ? p->microCacheR : r.in / 10;
    r.write = r.in;
    r.cache = host == "zai" ? "reads at the row's rate (assumed)" : "reads discounted, no write premium";
  } else {   // the router until RB-CUV, Mistral, a paid custom endpoint: no cache discount assumed
    r.read = r.write = r.in;
    r.cache = "no cache discount assumed";
  }
  return r;
}

// The blocks a turn's cost sums (PRD section 2's cost model, measured): the request input as the
// provider counted it (the fixed part, the digest block, the conversation and the photos), the
// output (thinking included, as billed), the background digests and the other services.
struct CostBlocks {
  double fixedTokens = 0;          // system and tools: at most kFixedOverheadCeiling at the wire ratio
  double digestTokens = 0;         // the digest block and the recent-files list
  double conversationTokens = 0;   // owner, assistant, calls, receipts, stubs and photos
  double inputUsd = 0;             // all input a turn, priced by class
  double outputUsd = 0;            // output a turn, thinking included
  double backgroundUsd = 0;        // the digest requests, a turn
  double otherUsdMonth = 0;        // search, speech, transcription (PRD: 1.2 USD a month, not measured)
  double turnUsd() const { return inputUsd + outputUsd + backgroundUsd; }
  double monthUsd(double turnsADay) const { return 30 * turnsADay * turnUsd() + otherUsdMonth; }
  long turnsInside(double ceiling) const { return turnUsd() > 0 ? (long)std::floor((ceiling - otherUsdMonth) / turnUsd()) : -1; }
};

inline CostBlocks costOf(const Head& h) {
  const JsonDocument& m = measured();
  JsonObjectConst arm = m["full_arm"], cls = arm["turn_classes_blended"];
  const tx::HeadBudget b = budgetOf(h);
  tx::HeadBudget sonnet;
  TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{"anthropic", "claude-sonnet-5-5", false}, tx::Profile::Full,
                                 BudgetLink::Device, false, sonnet)
                       .ok());
  // The arm ran claude-sonnet-5-5 at full; a head's input scales with its steady-state request,
  // the middle of its due mark and its keep.
  const double k = (double)(b.tokens.due + b.tokens.keep) / (sonnet.tokens.due + sonnet.tokens.keep);
  const Rates r = ratesOf(h);
  const double fresh = cls["fresh"] | 0.0, read = cls["read"] | 0.0, write = cls["write"] | 0.0, out = cls["output"] | 0.0;
  CostBlocks c;
  const double ratio = b.bodyMilliBytesPerToken / 1000.0;
  c.fixedTokens = tx::kFixedOverheadCeiling / ratio;
  c.digestTokens = tx::kFullDigestAndFilesBytes / ratio;
  const double perRequest = (double)(arm["prompt_tokens_per_request_mean"] | 0) * k;
  c.conversationTokens = perRequest - c.fixedTokens - c.digestTokens;
  c.inputUsd = k * (fresh * r.in + read * r.read + write * r.write) / 1e12;
  c.outputUsd = out * r.out / 1e12;
  c.backgroundUsd = arm["background_usd_per_turn"] | 0.0;
  c.otherUsdMonth = m["other_services_usd_month"] | 0.0;
  return c;
}

// ---- latency at full (reported) --------------------------------------------------------------

struct Latency {
  double linkProd = 0, linkBench = 0;   // connect and upload of the request at the due mark, s
  double coldProvider = 0, warmProvider = 0, photoTurn = 0;
  double chatToday = 0, chatCut = 0, toolToday = 0, toolCut = 0, photoToday = 0, photoCut = 0;
};
// Today's paths on Lumi (ruling 19): 3.6 s of arrival over the hold, 4.5 s from the result to the send.
// RB-TS1's targets (ruling 19): 1.0 s and 0.7 s. The hold: 2 s (ruling 13).
inline constexpr double kArrivalToday = 3.6, kSendToday = 4.5, kArrivalCut = 1.0, kSendCut = 0.7, kHold = 2.0;
inline Latency latencyOf(const Head& h) {
  const JsonDocument& m = measured();
  JsonObjectConst lk = m["link"], arm = m["full_arm"];
  const tx::HeadBudget b = budgetOf(h);
  const double body = tx::bodyBytesFor(b, b.tokens.due);
  Latency l;
  l.linkProd = (lk["production"]["connect_ms"] | 0.0) / 1000 + body / (lk["production"]["upload_Bps"] | 1.0);
  l.linkBench = (lk["bench"]["connect_ms"] | 0.0) / 1000 + body / (lk["bench"]["upload_Bps"] | 1.0);
  l.coldProvider = arm["request_seconds"]["cold_first"]["p50"] | 0.0;
  l.warmProvider = arm["request_seconds"]["warm"]["p50"] | 0.0;
  l.photoTurn = arm["seconds"]["photo"]["p50"] | 0.0;
  const double chat = l.linkProd + l.coldProvider, round = l.linkProd + l.warmProvider;
  l.chatToday = kArrivalToday + kHold + chat + kSendToday;
  l.chatCut = kArrivalCut + kHold + chat + kSendCut;
  l.toolToday = l.chatToday + round;
  l.toolCut = l.chatCut + round;
  l.photoToday = kArrivalToday + kHold + l.linkProd + l.photoTurn + kSendToday;
  l.photoCut = kArrivalCut + kHold + l.linkProd + l.photoTurn + kSendCut;
  return l;
}

// ---- rendering --------------------------------------------------------------------------------

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
  va_list ap, again;
  va_start(ap, f);
  va_copy(again, ap);
  const int n = std::vsnprintf(nullptr, 0, f, ap);
  va_end(ap);
  std::string out(n > 0 ? (size_t)n : 0, '\0');
  if (n > 0) std::vsnprintf(&out[0], (size_t)n + 1, f, again);
  va_end(again);
  return out;
}
inline std::string num(double v) {   // 1234567 -> "1,234,567"
  char raw[32];
  std::snprintf(raw, sizeof raw, "%.0f", v);
  std::string s = raw, out;
  const size_t start = s[0] == '-' ? 1 : 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i > start && (s.size() - i) % 3 == 0) out += ',';
    out += s[i];
  }
  return out;
}

// What set the aim, said as what it is (ruling 91: table/F4, table/F9, T4-ADV-10).
inline const char* boundLabel(tx::AimBound b) {
  switch (b) {
    case tx::AimBound::Clamp: return "clamp: 100,000 tokens";
    case tx::AimBound::Tier: return "price tier: a cost bound (0.9 of it; ruling 89)";
    case tx::AimBound::Window: return "window: v5.1.2's dense-ratio byte cap (0.75 x window x 2.5 B)";
    case tx::AimBound::Latency: return "modeled read-window deadline (v5.1.2's fit, no failing request; ruling 89)";
    case tx::AimBound::Floor: return "floor: 85,000 B body (routed with the lift off, custom)";
  }
  return "";
}
// Whether a head's token figures stand on a measured wire ratio or v5.1.2's nominal one (table/F3).
inline const char* ratioLabel(const tx::HeadBudget& b) { return b.bodyInterceptBytes ? "measured" : "nominal"; }

inline std::string headsSection() {
  std::string o =
      "## Head classes at the default profile, full\n\n"
      "Tokens first. \"Tokens are\" says whether a row's token figures stand on a measured bytes-per-token ratio of "
      "its wire or on v5.1.2's nominal one (on Lumi's own content the OpenAI wire measured 4.1 B a token against "
      "the nominal 3.6, so a nominal row may carry about 12% fewer tokens than it says; no OpenAI, Mistral or "
      "Z.ai ratio is measured).\n\n"
      "| Head class | Aim (tokens) | Due (tokens) | Keep (tokens) | Tokens are | Bound | Body (B) | Due mark H (B) "
      "| Keep L (B) | Digest block (B) | Message cap (B) | Floor head | v5.1.2 fold / keep (B, text) |\n"
      "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (const Head& h : heads()) {
    const tx::HeadBudget b = budgetOf(h);
    o += fmt("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s / %s |\n", h.name, num(b.tokens.aim).c_str(),
             num(b.tokens.due).c_str(), num(b.tokens.keep).c_str(), ratioLabel(b), boundLabel(b.bound),
             num(b.bodyCap).c_str(), num(b.marks.high).c_str(), num(b.marks.low).c_str(),
             num(tx::blockCapFor(b.profile) + tx::kRecentFilesMaxBytes).c_str(), num(b.messageCap).c_str(),
             b.floorHead ? "yes" : "no", num(b.releasedFold).c_str(), num(b.releasedKeep).c_str());
  }
  return o + "\n";
}

inline std::string costSection() {
  const JsonDocument& m = measured();
  const double perDay = m["turns_per_day"] | 0.0;
  std::string o = fmt(
      "## Cost of a turn at full (reported, never a pass condition)\n\n"
      "claude-sonnet-5-5 is measured: the full arm (%s turns, %s requests, %.2f requests a turn, a mean "
      "request of %s tokens, billed %.2f USD). Every other row is derived: the arm's tokens a turn by "
      "price class (fresh input, cache reads, cache writes, output; a cold and a warm opening blended at "
      "the measured cold share %.4f), the input scaled by the head's (due + keep) over Sonnet's, priced "
      "at the head's built-in price row (model_price_defaults.h), plus the measured digest requests a "
      "turn and %.1f USD a month of other services (search, speech, transcription; the PRD's figure, not "
      "measured). Month: 30 days.\n\n"
      "| Head class | Mean request (tokens) | Fixed + digest (tokens) | USD a turn | Month at 12 a day | Month at "
      "%.2f a day | Turns a month inside 20 USD | Cache |\n|---|---|---|---|---|---|---|---|\n",
      num(m["full_arm"]["turns"] | 0).c_str(), num(m["full_arm"]["requests"] | 0).c_str(),
      (double)(m["full_arm"]["requests_per_turn"] | 0.0), num(m["full_arm"]["prompt_tokens_per_request_mean"] | 0).c_str(),
      (double)(m["full_arm"]["billed_usd"] | 0.0), (double)(m["cold_share_5m"] | 0.0),
      (double)(m["other_services_usd_month"] | 0.0), perDay);
  for (const Head& h : heads()) {
    const CostBlocks c = costOf(h);
    o += fmt("| %s | %s | %s | %.4f | %.1f | %.1f | %s | %s |\n", h.name,
             num(c.fixedTokens + c.digestTokens + c.conversationTokens).c_str(),
             num(c.fixedTokens + c.digestTokens).c_str(), c.turnUsd(), c.monthUsd(12), c.monthUsd(perDay),
             num((double)c.turnsInside(20)).c_str(), ratesOf(h).cache);
  }
  return o + "\n";
}

inline std::string latencySection() {
  const JsonDocument& m = measured();
  std::string o = fmt(
      "## Latency at full (reported, ruling 82 item 5)\n\n"
      "A request at the due mark on the head's wire, through the production link (connect %.2f s, %s B/s: "
      "Lumi's own logs) and the bench link (connect %.2f s, %s B/s: RB-M2). Provider time is the full arm's "
      "(claude-sonnet-5-5: a cold first request p50 %.2f s, a warm round p50 %.2f s, a photo turn p50 %.2f s). "
      "A chat turn is the arrival over the hold, the 2 s hold, one cold request and the send path; a one-tool "
      "turn adds a warm round; a photo turn takes the arm's photo turn. Today's paths: 3.6 s arrival and "
      "4.5 s send on Lumi; RB-TS1's targets: 1.0 s and 0.7 s (ruling 19). The figure times 1.2 is shown "
      "as the reported ceiling; no figure here gates a release or shrinks a window.\n\n"
      "| Head class | Due (tokens) | Body at due (B) | Link production / bench (s) | Chat today / cut (s) "
      "| One-tool today / cut (s) | Photo today / cut (s) | Reported ceilings chat / tool / photo, cut (s) |\n"
      "|---|---|---|---|---|---|---|---|\n",
      (double)(m["link"]["production"]["connect_ms"] | 0.0) / 1000, num(m["link"]["production"]["upload_Bps"] | 0).c_str(),
      (double)(m["link"]["bench"]["connect_ms"] | 0.0) / 1000, num(m["link"]["bench"]["upload_Bps"] | 0).c_str(),
      (double)(m["full_arm"]["request_seconds"]["cold_first"]["p50"] | 0.0),
      (double)(m["full_arm"]["request_seconds"]["warm"]["p50"] | 0.0), (double)(m["full_arm"]["seconds"]["photo"]["p50"] | 0.0));
  for (const Head& h : heads()) {
    const tx::HeadBudget b = budgetOf(h);
    const Latency l = latencyOf(h);
    o += fmt("| %s | %s | %s | %.1f / %.1f | %.1f / %.1f | %.1f / %.1f | %.1f / %.1f | %.0f / %.0f / %.0f |\n", h.name,
             num(b.tokens.due).c_str(), num(tx::bodyBytesFor(b, b.tokens.due)).c_str(), l.linkProd, l.linkBench, l.chatToday, l.chatCut, l.toolToday,
             l.toolCut, l.photoToday, l.photoCut, std::ceil(l.chatCut * 1.2), std::ceil(l.toolCut * 1.2),
             std::ceil(l.photoCut * 1.2));
  }
  return o + "\n";
}

// The resident of the largest head after a restart, read from the card (RB-M2's 130 KB sequential read).
inline std::string restartSection(size_t arenaNeed) {
  const JsonDocument& m = measured();
  const double ms = arenaNeed * (m["link"]["card_read_ms_per_130kb"] | 0.0) / (130.0 * 1024);
  return fmt(
      "## Arena and restart fill at full\n\n"
      "The arena of the active chat is %s B (tx/arena.h). The largest head at full (gpt-5.6-luna: due mark "
      "H plus one request's growth) takes up to %s B of it, by turn mix, in test_tx_budget. Read back after "
      "a restart at RB-M2's measured card rate (%.1f ms per 130 KB in 8 KB slices) that is about %.0f ms of "
      "card time, inside the 3 s gate: the fill stays one sequential read. With no card, v5.1.2 kept its "
      "normal marks over a RAM ring (a01ef7fb); engine 2 keeps the same marks in the same arena.\n\n",
      num(tx::kActiveChatArenaBytes).c_str(), num(arenaNeed).c_str(), (double)(m["link"]["card_read_ms_per_130kb"] | 0.0),
      ms);
}

inline std::string profilesSection() {
  std::string o =
      "## The owner's smaller profiles\n\n"
      "Only as the owner's pinned choice; no fallback, migration or automatic step resolves to one (rulings "
      "81 and 83). Each caps the stored marks at its figures and at the head's full marks; the body, the aim, "
      "the digest block and the caps stay the head's own. Tokens first: the request at the profile's due mark "
      "and just after its compaction, on each head's wire (nominal where its ratio is), then the stored marks.\n\n"
      "| Head class | Profile | Due (tokens, about) | Keep (tokens, about) | Tokens are | H (B) | L (B) |\n"
      "|---|---|---|---|---|---|---|\n";
  const double around = tx::kFixedOverheadCeiling + tx::kFullDigestAndFilesBytes;
  for (const Head& h : heads()) {
    for (tx::Profile p : {tx::Profile::Tight, tx::Profile::Lean, tx::Profile::Standard, tx::Profile::Wide}) {
      tx::HeadBudget b;
      TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{h.host, h.model, h.routed}, p, h.link, h.lift, b).ok());
      const double ratio = b.bodyMilliBytesPerToken / 1000.0, icpt = b.bodyInterceptBytes;
      o += fmt("| %s | %s | %s | %s | %s | %s | %s |\n", h.name, tx::profileSlug(p),
               num((b.marks.high + around - icpt) / ratio).c_str(), num((b.marks.low + around - icpt) / ratio).c_str(),
               ratioLabel(b), num(b.marks.high).c_str(), num(b.marks.low).c_str());
    }
  }
  return o + "\n";
}

inline std::string everyHeadSection() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, recorded::readFixture("test/support/fixtures/budgets_v5_1_2.json")));
  std::string o =
      "## Every recorded head\n\n"
      "Every head of budgets_v5_1_2.json (the released v5.1.2 rows), at full, routed lift off.\n\n"
      "| Link | Provider | Model | Aim (tokens) | Due (tokens) | Keep (tokens) | Tokens are | Bound | Body (B) | H (B) "
      "| L (B) |\n|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (JsonObjectConst r : doc["models"].as<JsonArrayConst>()) {
    const std::string link = r["link"] | "", host = r["provider"] | "", model = r["id"] | "";
    tx::HeadBudget b;
    TEST_ASSERT_TRUE(tx::budgetFor(tx::HeadKey{host, model, host == "cumulo"}, tx::Profile::Full,
                                   link == "server" ? BudgetLink::Server : BudgetLink::Device, false, b)
                         .ok());
    o += fmt("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", link.c_str(), host.c_str(), model.c_str(),
             num(b.tokens.aim).c_str(), num(b.tokens.due).c_str(), num(b.tokens.keep).c_str(), ratioLabel(b), tx::aimBoundSlug(b.bound),
             num(b.bodyCap).c_str(), num(b.marks.high).c_str(), num(b.marks.low).c_str());
  }
  return o;
}

inline std::string intro() {
  const JsonDocument& m = measured();
  JsonObjectConst w = m["wire_fit"], g = m["next_request_growth"];
  return fmt(
      "# Transcript budgets (engine 2)\n\n"
      "Generated by `test_tx_budget` from `tx::budgetFor` (`lib/core/src/orch_tx_budget.cpp`) and the measured "
      "inputs in `tests/evals/measured/transcript_budget.json` (written by `tests/evals/replay/budget_inputs.py`). "
      "Do not edit by hand: re-run `GOLDEN_UPDATE=1 pio test -e native -f test_tx_budget` and read the diff.\n\n"
      "Tokens are the unit, as the provider counts them (owner ruling 82). A head's request aims at about "
      "100,000 tokens once the conversation can fill it, or at a lower limit read from its released v5.1.2 "
      "row, each labelled for what it is: a price tier (a cost bound: 0.9 of the tier, Haiku 90,000; accepted "
      "by ruling 89, put to the owner by ruling 91), a window (what v5.1.2's byte cap of 0.75 x window x 2.5 B "
      "a token carries at the wire's ratio, so under 0.75 of the window on a nominal wire), v5.1.2's modeled "
      "read-window deadline (a fit, not a failing request: glm-5.3 on a device; accepted by ruling 89), the "
      "routed floor while the lift is off. It never aims lower than v5.1.2 did. Token figures on a wire with "
      "no measured ratio are nominal and marked so. Compaction is due once a "
      "request reached the aim less the next request's growth (%s tokens: the largest growth in RB-M0b's "
      "aged replays was %s inside a turn and %s from turn to turn, %s turns), and keeps 70%% of the aim. "
      "Bytes stand beside the tokens only where a device limit is in bytes: the body cap and the stored "
      "marks H and L. They are the token marks on the head's wire: on the Anthropic Messages wire MEASURED "
      "(%s photo-free %s requests: body = %.3f x tokens %s %s B), on every other wire v5.1.2's released "
      "nominal ratio; less the fixed part (%s B) and the digest block with the recent-files list (%s B). No "
      "default carries less than the head's v5.1.2 row (owner ruling 81): at every compaction the owner and "
      "assistant text kept is at least v5.1.2's keep.\n\n",
      num(tx::kNextRequestReserveTokens).c_str(), num(g["in_turn_max"] | 0).c_str(), num(g["turn_to_turn_max"] | 0).c_str(),
      num(g["turns"] | 0).c_str(), num(w["requests"] | 0).c_str(), (const char*)(w["model"] | ""),
      (double)(w["milli_bytes_per_token"] | 0) / 1000, (int)(w["intercept_bytes"] | 0) < 0 ? "-" : "+",
      num(std::fabs((double)(w["intercept_bytes"] | 0))).c_str(), num(tx::kFixedOverheadCeiling).c_str(),
      num(tx::kFullDigestAndFilesBytes).c_str());
}

inline std::string render(size_t arenaNeed) {
  return intro() + headsSection() + costSection() + latencySection() + restartSection(arenaNeed) + profilesSection() +
         everyHeadSection();
}

}  // namespace budgettable
