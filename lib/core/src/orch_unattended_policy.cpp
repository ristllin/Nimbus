#include "nimbus/orch/unattended_policy.h"

#include <cstring>

#include "nimbus/orch/action_rules.h"     // toolActionKey, actionRuleRow: every tool the old engine registers
#include "nimbus/orch/device_actions.h"   // validateAction: the kind of a device[] element

namespace nimbus {
namespace orch {

namespace {

using U = ToolUse;
constexpr Grant N = Grant::No;
constexpr Grant Y = Grant::Yes;
constexpr Grant L = Grant::ListedPaths;

// One row per class (ruling 76). Columns: Read, Write, Memory, Send, Spend, Schedule,
// Ambient, Control, Scratch. A routine, a reminder, a wakeup and a held note send only through
// their own reply (held delivery); messages and files to a chat other than the turn's own are
// the owner's, writing now, in every class (action_rules.h, OwnerAttended).
const UnattendedPolicyRow kPolicy[] = {
    {TurnClass::Attended, ModelCall::Sub, Output::Reply, true, {Y, Y, Y, Y, Y, Y, Y, Y, Y}},
    {TurnClass::UpdateNotice, ModelCall::None, Output::Fixed, false, {N, N, N, N, N, N, N, N, N}},
    // the owner's routine reads, sets its follow-up and replies; his switch adds his
    // connectors and sub-agents (UnattendedFacts::routineGrants); writes only to its listed paths
    {TurnClass::Routine, ModelCall::Sub, Output::Reply, true, {Y, L, N, N, N, Y, N, N, N}},
    // the owner's own one-time reminder ("lights off at 23:00", or said aloud)
    {TurnClass::Reminder, ModelCall::Sub, Output::Reply, true, {Y, N, N, N, N, N, Y, N, N}},
    // a one-time wakeup the assistant set, mostly at the owner's word in chat ("lights off
    // at 23:00", "remind me at 7, out loud"): the light and speech, and the next hop
    {TurnClass::Wakeup, ModelCall::Sub, Output::Reply, true, {Y, N, N, N, N, Y, Y, N, N}},
    // a held note: one short request, no tools
    {TurnClass::SelfWakeup, ModelCall::Sub, Output::Reply, false, {N, N, N, N, N, N, N, N, N}},
    // results arriving (PRD section 6): its scratchpad (the wave checklist), a named artifact
    // to its own chat, the next wave inside the run's caps (jobs.h); memory and files closed
    {TurnClass::SubagentReport, ModelCall::Sub, Output::Reply, true, {Y, N, N, Y, Y, N, N, N, Y}},
    // the nightly upkeep grooms the assistant's own memory and sends nothing
    {TurnClass::Dream, ModelCall::Sub, Output::Silent, true, {Y, N, Y, N, N, N, N, N, Y}},
};

// The tool class of each name or argument-narrowed key that is not a plain read, and of the
// engine-2 reads (whose names have no action rule). Any other name the action rules know (a
// class test keeps every registered tool in that table) is a read; a name neither table knows
// (a connected server's tool, anything unknown) is Control (fail closed). test_unattended_policy
// holds this table to tool_groups.h: no tool registered as an effect reads as Read.
struct UseRow {
  const char* key;
  ToolUse use;
};
const UseRow kUses[] = {
    // files the owner keeps (engine 1, then engine 2)
    {"files.append", U::Write},
    {"files.edit", U::Write},
    {"files.delete", U::Write},
    {"files.fetch", U::Write},   // saves a download as a file
    {"files.pdf", U::Write},
    {"artifact.save", U::Write},
    {"write_file", U::Write},
    {"append_file", U::Write},
    {"edit_file", U::Write},
    {"delete_file", U::Write},
    {"move_file", U::Write},
    {"fetch_url", U::Write},
    {"pdf_write", U::Write},
    {"ledger_write", U::Write},
    {"ledger_target", U::Write},
    // the assistant's own memory and notes
    {"memory.write", U::Memory},
    {"memory.update", U::Memory},
    {"memory.pin", U::Memory},
    {"memory.scratchpad", U::Scratch},
    {"memory.archive", U::Memory},   // a restore writes
    {"openq.edit.change", U::Memory},
    {"image.note", U::Memory},   // records what a photo showed
    {"turn.scratchpad", U::Scratch},
    {"note", U::Memory},
    // messages and files to a chat
    {"reply.telegram", U::Send},
    {"reply.telegram.other_chat", U::Send},
    {"files.send", U::Send},
    {"files.send.other_chat", U::Send},
    {"send", U::Send},
    // paid work beyond the turn
    {"session.spawn", U::Spend},
    {"session.tell", U::Spend},
    {"turn.spawn", U::Spend},
    {"image.generate", U::Spend},
    {"image.look", U::Spend},   // a paid vision request
    {"web.search", U::Spend},   // a paid search
    {"web_search", U::Spend},
    {"agent_start", U::Spend},
    {"image_generate", U::Spend},
    // routines and wakeups
    {"loop.create", U::Schedule},
    {"loop.cancel", U::Schedule},
    {"wakeup.set", U::Schedule},
    {"schedule", U::Schedule},
    // the device's light, sound, music and speech on its own speaker (a reminder said aloud)
    {"reply.speak", U::Ambient},
    {"device.tts", U::Ambient},
    {"speak", U::Ambient},
    {"device.led", U::Ambient},
    {"device.lights", U::Ambient},
    {"media.play", U::Ambient},
    {"media.pause", U::Ambient},
    {"media.stop", U::Ambient},
    {"device.selftest.audible", U::Ambient},
    // settings, skills, people, the head
    {"device.reboot", U::Control},
    {"device.orch_model", U::Control},
    {"prefs.edit.change", U::Control},
    {"memory.config.update", U::Control},
    {"models.list.refresh", U::Control},
    {"skill.save", U::Control},
    {"skill.delete", U::Control},
    {"skill_save", U::Control},
    {"skill_delete", U::Control},
    {"tenant.set_role", U::Control},
    {"tenant.set_quota", U::Control},
    {"session.terminate", U::Control},
    {"agent_stop", U::Control},
    {"files.share", U::Control},   // makes a file readable by everyone who talks to the device
    {"files.share.owner", U::Control},
    {"share_file", U::Control},
    // engine 2's reads (they have no action rule row)
    {"read_file", U::Read},
    {"list_files", U::Read},
    {"search", U::Read},
    {"view", U::Read},
    {"more", U::Read},   // a router: each tool it reaches is its own dispatch, checked on its own
    {"calc", U::Read},
    {"device_status", U::Read},
    {"device_log", U::Read},
    {"ota_status", U::Read},
    {"selftest", U::Read},
    {"system_health", U::Read},
    {"agent_list", U::Read},
    {"file_stat", U::Read},
    {"read_pdf", U::Read},
    {"skill_list", U::Read},
};

const UseRow* useRow(const std::string& key) {
  for (const UseRow& r : kUses)
    if (key == r.key) return &r;
  return nullptr;
}

ToolUse useOfKey(const std::string& key) {
  if (const UseRow* r = useRow(key)) return r->use;
  return actionRuleRow(key) ? U::Read : U::Control;
}

// A config element by its knobs: brightness, theme and sound level are Ambient, any other
// knob makes the element a setting (Control).
ToolUse useOfConfig(const ValidatedAction& va) {
  static const char* const kAmbient[] = {"ledBrightness", "theme", "sfxLvlN", "sfxLvlO", "sfxVol", "sfxTheme"};
  size_t n = 0;
  const ConfigKnob* k = configKnobTable(n);
  for (size_t i = 0; i < n; ++i) {
    if (!(va.*(k[i].has))) continue;
    bool ambient = false;
    for (const char* a : kAmbient) ambient = ambient || !strcmp(a, k[i].knob);
    if (!ambient) return U::Control;
  }
  return U::Ambient;
}

// A device action by its kind: light, sound and speech Ambient, config by knob, the rest Control.
ToolUse useOfDeviceAction(ArduinoJson::JsonVariantConst el) {
  const ValidatedAction va = validateAction(el);
  if (va.kind == ActionKind::Config) return useOfConfig(va);
  const char* key = deviceActionKey(va.kind);
  return key && *key ? useOfKey(key) : U::Control;
}

const char* classNoun(TurnClass c) {
  switch (c) {
    case TurnClass::UpdateNotice:   return "the update notice";
    case TurnClass::Reminder:       return "a reminder";
    case TurnClass::Wakeup:         return "a wakeup";
    case TurnClass::SelfWakeup:     return "a held note";
    case TurnClass::SubagentReport: return "a sub-agent report";
    case TurnClass::Dream:          return "the nightly upkeep";
    default:                        return "a routine";   // Routine (Attended is never refused)
  }
}

// What a tool class does, as the noun the refusal names.
const char* useNoun(ToolUse u) {
  switch (u) {
    case U::Write:    return "changes to files";
    case U::Memory:
    case U::Scratch:  return "memory and note writes";
    case U::Send:     return "messages and files to a chat (its reply is what reaches the owner)";
    case U::Spend:    return "paid work such as sub-agents or images";
    case U::Schedule: return "routine and wakeup changes";
    case U::Ambient:  return "the light, sound and speech";
    default:          return "settings, skills and permissions";   // Control (a Read is never refused)
  }
}

std::string refusalFor(TurnClass c, ToolUse u) {
  return std::string("Not run: ") + classNoun(c) + " runs with nobody writing, and " + useNoun(u) +
         " are closed to it. Nothing was changed.";
}

void cutField(bool present, ToolUse use, const UnattendedFacts& f, const char* field, int& cut,
              std::string& refusals) {
  if (!present || unattendedGranted(f, use)) return;
  ++cut;
  refusals += std::string(field) + ": " + refusalFor(f.cls, use) + "\n";
}

bool isFileSend(const std::string& name) { return name == "files.send" || name == "send"; }

}  // namespace

const char* turnClassSlug(TurnClass c) {
  switch (c) {
    case TurnClass::Attended:       return "attended";
    case TurnClass::UpdateNotice:   return "update-notice";
    case TurnClass::Routine:        return "routine";
    case TurnClass::Reminder:       return "reminder";
    case TurnClass::Wakeup:         return "wakeup";
    case TurnClass::SelfWakeup:     return "self-wakeup";
    case TurnClass::SubagentReport: return "subagent-report";
    case TurnClass::Dream:          return "dream";
  }
  return "routine";
}

const char* toolUseSlug(ToolUse u) {
  switch (u) {
    case U::Read:     return "read";
    case U::Write:    return "write";
    case U::Memory:   return "memory";
    case U::Send:     return "send";
    case U::Spend:    return "spend";
    case U::Schedule: return "schedule";
    case U::Ambient:  return "ambient";
    case U::Control:  return "control";
    case U::Scratch:  return "scratch";
  }
  return "control";
}

const UnattendedPolicyRow& unattendedPolicy(TurnClass c) {
  for (const UnattendedPolicyRow& r : kPolicy)
    if (r.cls == c) return r;
  return kPolicy[(int)TurnClass::UpdateNotice];   // an unknown class grants nothing
}

const UnattendedPolicyRow* unattendedPolicyTable(size_t& count) {
  count = sizeof(kPolicy) / sizeof(kPolicy[0]);
  return kPolicy;
}

TurnClass fireClass(bool dream, bool selfWakeup, bool ownerReminder, bool once) {
  if (dream) return TurnClass::Dream;
  if (selfWakeup) return TurnClass::SelfWakeup;
  if (ownerReminder) return TurnClass::Reminder;
  if (once) return TurnClass::Wakeup;
  return TurnClass::Routine;
}

ToolUse toolUseFor(const std::string& name, ArduinoJson::JsonObjectConst args) {
  if (name == "device.control" || name == "device_control") {
    if (!args["action"].is<ArduinoJson::JsonObjectConst>()) return U::Control;
    return useOfDeviceAction(args["action"]);
  }
  // The narrowed key (a change to the standing rules, an audible self test, a refresh);
  // the send narrowing (another chat) stays Send. No caller facts: a class, not a verdict.
  return useOfKey(toolActionKey(name, args, ActionCaller{}));
}

std::string callPath(const std::string& tool, ArduinoJson::JsonObjectConst args) {
  const char* name = args["name"].is<const char*>() ? args["name"].as<const char*>() : "";
  if (tool.find('.') != std::string::npos) {   // the old engine's file tools
    if (args["project"].is<const char*>() && *name) return std::string(args["project"].as<const char*>()) + "/" + name;
    return name;
  }
  return args["path"].is<const char*>() ? args["path"].as<const char*>() : name;
}

bool pathListed(const std::string& path, const std::vector<std::string>& listed) {
  if (path.empty()) return false;
  const std::string p = path[0] == '/' ? path.substr(1) : path;
  if (p.empty() || p.find("..") != std::string::npos) return false;
  for (const std::string& raw : listed) {
    const std::string e = !raw.empty() && raw[0] == '/' ? raw.substr(1) : raw;
    if (e.empty()) continue;
    if (e.back() == '/' ? p.compare(0, e.size(), e) == 0 : p == e) return true;
  }
  return false;
}

bool unattendedGranted(const UnattendedFacts& f, ToolUse u, bool pathListedNow) {
  if (f.cls == TurnClass::Attended) return true;
  if (u == U::Schedule && (f.outside || f.hops >= kMaxWakeHops)) return false;
  if (u == U::Spend && f.cls == TurnClass::Routine && f.routineGrants) return true;
  const Grant g = unattendedPolicy(f.cls).grant[(int)u];
  return g == Grant::Yes || (g == Grant::ListedPaths && pathListedNow);
}

std::string unattendedRefusal(const UnattendedFacts& f, const std::string& name, ArduinoJson::JsonObjectConst args) {
  if (f.cls == TurnClass::Attended) return std::string();
  const ToolUse u = toolUseFor(name, args);
  const bool listedNow = u == U::Write && f.listed && pathListed(callPath(name, args), *f.listed);
  bool ok = unattendedGranted(f, u, listedNow);
  // Outside content seen: a send carries only a file the results' envelopes name.
  if (ok && u == U::Send && f.outside)
    ok = isFileSend(name) && f.artifacts && pathListed(callPath(name, args), *f.artifacts);
  return ok ? std::string() : refusalFor(f.cls, u);
}

bool unattendedMayList(const UnattendedFacts& f, const std::string& name) {
  if (f.cls == TurnClass::Attended) return true;
  if (!unattendedPolicy(f.cls).tools) return false;
  // device.control is classed by its action: offered when the light and speech are open
  // (a reboot or a setting through it is still refused at the call).
  if (name == "device.control" || name == "device_control") return unattendedGranted(f, U::Ambient);
  return unattendedGranted(f, toolUseFor(name, ArduinoJson::JsonObjectConst()), f.listed && !f.listed->empty());
}

namespace {

// One end-of-turn field's items, all of one class: kept when granted, else cut with one line.
template <typename T>
void cutAll(std::vector<T>& items, ToolUse use, const UnattendedFacts& f, const char* field, int& cut,
            std::string& refusals) {
  if (items.empty() || unattendedGranted(f, use)) return;
  cut += (int)items.size() - 1;
  cutField(true, use, f, field, cut, refusals);
  items.clear();
}

// Items of mixed classes: each kept or cut by its own class.
template <typename T, typename UseOf>
void cutEach(std::vector<T>& items, UseOf useOf, const UnattendedFacts& f, const char* field, int& cut,
             std::string& refusals) {
  std::vector<T> kept;
  for (T& item : items) {
    const ToolUse u = useOf(item);
    if (unattendedGranted(f, u)) kept.push_back(std::move(item));
    else cutField(true, u, f, field, cut, refusals);
  }
  items.swap(kept);
}

ToolUse useOfSessionOp(const SessionOp& op) {
  if (op.op == "spawn" || op.op == "tell") return U::Spend;
  return op.op == "terminate" ? U::Control : U::Read;
}

ToolUse useOfDeviceJson(const DeviceAction& da) {
  ArduinoJson::JsonDocument doc;
  if (deserializeJson(doc, da.json.c_str())) return U::Control;
  return useOfDeviceAction(doc.as<ArduinoJson::JsonVariantConst>());
}

}  // namespace

int restrictUnattendedTurn(Turn& t, const UnattendedFacts& f, std::string& refusals) {
  if (f.cls == TurnClass::Attended) return 0;
  int cut = 0;
  cutField(!t.memory.empty(), U::Memory, f, "memory", cut, refusals);
  if (!unattendedGranted(f, U::Memory)) t.memory.clear();
  cutField(t.scratchpad.present, U::Scratch, f, "scratchpad", cut, refusals);
  if (!unattendedGranted(f, U::Scratch)) t.scratchpad = ScratchUpdate{};
  cutAll(t.mem_write, U::Memory, f, "mem_write", cut, refusals);
  cutAll(t.spawn, U::Spend, f, "spawn", cut, refusals);
  if (!unattendedGranted(f, U::Read)) {   // a class with no model call reads nothing either
    cutField(!t.mem_query.empty() || !t.await_.empty(), U::Read, f, "mem_query", cut, refusals);
    t.mem_query.clear();
    t.await_.clear();
  }
  cutEach(t.session_ops, useOfSessionOp, f, "session_ops", cut, refusals);
  cutEach(t.device, useOfDeviceJson, f, "device", cut, refusals);
  return cut;
}

std::string updateNoticeText(const std::string& version, const std::string& notes) {
  std::string text = "Updated to " + version + " \xE2\x9C\x93";   // the check mark the notice always had
  size_t end = notes.find_first_of("\r\n");
  std::string line = notes.substr(0, end);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
  size_t start = line.find_first_not_of(" \t");
  if (start == std::string::npos) return text;
  line = line.substr(start);
  if (line.size() > kUpdateNoteMax) {
    size_t cut = kUpdateNoteMax;
    while (cut > 0 && ((unsigned char)line[cut] & 0xC0) == 0x80) --cut;   // never inside a character
    line = line.substr(0, cut);
  }
  return text + "\n" + line;
}

}  // namespace orch
}  // namespace nimbus
