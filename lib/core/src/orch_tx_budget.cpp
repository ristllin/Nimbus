// Engine 2's per-head budget (tx/budget.h): the token aim, the due mark and the keep, read from
// the head's released v5.1.2 row (orch::requestBudgetFor) and the measured wire ratio. Lane RB-T4,
// owner rulings 81 and 82, ruling 83. The generated table is docs/reference/transcript-budgets.md.
#include "nimbus/orch/tx/budget.h"

#include <algorithm>
#include <cctype>

#include "nimbus/orch/budget.h"   // deriveRequestContext - the released history marks (the floors)
#include "nimbus/orch/router_route.h"   // resolveRouterRoute - a routed head's upstream wire
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/rules.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// Engine 2 always runs the tool loop: a head's turn goes out split (the history as messages), so its
// released row is turnBudgetFor's with the loop on (the one seam, test_orch_budget).
constexpr bool kToolLoopOn = true;
constexpr Profile kAll[] = {Profile::Tight, Profile::Lean, Profile::Standard, Profile::Wide, Profile::Full};
constexpr ProfileMarks kFixed[] = {{24000, 12000}, {40000, 20000}, {60000, 30000}, {100000, 50000}};

std::string lower(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

// The wire's body bytes per token: measured on the Anthropic Messages wire of a direct head, the
// released nominal ratio everywhere else.
struct WireRatio {
  uint32_t milli = 0;
  int32_t intercept = 0;
};
// The wire a head's request goes out on: a routed head's is its upstream's (the router relays the
// native body, orch_router_route.cpp headRunsToolLoop).
std::string upstreamOf(const HeadKey& head) {
  const std::string h = lower(head.host);
  return h == "cumulo" ? resolveRouterRoute(lower(head.model)).upstream : h;
}

// The measured ratio applies where the body may rise to carry the aim (a clamp or tier bound) on the
// Anthropic Messages wire, direct or routed; everywhere else the released nominal ratio, as v5.1.2
// sized that row (and the table marks those token figures nominal).
WireRatio wireOf(const RequestBudget& r, const HeadKey& head, AimBound bound) {
  if (upstreamOf(head) == "anthropic" && (bound == AimBound::Clamp || bound == AimBound::Tier))
    return {kAnthropicBodyMilliBytesPerToken, kAnthropicBodyInterceptBytes};
  return {(uint32_t)r.rNomX100 * 10u, 0};
}

// Routed means what the key says (lane RB-T4b): a routed head named by its upstream's slug is the
// router's head "<upstream>/<model>".
HeadKey routedKey(const HeadKey& head) {
  if (!head.routed || lower(head.host) == "cumulo") return head;
  return HeadKey{"cumulo", lower(head.host) + "/" + head.model, true};
}

uint32_t bytesAt(const WireRatio& w, uint32_t tokens) {
  const int64_t b = (int64_t)tokens * w.milli / 1000 + w.intercept;
  return b > 0 ? (uint32_t)b : 0;
}

// The tokens a body of `bytes` carries on the wire.
uint32_t tokensIn(const WireRatio& w, uint32_t bytes) {
  const int64_t t = ((int64_t)bytes - w.intercept) * 1000 / w.milli;
  return t > 0 ? (uint32_t)std::min<int64_t>(t, UINT32_MAX) : 0;
}

uint32_t roundUp(uint32_t v, uint32_t step) { return (v + step - 1) / step * step; }
uint32_t roundDown(uint32_t v, uint32_t step) { return v / step * step; }

// The aim in tokens: the head's own limit, read from the bound of its released row.
void aimOf(const RequestBudget& r, uint32_t& aim, AimBound& bound) {
  switch (r.bound) {
    case BudgetBound::Clamp: bound = AimBound::Clamp, aim = kAimClampTokens; break;
    case BudgetBound::Tier: bound = AimBound::Tier, aim = (uint32_t)((uint64_t)r.tierTokens * 9 / 10); break;
    case BudgetBound::Window: bound = AimBound::Window, aim = (uint32_t)((uint64_t)r.windowTokens * 3 / 4); break;
    case BudgetBound::Latency: bound = AimBound::Latency, aim = r.latencyTokens; break;
    case BudgetBound::Floor:
    case BudgetBound::Held: bound = AimBound::Floor, aim = r.targetTokens; break;
  }
  aim = std::max(std::min(aim, kAimClampTokens), r.targetTokens);   // never lower than v5.1.2 aimed
}

}  // namespace

