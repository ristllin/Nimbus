#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/plain/turn.h"

// plain/hooks.h - the seams of the plain engine (PRD section 1, "Seams instead of a
// shared file"). The engine calls six seams at fixed points of a turn; intents, rails,
// truth, agents, routines, media, suppression, compaction and resume register their
// handlers here from their own files, so no lane edits plain_engine.cpp to hook in.
//
//   beforeDispatch  before one tool call runs (suppression, intent, taint gating)
//   afterResult     after its result is stored, before the next request leaves
//   beforeDeliver   before the reply is delivered (the truth seam)
//   afterDeliver    after delivery and the d item
//   idleStep        idle work, one unit per step, in fixed order
//   bootResume      once at boot, before the first turn (crash resume, stored-not-sent)
//
// Order never depends on who registered first: each handler sits in a HookSlot, and a
// seam runs its handlers in slot order. One handler per slot per seam; a second
// registration in a taken slot is refused (Err::Busy). The Observer slot observes only:
// it is refused on the deciding seams (beforeDispatch, beforeDeliver, idleStep). Handlers
// are registered once at boot, before the first turn; registering while a turn runs is
// not supported.
//
// Seam declared by lane RB-B0a, with the registry itself, defined inline below: an
// out-of-line registry costs unwind tables on the device even while nothing references
// it (the device links lib objects whole and keeps their .eh_frame; measured 224 B).
// The engine lanes RB-E1a and RB-E1b call the run* functions at their points.
namespace agent {
namespace plain {

// The six seams, by name.
enum class Seam : uint8_t { BeforeDispatch = 0, AfterResult = 1, BeforeDeliver = 2, AfterDeliver = 3, IdleStep = 4, BootResume = 5 };
inline constexpr Seam kAllSeams[] = {Seam::BeforeDispatch, Seam::AfterResult, Seam::BeforeDeliver,
                                     Seam::AfterDeliver,   Seam::IdleStep,    Seam::BootResume};
inline constexpr size_t kSeamCount = sizeof(kAllSeams) / sizeof(kAllSeams[0]);
inline const char* seamName(Seam s) {
  switch (s) {
    case Seam::BeforeDispatch: return "beforeDispatch";
    case Seam::AfterResult: return "afterResult";
    case Seam::BeforeDeliver: return "beforeDeliver";
    case Seam::AfterDeliver: return "afterDeliver";
    case Seam::IdleStep: return "idleStep";
    case Seam::BootResume: return "bootResume";
  }
  return "?";
}

// Who a handler is, and so where it runs in a seam. The idle order of the PRD (owner
// message, agent result, routine, compaction slice, shadow probe) is the slot order of
// Agents, Routines, Compaction, ShadowProbe; the owner message is the engine's own and
// always comes first. Append new slots before Observer; never renumber.
enum class HookSlot : uint8_t {
  Policy = 0,       // the unattended policy table: tools a routine or agent turn may use
  Rails = 1,        // taint gating, pending approvals, refusals with a reason
  Suppression = 2,  // identical-call suppression of pure tools
  Intents = 3,      // write-ahead intent before an effect tool runs
  Media = 4,        // photos and documents
  Truth = 5,        // the truth check (enforce before delivery, shadow from idle)
  Agents = 6,       // sub-agent arrival
  Routines = 7,     // routine fires and held quiet-hours delivery
  Compaction = 8,   // one compaction slice
  ShadowProbe = 9,  // shadow-mode truth probes
  Resume = 10,      // crash resume and stored-not-sent replies
  Observer = 11,    // metrics and traces: observes, never decides; always last
  None = 255,       // in a decision: no slot decided
};
inline constexpr size_t kHookSlotCount = 12;

// ---- beforeDispatch ------------------------------------------------------------
enum class DispatchVerdict : uint8_t {
  Run = 0,       // let the call run (the next slot still decides)
  Answer = 1,    // do not run; the model gets `text` as the result (suppression, pending)
  Refuse = 2,    // do not run; the model gets `text` as an error result
};
struct DispatchCtx {
  const TurnCtx* turn = nullptr;
  const ToolCall* call = nullptr;
  bool pure = false;     // declared pure (read_file, list_files, ledger_day, search)
  bool effect = false;   // an effect tool (writes, sends, schedules, device control)
};
struct DispatchDecision {
  DispatchVerdict verdict = DispatchVerdict::Run;
  std::string text;      // Answer and Refuse: the result text the model sees
  HookSlot by = HookSlot::None;       // set by the runner: the slot that decided
};

// ---- afterResult -----------------------------------------------------------------
struct ResultCtx {
  const TurnCtx* turn = nullptr;
  const ToolCall* call = nullptr;
  const ToolOutput* output = nullptr;
  uint64_t q = 0;        // the stored r item
  bool stored = true;    // false: the append failed and the item is held in RAM
};

// ---- beforeDeliver ---------------------------------------------------------------
enum class DeliverVerdict : uint8_t {
  Deliver = 0,   // deliver the text as it is
  Hold = 1,      // hold it; add `deviceNote` as one device item and run one more round
};
struct DeliverCtx {
  const TurnCtx* turn = nullptr;
  const std::string* text = nullptr;   // the reply; nothing may rewrite it
  bool held = false;                   // this turn already held once (at most one hold)
};
struct DeliverDecision {
  DeliverVerdict verdict = DeliverVerdict::Deliver;
  std::string deviceNote;              // Hold: facts only, starting '[device]'
  HookSlot by = HookSlot::None;
};

// ---- afterDeliver ----------------------------------------------------------------
struct DeliveredCtx {
  const TurnCtx* turn = nullptr;
  const std::string* text = nullptr;
  const std::vector<int64_t>* msgIds = nullptr;
  bool sent = false;
};

// ---- idleStep --------------------------------------------------------------------
struct IdleCtx {
  uint64_t now = 0;              // epoch seconds; 0 = clock not set
  uint32_t nowMs = 0;
  bool ownerWaiting = false;     // an owner message is queued: idle work must yield
  uint32_t quietForS = 0;        // seconds since the last turn ended
  int16_t tzMinutes = 0;         // local offset from UTC (the 03:00 to 05:00 window, local days)
  bool clockSet = false;         // false: no local-time rule may fire
};
struct IdleOutcome {
  bool worked = false;           // this handler did one unit of work
  HookSlot by = HookSlot::None;
};

// ---- bootResume ------------------------------------------------------------------
struct BootCtx {
  uint64_t now = 0;
  bool afterCrash = false;       // the RTC record says the last run did not end cleanly
};

using BeforeDispatchFn = std::function<DispatchDecision(const DispatchCtx&)>;
using AfterResultFn = std::function<void(const ResultCtx&)>;
using BeforeDeliverFn = std::function<DeliverDecision(const DeliverCtx&)>;
using AfterDeliverFn = std::function<void(const DeliveredCtx&)>;
using IdleStepFn = std::function<IdleOutcome(const IdleCtx&)>;
using BootResumeFn = std::function<Status(const BootCtx&)>;

class PlainHooks {
 public:
  // Registration, one handler per slot per seam (Err::Busy when taken,
  // Err::InvalidArgument for a null handler or a slot out of range).
  Status beforeDispatch(HookSlot slot, BeforeDispatchFn fn);
  Status afterResult(HookSlot slot, AfterResultFn fn);
  Status beforeDeliver(HookSlot slot, BeforeDeliverFn fn);
  Status afterDeliver(HookSlot slot, AfterDeliverFn fn);
  Status idleStep(HookSlot slot, IdleStepFn fn);
  Status bootResume(HookSlot slot, BootResumeFn fn);

