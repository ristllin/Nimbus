#include "nimbus/orch/budget.h"

#include <algorithm>
#include <atomic>
#include <cctype>

#include "nimbus/orch/compact.h"       // kChatSummaryMax - what the fold store keeps
#include "nimbus/orch/model_window.h"   // kCtxDefaultTokens - the window table
#include "nimbus/orch/request_budget.h" // requestBudgetFor - the seam's body
#include "nimbus/orch/mem_config.h"  // MemConfig::kContextMin/Max - the owner-override clamp
#include "nimbus/orch/wire_limits.h" // kToolResultCapMax - the per-result ceiling; foldDigestCap

namespace nimbus {
namespace orch {

namespace {

// The 200K-anchor window in bytes: every ratio is <anchor constant> / this, so
// scaling is exact integer math and deriveBudget(200000,{}) is the identity.
constexpr uint64_t kAnchorBytes = 200000ull * kBudgetBytesPerToken;

// value = anchorConst * windowBytes / kAnchorBytes, clamped. Linear + clamp =>
// monotonic in the window size (pinned by test_orch_budget).
size_t scaled(uint64_t anchorConst, uint64_t windowBytes, size_t floor_, size_t cap) {
  uint64_t v = anchorConst * windowBytes / kAnchorBytes;
  if (v < floor_) v = floor_;
  if (v > cap) v = cap;
  return (size_t)v;
}

int clampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

ContextBudget deriveBudget(uint32_t ctxTokens, const BudgetOverrides& ov) {
  ContextBudget b;
  if (ctxTokens == 0) ctxTokens = kCtxDefaultTokens;  // conservative, same as compaction
  b.ctxTokens = ctxTokens;
  const uint64_t wb = (uint64_t)ctxTokens * kBudgetBytesPerToken;

  // Floors keep a small-window model functional; caps repeat the existing NVS
  // clamp ceilings (store.cpp / MemConfig) so derive can never exceed what an
  // owner could set by hand.
  b.systemPromptBytes = (int)scaled(32768, wb, 8192, (size_t)MemConfig::kContextMax);
  b.chatSummaryBytes  = scaled(4096, wb, 2048, 8192);
  // CUM-485: the verbatim recent window is the head's ONLY short-term context on
  // a stateless tool-loop turn (no provider-side thread), so it gets a quarter of
  // the system-prompt budget (8192 of 32768 at the anchor). Floor 3000 = the old
  // shipped total, so no head regresses; cap 16384 bounds the PSRAM block and the
  // prompt on a 1M head. assembleContext still trims the OLDEST rows when the
  // prompt has less room left than this (the 128K head case).
  b.recentConvBytes   = scaled(8192, wb, 3000, 16384);
  // Capped under the provider string guard (wire_limits.h): a clamped result plus
  // its truncation note and an "ERROR: " prefix is still one whole string.
  b.toolResultBytes   = scaled(8192, wb, 512, kToolResultCapMax);
  b.toolTotalBytes    = scaled(65536, wb, 8192, 1048576);
  b.briefBytes        = scaled(1200, wb, 600, 4096);
  b.foldSliceBytes    = scaled(65536, wb, 16384, 131072);

  // Owner overrides win verbatim, then clamp to the same ranges the NVS
  // setters enforce - web/NVS and derive can never disagree about the range.
  if (ov.maxContextBytes > 0)
    b.systemPromptBytes = clampI(ov.maxContextBytes, MemConfig::kContextMin, MemConfig::kContextMax);
  if (ov.toolResultCap > 0)
    b.toolResultBytes = (size_t)clampI(ov.toolResultCap, 512, (int)kToolResultCapMax);
  if (ov.toolTotalCap > 0)
    b.toolTotalBytes = (size_t)clampI(ov.toolTotalCap, 2048, 1048576);

  // The history fix's fields at today's figures (lane P0): the window is the recent
  // window's high water and the shift and the fold both stop at half of it
  // (conversation.h HistoryBudget::lowBytes), one fold takes the fold slice, the replay
  // cap is the smallest any wire keeps (Anthropic's 16 KB), and recall has no bound of
  // its own beyond the system budget it shares.
  b.historyMaxBytes = b.recentConvBytes;
  b.historySoftBytes = b.recentConvBytes / 2;
  b.historyKeepBytes = b.recentConvBytes / 2;
  b.foldStepBytes = b.foldSliceBytes;
  b.recallBytes = (size_t)b.systemPromptBytes;
  return b;
}

ContextBudget headContextBudget(const std::string& host, const std::string& model, bool loopOn,
                                const BudgetOverrides& ov) {
  // Lane H's switch-over: every consumer reads the head's request budget (a Cumulo head is
  // budgeted as its upstream, routed: the budget reads the selector). Lane L: in the form
  // the head's turn goes out in.
  return deriveRequestContext(turnBudgetFor(host, model, loopOn), ov);
}

namespace {

// The protocol's KB (plan v3: its figures reproduce at 1,024 B).
constexpr size_t kKB = 1024;
// What a request carries besides the history (plan v3 protocols): the tools array, the
// static system block, the per-turn block and a slack.
constexpr size_t kToolsBytes = 34 * kKB;
constexpr size_t kSystemNominalBytes = 24 * kKB;
constexpr size_t kPerTurnBytes = 8 * kKB;
constexpr size_t kSlackBytes = 8 * kKB;
// A head at or above this target gets the larger summary and message clip.
constexpr uint32_t kLargeTargetTokens = 60000;

size_t pct(uint64_t bytes, unsigned p, size_t lo, size_t hi) {
  const uint64_t v = bytes * p / 100;
  return (size_t)(v < lo ? lo : v > hi ? hi : v);
}

// The history marks of a head with room (not the floor branch).
void deriveHistory(ContextBudget& b, size_t historyMax) {
  b.historyMaxBytes = historyMax;
  b.recentConvBytes = historyMax;
  b.historySoftBytes = historyMax * 4 / 5;
  const size_t digest = foldDigestCap(b.chatSummaryBytes);
  const size_t byDigest = b.historySoftBytes > digest ? b.historySoftBytes - digest : 0;
  b.historyKeepBytes = std::max(historyMax / 2, byDigest);
  b.foldStepBytes = b.historySoftBytes - b.historyKeepBytes;
  b.historyRows = std::max<size_t>(64, historyMax / 192);
  b.foldTriggerBytes = b.historySoftBytes;
  b.floorBranch = false;
}

// Lane RB-BG: the history profile caps a direct head's marks. The fold is due at
// min(soft, H) and keeps min(keep, L) (never less than one fold's digest can take), and the
// window's high water is 5/4 of the due mark. A head with smaller marks keeps its own. The
// due mark never goes under kFoldTriggerMinBytes (RULINGS-v3 item 3): tight's 24,000 B reads
// as 24,576 B.
void capToProfile(ContextBudget& b, HistoryProfile p) {
  const HistoryProfileMarks m = historyProfileMarks(p);
  const size_t soft = std::max<size_t>(m.high, kFoldTriggerMinBytes);
  if (b.historySoftBytes <= soft) return;
  const size_t digest = foldDigestCap(b.chatSummaryBytes);
  const size_t keep = std::max<size_t>(std::min<size_t>(b.historyKeepBytes, m.low), soft > digest ? soft - digest : 0);
  const size_t max = std::min<size_t>(b.historyMaxBytes, soft + soft / 4);
  b.historyMaxBytes = max;
  b.recentConvBytes = max;
  b.historySoftBytes = soft;
  b.historyKeepBytes = keep;
  b.foldStepBytes = soft - keep;
  b.historyRows = std::max<size_t>(64, max / 192);
  b.foldTriggerBytes = soft;
}

// Lane RB-BG: a split-prompt head at the floor. What the body leaves after the measured fixed
// overhead and the summary at its cap, in window bytes (the window counts rendered turn lines,
// the wire adds each message's framing). 0 when nothing is left.
size_t floorRoomWindowBytes(const RequestBudget& rb, size_t summaryBytes) {
  // The sending wire: Anthropic's v5.1.1 replay cap is 16,384 B, every other wire's 24,576 B.
  const bool anthropic = rb.v511ReplayBytes == 16384;
  const size_t fixed = (anthropic ? kFloorFixedAnthropicBytes : kFloorFixedOpenAIBytes) + summaryBytes +
                       kFloorMarginBytes;
  if (rb.wireBodyBytes <= fixed) return 0;
  const unsigned pct = anthropic ? kFloorWirePerWindowAnthropicPct : kFloorWirePerWindowOpenAIPct;
  return (size_t)((uint64_t)(rb.wireBodyBytes - fixed) * 100 / pct);
}

// The marks of a measured floor head: the fold-before-shift window up to `room`, a shift
// landing at 9/10 of it (a fold is then due, for the rows it hid), the fold keeping 4/5, and
// v5.1.1's fold trigger (`v511Trigger`, never under kFoldTriggerMinBytes). A floor head's
// conversation then stays between 4/5 and all of the room: v5.1.1's window fell to half of a
// smaller one at every shift (the 17 Nov device carried one exchange of 516 B). One fold runs
// for about a fifth of the room of new conversation (v5.1.1: half of its 8 KB window).
void deriveFloorHistory(ContextBudget& b, size_t room, size_t v511Trigger) {
  b.historyMaxBytes = room;
  b.recentConvBytes = room;
  b.historySoftBytes = room * 9 / 10;
  b.historyKeepBytes = room * 4 / 5;
  b.foldStepBytes = b.historySoftBytes - b.historyKeepBytes;
  b.historyRows = std::max<size_t>(64, room / 192);
  b.foldTriggerBytes = v511Trigger;
  b.floorBranch = false;
  b.floorMeasured = true;
}

std::atomic<uint8_t>& profileSlot() {
  static std::atomic<uint8_t> slot{(uint8_t)kDefaultHistoryProfile};
  return slot;
}

}  // namespace

HistoryProfileMarks historyProfileMarks(HistoryProfile p) {
  switch (p) {
    case HistoryProfile::Tight: return {24000, 12000};
    case HistoryProfile::Lean: return {40000, 20000};
    case HistoryProfile::Wide: return {100000, 50000};
    case HistoryProfile::Standard: break;
  }
  return {60000, 30000};
}

const char* historyProfileSlug(HistoryProfile p) {
  switch (p) {
    case HistoryProfile::Tight: return "tight";
    case HistoryProfile::Lean: return "lean";
    case HistoryProfile::Wide: return "wide";
    case HistoryProfile::Standard: break;
  }
  return "standard";
}

bool parseHistoryProfile(const std::string& word, HistoryProfile& out) {
  std::string w;
  for (char c : word) w += (char)std::tolower((unsigned char)c);
  for (HistoryProfile p : {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard, HistoryProfile::Wide})
    if (w == historyProfileSlug(p)) {
      out = p;
      return true;
    }
  return false;
}

void installHistoryProfile(HistoryProfile p) { profileSlot().store((uint8_t)p); }
HistoryProfile installedHistoryProfile() {
  const uint8_t v = profileSlot().load();
  return v <= (uint8_t)HistoryProfile::Wide ? (HistoryProfile)v : kDefaultHistoryProfile;
}

ContextBudget deriveRequestContext(const RequestBudget& rb, const BudgetOverrides& ov) {
  const ContextBudget old = deriveBudget(rb.v511CtxTokens, ov);   // v5.1.1, owner overrides applied
  ContextBudget b = old;
  b.ctxTokens = rb.windowTokens;
  b.v511KeepBytes = old.recentConvBytes / 2;   // candidate 4, F5: the fold after a send-floor cut
  // Lane L: a flat turn is sized from the flat prompt budget, which leaves the formula no
  // room for history (the floor branch below: v5.1.1's window) and is never a large target.
  const uint64_t body = rb.contextBytes();
  const bool large = !rb.flatTurn && rb.targetTokens >= kLargeTargetTokens;
  // Never more than the fold store keeps (lane CTXR1: a gpt-6 head was told and capped at
  // v5.1.1's 8,192 B, then stored at kChatSummaryMax, so every fold lost its tail lines).
  b.chatSummaryBytes = std::min<size_t>(kChatSummaryMax, std::max<size_t>(old.chatSummaryBytes, large ? 6144 : 4096));
  if (ov.maxContextBytes <= 0)
    b.systemPromptBytes = std::max(old.systemPromptBytes, (int)pct(body, 12, 32768, 56000));
  if (ov.toolResultCap <= 0) b.toolResultBytes = std::max(old.toolResultBytes, pct(body, 3, 8192, 16384));
  b.recallBytes = std::max(old.recallBytes, pct(body, 3, 4096, 12288));
  b.replayBytes = std::max({old.replayBytes, (size_t)rb.v511ReplayBytes, pct(body, 15, 16384, 65536)});
  b.perTurnFloorBytes = std::max(old.perTurnFloorBytes, kPerTurnBytes);
  b.historyMessageCap = std::max<size_t>(old.historyMessageCap, large ? 12288 : 6144);
  const int64_t fixed = (int64_t)(kToolsBytes + kSystemNominalBytes + b.chatSummaryBytes + kPerTurnBytes + kSlackBytes);
  const int64_t historyMax = (int64_t)body - fixed - (int64_t)b.replayBytes - (int64_t)(body / 20);
  // The floor branch (RULINGS-v3 item 3): no more room than v5.1.1's window, or a trigger
  // under kFoldTriggerMinBytes - the history stays exactly v5.1.1's (old's fields).
  const bool room = historyMax > (int64_t)old.historyMaxBytes && historyMax * 4 / 5 >= (int64_t)kFoldTriggerMinBytes;
  if (room) {
    deriveHistory(b, (size_t)historyMax);
    if (!ov.noProfile) capToProfile(b, installedHistoryProfile());
  } else if (!rb.flatTurn && !rb.flat && rb.wireBodyBytes <= kLegacyWireBodyBytes) {
    // Lane RB-BG: a split-prompt turn at the floor sizes its history from the measured overhead
    // when that gives more than v5.1.1's window; otherwise the floor branch stands.
    const size_t floorRoom = floorRoomWindowBytes(rb, b.chatSummaryBytes);
    if (floorRoom > old.historyMaxBytes) deriveFloorHistory(b, floorRoom, old.foldTriggerBytes);
  }
  b.foldSliceBytes = std::max(old.foldSliceBytes, b.foldStepBytes);
  return b;
}

size_t foldCycleBytes(const ContextBudget& b) {
  return std::min(b.foldSliceBytes, foldDigestCap(b.chatSummaryBytes));
}

size_t foldTriggerBytes(const ContextBudget& b, uint32_t compactAtKB) {
  if (compactAtKB == 0) return 0;
  if (b.floorBranch || b.floorMeasured) return (size_t)compactAtKB * kKB;
  if (compactAtKB == kCompactAtKbDefault) return b.historySoftBytes;
  const size_t lo = 8 * kKB;
  const size_t hi = b.historyMaxBytes > 2 * lo ? b.historyMaxBytes - lo : lo;
  return std::min(std::max((size_t)compactAtKB * kKB, lo), hi);
}

}  // namespace orch
}  // namespace nimbus