Profile profileOf(const std::string& slug) {
  for (Profile p : kAll)
    if (slug == profileSlug(p)) return p;
  return kDefaultProfile;
}

const char* aimBoundSlug(AimBound b) {
  switch (b) {
    case AimBound::Clamp: return "clamp";
    case AimBound::Tier: return "tier";
    case AimBound::Window: return "window";
    case AimBound::Latency: return "latency";
    case AimBound::Floor: return "floor";
  }
  return "clamp";
}

Status profileMarks(Profile p, ProfileMarks& out) {
  if ((size_t)p >= sizeof(kFixed) / sizeof(kFixed[0])) return Status::fail(Err::InvalidArgument, "tx::profileMarks");
  out = kFixed[(size_t)p];
  return Status::success();
}

EngineSettings clampSettings(const EngineSettings& s) {
  auto in = [](uint32_t v, uint32_t def, uint32_t lo, uint32_t hi) { return v == 0 ? def : std::min(std::max(v, lo), hi); };
  EngineSettings o;
  o.recentTokens = in(s.recentTokens, kRecentTokensDefault, kRecentTokensMin, kRecentTokensMax);
  o.summaryTokens = in(s.summaryTokens, kSummaryTokensDefault, kSummaryTokensMin, kSummaryTokensMax);
  o.maxRounds = (uint16_t)in(s.maxRounds, kMaxRoundsDefault, kMaxRoundsMin, kMaxRoundsMax);
  o.hardTimeS = in(s.hardTimeS, kHardTimeDefaultS, kHardTimeMinS, kHardTimeMaxS);
  return o;
}

namespace {
// The owner's summary and recent part as this head carries them: together at most half of what its due
// mark leaves beside the fixed part, so a compaction leaves room for turns before the next one; the
// summary yields first (down to kSummaryTokensMin), then the recent part (down to kRecentTokensFloor).
void keepOf(const EngineSettings& set, const WireRatio& w, HeadBudget& b) {
  const EngineSettings s = clampSettings(set);
  // The fixed part's ceiling, at most half the due mark (a tiny window cannot carry the whole ceiling).
  b.fixedTokens = std::min((uint32_t)((uint64_t)(kFixedOverheadCeiling + kRecentFilesMaxBytes) * 1000 / w.milli),
                           b.tokens.due / 2);
  const uint32_t room = (b.tokens.due - b.fixedTokens) / 2;
  uint32_t summary = s.summaryTokens, recent = s.recentTokens;
  if (room < kSummaryTokensMin + kRecentTokensFloor) {   // a tiny window: a third for the summary
    summary = room / 3;
    recent = room - summary;
  } else if (summary + recent > room)
    summary = std::max(std::min(summary, room > recent ? room - recent : 0u), std::min(summary, kSummaryTokensMin));
  if (summary + recent > room) recent = std::max(room > summary ? room - summary : 0u, kRecentTokensFloor);
  b.summaryTokens = summary;
  b.recentTokens = recent;
  b.summaryBytes = (uint32_t)((uint64_t)summary * w.milli / 1000);
  b.tokens.keep = b.fixedTokens + summary + recent;
}
}  // namespace