  // What the engine calls. beforeDispatch: slot order, the first verdict other than
  // Run decides. afterResult and afterDeliver: every handler, slot order.
  // beforeDeliver: slot order, the first Hold decides (none when ctx.held).
  // idleStep: slot order, stops at the first handler that worked; nothing runs while
  // ctx.ownerWaiting. bootResume: every handler; the first failure is returned after all ran.
  DispatchDecision runBeforeDispatch(const DispatchCtx& ctx) const;
  void runAfterResult(const ResultCtx& ctx) const;
  DeliverDecision runBeforeDeliver(const DeliverCtx& ctx) const;
  void runAfterDeliver(const DeliveredCtx& ctx) const;
  IdleOutcome runIdleStep(const IdleCtx& ctx) const;
  Status runBootResume(const BootCtx& ctx) const;

  // How many handlers a seam holds, and whether a slot is taken (tests, boot log).
  size_t registered(Seam seam) const;
  bool has(Seam seam, HookSlot slot) const;
  void clear();

 private:
  std::array<BeforeDispatchFn, kHookSlotCount> beforeDispatch_{};
  std::array<AfterResultFn, kHookSlotCount> afterResult_{};
  std::array<BeforeDeliverFn, kHookSlotCount> beforeDeliver_{};
  std::array<AfterDeliverFn, kHookSlotCount> afterDeliver_{};
  std::array<IdleStepFn, kHookSlotCount> idleStep_{};
  std::array<BootResumeFn, kHookSlotCount> bootResume_{};
};

// ---- the registry ----------------------------------------------------------------
namespace hooks_detail {

// `decides`: the seam's handlers return a decision, which the Observer slot may not make.
template <class Fn>
inline Status put(std::array<Fn, kHookSlotCount>& slots, HookSlot slot, Fn fn, const char* where, bool decides) {
  const size_t i = static_cast<size_t>(slot);
  if (i >= kHookSlotCount || !fn) return Status::fail(Err::InvalidArgument, where);
  if (decides && slot == HookSlot::Observer) return Status::fail(Err::InvalidArgument, where);
  if (slots[i]) return Status::fail(Err::Busy, where);
  slots[i] = std::move(fn);
  return Status::success();
}

template <class Fn>
inline size_t countOf(const std::array<Fn, kHookSlotCount>& slots) {
  size_t n = 0;
  for (const Fn& f : slots)
    if (f) ++n;
  return n;
}

template <class Fn>
inline bool taken(const std::array<Fn, kHookSlotCount>& slots, HookSlot slot) {
  const size_t i = static_cast<size_t>(slot);
  return i < kHookSlotCount && static_cast<bool>(slots[i]);
}

}  // namespace hooks_detail

inline Status PlainHooks::beforeDispatch(HookSlot slot, BeforeDispatchFn fn) {
  return hooks_detail::put(beforeDispatch_, slot, std::move(fn), "plain::PlainHooks::beforeDispatch", true);
}
inline Status PlainHooks::afterResult(HookSlot slot, AfterResultFn fn) {
  return hooks_detail::put(afterResult_, slot, std::move(fn), "plain::PlainHooks::afterResult", false);
}
inline Status PlainHooks::beforeDeliver(HookSlot slot, BeforeDeliverFn fn) {
  return hooks_detail::put(beforeDeliver_, slot, std::move(fn), "plain::PlainHooks::beforeDeliver", true);
}
inline Status PlainHooks::afterDeliver(HookSlot slot, AfterDeliverFn fn) {
  return hooks_detail::put(afterDeliver_, slot, std::move(fn), "plain::PlainHooks::afterDeliver", false);
}
inline Status PlainHooks::idleStep(HookSlot slot, IdleStepFn fn) {
  return hooks_detail::put(idleStep_, slot, std::move(fn), "plain::PlainHooks::idleStep", true);
}
inline Status PlainHooks::bootResume(HookSlot slot, BootResumeFn fn) {
  return hooks_detail::put(bootResume_, slot, std::move(fn), "plain::PlainHooks::bootResume", false);
}

inline DispatchDecision PlainHooks::runBeforeDispatch(const DispatchCtx& ctx) const {
  for (size_t i = 0; i < kHookSlotCount; ++i) {
    if (!beforeDispatch_[i]) continue;
    DispatchDecision d = beforeDispatch_[i](ctx);
    if (d.verdict == DispatchVerdict::Run) continue;
    d.by = static_cast<HookSlot>(i);
    return d;
  }
  return DispatchDecision{};
}

inline void PlainHooks::runAfterResult(const ResultCtx& ctx) const {
  for (const AfterResultFn& f : afterResult_)
    if (f) f(ctx);
}

inline DeliverDecision PlainHooks::runBeforeDeliver(const DeliverCtx& ctx) const {
  if (ctx.held) return DeliverDecision{};   // at most one hold a turn
  for (size_t i = 0; i < kHookSlotCount; ++i) {
    if (!beforeDeliver_[i]) continue;
    DeliverDecision d = beforeDeliver_[i](ctx);
    if (d.verdict != DeliverVerdict::Hold) continue;
    d.by = static_cast<HookSlot>(i);
    return d;
  }
  return DeliverDecision{};
}

inline void PlainHooks::runAfterDeliver(const DeliveredCtx& ctx) const {
  for (const AfterDeliverFn& f : afterDeliver_)
    if (f) f(ctx);
}

inline IdleOutcome PlainHooks::runIdleStep(const IdleCtx& ctx) const {
  if (ctx.ownerWaiting) return IdleOutcome{};   // the owner message goes first
  for (size_t i = 0; i < kHookSlotCount; ++i) {
    if (!idleStep_[i]) continue;
    IdleOutcome o = idleStep_[i](ctx);
    if (!o.worked) continue;
    o.by = static_cast<HookSlot>(i);
    return o;
  }
  return IdleOutcome{};
}

inline Status PlainHooks::runBootResume(const BootCtx& ctx) const {
  Status first = Status::success();
  for (const BootResumeFn& f : bootResume_) {
    if (!f) continue;
    const Status s = f(ctx);
    if (!s.ok() && first.ok()) first = s;
  }
  return first;
}

inline size_t PlainHooks::registered(Seam seam) const {
  switch (seam) {
    case Seam::BeforeDispatch: return hooks_detail::countOf(beforeDispatch_);
    case Seam::AfterResult: return hooks_detail::countOf(afterResult_);
    case Seam::BeforeDeliver: return hooks_detail::countOf(beforeDeliver_);
    case Seam::AfterDeliver: return hooks_detail::countOf(afterDeliver_);
    case Seam::IdleStep: return hooks_detail::countOf(idleStep_);
    case Seam::BootResume: return hooks_detail::countOf(bootResume_);
  }
  return 0;
}

inline bool PlainHooks::has(Seam seam, HookSlot slot) const {
  switch (seam) {
    case Seam::BeforeDispatch: return hooks_detail::taken(beforeDispatch_, slot);
    case Seam::AfterResult: return hooks_detail::taken(afterResult_, slot);
    case Seam::BeforeDeliver: return hooks_detail::taken(beforeDeliver_, slot);
    case Seam::AfterDeliver: return hooks_detail::taken(afterDeliver_, slot);
    case Seam::IdleStep: return hooks_detail::taken(idleStep_, slot);
    case Seam::BootResume: return hooks_detail::taken(bootResume_, slot);
  }
  return false;
}

inline void PlainHooks::clear() { *this = PlainHooks{}; }

}  // namespace plain
}  // namespace agent
