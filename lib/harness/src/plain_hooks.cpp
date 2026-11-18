// The seam registry of the plain engine (plain/hooks.h), lane RB-B0a. Real, not a stub:
// a lane that registers a handler can test its registration before the engine exists.
#include "nimbus/harness/plain/hooks.h"

namespace agent {
namespace plain {

namespace {

template <class Fn>
Status put(std::array<Fn, kHookSlotCount>& slots, HookSlot slot, Fn fn, const char* where) {
  const size_t i = static_cast<size_t>(slot);
  if (i >= kHookSlotCount || !fn) return Status::fail(Err::InvalidArgument, where);
  if (slots[i]) return Status::fail(Err::Busy, where);
  slots[i] = std::move(fn);
  return Status::success();
}

template <class Fn>
size_t countOf(const std::array<Fn, kHookSlotCount>& slots) {
  size_t n = 0;
  for (const Fn& f : slots)
    if (f) ++n;
  return n;
}

template <class Fn>
bool taken(const std::array<Fn, kHookSlotCount>& slots, HookSlot slot) {
  const size_t i = static_cast<size_t>(slot);
  return i < kHookSlotCount && static_cast<bool>(slots[i]);
}

}  // namespace

Status PlainHooks::beforeDispatch(HookSlot slot, BeforeDispatchFn fn) {
  return put(beforeDispatch_, slot, std::move(fn), "plain::PlainHooks::beforeDispatch");
}
Status PlainHooks::afterResult(HookSlot slot, AfterResultFn fn) {
  return put(afterResult_, slot, std::move(fn), "plain::PlainHooks::afterResult");
}
Status PlainHooks::beforeDeliver(HookSlot slot, BeforeDeliverFn fn) {
  return put(beforeDeliver_, slot, std::move(fn), "plain::PlainHooks::beforeDeliver");
}
Status PlainHooks::afterDeliver(HookSlot slot, AfterDeliverFn fn) {
  return put(afterDeliver_, slot, std::move(fn), "plain::PlainHooks::afterDeliver");
}
Status PlainHooks::idleStep(HookSlot slot, IdleStepFn fn) {
  return put(idleStep_, slot, std::move(fn), "plain::PlainHooks::idleStep");
}
Status PlainHooks::bootResume(HookSlot slot, BootResumeFn fn) {
  return put(bootResume_, slot, std::move(fn), "plain::PlainHooks::bootResume");
}

DispatchDecision PlainHooks::runBeforeDispatch(const DispatchCtx& ctx) const {
  for (size_t i = 0; i < kHookSlotCount; ++i) {
    if (!beforeDispatch_[i]) continue;
    DispatchDecision d = beforeDispatch_[i](ctx);
    if (d.verdict == DispatchVerdict::Run) continue;
    d.by = static_cast<HookSlot>(i);
    return d;
  }
  return DispatchDecision{};
}

void PlainHooks::runAfterResult(const ResultCtx& ctx) const {
  for (const AfterResultFn& f : afterResult_)
    if (f) f(ctx);
}

DeliverDecision PlainHooks::runBeforeDeliver(const DeliverCtx& ctx) const {
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

void PlainHooks::runAfterDeliver(const DeliveredCtx& ctx) const {
  for (const AfterDeliverFn& f : afterDeliver_)
    if (f) f(ctx);
}

IdleOutcome PlainHooks::runIdleStep(const IdleCtx& ctx) const {
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

Status PlainHooks::runBootResume(const BootCtx& ctx) const {
  Status first = Status::success();
  for (const BootResumeFn& f : bootResume_) {
    if (!f) continue;
    const Status s = f(ctx);
    if (!s.ok() && first.ok()) first = s;
  }
  return first;
}

size_t PlainHooks::registered(Seam seam) const {
  switch (seam) {
    case Seam::BeforeDispatch: return countOf(beforeDispatch_);
    case Seam::AfterResult: return countOf(afterResult_);
    case Seam::BeforeDeliver: return countOf(beforeDeliver_);
    case Seam::AfterDeliver: return countOf(afterDeliver_);
    case Seam::IdleStep: return countOf(idleStep_);
    case Seam::BootResume: return countOf(bootResume_);
  }
  return 0;
}

bool PlainHooks::has(Seam seam, HookSlot slot) const {
  switch (seam) {
    case Seam::BeforeDispatch: return taken(beforeDispatch_, slot);
    case Seam::AfterResult: return taken(afterResult_, slot);
    case Seam::BeforeDeliver: return taken(beforeDeliver_, slot);
    case Seam::AfterDeliver: return taken(afterDeliver_, slot);
    case Seam::IdleStep: return taken(idleStep_, slot);
    case Seam::BootResume: return taken(bootResume_, slot);
  }
  return false;
}

void PlainHooks::clear() { *this = PlainHooks{}; }

}  // namespace plain
}  // namespace agent