HeadBudget budgetFromRow(const RequestBudget& r, const HeadKey& head, Profile requested, const EngineSettings& settings) {
  HeadBudget b;
  aimOf(r, b.tokens.aim, b.bound);
  const WireRatio w = wireOf(r, head, b.bound);
  BudgetOverrides released;
  released.noProfile = true;   // the head's own v5.1.2 marks, whatever engine 1's history profile is
  const ContextBudget c = deriveRequestContext(r, released);   // the floors
  b.profile = (uint8_t)requested <= (uint8_t)Profile::Full ? requested : kDefaultProfile;
  b.floorHead = r.bound == BudgetBound::Floor || r.bound == BudgetBound::Held;
  b.bodyMilliBytesPerToken = w.milli;
  b.bodyInterceptBytes = w.intercept;
  b.bodyCap = r.wireBodyBytes;
  // Where the released bytes stood for the 100,000-token clamp or the price tier at a nominal
  // ratio, the measured ratio sets the bytes that carry the same tokens.
  if (w.intercept != 0 && (b.bound == AimBound::Clamp || b.bound == AimBound::Tier))
    b.bodyCap = std::max(b.bodyCap, roundUp(bytesAt(w, b.tokens.aim), 1000));
  // Elsewhere the released body is the limit (a window or a read window at the dense ratio, the
  // floor): the aim is what it carries at the wire's ratio, and never under v5.1.2's own target.
  b.tokens.aim = std::max(std::min(b.tokens.aim, tokensIn(w, b.bodyCap)), r.targetTokens);
  b.tokens.due = b.tokens.aim - nextRequestReserve(b.tokens.aim);
  const uint32_t around = kFixedOverheadCeiling + kFullDigestAndFilesBytes;
  // v5.1.2's fold mark: its trigger, which a floor row counts in raw bytes since the last fold (49,152 B).
  // Where that trigger does not fit beside the fixed part and the digest block of the head's own body (a
  // tiny window), what the released window carried stands in for it.
  const uint32_t foldMark = c.foldTriggerBytes + around <= b.bodyCap
                                ? (uint32_t)c.foldTriggerBytes
                                : (uint32_t)std::max(c.historyMaxBytes, c.historySoftBytes);
  ProfileMarks full;
  full.high = std::max(roundDown(bytesAt(w, b.tokens.due) - std::min(around, bytesAt(w, b.tokens.due)), 1000), foldMark);
  // Where a released floor lifted H above the token due (a floor row's 49,152 B trigger), the token due
  // rises with it, never past the aim, so the two triggers agree (ruling 91, table/F8).
  b.tokens.due = std::max(b.tokens.due, std::min(b.tokens.aim - 1, tokensIn(w, full.high + around)));
  // The keep (rulings 105 and 106): the summary and the owner's recent part; L is the recent part in the
  // bytes a request renders (the wire's slope, no intercept: a part of a request, not a body). The
  // v5.1.2 text keep no longer binds the default (the owner's choice); compaction holds it only for an
  // owner's recent part that reaches it (plain_compact.cpp rulesFor).
  keepOf(settings, w, b);
  full.low = std::min((uint32_t)((uint64_t)b.recentTokens * w.milli / 1000), full.high - 1);
  b.marks = full;
  b.releasedFold = foldMark;
  b.releasedKeep = (uint32_t)c.historyKeepBytes;
  ProfileMarks chosen;
  if (profileMarks(b.profile, chosen).ok())   // an owner's smaller profile lowers the marks only
    b.marks = {std::min(chosen.high, full.high), std::min(chosen.low, full.low)};
  b.bulkWholeCap = std::max(std::min(kBulkWholeMax, b.bodyCap / 8), (uint32_t)c.toolResultBytes);
  b.messageCap = std::max(kInlineMessageCap, (uint32_t)c.historyMessageCap);
  return b;
}

Status budgetFor(const HeadKey& head, Profile requested, BudgetLink link, bool routedLift,
                 const EngineSettings& settings, HeadBudget& out) {
  if (head.host.empty() || head.model.empty()) return Status::fail(Err::InvalidArgument, "tx::budgetFor");
  const HeadKey key = routedKey(head);
  out = budgetFromRow(turnBudgetFor(key.host, key.model, kToolLoopOn, link, routedLift), key, requested, settings);
  return Status::success();
}

Status budgetFor(const HeadKey& head, Profile requested, const EngineSettings& settings, HeadBudget& out) {
  if (head.host.empty() || head.model.empty()) return Status::fail(Err::InvalidArgument, "tx::budgetFor");
  const HeadKey key = routedKey(head);
  out = budgetFromRow(turnBudgetFor(key.host, key.model, kToolLoopOn), key, requested, settings);
  return Status::success();
}

Status budgetFor(const HeadKey& head, Profile requested, BudgetLink link, bool routedLift, HeadBudget& out) {
  return budgetFor(head, requested, link, routedLift, EngineSettings{}, out);
}

Status budgetFor(const HeadKey& head, Profile requested, HeadBudget& out) {
  return budgetFor(head, requested, EngineSettings{}, out);
}

uint32_t arenaBytesFor(const HeadBudget& b) {
  const uint64_t growth = bodyBytesFor(b, kNextRequestReserveTokens + 1000) - bodyBytesFor(b, 1000);
  const uint64_t marks = ((uint64_t)b.marks.high + growth) * kArenaMilliBytesPerResidentByte / 1000;
  const uint64_t text = (uint64_t)b.releasedKeep * kArenaMilliBytesPerTextByte / 1000;
  const uint64_t need = std::max(marks, text) + kTurnOpenArenaReserve;
  return (uint32_t)((need + kArenaStep - 1) / kArenaStep * kArenaStep);
}

uint32_t bodyBytesFor(const HeadBudget& b, uint32_t tokens) {
  return bytesAt(WireRatio{b.bodyMilliBytesPerToken, b.bodyInterceptBytes}, tokens);
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
