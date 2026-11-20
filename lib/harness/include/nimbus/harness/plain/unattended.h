#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/unattended_policy.h"

namespace nimbus {
namespace orch {
class ToolRegistry;
namespace tx {
class TxFiles;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/unattended.h - isolated routine runs and held delivery (PRD section 6, lane RB-U1a).
//
// A routine fire runs on its own engine and its own transcript, `routine-<id>`: the system core,
// the person's notes, the routine's last outputs (its state, its only continuity) and its stored
// instruction stamped with the fire time. Zero bytes of the owner's chat reach its request: it is
// another log, another arena, no recent-files list, no digests, no recall. Each fire starts from
// its state alone (the earlier fires stay stored on the card, out of the request). Its tools are
// the ones its definition names (default web_search, read_file, send) that RB-Q2a's policy row
// for a routine grants (nimbus::orch::unattendedMayList): the one policy table, bound here, never
// copied; every call is checked again with its arguments (nimbus::orch::unattendedRefusal) in
// HookSlot::Policy, so a write reaches only a path the definition lists. Its limits are its own
// (6 rounds, 90 s soft, 180 s hard, 0.05 USD a fire: TurnSetup::ceiling) and a routine that
// reaches one ends honestly: what reaches the owner says it stopped at that limit.
//
// Delivery is held delivery: the routine's reply is first stored as a note (the outbox, with
// HeldFile persistence), then sent to the owner's chat through the owner's delivery and appended
// WHOLE to the owner's transcript as one device note "[routine <name> <stamp>] <text>" (owner
// ruling 81: isolation bounds what the routine's request sees, never what the owner's later
// requests carry). During quiet hours the note is held with an expiry (12 h, or the routine's own)
// and sent as stored when they end, by the idle step (HookSlot::Routines), dropped when it expired
// or was already delivered; there is no second model turn. Status lines of the routine's own
// engine (a provider error, storage) are reported to the binding, not sent to the owner.
//
// Seam declared by lane RB-B0a; the body (plain_unattended.cpp) is lane RB-U1a's.
namespace agent {
namespace plain {

// A routine's own limits (PRD section 6; ruling 84 item 9 keeps them for unattended work).
inline constexpr uint8_t kRoutineRounds = 6;
inline constexpr uint32_t kRoutineSoftMs = 90000;
inline constexpr uint32_t kRoutineHardMs = 180000;
inline constexpr uint32_t kRoutineMaxMicroUsd = 50000;   // 0.05 USD a fire
inline constexpr uint32_t kHeldNoteExpiryS = 12 * 3600;
// A routine id as a transcript key takes: [a-z0-9_-], at most this long ("lp" + 6 hex today).
inline constexpr size_t kRoutineIdMax = 32;
// The state: the last outputs, at most this many and this many bytes rendered.
inline constexpr size_t kRoutineStateOutputs = 3;
inline constexpr size_t kRoutineStateMax = 2048;
// A routine's arena: one fire, from its state (each fire starts fresh).
inline constexpr size_t kRoutineArenaBytes = 128 * 1024;
// The outbox: held notes at most, and the keys of delivered texts kept for "already delivered".
inline constexpr size_t kHeldNotesMax = 8;
inline constexpr size_t kDeliveredKeysMax = 16;

// The transcript key of a routine's own conversation: "routine-<id>". InvalidArgument (and "")
// for an id that is empty, longer than kRoutineIdMax or has a character a chat key cannot hold.
Status routineChatKey(const std::string& routineId, std::string& out);
// Is `chatKey` a routine's own transcript?
bool isRoutineChat(const std::string& chatKey);

// A routine as its definition stores it (loops.json and loops2.json; RB-U1b reads them).
struct RoutineDef {
  std::string id;
  std::string name;
  std::string instruction;
  std::vector<std::string> tools;   // the tools it names; empty: kDefaultRoutineTools
  std::vector<std::string> paths;   // the files it may write; an entry ending in '/' lists a folder
  uint32_t holdS = 0;               // a held note's expiry in seconds; 0: kHeldNoteExpiryS
};
// The default tools of a routine with none named (PRD section 6), before the policy row cuts them.
extern const char* const kDefaultRoutineTools[3];

// One earlier output of a routine (its state file holds its last kRoutineStateOutputs).
struct RoutineOutput {
  uint64_t at = 0;    // epoch seconds of the fire
  std::string text;   // what reached the owner (the reply; empty when it was silent)
};

// One fire.
struct RoutineFire {
  RoutineDef def;
  uint64_t fireAt = 0;                 // epoch seconds; 0: the clock is not set (refused)
  std::vector<RoutineOutput> last;     // its state, oldest first
  bool grants = false;                 // the owner's switch "/loop grants on" (ruling 76 item 7)
};

// The policy facts of a fire (class Routine, the owner's switch, the definition's paths as the
// listed ones). `f` must outlive the facts (listed points into it).
nimbus::orch::UnattendedFacts routineFacts(const RoutineFire& f, bool outside = false);
// The tools a fire is offered: the names its definition lists (or the default) that the policy
// row lets a routine be offered at all, in the definition's order, each once.
std::vector<std::string> routineTools(const RoutineFire& f);
// The registry a fire's request is composed from: `base` with every tool that is not in
// routineTools removed (`more` too, unless a listed tool is not always on). Handlers, classes,
// gates, policies and the access rule are the base's. Copied per fire.
Status routineRegistry(const nimbus::orch::ToolRegistry& base, const RoutineFire& f, nimbus::orch::ToolRegistry& out);
// The routine's own bounds as the engine takes them.
TurnCeiling routineCeiling();

// The state file (RB-U1b binds its path, /mem/routines/<id>.state): one JSON object per line,
// {"at":<epoch>,"text":"..."}, oldest first. decode keeps what reads and skips the rest.
std::string encodeRoutineState(const std::vector<RoutineOutput>& last);
std::vector<RoutineOutput> decodeRoutineState(const std::string& text);
// The state after a fire: `now` appended (a silent fire adds nothing), the oldest dropped past
// kRoutineStateOutputs, then past kRoutineStateMax bytes of text (a single output over it is cut
// on a character boundary).
void nextRoutineState(std::vector<RoutineOutput>& last, const RoutineOutput& now);

// The device note a routine's text is shown by, in the routine's own request (its state) and in
// the owner's transcript: "[routine <name> <message stamp without its '['> <text>". The stamp is
// tx::messageStamp's (with the year); no clock: the stamp's "time unknown" form.
std::string routineNoteText(const std::string& name, uint64_t at, int16_t tzMinutes, uint16_t dayStartMin,
                            const std::string& text);
// The text a fire starts from (its device note in its own transcript): each state output as a
// routineNoteText line, then the instruction under the fire's message stamp.
std::string routineInputText(const RoutineFire& f, int16_t tzMinutes, uint16_t dayStartMin);

// ---- held delivery (the outbox) ----------------------------------------------------
enum class HeldState : uint8_t {
  Pending = 0,   // stored by the fire's delivery; the fire has not finished (a cut: boot readies it)
  Ready = 1,     // final; sent when not quiet
  Sent = 2,      // sent to the owner; its append to his transcript is still owed
};
struct HeldNote {
  std::string id;           // "<routine id>-<fire epoch>"
  std::string routine;      // the routine's id
  std::string name;         // its name (the note's label)
  std::string chatId;       // the owner's chat it goes to
  uint64_t at = 0;          // the fire's time (the label's stamp)
  uint64_t expires = 0;     // epoch seconds; past it the note is dropped unsent
  std::string text;         // the routine's reply, exactly as the model wrote it
  std::string stop;         // the guard slug when the fire stopped at a limit ("" when it did not)
  HeldState state = HeldState::Pending;
  uint64_t key() const;     // fnv1a-64 of text: "already delivered"
};

// Where the outbox lives (the binding's file, for example /mem/routines/held.json).
struct HeldFile {
  std::function<std::string()> load;                   // "" when there is none
  std::function<bool(const std::string& text)> save;   // false: storage refused it
};

// The outbox: held notes and the keys of the last delivered texts, saved whole at every change.
class HeldStore {
 public:
  explicit HeldStore(HeldFile file) : file_(std::move(file)) {}
  // Read the file (a missing or damaged file is an empty outbox). Pending notes become Ready:
  // the fire that wrote them is gone (a restart).
  Status load();
  // Put or replace a note by id. TooLarge when kHeldNotesMax notes are held already and none
  // of them is this one (the caller reports it; the outbox never drops a note to make room).
  Status put(const HeldNote& n);
  Status remove(const std::string& id);
  // Remember a delivered text's key (the newest kDeliveredKeysMax are kept).
  Status delivered(uint64_t key);
  bool wasDelivered(uint64_t key) const;
  const std::vector<HeldNote>& notes() const { return notes_; }
  const HeldNote* find(const std::string& id) const;

