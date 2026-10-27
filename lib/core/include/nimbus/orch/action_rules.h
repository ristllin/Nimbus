#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/device_actions.h"   // ValidatedAction - the config knobs a turn may set
#include "nimbus/orch/rbac.h"             // Role
#include "nimbus/orch/tool_registry.h"    // Principal, ToolRegistry
#include "nimbus/orch/world.h"            // SessionInfo

// action_rules - who may run each action the head can take.
//
// The bug class this closes (CUM-813): the head's actions were checked for WHAT
// they asked (a valid model, a keyed provider, a protected key) and never for WHO
// asked. A Telegram member, a public-mode guest, a routine, a sub-agent results
// turn or a LAN /mcp client could move the owner's main model, stop the owner's
// sub-agents or play audio in the owner's room whenever the action itself was
// well formed.
//
// One table, one row per action: every device[] action and config knob, every
// tool the firmware registers by name, and the turn fields that change state. A
// row's rule reads three facts about the caller:
//   - its role: the Principal the turn (or the LAN endpoint) was admitted under;
//   - unattended: nobody is writing this turn right now (a routine, a wakeup, the
//     nightly dream, a sub-agent results turn, a device-staged system turn);
//   - lan: the call came over the LAN /mcp endpoint, which carries one device
//     sign-in code and no per-caller identity (User rights, its own namespace).
//
// The rules, most open first:
//   Open           every caller; the handler draws the data line (own namespace,
//                  quota), so a member reads and writes only its own data
//   TurnOnly       every turn caller; never the LAN endpoint
//   Owner          the owner's principal, attended or not, so the owner's own
//                  routines keep working; never a member, guest or LAN client
//   OwnerAttended  the owner writing now; never a member, guest, LAN client or
//                  unattended turn
//
// A name with no row is refused to everyone but the owner writing now (fail
// closed: a forgotten row never opens an action to a member), and the class test
// (test_action_rules) fails on it: it scans every tool the firmware registers,
// every device action and config knob the turn schema offers, and every knob the
// validator reads.
//
// Enforcement: applyDeviceAction (the one envelope for end-of-turn device[] and
// mid-turn device.control) checks each action and knob; ToolRegistry::dispatch
// checks every tool call through installActionRules, on every route (the turn's
// tool loop, the LAN endpoint, the test seams).
//
// Portable, Arduino-free, host-tested.
namespace nimbus {
namespace orch {

enum class ActionRule : uint8_t { Open = 0, TurnOnly, Owner, OwnerAttended };

const char* actionRuleName(ActionRule r);   // "open" | "turn" | "owner" | "owner-attended"

// The caller, in the three facts every rule reads.
struct ActionCaller {
  Role role = Role::Unknown;
  bool unattended = false;   // routine, wakeup, dream, results synthesis or system turn
  bool lan = false;          // the LAN /mcp endpoint
  std::string chat;          // the caller's own chat: a member's or guest's, or the chat the
                             // owner's turn runs for ("" for the LAN or when unknown)

  bool owner() const { return !lan && permsFor(role).manageTenants; }
};

// Build the caller from the dispatch Principal, the turn's unattended flag and, for the
// owner (whose namespace names no chat), the chat the turn runs for.
ActionCaller actionCaller(const Principal& who, bool unattended, const std::string& turnChat = std::string());

struct ActionRuleRow {
  const char* key;    // a tool name ("memory.write"), an argument-narrowed key
                      // ("memory.config.update"), a device action ("device.reboot"),
                      // a config knob ("device.config.ttsOn") or a turn field
  ActionRule  rule;
  const char* what;   // what the action does, in plain words, for the refusal
};

// The row for a key, or nullptr when it has none.
const ActionRuleRow* actionRuleRow(const std::string& key);
// The rule for a key: its row's, the connected tool servers' for "mcp.<server>.<tool>",
// else OwnerAttended (fail closed).
ActionRule actionRuleFor(const std::string& key);
bool actionAllowed(const std::string& key, const ActionCaller& c);
// The whole table, for the class test and the docs.
const ActionRuleRow* actionRuleTable(size_t& count);

// The key a tool call stands for: its name, or a narrower key when an argument
// changes what the call does (memory.config update, models.list refresh,
// device.selftest audible, prefs.edit changes, a message or file sent to a chat
// other than the caller's own).
std::string toolActionKey(const std::string& name, ArduinoJson::JsonObjectConst args,
                          const ActionCaller& c);

// Where files.send delivers when it names no chat: the turn's own chat when that is
// a Telegram chat on the allowlist; otherwise (the web chat, voice, serial, a
// public-mode chat) the owner's chat for the owner, and nowhere ("") for anyone else,
// who is refused rather than sent somewhere it did not ask for. The access rule counts
// the owner's case as another chat (files.send.other_chat: the owner writing now).
std::string filesSendTarget(const std::string& turnChat, bool turnChatAllowed, bool owner,
                            const std::string& ownerChat);

// The plain refusal for a key the caller may not run ("" when it may): who may,
// and why this caller is not that.
std::string actionRefusal(const std::string& key, const ActionCaller& c);

// The device action key of one validated device[] element ("device.led", ...,
// "device.orch_model"); "" for an Unknown kind. Config knobs have their own keys.
const char* deviceActionKey(ActionKind kind);

// The config knobs a device[] config element may carry, each with its has-flag, so
// a refused knob is stripped from the action before it is applied. The key of a
// knob is "device.config.<knob>".
struct ConfigKnob {
  const char* knob;                 // the wire name ("ttsOn")
  bool ValidatedAction::*has;       // set when the element carried the knob
};
const ConfigKnob* configKnobTable(size_t& count);
std::string configKnobKey(const char* knob);   // "device.config.<knob>"

// Whose sub-agents a caller sees (CUM-830): the owner sees every one; a member or a
// guest only those its own chat started; the LAN endpoint none. Every view of the
// running sub-agents (the turn's [ACTIVE SESSIONS] digest, session.list, the session
// tools by id) reads this one rule.
bool sessionVisible(const SessionInfo& s, const ActionCaller& c);
std::vector<SessionInfo> visibleSessions(const std::vector<SessionInfo>& all, const ActionCaller& c);

// The dispatch seam: every tool call through `reg` is checked against the table
// for the dispatch Principal, `unattended()` (null = attended) and `turnChat()` (the
// chat the turn in flight runs for; null = unknown), on every route.
void installActionRules(ToolRegistry& reg, std::function<bool()> unattended,
                        std::function<std::string()> turnChat = nullptr);

}  // namespace orch
}  // namespace nimbus
