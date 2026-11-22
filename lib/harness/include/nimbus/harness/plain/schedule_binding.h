#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/unattended.h"
#include "nimbus/harness/schedule_fire.h"
#include "nimbus/orch/tx/log.h"

// plain/schedule_binding.h - the fires of engine 2's schedule entries over one device chat (lane
// E2-SCHED). The loops tick picks a due entry and resolves the chat it delivers to; the seam boots
// that chat's ChatBinding and builds the FireBinding here:
//   - a reminder: its stored text to the chat through the binding's channel, no model request, the
//     routine note in the chat's transcript (schedule::fireReminder over ChatBinding::ownerSink);
//   - a routine: plain::runRoutine on its own transcript routine-<id> with the binding's parts
//     (ChatBinding::routineDeps: the registry cut to its tools, the money guard's cost, its
//     ceiling), on the host's small model, its outbox on the card (a quiet-hours note waits there),
//     its state at /mem/routines/<id>.state (schedule::fire), and a fire whose state holds an output
//     that came from outside content opens tainted.
// Only lib headers: test/test_schedule_binding drives it with a scripted provider and a fake card.
namespace agent {
namespace plain {

struct ScheduleBindingDeps {
  HeldStore* held = nullptr;        // the chat's outbox (heldFileOn); null: a routine fire is refused
  QuietHours quiet;                 // the owner's quiet hours
  bool grants = false;              // the owner's "/loop grants on"
  int16_t tzMinutes = 0;            // the local offset now
  std::string routineModel;         // the head of a routine fire ("" = the chat's head)
  // After each routine run: its report (the device files its spend in usage). Null: none.
  std::function<void(const FireReport& rep)> fired;
};

// The outbox file of one chat on the card, written by temp and rename (a cut leaves the old file).
inline HeldFile heldFileOn(nimbus::orch::tx::TxFiles& files, const std::string& path) {
  HeldFile h;
  nimbus::orch::tx::TxFiles* f = &files;
  h.load = [f, path] {
    size_t n = 0;
    std::string text;
    if (!f->size(path, n) || n == 0) return text;
    text.resize(n);
    size_t got = 0;
    if (!f->readAt(path, 0, &text[0], n, got) || got != n) text.clear();   // damaged: an empty outbox
    return text;
  };
  h.save = [f, path](const std::string& text) {
    const std::string tmp = path + nimbus::orch::schedule::store::kTmpExt;
    return f->writeWhole(tmp, text) && f->rename(tmp, path);
  };
  return h;
}

inline std::string heldPathFor(const std::string& chatKey) {
  return std::string(nimbus::orch::schedule::kStateDir) + "/held-" + chatKey + ".json";
}

// The FireBinding of one fire into `chat` (booted). `files` is the card (null: routines are not
// run, reminders still go).
inline nimbus::orch::schedule::FireBinding fireBindingFor(ChatBinding& chat, nimbus::orch::tx::TxFiles* files,
                                                          const ScheduleBindingDeps& d) {
  nimbus::orch::schedule::FireBinding b;
  b.owner = chat.ownerSink();
  b.tzMinutes = d.tzMinutes;
  b.dayStartMin = chat.config().dayStartMin;
  b.files = files;
  b.grants = d.grants;
  if (!d.held) return b;   // no outbox: a routine is skipped (no runner), a reminder still goes
  ChatBinding* c = &chat;
  b.run = [c, d](const RoutineFire& f, bool tainted, FireReport& out) {
    RoutineDeps r = c->routineDeps(d.routineModel);
    r.head.tainted = tainted;
    r.tzMinutes = d.tzMinutes;
    r.held = d.held;
    r.quiet = d.quiet;
    const Status st = runRoutine(r, f, out);
    if (d.fired) d.fired(out);
    return st;
  };
  return b;
}

// The outbox's due work for `chat` (a quiet-hours note once they end, an append that was owed):
// one note per call, oldest first; a note past its expiry, or one already delivered, is dropped.
// True when it did work.
inline bool drainHeld(HeldStore& held, ChatBinding& chat, uint64_t now, int16_t tzMinutes, const QuietHours& quiet) {
  if (!now) return false;
  const OwnerSink sink = chat.ownerSink();
  for (const HeldNote& n : held.notes()) {
    if (n.state != HeldState::Sent && (n.expires <= now || held.wasDelivered(n))) {
      (void)held.remove(n.id);
      return true;
    }
    if (!n.chatId.empty() && n.chatId != sink.chatId) continue;
    if (n.state == HeldState::Sent || !inQuiet(quiet, now, tzMinutes)) {
      const std::string id = n.id;
      if (deliverHeld(held, sink, id, now, tzMinutes, chat.config().dayStartMin).ok()) return true;
    }
  }
  return false;
}

}  // namespace plain
}  // namespace agent
