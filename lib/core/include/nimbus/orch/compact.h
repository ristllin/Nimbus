#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// compact - the portable core of context compaction (plans/compaction-plan.md,
// v3.6.0). Nimbus cannot edit provider-side conversation chains (opaque ids), so
// compaction is: summarize the chat FROM THE EPISODIC STORE into an anchored
// per-chat summary, then RESET the provider chain; the existing per-chat
// RECENT CONVERSATION window supplies the verbatim tail. This module owns every
// decision that can be host-tested: the per-model context table, the trigger
// threshold math, the breaker, and the fold-prompt assembly. Device/engine glue
// (episodic slice, provider call, chain reset, UX) stays out.
//
// Research grounding (2026-07-27): every surveyed harness anchors token counts
// on PROVIDER-REPORTED usage (no client tokenizer) and compacts at
// window − reserved-output − buffer, re-injecting a structured summary plus a
// verbatim recent tail. Claude Code's observed constants: 20K output reserve,
// 13K buffer (≈167K trigger on a 200K window).
namespace nimbus {
namespace orch {

// ---- per-model context windows ----------------------------------------------
// Family-prefix table, deliberately conservative: an unknown model gets
// kCtxDefaultTokens so a wrong guess compacts EARLY (cheap), never late (400s).
// The device may layer an NVS override on top (store::ctxTokensOverride).
constexpr uint32_t kCtxDefaultTokens = 100000;

// Returns the context window (tokens) for a provider/model pair.
// Matching: exact family prefixes on the known catalogs - "claude-*" 200K,
// "gpt-5*" 272K, "o<digit>*" 200K, "mistral-*" 128K - else the default.
uint32_t modelCtxTokens(const std::string& provider, const std::string& model);

// ---- trigger math ------------------------------------------------------------
// threshold = min(ctx − reservedOut − kCompactBufferTokens, ownerCapTokens).
// reservedOut is the caller's REQUESTED max_tokens (the head asks ~3-4K, not the
// model max). ownerCapTokens (NVS compactAtK, default 60K) exists because a desk
// assistant gains nothing from a 150K tail - compacting earlier keeps every turn
// faster and cheaper. 0 disables the owner cap (window math only). The result is
// floored at kCompactMinThreshold so a tiny/misconfigured window can't make the
// device compact on every turn.
constexpr uint32_t kCompactBufferTokens  = 13000;
constexpr uint32_t kCompactMinThreshold  = 1000;

uint32_t compactThreshold(uint32_t ctxTokens, uint32_t reservedOutTokens,
                          uint32_t ownerCapTokens);

// True when the chat's chain (provider-reported input tokens of its last turn)
// has reached the threshold.
inline bool shouldCompact(uint32_t chainTokens, uint32_t threshold) {
  return threshold > 0 && chainTokens >= threshold;
}

// ---- reactive provider-error classification ---------------------------------
// True when a provider error string indicates the CONTEXT WINDOW was exceeded -
// the reactive force-compact path. Matched case-insensitively on the known
// wordings (OpenAI "context_length_exceeded"/"maximum context length",
// Anthropic "prompt is too long", Mistral "too large for model"/"context").
bool isContextOverflowError(const std::string& err);

// ---- circuit breaker ---------------------------------------------------------
// 3 consecutive compaction failures pause AUTO compaction (manual stays allowed)
// until a success or an explicit reset; the device alerts the owner ONCE on the
// pause edge (loop-breaker pattern).
struct CompactBreaker {
  uint8_t fails  = 0;
  bool    paused = false;
  // Returns true exactly when this failure newly paused the breaker (alert edge).
  bool noteResult(bool ok) {
    if (ok) { fails = 0; paused = false; return false; }
    if (paused) return false;
    if (++fails >= 3) { paused = true; return true; }
    return false;
  }
};

// ---- the fold prompt ---------------------------------------------------------
// Instructions for the compaction call. Text-only, no tools; the reply IS the
// summary (the engine never applies any other field of the turn). Anchored:
// when a previous summary exists it is UPDATED, not replaced (opencode pattern),
// so cost stays bounded no matter how long a chat lives.
extern const char* const ORCH_COMPACT_PROMPT;

// Assemble the compaction call's input: the previous anchored summary (may be
// empty) + the new episodic digest since the last compaction. digest should
// already be byte-bounded by the caller (the device stages ≤64 KB in PSRAM).
std::string buildCompactInputs(const std::string& prevSummary,
                               const std::string& digest);

// Cap a produced summary to kChatSummaryMax bytes, UTF-8-safe (the fold output
// is re-injected into every future prompt - a torn sequence would 400 them all).
constexpr size_t kChatSummaryMax = 4096;

// How many chats keep a fold state. Raised 8 -> 16 (R5): a household or a small
// group easily exceeds eight correspondents, and evicting a chat's slot discards
// its summary - the next turn there starts with no continuity and re-pays the
// whole fold. Sixteen slots at the worst case below is ~66 KB of LittleFS, which
// the 3.5 MB partition absorbs.
constexpr size_t kFoldMaxChats = 16;

// ...but slot COUNT alone does not bound the blob, because a summary is capped
// per chat and nothing capped the total. Sixteen full summaries serialize to
// ~66 KB, and the whole blob is rewritten on every fold. This is the real
// ceiling: serialize() drops the least-recently-used slots until the blob fits,
// so a device with many chatty correspondents degrades by forgetting the oldest
// conversation's summary rather than by growing without limit.
constexpr size_t kFoldBlobMax = 48u * 1024;
std::string capSummary(const std::string& s);

// ---- per-chat fold state (the durable store) ---------------------------------
// PRISM plan-review revision 2: fold state must NOT ride the NVS conv map (the
// shipped parser treats everything after the first '|' as the convId, and an OTA
// rollback would feed it a poisoned previous_response_id). ALL of it lives in
// ONE LittleFS blob a pre-v3.6 image simply ignores - rollback-safe by
// construction. The trigger basis is provider-independent (revision 1): episodic
// BYTES + MESSAGE COUNT accumulated on the chat since its last fold (only OpenAI
// even has a growing provider chain; on Anthropic/Mistral the fold is purely the
// long-term continuity mechanism).
struct ChatFold {
  std::string summary;             // anchored summary, ≤ kChatSummaryMax
  uint32_t bytesSinceFold  = 0;    // message bytes captured since the last fold
  uint32_t msgsSinceFold   = 0;
  uint32_t lastFoldEpoch   = 0;    // wall epoch of the last fold (0 = never/pre-sync)
  uint32_t turnsSinceFold  = 0;    // thrash guard input (saturating)
  uint8_t  breakerFails    = 0;    // per-chat CompactBreaker persistence
  bool     breakerPaused   = false;
};

// Device implements against LittleFS /data/chatsum.txt; host tests in-memory.
struct FoldStoreIO {
  virtual ~FoldStoreIO() = default;
  virtual std::string load() = 0;             // "" if none
  virtual void        save(const std::string& blob) = 0;
};

// The fold-due decision, including the thrash guard (revision 7): a chat that is
// due again within kThrashTurns turns of a completed fold is paused instead of
// re-folded (the fold isn't shrinking it - alert the owner once).
enum class FoldDue : uint8_t { No = 0, Yes, ThrashPaused };
constexpr uint32_t kThrashTurns = 2;

class FoldStore {
 public:
  void begin(FoldStoreIO* io);                 // load + parse (tolerant)

