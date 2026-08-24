#pragma once
#include <cstdint>

// screen_sched - the render scheduler, a pure FSM. It decides WHEN to render;
// WHAT a screen looks like is the renderer's job, and WHICH screen matters is
// the attention router's. The caller owns the panel: it executes at most one
// RenderCommand at a time and reports completion via onRenderDone().
//
// Behavior contract (each host-tested):
//  - Dwell: cursor detents never render. dwellMs after the LAST detent, the
//    detail screen becomes due. New detents during panel-busy re-arm the dwell.
//  - Latest-wins: exactly one pending slot. A newer request (any source)
//    replaces the pending one; nothing queues deeper.
//  - Attention bypass: attention intents become due immediately (no dwell, no
//    coalesce window) - but still wait for a busy panel to free.
//  - Coalescing: non-attention (ambient) intents sit in the pending slot until
//    the coalesce window since the last COMPLETED ambient render elapses.
//
// `Kind` and the fullClear refresh-upgrade counter are a legacy of slow-panel
// support: a fast color panel repaints in ~31 ms and ignores both. They are
// retained so the FSM contract and its host tests stay byte-stable.

namespace nimbus::render {

enum class Kind : uint8_t { FastBW = 0, Partial = 1, Color = 2 };

struct RenderCommand {
  bool    render = false;   // false: nothing to do this tick
  uint8_t screenId = 0;
  Kind    kind = Kind::FastBW;
  bool    fullClear = false;  // refresh-upgrade hint (legacy; fast panel ignores)
};

struct SchedConfig {
  uint32_t dwellMs = 300;
  uint32_t coalesceMs = 30000;
  uint8_t  fullEveryN = 6;
};

class Scheduler {
 public:
  void configure(const SchedConfig& cfg);

  // Encoder detent while `screenId` is the detail screen the cursor addresses.
  void onDetent(uint8_t screenId, uint32_t nowMs);

  // A screen wants to be (re)rendered. attention=true bypasses dwell/coalesce.
  void onIntent(uint8_t screenId, bool attention, uint32_t nowMs,
                Kind kind = Kind::FastBW);

  // The caller finished executing the last issued command.
  void onRenderDone(uint32_t nowMs);

  // Poll. Returns at most one command; after a command with render=true the
  // scheduler considers the panel busy until onRenderDone().
  RenderCommand tick(uint32_t nowMs);

  // Drop a NON-attention (ambient) pending intent without rendering it (F28):
  // when the screensaver arms, a StatusIdle intent queued just before it would
  // otherwise flush over the logo on the next tick. Attention intents (CTA/Ask)
  // are NEVER dropped - a job waiting on the owner must still paint over the
  // saver. Returns true if something was discarded.
  bool clearPendingAmbient();

  bool panelBusy() const { return busy_; }

 private:
  uint32_t dwellElapsed(uint32_t nowMs) const;  // wrap/out-of-order-safe

  SchedConfig cfg_;
  bool busy_ = false;

  // single pending slot (latest wins)
  bool     hasPending_ = false;
  uint8_t  pendingScreen_ = 0;
  Kind     pendingKind_ = Kind::FastBW;
  bool     pendingAttention_ = false;

  // dwell
  bool     dwellArmed_ = false;
  uint32_t lastDetentMs_ = 0;
  uint8_t  dwellScreen_ = 0;

  // coalescing + ghosting
  uint32_t lastAmbientDoneMs_ = 0;
  bool     ambientEverDone_ = false;
  uint8_t  sinceFullClear_ = 0;
  bool     lastIssuedCounts_ = false;  // fast/partial (not color) → ghost counter
};

}  // namespace nimbus::render
