#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/unattended.h"   // RoutineFire, OwnerSink, FireReport (RB-U1a)
#include "nimbus/orch/loops.h"                 // LoopRecord, SchedSpec, CivilTime, FireOutcome
#include "nimbus/orch/notes.h"                 // notes::Origin: the same origin rule as the note tool
#include "nimbus/orch/tool_registry.h"         // ToolResult
#include "nimbus/orch/tx/log.h"                // TxFiles (the routine state files)

// schedule_tool.h - engine 2's `schedule` tool (lane RB-U1b; PRD sections 4 and 6), shared by the
// device (src/agent/schedule_device.cpp) and nimbusd (nimbusd/src/hosted_loops.h).
//
// Storage. Every entry is a record of /data/loops.json, written by v5.1.1's dumpLoops and read by
// its loadLoops (orch_loops.cpp is not touched): engine 1 lists and fires what engine 2 made. The
// fields engine 1 does not know live in the sidecar /data/loops2.json, keyed by loop id (the old
// writer drops unknown fields). The sidecar format, version 1 ("loops2"):
//   {"v":1,"loops":[{"id":"lp1a2b3c","kind":"reminder"|"routine","tools":["..."],"paths":["..."],
//                    "holdS":0,"outsideAt":[<epoch>,...]}]}
// An id with no sidecar entry is an old record (kindOf). An entry whose id is not in loops.json
// is an orphan: ignored on load, dropped on the next write. Write order: a create or update writes
// the sidecar first and loops.json second; a delete writes loops.json first. A cut between the two
// leaves an orphan entry or an old record, never a record that reads wrong.
//
// Nothing the owner sent is cut (ruling 81): a text over kLoopPromptMax is refused with the limit
// named. The derived name (nameFor) is a label, not the owner's text.
//
// Fires. A reminder delivers its fixed text through the owner's sink with no model call; a routine
// runs through plain::runRoutine (its own transcript, tools and ceiling) and its state file
// /mem/routines/<id>.state is RB-U1a's codec, written by temp and rename.
namespace nimbus {
namespace orch {
namespace schedule {

namespace plain = agent::plain;

enum class Kind : uint8_t { Reminder = 0, Routine = 1 };

// Storage words no model reads: the sidecar's own keys (beside "id" and "kind", which the tool's
// arguments share), the state file's extensions and the e item event of a fire that read outside
// content (plain_unattended.cpp). The tool's words and result lines live in orch_schedule_tool.cpp.
namespace store {
inline constexpr char kV[] = "v", kLoops[] = "loops", kTools[] = "tools", kPaths[] = "paths", kHoldS[] = "holdS",
                      kOutsideAt[] = "outsideAt";
inline constexpr char kStateExt[] = ".state", kTmpExt[] = ".tmp";
inline constexpr char kOutsideEvent[] = "change:outside";
}  // namespace store

// One sidecar entry: what engine 2 knows about a loop beyond loops.json.
struct Extra {
  std::string id;
  Kind kind = Kind::Routine;
  std::vector<std::string> tools;    // a routine's tools; empty: plain::kDefaultRoutineTools
  std::vector<std::string> paths;    // the files it may write (none by default)
  uint32_t holdS = 0;                // a held note's expiry; 0: plain::kHeldNoteExpiryS
  std::vector<uint64_t> outsideAt;   // fire times of state outputs from a fire that read outside content
};

std::string dumpSidecar(const std::vector<Extra>& extras);
// "" is an empty sidecar (true). False on JSON that is not a version-1 sidecar; `out` is then empty.
bool loadSidecar(const std::string& json, std::vector<Extra>& out);
const Extra* findExtra(const std::vector<Extra>& extras, const std::string& id);
// The entries whose id is a loop of `loops`, in sidecar order.
std::vector<Extra> prunedSidecar(const std::vector<Extra>& extras, const std::vector<LoopRecord>& loops);
// A record's kind: its entry's; with none (an old record), a one-time record the owner made (the
// old /remind) is a reminder, every other record a routine.
Kind kindOf(const LoopRecord& l, const Extra* e);

// ---- arguments -------------------------------------------------------------------------------
// "YYYY-MM-DD HH:MM" (local wall clock). False on any other shape or an impossible date or time.
bool parseWhen(const std::string& s, CivilTime& out);
// "daily HH:MM", "weekly mon,thu HH:MM" (sun..sat), "every N min" or "every N h". An interval under
// `minIntervalSec` is refused.
bool parseEvery(const std::string& s, uint32_t minIntervalSec, SchedSpec& out);
// The cadence in the form parseEvery reads.
std::string everyText(const SchedSpec& s);
// The first words of `text`, at most kLoopNameMax bytes, cut at a space or a character boundary.
std::string nameFor(const std::string& text);

// ---- the binding ------------------------------------------------------------------------------
struct Clock {
  bool valid = false;   // the wall clock is real (synced)
  uint64_t now = 0;     // epoch seconds
};

// What the device and nimbusd give the tool: the loop table and its two files, the clock, the ids.
class Host {
 public:
  virtual ~Host() = default;
  // Run `fn` over the live loop table with the host's lock held: nothing else writes it meanwhile.
  virtual void withTable(const std::function<void(std::vector<LoopRecord>&)>& fn) = 0;
  // loops.json from `loops` (dumpLoops, atomic); called inside withTable.
  virtual bool saveLoops(const std::vector<LoopRecord>& loops) = 0;
  // loops2.json: a missing file reads true and empty; the write is atomic.
  virtual bool readSidecar(std::string& out) = 0;
  virtual bool writeSidecar(const std::string& json) = 0;
  virtual Clock clock() = 0;
  virtual bool localToEpoch(const CivilTime& local, uint64_t& out) = 0;
  virtual CivilTime epochToLocal(uint64_t epoch) = 0;
  // The first fire of a recurring record from `now` (the host's own clock math).
  virtual uint64_t nextRun(const LoopRecord& l, uint64_t now) = 0;
  virtual std::string newId(const std::vector<LoopRecord>& loops) = 0;
  virtual bool askFirst() = 0;   // the owner's "Wake-ups: ask me first"
  virtual bool reserved(const std::string& id) = 0;
  virtual int maxCount() = 0;    // LoopCaps::maxCount (the owner may only lower it)
  virtual uint32_t minIntervalSec() { return (uint32_t)kLoopMinIntervalSec; }
  // A deleted routine's state file goes (a later entry never inherits it). Default: no files.
  virtual void forgetState(const std::string& id) { (void)id; }
};

// Who calls: the origin of the turn (notes.h rule: a tainted turn's change never reaches the tool,
// the rails keep it) and the chat the turn answers in (where an entry delivers).
struct CallCtx {
  notes::Origin origin = notes::Origin::Unattended;
  std::string chatId;
};

// One `schedule` call: op create | update | delete | list; kind reminder | routine; id, when, text,
// every. An attended turn may do all of it; any other origin may list and create a reminder only.
ToolResult call(Host& host, ArduinoJson::JsonObjectConst args, const CallCtx& ctx);

// ---- fires ------------------------------------------------------------------------------------
inline constexpr char kStateDir[] = "/mem/routines";
std::string statePath(const std::string& id);   // /mem/routines/<id>.state
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
