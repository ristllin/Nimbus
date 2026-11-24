// The safety rails on the plain engine (plain/rails.h, lane RB-RL; PRD section 8.1).
#include "nimbus/harness/plain/rails.h"

#include <ArduinoJson.h>

#include <cstring>
#include <initializer_list>
#include <string>

#include "nimbus/harness/jobs.h"   // kOutsideContentOpen: the one fence (RB-Q2a)
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/action_rules.h"   // actionRuleRow
#include "nimbus/orch/path_rules.h"
#include "nimbus/orch/tool_groups.h"   // kMoreTool

namespace agent {
namespace plain {

namespace {

namespace orch = nimbus::orch;
namespace tx = nimbus::orch::tx;
using Use = orch::ToolUse;
using Verdict = RailVerdict;
using Args = RailArgs;

// THE table: what a turn that read outside content may still do. The class of a call is
// RB-Q2a's toolUseFor (one classification for both rails); the first row that matches decides;
// a call no row matches is refused. A row naming a tool comes before its class's catch-all row.
const RailRow kTaintExemptions[] = {
    {Use::Read, orch::kMoreTool, Args::Listing, Verdict::Allow},    // a listing of a group's tools
    {Use::Read, orch::kMoreTool, Args::Any, Verdict::Refuse},       // a `more` call whose target is not known
    {Use::Read, nullptr, Args::Any, Verdict::Allow},                // reads change nothing
    {Use::Write, nullptr, Args::Any, Verdict::Allow},               // the owner's files; every write says what changed
    {Use::Spend, "agent_start", Args::Any, Verdict::Pending},       // a task written after outside text, run with the owner's connectors
    {Use::Spend, nullptr, Args::Any, Verdict::Allow},               // web search, an image
    {Use::Ambient, nullptr, Args::Any, Verdict::Allow},             // the light and speech on the device
    {Use::Send, nullptr, Args::ThisChat, Verdict::Allow},           // to this chat only
    {Use::Schedule, "schedule", Args::ScheduleList, Verdict::Allow},
    {Use::Schedule, "schedule", Args::FixedReminder, Verdict::Allow},     // 'look it up and remind me at 7'
    {Use::Schedule, "schedule", Args::ScheduleChange, Verdict::Pending},  // a routine create or update
    {Use::Memory, nullptr, Args::Any, Verdict::Pending},            // note
    {Use::Scratch, nullptr, Args::Any, Verdict::Pending},
    {Use::Control, "skill_save", Args::Any, Verdict::Pending},
};
constexpr size_t kRowCount = sizeof(kTaintExemptions) / sizeof(kTaintExemptions[0]);

bool onlyKeys(ArduinoJson::JsonObjectConst a, std::initializer_list<const char*> keys) {
  for (ArduinoJson::JsonPairConst kv : a) {
    bool known = false;
    for (const char* k : keys) known = known || std::strcmp(kv.key().c_str(), k) == 0;
    if (!known) return false;
  }
  return true;
}

// No `chat_id`, or the turn's own chat (a string or a number).
bool thisChat(ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  ArduinoJson::JsonVariantConst c = a["chat_id"];
  if (c.isNull()) return true;
  if (c.is<const char*>()) return turn.chatId == c.as<const char*>();
  if (c.is<long long>()) return turn.chatId == std::to_string(c.as<long long>());
  return false;
}

std::string field(ArduinoJson::JsonObjectConst a, const char* key) {
  const char* v = a[key] | "";
  return v;
}

// An absent op reads as create.
bool isCreate(ArduinoJson::JsonObjectConst a) { return a["op"].isNull() || field(a, "op") == "create"; }
bool changeOp(ArduinoJson::JsonObjectConst a) { return isCreate(a) || field(a, "op") == "update"; }

// A new reminder only: an update names an id whose entry the rail cannot see (it may be a routine).
bool fixedReminder(ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  return isCreate(a) && field(a, "kind") == "reminder" && !field(a, "text").empty() && thisChat(a, turn) &&
         onlyKeys(a, {"op", "kind", "when", "text", "chat_id"});
}

bool argsMatch(RailArgs rule, ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  switch (rule) {
    case Args::Any: return true;
    case Args::Listing: return a["name"].isNull();
    case Args::ThisChat: return thisChat(a, turn) && onlyKeys(a, {"kind", "path", "ref", "text", "caption", "chat_id"});
    case Args::ScheduleList: return field(a, "op") == "list";
    case Args::FixedReminder: return fixedReminder(a, turn);
    case Args::ScheduleChange: return changeOp(a) && thisChat(a, turn);
  }
  return false;   // not a RailArgs value
}

// A name no static table knows (a tool the device declared at runtime with setClass): toolUseFor
// read it as Control by default, and its header says such a name reads by its registration class.
bool unplaced(const std::string& name) {
  return !orch::actionRuleRow(name) && !orch::toolClassRow(name) && !orch::toolTaintRow(name);
}

bool rowMatches(const RailRow& r, Use use, const ToolCall& call, ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  if (r.use != use || (r.tool && call.name != r.tool)) return false;
  return argsMatch(r.args, a, turn);
}

// The family of a store path's folder (its first segment, made valid as the store makes it).
bool outsideFolder(const std::string& path) {
  const size_t from = !path.empty() && path[0] == '/' ? 1 : 0;
  const size_t slash = path.find('/', from);
  const std::string first = path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
  if (first.empty()) return false;
  const orch::PathRule rule = orch::pathRuleFor(orch::validSegmentFor(first, orch::kStoreProjectMax), "x");
  return rule.kind == orch::Reserved::Inbox || rule.kind == orch::Reserved::Agents;
}

// A call's arguments as the engine reads them (viewOf's nesting limit); "" is no arguments.
bool parseArgs(const std::string& json, ArduinoJson::JsonDocument& doc) {
  if (json.empty()) return !deserializeJson(doc, "{}");
  return !deserializeJson(doc, json, ArduinoJson::DeserializationOption::NestingLimit(64)) &&
         doc.is<ArduinoJson::JsonObjectConst>();
}

}  // namespace

const char* railVerdictSlug(RailVerdict v) {
  switch (v) {
    case Verdict::Allow: return "allow";
    case Verdict::Pending: return "pending";
    case Verdict::Refuse: return "refuse";
  }
  return "refuse";   // not a RailVerdict value
}

const RailRow* taintExemptions(size_t& count) {
  count = kRowCount;
  return kTaintExemptions;
}

bool itemOutside(const tx::Item& it) {
  switch (it.kind) {
    case tx::ItemKind::Result: return it.cls == tx::ResultClass::Outside;
    case tx::ItemKind::DeviceNote:   // a sub-agent's report; a routine's reply the device fenced as outside
      return it.note == tx::NoteClass::Agent ||
             (it.note == tx::NoteClass::Routine && it.x.find(agent::kOutsideContentOpen) != std::string::npos);
    case tx::ItemKind::User:
      for (const tx::Part& p : it.parts)
        if (p.kind == tx::PartKind::File) return true;
      return false;
    default: return false;
  }
}

bool inputOutside(Trigger trigger, const std::vector<tx::Part>& parts) {
  if (trigger == Trigger::AgentResult) return true;
  for (const tx::Part& p : parts)
    if (p.kind == tx::PartKind::File) return true;
  return false;
}

bool readsOutsidePath(const std::string& tool, const std::string& argsJson) {
  ArduinoJson::JsonDocument doc;
  if (!parseArgs(argsJson, doc)) return true;   // what the rail cannot read, the tool may: outside
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  return outsideFolder(orch::callPath(tool, a)) || outsideFolder(field(a, "prefix"));
}

// The engine also opens a report turn tainted (inputOutside); the trigger is read here too so a turn
// context built elsewhere (a test, a binding) cannot leave it out.
bool turnTainted(const TurnCtx& turn) { return turn.tainted || turn.trigger == Trigger::AgentResult; }

Status railFor(const TurnCtx& turn, const ToolCall& call, bool declaredEffect, RailVerdict& out, size_t* row) {
  out = Verdict::Allow;
  if (row) *row = kRowCount;
  if (!turnTainted(turn)) return Status::success();
  out = Verdict::Refuse;   // fail closed from here on
  ArduinoJson::JsonDocument doc;
  if (!parseArgs(call.argsJson, doc)) return Status::success();
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  Use use = orch::toolUseFor(call.name, a);
  if (use == Use::Control && !declaredEffect && unplaced(call.name)) use = Use::Read;
  for (size_t i = 0; i < kRowCount; ++i) {
    if (!rowMatches(kTaintExemptions[i], use, call, a, turn)) continue;
    out = kTaintExemptions[i].verdict;
    if (row) *row = i;
    break;
  }
  return Status::success();
}

namespace {

DispatchDecision decide(const RailsDeps& deps, const DispatchCtx& dc) {
  DispatchDecision d;
  d.verdict = DispatchVerdict::Refuse;
  d.text = texts::tool_results::kRailRefused;
  if (!dc.turn || !dc.call) return d;   // the engine always passes both: anything else is refused
  const ToolCall& call = dc.target ? *dc.target : *dc.call;
  RailVerdict v = Verdict::Refuse;
  if (!railFor(*dc.turn, call, dc.effect, v).ok()) return d;
  if (v == Verdict::Allow) return DispatchDecision{};
  if (v == Verdict::Refuse) {
    if (deps.noted) deps.noted(*dc.turn, call, false);
    return d;
  }
  d.text = texts::tool_results::kRailNoPending;
  if (!deps.pending || !deps.pending(*dc.turn, call).ok()) {
    if (deps.noted) deps.noted(*dc.turn, call, false);
    return d;
  }
  d.verdict = DispatchVerdict::Answer;
  d.text = texts::tool_results::kRailPending;
  if (deps.noted) deps.noted(*dc.turn, call, true);
  return d;
}

}  // namespace

Status installRails(PlainHooks& hooks, const RailsDeps& deps) {
  return hooks.beforeDispatch(HookSlot::Rails, [deps](const DispatchCtx& dc) { return decide(deps, dc); });
}

}  // namespace plain
}  // namespace agent
