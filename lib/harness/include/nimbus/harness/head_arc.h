#pragma once
#include <cstdint>

// HeadArcTracker - the pure decision for keeping the orchestrator's own "head"
// ring arc lit while its sub-agents run (W6). Host-tested; no Arduino / router.
//
// The problem it fixes: the TurnEngine paints a blue "head" arc while a turn
// executes and clears it the instant runTurn() returns - which is right after
// the turn ENQUEUES its sub-agents, before any of them dispatch. So on a fan-out
// the head arc collapsed before the child arcs appeared; you never saw
// "orchestrator + N children" on the ring.
//
// The TurnGuard now leaves the arc lit at turn-end when children are active, and
// this tracker (driven from the main-loop ring watchdog) owns the rest of the
// arc's life in the post-turn window: it re-lights periodically so the ambient
// hold can't expire it during a long run, and clears it once - on the edge -
// when the last child finishes.
//
// It deliberately stands DOWN during a turn (turnInFlight): the TurnGuard owns
// the arc then, and the tracker must not fight it.
//
// Prism-hardened edges (v4.1 review):
// - "Clear owed after every turn" (sawTurn_): the guard may leave the arc lit
//   (children>0 at dtor) and the children can hit 0 BEFORE this tracker's first
//   post-turn tick (a fast-fail dispatch) - gating Clear on lit_ alone stranded
//   that guard-lit arc forever. After any turn, the first children==0 tick emits
//   one Clear regardless of lit_ (an Offline for an already-off arc is a no-op).
// - Frozen-children backstop: children only drain on the tg_poll task; if that
//   task dies with children>0, re-lighting every refresh would pulse "working"
//   forever (no other expiry covers a Running arc). If the child COUNT hasn't
//   changed for frozenMs while lit, emit Clear and LATCH (no re-light) until a
//   turn runs or the count moves - a live fan-out changes the count as spawns
//   dispatch/finish, so the backstop only fires on a genuinely wedged system
//   (or an extreme single >30 min sub, an accepted trade documented in
//   docs/sub-sessions.md; [ACTIVE SESSIONS] stays authoritative).

namespace nimbus {
namespace harness {

class HeadArcTracker {
 public:
  enum class Action { None, Light, Clear };

  // refreshMs: how often to re-emit the lit arc while children run, so the
  // router's posture-scaled ambient hold (Full = 5 min) never ages it out.
  // frozenMs: the dead-tg_poll backstop (see header comment).
  explicit HeadArcTracker(uint32_t refreshMs = 60000, uint32_t frozenMs = 1800000)
      : refreshMs_(refreshMs), frozenMs_(frozenMs) {}

  // turnEndedSinceLast: a turn COMPLETED since the previous call (an edge derived
  // from the engine's monotonic turnCount). This catches the wake-up stuck-arc
  // root cause: a short wake-up turn fires on tg_poll and finishes BETWEEN two
  // 5 s watchdog ticks, so this tracker never samples turnInFlight=true. If that
  // turn fanned out and the TurnGuard left the arc lit, then the children drained
  // to 0 before our first tick, nothing here owed a Clear (sawTurn_ was never set)
  // and the guard-lit arc stranded until a backstop. The completion edge sets the
  // clear-owed flag even for a turn we never saw in flight, so the PRIMARY path
  // clears it and the backstop stays at 0 (CUM-11).
  Action reconcile(bool turnInFlight, int activeChildren, uint32_t nowMs,
                   bool turnEndedSinceLast = false) {
    if (turnEndedSinceLast) {
      // A turn happened (even one too brief to ever be sampled in flight): owe a
      // Clear once children reach 0, and treat the completed turn as proof of life
      // so a stale frozen backstop releases.
      sawTurn_ = true;
      backstopped_ = false;
    }
    if (turnInFlight) {
      // The turn owns the arc; forget our state so we re-light on the first
      // post-turn tick if children remain. A turn also proves the system is
      // alive: release the frozen backstop and owe one Clear.
      lit_ = false;
      sawTurn_ = true;
      backstopped_ = false;
      return Action::None;
    }
    if (activeChildren > 0) {
      if (activeChildren != lastCount_) {   // count moved => tg_poll is alive
        lastCount_ = activeChildren;
        lastCountChangeMs_ = nowMs;
        backstopped_ = false;
      }
      if (backstopped_) return Action::None;
      if (lit_ && (int32_t)(nowMs - lastCountChangeMs_) >= (int32_t)frozenMs_) {
        backstopped_ = true;   // frozen count: stop claiming "working"
        lit_ = false;
        ++backstopFires_;      // CUM-11 metric: the belt-and-braces path fired. In a
                               // healthy system the PRIMARY edge (a count move or a
                               // turn) always clears first, so this stays 0; a nonzero
                               // count means a real wedge slipped past the primary path.
        return Action::Clear;
      }
      if (!lit_ || (int32_t)(nowMs - lastLightMs_) >= (int32_t)refreshMs_) {
        if (!lit_) lastCountChangeMs_ = nowMs;   // fresh light restarts the freeze clock
        lit_ = true;
        lastLightMs_ = nowMs;
        return Action::Light;
      }
      return Action::None;
    }
    // children == 0
    lastCount_ = 0;
    backstopped_ = false;
    if (lit_ || sawTurn_) {
      lit_ = false;
      sawTurn_ = false;
      return Action::Clear;
    }
    return Action::None;
  }

  bool lit() const { return lit_; }
  bool backstopped() const { return backstopped_; }
  // CUM-11: how many times the frozen-children backstop has had to fire. The 24 h
  // soak acceptance is this == 0: the backstop is belt-and-braces, never the
  // mechanism that clears a healthy wake-up's arc.
  uint32_t backstopFires() const { return backstopFires_; }

 private:
  uint32_t refreshMs_;
  uint32_t frozenMs_;
  bool lit_ = false;
  bool sawTurn_ = false;
  bool backstopped_ = false;
  int lastCount_ = 0;
  uint32_t lastLightMs_ = 0;
  uint32_t lastCountChangeMs_ = 0;
  uint32_t backstopFires_ = 0;
};

}  // namespace harness
}  // namespace nimbus
