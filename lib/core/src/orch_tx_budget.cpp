// Engine 2's per-head budget (tx/budget.h): the token aim, the due mark and the keep, read from
// the head's released v5.1.2 row (orch::requestBudgetFor) and the measured wire ratio. Lane RB-T4,
// owner rulings 81 and 82, ruling 83. The generated table is docs/reference/transcript-budgets.md.
#include "nimbus/orch/tx/budget.h"

#include <algorithm>
#include <cctype>

#include "nimbus/orch/budget.h"   // deriveRequestContext - the released history marks (the floors)
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/rules.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace {

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
WireRatio wireOf(const RequestBudget& r, const HeadKey& head) {
  if (!r.routed && lower(head.host) == "anthropic") return {kAnthropicBodyMilliBytesPerToken, kAnthropicBodyInterceptBytes};
  return {(uint32_t)r.rNomX100 * 10u, 0};
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

HeadBudget budgetFromRow(const RequestBudget& r, const HeadKey& head, Profile requested) {
  HeadBudget b;
  const WireRatio w = wireOf(r, head);
  const ContextBudget c = deriveRequestContext(r, {});   // v5.1.2's own marks: the floors
  b.profile = profileOf(profileSlug(requested));
  b.floorHead = r.bound == BudgetBound::Floor || r.bound == BudgetBound::Held;
  b.bodyMilliBytesPerToken = w.milli;
  b.bodyInterceptBytes = w.intercept;
  aimOf(r, b.tokens.aim, b.bound);
  b.bodyCap = r.wireBodyBytes;
  // Where the released bytes stood for the 100,000-token clamp or the price tier at a nominal
  // ratio, the measured ratio sets the bytes that carry the same tokens.
  if (w.intercept != 0 && (b.bound == AimBound::Clamp || b.bound == AimBound::Tier))
    b.bodyCap = std::max(b.bodyCap, roundUp(bytesAt(w, b.tokens.aim), 1000));
  // Elsewhere the released body is the limit (a window or a read window at the dense ratio, the
  // floor): the aim is what it carries at the wire's ratio, and never under v5.1.2's own target.
  b.tokens.aim = std::max(std::min(b.tokens.aim, tokensIn(w, b.bodyCap)), r.targetTokens);
  b.tokens.due = b.tokens.aim - kNextRequestReserveTokens;
  b.tokens.keep = (uint32_t)((uint64_t)b.tokens.aim * kKeepPercent / 100);
  const uint32_t around = kFixedOverheadCeiling + kFullDigestAndFilesBytes;
  const uint32_t foldMark = (uint32_t)std::min(c.foldTriggerBytes, std::max(c.historyMaxBytes, c.historySoftBytes));
  ProfileMarks full;
  full.high = std::max(roundDown(bytesAt(w, b.tokens.due) - std::min(around, bytesAt(w, b.tokens.due)), 1000), foldMark);
  full.low = std::max(roundDown(bytesAt(w, b.tokens.keep) - std::min(around, bytesAt(w, b.tokens.keep)), 1000),
                      (uint32_t)c.historyKeepBytes);
  b.marks = full;
  ProfileMarks chosen;
  if (profileMarks(b.profile, chosen).ok())   // an owner's smaller profile lowers the marks only
    b.marks = {std::min(chosen.high, full.high), std::min(chosen.low, full.low)};
  b.bulkWholeCap = std::max(std::min(kBulkWholeMax, b.bodyCap / 8), (uint32_t)c.toolResultBytes);
  b.warmBulk = b.floorHead ? kWarmBulkBytesFloor : kWarmBulkBytes;
  b.digestBlock = std::max(blockCapFor(b.profile) + kRecentFilesMaxBytes,
                           b.profile == Profile::Full ? (uint32_t)c.chatSummaryBytes + kRecentFilesMaxBytes : 0u);
  b.messageCap = std::max(kInlineMessageCap, (uint32_t)c.historyMessageCap);
  return b;
}

Status budgetFor(const HeadKey& head, Profile requested, BudgetLink link, bool routedLift, HeadBudget& out) {
  if (head.host.empty() || head.model.empty()) return Status::fail(Err::InvalidArgument, "tx::budgetFor");
  out = budgetFromRow(requestBudgetFor(head.host, head.model, head.routed, link, routedLift), head, requested);
  return Status::success();
}

Status budgetFor(const HeadKey& head, Profile requested, HeadBudget& out) {
  if (head.host.empty() || head.model.empty()) return Status::fail(Err::InvalidArgument, "tx::budgetFor");
  out = budgetFromRow(requestBudgetFor(head.host, head.model, head.routed), head, requested);
  return Status::success();
}

uint32_t bodyBytesFor(const HeadBudget& b, uint32_t tokens) {
  return bytesAt(WireRatio{b.bodyMilliBytesPerToken, b.bodyInterceptBytes}, tokens);
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
