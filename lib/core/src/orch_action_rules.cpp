#include "nimbus/orch/action_rules.h"

#include "nimbus/orch/vector_memory.h"   // kMcpNs

namespace nimbus {
namespace orch {

namespace {

using R = ActionRule;

// One row per action. Grouped by where the action lives; the class test keeps the
// rows and the firmware in step (every registered tool, every schema device action
// and config knob, every knob the validator reads has a row; every row names one).
const ActionRuleRow kRows[] = {
    // ---- device[] actions and mid-turn device.control (lib/harness/src/apply.cpp) ----
    {"device.led", R::Owner, "change the light ring"},
    {"device.lights", R::Owner, "turn the light ring on or off"},
    {"device.tts", R::Owner, "speak aloud on the device"},
    {"device.reboot", R::OwnerAttended, "restart the device"},
    {"device.orch_model", R::OwnerAttended, "change the main model"},
    // a config element as a whole: the owner's (a member's is refused as a unit); then
    // each knob it carries is checked on its own row below
    {"device.config", R::Owner, "change device settings"},
    // config knobs: the ambient ones stay open to the owner's routines ("dim the ring
    // at night"); anything that routes paid calls, changes the voice or identity, or
    // carries a physical risk needs the owner writing now
    {"device.config.ledBrightness", R::Owner, "change the ring brightness"},
    {"device.config.priority", R::OwnerAttended, "change which providers sub-agents use"},
    {"device.config.posture", R::Owner, "change the ring level"},
    {"device.config.profile", R::Owner, "change the battery mode"},
    {"device.config.theme", R::Owner, "change the ring color theme"},
    {"device.config.attnHoldMs", R::Owner, "change how long a needs-you cue holds"},
    {"device.config.ttsVoice", R::OwnerAttended, "change the speaking voice"},
    {"device.config.ttsProv", R::OwnerAttended, "change the voice provider"},
    {"device.config.ttsOn", R::OwnerAttended, "switch voice replies on or off"},
    {"device.config.sttProv", R::OwnerAttended, "change the transcription provider"},
    {"device.config.sfxLvlN", R::Owner, "change the sound level"},
    {"device.config.sfxLvlO", R::Owner, "change the sound level"},
    {"device.config.sfxVol", R::Owner, "change the speaker volume"},
    {"device.config.sfxTheme", R::Owner, "change the sound theme"},
    {"device.config.sleepOvr", R::OwnerAttended, "turn off the low-battery sleep"},
    {"device.config.brightOvr", R::OwnerAttended, "lift the light ring's safety cap"},
    {"device.config.devName", R::OwnerAttended, "rename the device"},
    // ---- turn fields that change state (lib/harness/src/apply.cpp) ----
    {"turn.scratchpad", R::Owner, "write the scratchpad"},
    {"turn.ask", R::Owner, "raise the needs-you light"},   // the ask still reaches the person who asked
    {"turn.spawn", R::TurnOnly, "start a sub-agent"},       // an approved chat's (its own connectors only)
    // ---- memory (lib/core/src/orch_memory_tools.cpp): own namespace, the handler's line ----
    {"memory.write", R::Open, "store a memory"},
    {"memory.search", R::Open, "search memory"},
    {"memory.update", R::Open, "change a memory"},
    {"memory.pin", R::Open, "pin or delete a memory"},
    {"memory.episodic", R::Open, "search the history"},
    {"memory.archive", R::Open, "search or restore archived memories"},
    {"memory.config", R::Open, "view the memory settings"},
    {"memory.config.update", R::OwnerAttended, "change how memory works"},
    {"memory.scratchpad", R::Owner, "use the scratchpad"},   // the dream grooms it unattended
    // ---- standing rules (lib/core/src/orch_prefs_tools.cpp) ----
    {"prefs.edit", R::Owner, "see the standing rules"},
    {"prefs.edit.change", R::OwnerAttended, "change the standing rules"},
    // ---- recent results, sub-agents (lib/core) ----
    {"results.get", R::Open, "read a stored result"},
    {"results.list", R::Open, "list stored results"},
    {"session.spawn", R::TurnOnly, "start a sub-agent"},
    {"session.tell", R::OwnerAttended, "message a sub-agent"},
    {"session.poll", R::Open, "read a sub-agent's reply"},
    {"session.list", R::Open, "list sub-agents"},
    {"session.terminate", R::OwnerAttended, "stop a sub-agent"},
    // ---- web, models (lib/harness, lib/core) ----
    {"web.search", R::TurnOnly, "search the web"},
    {"models.list", R::Open, "list the models"},
    {"models.list.refresh", R::Owner, "refresh the model lists"},
    // ---- files (src/agent/files_subsystem.cpp, pdf_tool.cpp): own namespace ----
    {"artifact.save", R::Open, "save a file"},
    {"files.append", R::Open, "add rows to a file"},
    {"files.fetch", R::Open, "download a file"},
    {"files.list", R::Open, "list files"},
    {"files.pdf", R::Open, "make a PDF"},
    {"files.read", R::Open, "read a file"},
    {"files.search", R::Open, "search files"},
    {"files.send", R::TurnOnly, "send a file"},
    {"files.send.other_chat", R::OwnerAttended, "send a file to another chat"},
    {"files.share", R::Open, "share a file"},   // a member shares only its own files
    // the owner shares any file (a member's too) with everyone who talks to the device
    {"files.share.owner", R::OwnerAttended, "share a file with everyone"},
    {"files.stat", R::Open, "look up a file"},
    {"image.generate", R::TurnOnly, "make an image"},
    {"image.look", R::TurnOnly, "look at a photo"},
    // ---- docs, routines, replies, skills, people (src/agent/memory_subsystem.cpp) ----
    {"docs.list", R::Open, "browse the docs"},
    {"docs.read", R::Open, "read the docs"},
    {"docs.search", R::Open, "search the docs"},
    // lane CTX: the food coach's plan arithmetic - stateless, it writes nothing
    {"food.portions", R::Open, "portion a meal"},
    {"loop.create", R::OwnerAttended, "set up a routine"},
    {"loop.cancel", R::OwnerAttended, "cancel a routine"},
    {"loop.list", R::Owner, "see the routines"},
    {"wakeup.set", R::Owner, "set a wakeup"},   // a wakeup may set the next one (a chain)
    {"reply.telegram", R::TurnOnly, "send a message"},
    {"reply.telegram.other_chat", R::OwnerAttended, "message another chat"},
    {"reply.speak", R::Owner, "speak aloud on the device"},
    {"skill.get", R::Open, "read a skill"},
    {"skill.list", R::Open, "list skills"},
    {"skill.save", R::OwnerAttended, "write a skill"},
    {"skill.delete", R::OwnerAttended, "delete a skill"},
    {"system.health", R::Open, "check the hardware"},
    {"tenant.list", R::OwnerAttended, "see the people list"},
    {"tenant.set_role", R::OwnerAttended, "change who may use the device"},
    {"tenant.set_quota", R::OwnerAttended, "change someone's storage limits"},
    // ---- device (src/main.cpp, src/agent/orchestrator.cpp) ----
    // the device status carries the owner's spend and budgets (the owner-only /usage),
    // the log the device's own records: the owner's, routines included
    {"device.status", R::Owner, "read the device status"},
    {"device.log", R::Owner, "read the device log"},
    {"device.selftest", R::Open, "run the self-test"},
    {"device.selftest.audible", R::OwnerAttended, "run the audible self-test"},
    {"device.control", R::Owner, "change device settings"},
    {"ota.status", R::Open, "read the update status"},
    // ---- media (src/sfx/music.cpp) ----
    {"media.list", R::Open, "list the music"},
    {"media.play", R::Owner, "play music on the speaker"},
    {"media.pause", R::Owner, "pause the music"},
    {"media.stop", R::Owner, "stop the music"},
};

// The owner's connected tool servers ("mcp.<server>.<tool>", registered at runtime):
// they act with the owner's own credentials on the owner's accounts.
const char kMcpPrefix[] = "mcp.";
const ActionRuleRow kMcpRow = {kMcpPrefix, R::OwnerAttended, "use the owner's connected tool servers"};
// A key with no row: refused to everyone but the owner writing now.
const ActionRuleRow kUnknownRow = {"", R::OwnerAttended, "do that"};

const ActionRuleRow& rowOrDefault(const std::string& key) {
  if (const ActionRuleRow* r = actionRuleRow(key)) return *r;
  if (key.rfind(kMcpPrefix, 0) == 0) return kMcpRow;
  return kUnknownRow;
}

const ConfigKnob kKnobs[] = {
    {"ledBrightness", &ValidatedAction::hasLedBrightness},
    {"priority", &ValidatedAction::hasPriority},
    {"posture", &ValidatedAction::hasPosture},
    {"profile", &ValidatedAction::hasProfile},
    {"theme", &ValidatedAction::hasTheme},
    {"attnHoldMs", &ValidatedAction::hasAttnHoldMs},
    {"ttsVoice", &ValidatedAction::hasTtsVoice},
    {"ttsProv", &ValidatedAction::hasTtsProv},
    {"ttsOn", &ValidatedAction::hasTtsOn},
    {"sttProv", &ValidatedAction::hasSttProv},
    {"sfxLvlN", &ValidatedAction::hasSfxLvlN},
    {"sfxLvlO", &ValidatedAction::hasSfxLvlO},
    {"sfxVol", &ValidatedAction::hasSfxVol},
    {"sfxTheme", &ValidatedAction::hasSfxTheme},
    {"sleepOvr", &ValidatedAction::hasSleepOvr},
    {"brightOvr", &ValidatedAction::hasBrightOvr},
    {"devName", &ValidatedAction::hasDevName},
};

bool truthy(ArduinoJson::JsonObjectConst a, const char* k) { return a[k].is<bool>() && a[k].as<bool>(); }

std::string str(ArduinoJson::JsonObjectConst a, const char* k) {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
}

// A message or a file sent somewhere other than the caller's own chat (for the owner,
// the chat the turn runs for; "" when unknown, so any explicit target is another chat).
bool otherChat(ArduinoJson::JsonObjectConst a, const ActionCaller& c) {
  const std::string target = str(a, "chat_id");
  return !target.empty() && target != c.chat;
}

// Turns on the device itself (web chat, voice, serial) have no Telegram chat of their
// own: files.send there goes to another chat (the owner's), never "back to the caller".
bool deviceChannel(const std::string& chat) {
  return chat.empty() || chat == "web" || chat == "voice" || chat == "serial";
}

}  // namespace

const char* actionRuleName(ActionRule r) {
  switch (r) {
    case ActionRule::Open:          return "open";
    case ActionRule::TurnOnly:      return "turn";
    case ActionRule::Owner:         return "owner";
    case ActionRule::OwnerAttended: return "owner-attended";
  }
  return "owner-attended";
}

ActionCaller actionCaller(const Principal& who, bool unattended, const std::string& turnChat) {
  ActionCaller c;
  c.role = who.role;
  c.unattended = unattended;
  c.lan = who.ns == kMcpNs;
  static const char kChat[] = "chat:";
  // A member's or a guest's chat is its namespace; the owner's namespace names no chat
  // (every owner channel is one person), so the owner's is the chat the turn runs for.
  if (who.ns.rfind(kChat, 0) == 0) c.chat = who.ns.substr(sizeof(kChat) - 1);
  else if (!c.lan) c.chat = turnChat;
  return c;
}

const ActionRuleRow* actionRuleRow(const std::string& key) {
  for (const ActionRuleRow& r : kRows)
    if (key == r.key) return &r;
  return nullptr;
}

ActionRule actionRuleFor(const std::string& key) { return rowOrDefault(key).rule; }

bool actionAllowed(const std::string& key, const ActionCaller& c) {
  switch (actionRuleFor(key)) {
    case ActionRule::Open:          return true;
    case ActionRule::TurnOnly:      return !c.lan && permsFor(c.role).writeOwn;   // an approved caller
    case ActionRule::Owner:         return c.owner();
    case ActionRule::OwnerAttended: return c.owner() && !c.unattended;
  }
  return false;
}

const ActionRuleRow* actionRuleTable(size_t& count) {
  count = sizeof(kRows) / sizeof(kRows[0]);
  return kRows;
}

std::string filesSendTarget(const std::string& turnChat, bool turnChatAllowed, bool owner,
                            const std::string& ownerChat) {
  if (!deviceChannel(turnChat) && turnChatAllowed) return turnChat;
  return owner ? ownerChat : std::string();
}

// The keys a message or a file send narrows to: another chat than the caller's own,
// and files.send naming no chat from a device channel, which goes to the owner's chat
// (filesSendTarget).
static std::string sendKey(const std::string& name, ArduinoJson::JsonObjectConst a, const ActionCaller& c) {
  if (otherChat(a, c)) return name + ".other_chat";
  if (name == "files.send" && str(a, "chat_id").empty() && deviceChannel(c.chat)) return "files.send.other_chat";
  return name;
}

// Sharing makes a file readable by everyone who talks to the device: the owner may
// share any file, so only when writing now; stopping a share stays open.
static std::string shareKey(ArduinoJson::JsonObjectConst a, const ActionCaller& c) {
  const bool unshare = a["share"].is<bool>() && !a["share"].as<bool>();
  return c.owner() && !unshare ? "files.share.owner" : "files.share";
}

std::string toolActionKey(const std::string& name, ArduinoJson::JsonObjectConst a, const ActionCaller& c) {
  if (name == "memory.config") return str(a, "action") == "update" ? "memory.config.update" : name;
  if (name == "models.list") return truthy(a, "refresh") ? "models.list.refresh" : name;
  if (name == "device.selftest") return truthy(a, "audible") ? "device.selftest.audible" : name;
  if (name == "prefs.edit") {
    const std::string act = str(a, "action");
    return act == "add" || act == "replace" || act == "remove" ? "prefs.edit.change" : name;
  }
  if (name == "reply.telegram" || name == "files.send") return sendKey(name, a, c);
  if (name == "files.share") return shareKey(a, c);
  return name;
}

std::string actionRefusal(const std::string& key, const ActionCaller& c) {
  if (actionAllowed(key, c)) return std::string();
  const ActionRuleRow& row = rowOrDefault(key);
  const std::string what = row.what;
  if (row.rule == ActionRule::TurnOnly && !c.lan) return "this conversation is not approved to " + what;
  if (c.lan)
    return row.rule == ActionRule::TurnOnly
               ? "the LAN connection cannot " + what + "; it works only inside a conversation with the device"
               : "the LAN connection cannot " + what + "; it is open to the device's owner in a conversation";
  if (!c.owner()) return "only the device's owner can " + what;
  // The owner's principal on an unattended turn (OwnerAttended only).
  return "only the owner can " + what + ", in a message they write; an automatic turn (a routine, a "
         "wakeup or a sub-agent results summary) cannot";
}

const char* deviceActionKey(ActionKind kind) {
  switch (kind) {
    case ActionKind::Led:       return "device.led";
    case ActionKind::Lights:    return "device.lights";
    case ActionKind::Tts:       return "device.tts";
    case ActionKind::Reboot:    return "device.reboot";
    case ActionKind::OrchModel: return "device.orch_model";
    case ActionKind::Config:    return "device.config";   // and each knob its own row
    case ActionKind::Unknown:
    default:                    return "";
  }
}

bool sessionVisible(const SessionInfo& s, const ActionCaller& c) {
  if (c.owner()) return true;
  if (c.lan || c.chat.empty()) return false;
  return !s.byOwner && s.chat == c.chat;
}

std::vector<SessionInfo> visibleSessions(const std::vector<SessionInfo>& all, const ActionCaller& c) {
  std::vector<SessionInfo> out;
  for (const SessionInfo& s : all)
    if (sessionVisible(s, c)) out.push_back(s);
  return out;
}

const ConfigKnob* configKnobTable(size_t& count) {
  count = sizeof(kKnobs) / sizeof(kKnobs[0]);
  return kKnobs;
}

std::string configKnobKey(const char* knob) { return std::string("device.config.") + (knob ? knob : ""); }

void installActionRules(ToolRegistry& reg, std::function<bool()> unattended,
                        std::function<std::string()> turnChat) {
  reg.setAccessResolver([unattended, turnChat](const std::string& name, ArduinoJson::JsonObjectConst args,
                                               const Principal& who) -> ToolRegistry::Verdict {
    ActionCaller c = actionCaller(who, unattended && unattended());
    // The owner's own chat matters only to the two tools that name a target, and the
    // turn's chat is read only for them (never for the LAN, never ambiently on every call).
    if (!c.lan && c.chat.empty() && turnChat && (name == "reply.telegram" || name == "files.send"))
      c.chat = turnChat();
    const std::string key = toolActionKey(name, args, c);
    const std::string why = actionRefusal(key, c);
    return why.empty() ? ToolRegistry::Verdict::allow() : ToolRegistry::Verdict::deny(why);
  });
}

}  // namespace orch
}  // namespace nimbus
