#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/turn.h"   // Turn: the old engine's end-of-turn fields

// unattended_policy - what a turn nobody asked for may do (PRD section 6, lane RB-Q2a).
//
// The bug class (DIAGNOSIS RC-13, 20 incidents): turns that start without a fresh message
// from a person (the firmware-update notice, a routine such as a check-in, a reminder, a
// wakeup, a sub-agent report, the nightly upkeep) ran as full model turns with every tool.
// On 8 Nov the post-update turn rewrote eight rows of a day the owner had just corrected; on
// 14 Nov it added a row again; a check-in added another. None of them was asked.
//
// One table, one row per class of unattended turn, decides which classes of tool the turn
// may call, which model it runs on and how its output reaches the owner. It names tool
// classes, never a use case: every file the owner keeps is a "file".
//   - Read      changes nothing (a file or memory read, a search, a status)
//   - Write     changes a file the owner keeps (write, append, edit, delete, download)
//   - Memory    changes the assistant's own memory, notes or scratchpad
//   - Send      a message or a file to a chat
//   - Spend     starts paid work beyond the turn (a sub-agent, an image)
//   - Schedule  creates or cancels a routine or a wakeup
//   - Ambient   the device's light, sound, music, or speech on its own speaker
//   - Control   changes settings, skills, people or anything with no row (fail closed)
//   - Scratch   the assistant's own scratchpad (its working state across a run's turns)
// Paid calls a turn makes beyond its own request are Spend: a sub-agent, an image, a web
// search, a look at a photo. A class whose row says no spend gets no provider built-in either.
// A routine, a reminder, a wakeup and a held note send only through held delivery: their own
// reply, which the harness delivers (and holds into a self-wakeup during quiet hours). The
// sub-agent report may also send a sub-agent's file to its own chat and start the next wave
// (PRD section 6); another chat is the owner's, writing now, in every class (action_rules.h).
// Writes go only to the paths a routine's own definition lists; the old engine's routines
// list none. An unattended request carries the provider's built-ins only, never the owner's
// connectors (engine.cpp runTurn, connector_grant.h).
//
// Enforcement on the old engine (engine 1):
//   - every tool call, on every route (the head loop, the end-of-turn memory dispatch, a
//     router such as `more`), at the registry's access seam (installActionRules' extra rule);
//   - the advertised tool list of the turn (unattendedMayList);
//   - the end-of-turn fields (restrictUnattendedTurn), with one line per refusal for the
//     chat's next turn;
//   - the update notice is a fixed string (updateNoticeText) with no model call.
// Engine 2 (RB-U1a) binds the same table for its isolated routine runs.
//
// Portable, Arduino-free, host-tested (test_unattended_policy, nimbusd test_unattended_turns).
namespace nimbus {
namespace orch {

// The class of a turn. Attended: a person wrote it now; the table has no say.
enum class TurnClass : uint8_t {
  Attended = 0,
  UpdateNotice,     // the post-update notice (no model call)
  Routine,          // a recurring routine the owner approved (a check-in, a brief)
  Reminder,         // a one-time reminder the owner set (/remind)
  Wakeup,           // a one-time wakeup the assistant set for itself (wakeup.set)
  SelfWakeup,       // a note held through quiet hours, delivered at their end
  SubagentReport,   // sub-agent results arriving (the synthesis turn)
  Dream,            // the nightly upkeep (memory only; RB-Q2b removes its model turn)
};
constexpr int kTurnClassCount = 8;
const char* turnClassSlug(TurnClass c);   // "attended" | "update-notice" | "routine" | ...

enum class ToolUse : uint8_t { Read = 0, Write, Memory, Send, Spend, Schedule, Ambient, Control, Scratch };
constexpr int kToolUseCount = 9;
const char* toolUseSlug(ToolUse u);   // "read" | "write" | ...

enum class Grant : uint8_t {
  No = 0,
  Yes,
  ListedPaths,   // Write only: a path the routine's own definition lists
};
enum class ModelCall : uint8_t { None = 0, Sub };   // the update notice calls no model
enum class Output : uint8_t {
  Fixed = 0,   // a fixed string the harness writes
  Reply,       // the turn's own reply, delivered by the harness (held in quiet hours)
  Silent,      // nothing is sent
};

struct UnattendedPolicyRow {
  TurnClass cls;
  ModelCall model;
  Output    output;
  bool      tools;                  // offered tools at all (a held note is one short request)
  Grant     grant[kToolUseCount];   // indexed by ToolUse
};

// A wakeup an unattended turn sets is one hop of a chain; after this many hops without an
// attended message (and from any turn that has seen outside content) none is set.
constexpr uint8_t kMaxWakeHops = 3;

// What the decision reads about the turn in flight beyond its class (ruling 76).
struct UnattendedFacts {
  TurnClass cls = TurnClass::Attended;
  bool routineGrants = false;   // the owner's switch: his routines may use his connectors and start sub-agents
  bool outside = false;         // the turn has seen outside content (a result, a web page)
  uint8_t hops = 0;             // unattended wakeup fires since the owner last wrote
  const std::vector<std::string>* artifacts = nullptr;   // files the results' envelopes name
  const std::vector<std::string>* listed = nullptr;      // the routine's own paths (engine 2)
};

// The row of a class (Attended grants everything: other rules decide for a person's turn).
const UnattendedPolicyRow& unattendedPolicy(TurnClass c);
// The whole table, for the class tests and the docs.
const UnattendedPolicyRow* unattendedPolicyTable(size_t& count);

// The class of a scheduled fire, from the old engine's fire flags.
TurnClass fireClass(bool dream, bool selfWakeup, bool ownerReminder, bool once);

// The tool class of one call. `name` is the registry name (dotted, "files.append") or an
// engine-2 name ("append_file"); the arguments narrow a few tools (a change to the standing
// rules is Control, their view Read; device.control by its action's type). A name with no
// row reads by its registration class (tool_groups.h): pure or read is Read, an effect is
// Control (fail closed).
ToolUse toolUseFor(const std::string& name, ArduinoJson::JsonObjectConst args);

// The path a file call really acts on: the old engine's dotted tools ("files.send") read
// `project`/`name`, engine 2's tools `path`; else `name`; "" when none (ruling 80 item 2).
std::string callPath(const std::string& tool, ArduinoJson::JsonObjectConst args);
// Is `path` one of `listed`? An entry ending in '/' lists a folder and everything under it.
// A path with a ".." segment, or an empty one, is never listed.
bool pathListed(const std::string& path, const std::vector<std::string>& listed);

// May a turn with these facts use a tool class at all? The row's grant, plus: an owner's
// routine with his switch on may spend; a schedule needs fewer than kMaxWakeHops hops and no
// outside content.
bool unattendedGranted(const UnattendedFacts& f, ToolUse u, bool pathListedNow = false);

// The decision for one tool call: "" when it may run, else the plain refusal the model reads
// as the call's result. A send from an unattended turn that has seen outside content may carry
// only a file the results' envelopes name (`artifacts`).
std::string unattendedRefusal(const UnattendedFacts& f, const std::string& name, ArduinoJson::JsonObjectConst args);

// May the turn be offered `name` at all (its class with no arguments)? A tool whose arguments
// can narrow it (the standing rules' view, device.control's action) is offered when its open
// use is allowed; the rest is refused at the call.
bool unattendedMayList(const UnattendedFacts& f, const std::string& name);

// The old engine's end-of-turn fields of an unattended turn, cut to what its row allows:
// memory and mem_write are Memory, scratchpad Scratch; each device[] element by its kind
// (light, sound and tts Ambient, config by knob, reboot and model switch Control); spawn and
// session_ops spawn or
// tell Spend, terminate Control; mem_query, await and ask stay. Returns how many items were
// cut and appends one line per cut field to `refusals` ("" when nothing was cut).
int restrictUnattendedTurn(Turn& t, const UnattendedFacts& f, std::string& refusals);

// The update notice: a fixed string with the version, then the release's one-line note
// (its first line, at most kUpdateNoteMax bytes on a character boundary) when there is one.
constexpr size_t kUpdateNoteMax = 200;
std::string updateNoticeText(const std::string& version, const std::string& notes);

}  // namespace orch
}  // namespace nimbus
