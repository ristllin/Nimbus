#pragma once
// turn_effects - the per-turn effect ledger: what THIS turn actually did and saw.
//
// The device used to deliver the model's reply as written, so a reply could say
// "saved", "written and waiting", "three sub-agents started" or "survives a
// reboot" while the turn had done nothing of the kind (16-18 Oct, nine of 51
// replies). The evidence was always on the device - every tool result, every
// mem_write outcome, the spawn enqueue, the scratchpad persist - but scattered
// over log lines. This ledger collects it in one place so the claim check
// (claims.h) can compare the reply with it before delivery.
//
// Fed by:
//   - the head loop's tool dispatch (one entry per tool call, from the tool's
//     effect row and its result text: recordToolEffect);
//   - the turn's own fields before apply (pending: mem_write items, spawn items,
//     the scratchpad, device actions) and their real outcomes after apply;
//   - fresh sub-agent results (sourced, or with no lookup behind them).
//
// RAM only, one turn long, never persisted. At most kMax entries (about 1 KB),
// held in the working (PSRAM) allocator. Portable, Arduino-free, host-tested
// (test_orch_claims).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"

namespace nimbus {
namespace orch {

enum class FxKind : uint8_t {
  MemWrite = 0,    // a long-term memory was stored
  MemUpdate,       // a stored memory was changed, replaced or unpinned
  MemPin,          // a memory was pinned (it was not pinned before)
  MemPinNoop,      // a pin on a memory that was already pinned: nothing changed
  MemDelete,       // memories removed or archived
  Scratch,         // the scratchpad changed and was saved
  ScratchRamOnly,  // the scratchpad changed in RAM, but saving it failed
  FileWrite,       // a file or artifact was written to the store
  SkillSave,       // a skill was created, edited or deleted
  Spawn,           // a sub-agent was accepted by the job queue
  SpawnRefused,    // a spawn intent did not reach the queue
  Send,            // something was delivered to a person (message, file)
  Speak,           // something was spoken aloud on the device
  Schedule,        // a routine, reminder or wakeup was created or cancelled
  Config,          // a device or memory setting changed
  Read,            // a lookup ran (memory search, file read, web search, ...)
  SubSourced,      // a sub-agent result whose run made at least one tool call
  SubUnsourced,    // a live-data sub-agent result with no lookup behind it
  kCount
};

// Stable machine slug per kind (log lines, tests). Never empty.
const char* fxSlug(FxKind k);

enum : uint8_t {
  kFxPermanent = 1u << 0,   // a memory write that asked for permanent storage
  kFxPending   = 1u << 1,   // derived from the turn's fields before apply ran
  kFxVoice     = 1u << 2,   // a send asked for as a Telegram voice message (voice:true)
  kFxReceipt   = 1u << 3,   // the send's result names Telegram's message_id: it arrived
};

struct Effect {
  FxKind  kind = FxKind::Read;
  bool    ok = false;       // the effect landed (false: refused or failed)
  uint8_t flags = 0;        // kFx* bits
  uint8_t count = 1;        // how many (spawn items, lookups) this entry stands for
  char    ref[24] = {};     // a memory id, a file name tail or a job tag; "" when none
};

class TurnEffects {
 public:
  static constexpr size_t kMax = 32;

  void reset();
  // Record one effect. Returns false (and sets overflowed()) once kMax entries
  // are held; the entry is then dropped, never a reason to fail the turn.
  bool add(FxKind k, bool ok, const std::string& ref = std::string(), uint8_t count = 1,
           uint8_t flags = 0);
  size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }
  const Effect& at(size_t i) const { return v_[i]; }
  bool overflowed() const { return overflow_; }

  // Sum of `count` over entries of kind k that landed (ok) / did not.
  int countOk(FxKind k) const;
  int countFailed(FxKind k) const;
  bool anyOk(FxKind k) const { return countOk(k) > 0; }
  // True when an ok entry of kind k carries `flag`.
  bool anyOkWith(FxKind k, uint8_t flag) const;
  // Drop the pending entries (kFxPending) - apply replaces them with outcomes.
  void dropPending();

  // Plain summary for the model-facing correction text, e.g. "memory writes: 1
  // stored (m1a2b3c4d); scratchpad: kept in RAM only, saving failed; sub-agents
  // started: 0". "nothing (no tool changed or looked up anything)" when empty.
  std::string summary() const;
  // Cross-turn row tag: "fx:" + the kinds that landed ("fx:mem,file,spawn"), or
  // "fx:none". At most 24 bytes, whole names only.
  std::string fxTag() const;

 private:
  std::vector<Effect, WorkingAllocator<Effect>> v_;
  bool overflow_ = false;
};

// One head tool call, recorded from its effect class (the tool's row in the
// shared tool table) and its result. The result text refines the class where
// the tool says what really happened: "already pinned ... no change" is a
// MemPinNoop, "unpin ok" a MemUpdate and "delete ok" a MemDelete; a scratchpad
// result that says it is "kept until restart only" is ScratchRamOnly, and a
// scratchpad or config "view" is a Read. Ids are lifted from the tool's own text
// ("stored memory <id>", "pin ok (<id>)", "saved <path>").
// `flags` carries what the call's arguments said (kFxPermanent for a memory
// written with permanent:true, kFxVoice for a send asked for as a voice message).
// A send whose result names Telegram's message_id carries kFxReceipt (its id is
// the ref). A voice send is backed only by that receipt: without one it records as
// failed, whatever its text says (22 Oct: "sent to Telegram" came back for a voice
// message that never left the device); one that went out as text is a plain send.
// A failed send whose text still names receipts (a voice reply that stopped part
// way) records the delivered part as landed and the rest as failed.
void recordToolEffect(TurnEffects& fx, FxKind base, bool isError, const std::string& output,
                      uint8_t flags = 0);

// The id/name a tool result names, or "" ("stored memory m1a2b" -> "m1a2b").
std::string fxRefFromOutput(const std::string& output);

// The first Telegram message id a send's result names ("... (message_id 812, ..." ->
// "812"), or "" when it names none: the delivery receipt.
std::string fxReceiptId(const std::string& output);

// True when an ok send this turn carries a voice message's receipt.
bool voiceDelivered(const TurnEffects& fx);

}  // namespace orch
}  // namespace nimbus
