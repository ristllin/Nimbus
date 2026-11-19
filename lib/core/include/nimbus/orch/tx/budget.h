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
// the next request would pass the aim, and KEEPS about 70% of it. Bytes appear only where a device
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
inline constexpr uint32_t kKeepPercent = 70;               // compaction keeps about 70% of the aim
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

struct ProfileMarks {
  uint32_t high = 0;   // H: compaction is due above it (stored bytes since thru)
  uint32_t low = 0;    // L: compaction keeps this much
};
struct TokenMarks {
  uint32_t aim = 0;    // the request this head aims at once the conversation fills it
  uint32_t due = 0;    // compaction is due once a request reached this (aim less the next request's growth)
  uint32_t keep = 0;   // a request just after a compaction (kKeepPercent of the aim)
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
  uint32_t bulkWholeCap = 0;     // tier 2 whole in its own turn: max(min(48,000, body / 8), v5.1.2 tool result)
  uint32_t warmBulk = 0;         // whole internal bulk above the pointer that moves it on a warm open
  uint32_t digestBlock = 0;      // digest block plus recent-files list, bytes
  uint32_t messageCap = 0;       // owner and assistant text kept inline, bytes
  uint32_t releasedFold = 0;     // v5.1.2's fold mark for this head (owner and assistant text)
  uint32_t releasedKeep = 0;     // v5.1.2's keep: a compaction leaves at least this much text
  uint32_t bodyMilliBytesPerToken = 0;   // the wire ratio the byte marks were derived with (x1000)
  int32_t bodyInterceptBytes = 0;        // and its intercept (0 on a released nominal ratio)
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
// The budget from a released row already in hand (pure; the two above read orch::turnBudgetFor with
// the tool loop on).
HeadBudget budgetFromRow(const RequestBudget& row, const HeadKey& head, Profile requested);

// Body bytes of a request of `tokens` on the head's wire (the ratio budgetFor used).
uint32_t bodyBytesFor(const HeadBudget& b, uint32_t tokens);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