  ChatFold get(const std::string& chatId) const;   // default-constructed if absent

  // Accumulate one captured message on the chat (called at episodic capture
  // time for kind=message rows). Persist is DEFERRED to the next noteTurn/
  // applyFold to keep per-message flash writes off the hot path.
  void noteMessage(const std::string& chatId, size_t bytes);

  // One completed turn on the chat: bumps the thrash counter and persists the
  // accumulated state (one small blob write per turn, not per message).
  void noteTurn(const std::string& chatId);

  // Evaluate the auto-fold trigger. bytesThreshold==0 disables (manual only).
  // Returns ThrashPaused EXACTLY ONCE on the pause edge (alert seam).
  FoldDue evaluateDue(const std::string& chatId, uint32_t bytesThreshold,
                      uint32_t msgsThreshold);

  // A fold completed: store the capped summary, reset the counters, stamp the
  // epoch, clear the breaker, persist. (Write order: this blob FIRST - the
  // caller resets the provider chain only after this returns.)
  void applyFold(const std::string& chatId, const std::string& summary,
                 uint32_t epoch);

  // A fold attempt failed: breaker bookkeeping. True on the 3-fail pause edge
  // (alert once). Persisted.
  bool noteFoldFailed(const std::string& chatId);

  // Manual override: un-pause a chat's auto-fold (owner /compact succeeds, or
  // an explicit resume). Persisted.
  void resume(const std::string& chatId);

  // Reactive path: a provider context-overflow error was classified on this chat
  // - force it over any byte threshold so the next pump pass folds it. The
  // existing zero-tools fresh-retry already recovered the TURN; this recovers
  // the trajectory. Persisted.
  void markOverflow(const std::string& chatId);

  size_t chatCount() const { return chats_.size(); }
  std::string serialize() const;               // exposed for tests/diagnostics

 private:
  struct Slot { std::string chatId; ChatFold f; };
  Slot* find(const std::string& chatId);
  const Slot* find(const std::string& chatId) const;
  Slot& upsert(const std::string& chatId);     // LRU: oldest evicted at kFoldMaxChats
  static std::string encodeSlot(const Slot& s);  // one record, for the byte budget
  void  persist();
  std::vector<Slot> chats_;                    // back = most recently touched
  FoldStoreIO* io_ = nullptr;
};

}  // namespace orch
}  // namespace nimbus