 private:
  Status save();
  HeldFile file_;
  std::vector<HeldNote> notes_;
  std::vector<uint64_t> keys_;
};

// How a note reaches the owner: his chat's delivery (deliverTurn over the channel, or the
// binding's own EngineDeps::deliver) and an append to his transcript (appendOwnerNote).
struct OwnerSink {
  std::string chatKey;   // the owner chat's transcript key
  std::string chatId;    // where its messages go
  DeliverFn deliver;
  std::function<Status(const std::string& note, uint64_t at)> append;
};
// One device note (class routine) appended to a chat's transcript between its turns: stored
// after every item it holds, resident in its arena, head.json moved on. The engine's next turn
// takes its q after it. TooLarge when the arena is full (the note is stored on the card only and
// the next restart fill brings it back); InvalidArgument before boot.
Status appendOwnerNote(PlainEngine& owner, const std::string& note, uint64_t at);

// The owner's quiet hours as the policy reads them (self_wakeup.h: 22:00 to the owner's end).
struct QuietHours {
  bool respected = true;
  uint16_t endMinute = 540;     // the owner's setting (orch::kQuietHoursEndDefault)
};
// Is `now` (local offset tzMinutes) inside quiet hours? No clock (0): yes (a note waits for one).
bool inQuiet(const QuietHours& q, uint64_t now, int16_t tzMinutes);

// ---- one fire ------------------------------------------------------------------------
// What the binding gives a fire. The routine's engine is built for the fire on `files` under its
// own key; `head` is the turn context of the head that answers (host, model, dialect, profile,
// who, dayStartMin): the host's sub model unless the definition names the head (the binding's).
struct RoutineDeps {
  const Platform* platform = nullptr;
  HttpTransport* transport = nullptr;
  nimbus::orch::tx::TxFiles* files = nullptr;
  const nimbus::orch::ToolRegistry* reg = nullptr;   // the device's registry (cut per fire)
  ToolDispatchFn dispatch;                           // the binding's dispatch
  TurnSetupFn setup;                                 // the binding's setup for this head
  TurnCtx head;
  int16_t tzMinutes = 0;
  std::function<void(PlainHooks&)> moreHooks;        // the routine engine's other seams (not Policy)
  size_t arenaBytes = kRoutineArenaBytes;
  HeldStore* held = nullptr;
  OwnerSink owner;
  QuietHours quiet;
};

enum class FireResult : uint8_t {
  Silent = 0,    // no text and no limit: nothing reaches the owner
  Sent = 1,      // sent and appended to the owner's transcript
  Held = 2,      // held for the end of quiet hours
  Failed = 3,    // the engine refused or failed (status); nothing reached the owner
};
struct FireReport {
  FireResult result = FireResult::Silent;
  TurnOutcome outcome;            // the routine engine's turn (rounds, trip, status, end facts)
  std::string noteId;
  RoutineOutput output;           // for the state file (nextRoutineState)
  Status status = Status::success();
};
// Run one fire: its own engine on routine-<id>, starting from its state alone, the Policy hook
// installed, its ceiling, its delivery into the outbox, then sent (and appended) or held.
// InvalidArgument for a bad id, a fire with no clock, or missing deps; Busy when moreHooks took
// the Policy slot (a fire never runs without the policy).
Status runRoutine(const RoutineDeps& deps, const RoutineFire& fire, FireReport& out);

// Send one Ready note now: its reply and, after a limit, the stop line (the status-line class
// LimitReached), then the append. Sent marks it before the append; a note already Sent is only
// appended. Removed from the outbox at the end and its key remembered.
Status deliverHeld(HeldStore& held, const OwnerSink& owner, const std::string& id, int16_t tzMinutes,
                   uint16_t dayStartMin);
// What reaches the owner's transcript for a note: routineNoteText of its text, then on a new line
// the stop line when it stopped at a limit.
std::string heldNoteText(const HeldNote& n, int16_t tzMinutes, uint16_t dayStartMin);

// The owner engine's side, registered once at boot in HookSlot::Routines: bootResume loads the
// outbox; idleStep (one unit a step) drops an expired or already delivered note, finishes the
// append of a Sent one, sends a Ready one when quiet hours are over, else runs one due fire
// (`due`, optional: the binding's scheduler hands it over).
struct UnattendedDeps {
  HeldStore* held = nullptr;
  OwnerSink owner;
  QuietHours quiet;
  uint16_t dayStartMin = kDayStartDefaultMin;
  std::function<bool(uint64_t now, RoutineFire& out)> due;   // a fire is due now
  std::function<RoutineDeps()> fireDeps;                     // what runRoutine needs for it
  std::function<void(const FireReport&)> fired;              // after each fire (state, lastErr)
};
Status installUnattended(PlainHooks& hooks, const UnattendedDeps& deps);

}  // namespace plain
}  // namespace agent
