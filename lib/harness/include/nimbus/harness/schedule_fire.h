#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/unattended.h"   // RoutineFire, OwnerSink, FireReport (RB-U1a)
#include "nimbus/orch/schedule_tool.h"
#include "nimbus/orch/tx/log.h"                // TxFiles (the routine state files)

// schedule_fire.h - the fires of engine 2's schedule entries (lane RB-U1b). The tool and the loops2
// codec are lib/core's (nimbus/orch/schedule_tool.h); what needs the plain engine's types is here.
namespace nimbus {
namespace orch {
namespace schedule {

namespace plain = agent::plain;

// The state file (RB-U1a's codec). A missing file is an empty state (true); a write cut before
// its rename leaves the old file, a cut between the remove and the rename leaves only the temp,
// which is then read. False when the storage refused the read or the write.
bool loadState(tx::TxFiles& files, const std::string& id, std::vector<plain::RoutineOutput>& out);
bool saveState(tx::TxFiles& files, const std::string& id, const std::vector<plain::RoutineOutput>& last);

// The fire a routine record makes: its definition from loops.json and its entry (no entry: the
// default tools, no paths), its state, the time and the owner's switch.
plain::RoutineFire routineFire(const LoopRecord& l, const Extra* e, const std::vector<plain::RoutineOutput>& last,
                               uint64_t fireAt, bool grants);
// The next fire opens tainted while its state holds an output of a fire that read outside content.
bool opensTainted(const Extra* e, const std::vector<plain::RoutineOutput>& last);

// What a binding gives one fire.
struct FireBinding {
  plain::OwnerSink owner;                 // the owner's chat: deliver and append (RB-I1)
  int16_t tzMinutes = 0;
  uint16_t dayStartMin = plain::kDayStartDefaultMin;
  tx::TxFiles* files = nullptr;           // the card (state files); null: no card
  bool grants = false;                    // the owner's "/loop grants on"
  // runRoutine with the binding's RoutineDeps; `tainted` goes to RoutineDeps::head.tainted.
  std::function<plain::Status(const plain::RoutineFire& f, bool tainted, plain::FireReport& out)> run;
};

// A reminder's fire: its stored text, byte for byte, to the owner as a routine note
// (plain::deliverHeld), never held for quiet hours, no model call.
FireOutcome fireReminder(const FireBinding& b, const LoopRecord& l, const std::string& chatId, uint64_t now);
// The loops-subsystem fire hook for engine 2: a reminder (above) or a routine (runRoutine through
// `b.run`, then its state, its outside mark and its outcome). A routine with no card is not run:
// it is marked Skipped with lastErr "no card" and the fire reads as deferred (not counted).
FireOutcome fire(Host& host, const LoopFireRequest& req, const FireBinding& b, uint64_t now);

}  // namespace schedule
}  // namespace orch
}  // namespace nimbus
