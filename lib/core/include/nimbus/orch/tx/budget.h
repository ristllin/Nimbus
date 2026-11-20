#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/request_budget.h"   // BudgetLink, RequestBudget - the released v5.1.2 row
#include "nimbus/orch/tx/status.h"

// tx/budget.h - profiles and the per-head budget of engine 2 (PRD section 2, "Budget", as
// amended by owner rulings 81 and 82 and ruling 83).
//
// Tokens are the unit (ruling 82 item 3). A head's request AIMS at about 100,000 tokens once the
// conversation can fill it, or at the head's own lower limit (the provider's window, its price
// tier, v5.1.2's read-window fit, the routed floor while the lift is off). Compaction is DUE when
// the next request would pass the aim, and KEEPS the summary and the owner's recent part (rulings
// 105 and 106: default 4,000 and 10,000 tokens). Bytes appear only where a device
// limit is in bytes: the body cap, and the stored marks H and L the transcript is measured in.
//
// The default profile is `full` everywhere (rulings 81 and 83 item 2). The smaller profiles exist
// only as the owner's pinned choice, never as a fallback: an empty or unknown slug, a missing
// table or a head nobody measured resolves to full. No head's default carries less than its
// released v5.1.2 row (orch::turnBudgetFor, test/support/fixtures/budgets_v5_1_2.json).
//
// Seam declared by lane RB-B0a; the body (orch_tx_budget.cpp) and the generated table
// (docs/reference/transcript-budgets.md, test_tx_budget) are lane RB-T4's.
namespace nimbus {
namespace orch {
namespace tx {

// The slugs are stored in head.json and t items: frozen. Full has the same slug and value as
// engine 1's HistoryProfile::Full.
enum class Profile : uint8_t { Tight = 0, Lean = 1, Standard = 2, Wide = 3, Full = 4 };
inline constexpr Profile kDefaultProfile = Profile::Full;
inline const char* profileSlug(Profile p) {
  switch (p) {
    case Profile::Tight: return "tight";
    case Profile::Lean: return "lean";
    case Profile::Standard: return "standard";
    case Profile::Wide: return "wide";
    case Profile::Full: return "full";
  }
  return "full";
}
// The profile a stored slug names; anything else (empty, unknown, damaged) is the default, full.
Profile profileOf(const std::string& slug);

inline constexpr uint32_t kFixedOverheadCeiling = 18432;   // system 8,192 + tools 10,240
inline constexpr uint32_t kSystemCeiling = 8192;
inline constexpr uint32_t kToolsCeiling = 10240;
// The body of a floor head (routed while the lift is off, custom, a v5.1.2 floor row). Only a head
// whose released row is at this figure carries it; it is never a fallback.
inline constexpr uint32_t kFloorHeadBodyCap = 85000;
inline constexpr uint32_t kBulkWholeMax = 48000;           // tier 2 shown whole in its own turn

// ---- the token aim (ruling 82 item 4), and what it is derived from -----------------------------
inline constexpr uint32_t kAimClampTokens = 100000;        // "around 100k tokens" (owner, 19 Nov)
// The room under the aim a tiny window's due mark keeps (nextRequestReserve). Before rulings 105 and 106
// it was also the keep; the keep is now the owner's recent part and the summary (EngineSettings).
inline constexpr uint32_t kKeepPercent = 70;
// The next request's growth past the last one: RB-M0b's aged replays (15 to 17 Nov, 30 Oct and
// 4 Nov shapes, claude-sonnet-5-5, 244 turns) grew at most 3,774 tokens inside a turn and 3,414
// from one turn's first request to the next; rounded up to 1,000.
inline constexpr uint32_t kNextRequestReserveTokens = 4000;
// The reserve a head's due mark keeps under its aim: kNextRequestReserveTokens, or on a head whose
// aim is too small for it (a tiny window) half the room between the keep and the aim, so the due
// mark always sits between them.
inline constexpr uint32_t nextRequestReserve(uint32_t aim) {
  const uint32_t half = (aim - aim * kKeepPercent / 100) / 2;
  return half < kNextRequestReserveTokens ? half : kNextRequestReserveTokens;
}
// The Anthropic Messages wire's body bytes per prompt token, MEASURED (RB-M0b's replays, 396
// photo-free claude-sonnet-5-5 requests, least squares): body = 2.993 x tokens - 7,358 B. Every
// other wire uses v5.1.2's released nominal ratio (RequestBudget::rNomX100): not measured.
inline constexpr uint32_t kAnthropicBodyMilliBytesPerToken = 2993;
inline constexpr int32_t kAnthropicBodyInterceptBytes = -7358;
// The digest block at full (digest.h kDigestBlockStandard) plus the recent-files list
// (kRecentFilesMaxBytes): at least every head's v5.1.2 summary cap (6,144 / 5,570 / 4,096 B).
inline constexpr uint32_t kFullDigestBlockBytes = 6144;
inline constexpr uint32_t kFullDigestAndFilesBytes = kFullDigestBlockBytes + 600;
// Tier 1 text kept inline (item.h kTier1OwnerText / kTier1AssistantText).
inline constexpr uint32_t kInlineMessageCap = 12288;

// The arena a chat needs at a head's budget (ruling 91, T4-ADV-02; ruling 98 item 2). Measured in
// test_tx_budget on real engine turns, after RB-H81: every stored item (t, u, calls, intents, results,
// a, d and a fully populated e) takes arena; the marks count what a request renders. Two needs:
// - no full-arena drop under the due mark H plus one request's growth, whatever the mix: arena used per
//   rendered resident byte is at most 3.6726 (effect calls), 2.8000 (very short turns), 2.7735 (terse
//   tool rounds), 1.4345 (plain chat), 1.4140 (tool heavy); the worst plus 5% here;
// - the arena alone never holds less owner and assistant text than the head's v5.1.2 keep in a mix whose
//   text is at least kArenaTextShareFloorPercent of what renders: the tool-heavy mix (a 2 KB result a
//   turn, text 9.6%) takes 14.65 B of arena per text byte; plus 5% here. Under that share (terse tool
//   rounds 1.3%, effect calls 3.2%) the arena, not the marks, bounds the text kept, and compaction then
//   folds with a summary and a log line (RB-H81's arena-room trigger), never a silent drop.
inline constexpr uint32_t kArenaMilliBytesPerResidentByte = 3860;
inline constexpr uint32_t kArenaMilliBytesPerTextByte = 15390;
inline constexpr uint32_t kArenaTextShareFloorPercent = 9;
inline constexpr uint32_t kTurnOpenArenaReserve = 32 * 1024;   // plain_engine.cpp kTurnArenaReserve
inline constexpr uint32_t kArenaStep = 64 * 1024;

// ---- the owner's settings of engine 2 (rulings 105 and 106; lane RB-C3) ------------------------
// After a compaction the request is the system prompt, one cumulative summary of at most
// summaryTokens and the newest whole turns up to recentTokens (the recent part). Max rounds and the
// hard time limit bound an attended turn (plain::ownerCeiling turns them into TurnSetup::ceiling).
// Per device: the binding stores them (the device under the NVS keys, the hosted instance and the web
// page under the field names below), passes them through clampSettings, and gives them to the engine
// (budgetFor(..., settings, ...), plain::CompactDeps::settings). Unset reads as the defaults.
inline constexpr uint32_t kRecentTokensDefault = 10000, kRecentTokensMin = 2000, kRecentTokensMax = 80000;
inline constexpr uint32_t kSummaryTokensDefault = 4000, kSummaryTokensMin = 1000, kSummaryTokensMax = 8000;
inline constexpr uint16_t kMaxRoundsDefault = 64, kMaxRoundsMin = 8, kMaxRoundsMax = 64;
inline constexpr uint32_t kHardTimeDefaultS = 1200, kHardTimeMinS = 120, kHardTimeMaxS = 1200;
// The web and hosted field names, and the device's NVS keys (frozen once shipped).
inline constexpr char kSettingRecentTokens[] = "e2_recent_tokens";
inline constexpr char kSettingSummaryTokens[] = "e2_summary_tokens";
inline constexpr char kSettingMaxRounds[] = "e2_max_rounds";
inline constexpr char kSettingHardTimeS[] = "e2_hard_time_s";
inline constexpr char kNvsRecentTokens[] = "e2RecentTok";
inline constexpr char kNvsSummaryTokens[] = "e2SummaryTok";
inline constexpr char kNvsMaxRounds[] = "e2MaxRounds";
inline constexpr char kNvsHardTimeS[] = "e2HardTimeS";
// The least recent part a head keeps when its due mark is too small for the owner's figure.
inline constexpr uint32_t kRecentTokensFloor = 1000;

struct EngineSettings {
  uint32_t recentTokens = kRecentTokensDefault;
  uint32_t summaryTokens = kSummaryTokensDefault;
  uint16_t maxRounds = kMaxRoundsDefault;
  uint32_t hardTimeS = kHardTimeDefaultS;
};
// Each figure inside its bounds; 0 (unset) reads as the default.
EngineSettings clampSettings(const EngineSettings& s);

struct ProfileMarks {
  uint32_t high = 0;   // H: compaction is due above it (stored bytes since thru)
  uint32_t low = 0;    // L: compaction keeps this much
};
struct TokenMarks {
  uint32_t aim = 0;    // the request this head aims at once the conversation fills it
  uint32_t due = 0;    // compaction is due once a request reached this (aim less the next request's growth)
  uint32_t keep = 0;   // a request just after a compaction: the fixed part, the summary and the recent part
};

// Which head a budget is for. Qualification never sizes the window (ruling 83 item 6): it is not
// part of the key.
struct HeadKey {
  std::string host;     // provider slug ("anthropic", "openai", "zai", "mistral", "cumulo", "custom")
  std::string model;
  bool routed = false;  // through the Cumulo router: the router is host "cumulo" (turnBudgetFor reads its upstream)
};

// Why the aim sits where it does (the released row's bound, read in tokens).
enum class AimBound : uint8_t { Clamp = 0, Tier, Window, Latency, Floor };
const char* aimBoundSlug(AimBound b);   // "clamp" | "tier" | "window" | "latency" | "floor"

struct HeadBudget {
  uint32_t bodyCap = 0;          // whole request body ceiling, bytes (a device limit)
  Profile profile = kDefaultProfile;
  ProfileMarks marks;            // stored bytes, at `profile`
  TokenMarks tokens;             // at full; an owner's smaller profile lowers `marks`, not the aim
  AimBound bound = AimBound::Clamp;
  bool floorHead = false;        // the class: routed with the lift off, custom, a v5.1.2 floor row
  uint32_t bulkWholeCap = 0;     // a long call argument whole in its open turn (the engine reads it: RB-H81)
  uint32_t messageCap = 0;       // owner and assistant text kept inline, bytes (RB-H81 wires it: ruling 91 item 1)
  uint32_t releasedFold = 0;     // v5.1.2's fold mark for this head (owner and assistant text)
  uint32_t releasedKeep = 0;     // v5.1.2's keep: a compaction leaves at least this much text
  uint32_t bodyMilliBytesPerToken = 0;   // the wire ratio the byte marks were derived with (x1000)
  int32_t bodyInterceptBytes = 0;        // and its intercept (0 on a released nominal ratio)
  // The owner's settings as this head carries them (rulings 105 and 106): summary + recent part at most
  // half of what the due mark leaves beside the fixed part, the summary yielding first, never under
  // kSummaryTokensMin and kRecentTokensFloor. L (marks.low) is the recent part in bytes on the wire.
  uint32_t fixedTokens = 0;              // the fixed part's ceiling and the recent-files list, in tokens
  uint32_t summaryTokens = 0;
  uint32_t summaryBytes = 0;             // the summary's budget in bytes on the wire (fitSummary's cap)
  uint32_t recentTokens = 0;
};

// The fixed marks of an owner-chosen smaller profile (tight 24,000/12,000 ... wide 100,000/50,000).
// Full has no fixed marks (it is per head): InvalidArgument; use budgetFor.
Status profileMarks(Profile p, ProfileMarks& out);
// The budget of one head at a requested profile (the installed link and routed lift). A smaller
// profile caps the stored marks at its fixed figures and at the head's full marks; everything else
// (body, aim, digest block, caps) is the head's own.
Status budgetFor(const HeadKey& head, Profile requested, HeadBudget& out);
// The same on an explicit link and routed lift (the generated table and tests).
Status budgetFor(const HeadKey& head, Profile requested, BudgetLink link, bool routedLift, HeadBudget& out);
// The same with the owner's settings (the two above use the defaults).
Status budgetFor(const HeadKey& head, Profile requested, const EngineSettings& settings, HeadBudget& out);
Status budgetFor(const HeadKey& head, Profile requested, BudgetLink link, bool routedLift,
                 const EngineSettings& settings, HeadBudget& out);
// The budget from a released row already in hand (pure; the ones above read orch::turnBudgetFor with
// the tool loop on).
HeadBudget budgetFromRow(const RequestBudget& row, const HeadKey& head, Profile requested,
                         const EngineSettings& settings = EngineSettings{});

// Body bytes of a request of `tokens` on the head's wire (the ratio budgetFor used).
uint32_t bodyBytesFor(const HeadBudget& b, uint32_t tokens);
// The arena a chat of this head needs: the larger of the due mark H plus one request's growth at the
// worst measured arena rate, and the head's v5.1.2 text keep at the measured arena per text byte, plus
// the turn-open reserve, rounded up to kArenaStep. A binding sizes a chat's arena with it (or with
// kActiveChatArenaBytes, which holds every default head).
uint32_t arenaBytesFor(const HeadBudget& b);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
