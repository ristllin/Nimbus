// The safety rails on the plain engine (plain/rails.h, lane RB-RL; PRD section 8.1).
#include "nimbus/harness/plain/rails.h"

#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/path_rules.h"
#include "nimbus/orch/tool_groups.h"   // kMoreTool

namespace agent {
namespace plain {

namespace {

namespace orch = nimbus::orch;
namespace tx = nimbus::orch::tx;
using U = orch::ToolUse;
using V = RailVerdict;
using G = RailArgs;

// THE table: what a turn that read outside content may still do. The class of a call is
// RB-Q2a's toolUseFor (one classification for both rails); the first row that matches decides;
// a call no row matches is refused. Rows naming a tool come before the rows of their class.
const RailRow kTaintExemptions[] = {
    {U::Read, orch::kMoreTool, G::Listing, V::Allow},    // a listing of a group's tools
    {U::Read, orch::kMoreTool, G::Any, V::Refuse},       // a `more` call whose target is not known
    {U::Read, nullptr, G::Any, V::Allow},                // reads change nothing
    {U::Write, nullptr, G::Any, V::Allow},               // the owner's files; every write says what changed
    {U::Spend, nullptr, G::Any, V::Allow},               // web search, a sub-agent, an image
    {U::Ambient, nullptr, G::Any, V::Allow},             // the light and speech on the device
    {U::Send, nullptr, G::ThisChat, V::Allow},           // to this chat only
    {U::Schedule, "schedule", G::ScheduleList, V::Allow},
    {U::Schedule, "schedule", G::FixedReminder, V::Allow},     // 'look it up and remind me at 7'
    {U::Schedule, "schedule", G::ScheduleChange, V::Pending},  // a routine create or update
    {U::Memory, nullptr, G::Any, V::Pending},            // note
    {U::Scratch, nullptr, G::Any, V::Pending},
    {U::Control, "skill_save", G::Any, V::Pending},
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

std::string str(ArduinoJson::JsonObjectConst a, const char* key) {
  const char* v = a[key] | "";
  return v;
}

// create or update (an absent op reads as create)
bool changeOp(ArduinoJson::JsonObjectConst a) {
  if (a["op"].isNull()) return true;
  const std::string op = str(a, "op");
  return op == "create" || op == "update";
}

bool fixedReminder(ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  return str(a, "kind") == "reminder" && changeOp(a) && !str(a, "text").empty() && thisChat(a, turn) &&
         onlyKeys(a, {"op", "kind", "when", "text", "id", "chat_id"});
}

bool argsMatch(RailArgs rule, ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
  switch (rule) {
    case G::Any: return true;
    case G::Listing: return a["name"].isNull();
    case G::ThisChat: return thisChat(a, turn) && onlyKeys(a, {"kind", "path", "ref", "text", "caption", "chat_id"});
    case G::ScheduleList: return str(a, "op") == "list";
    case G::FixedReminder: return fixedReminder(a, turn);
    case G::ScheduleChange: return changeOp(a) && thisChat(a, turn);
  }
  return false;
}

bool rowMatches(const RailRow& r, U use, const ToolCall& call, ArduinoJson::JsonObjectConst a, const TurnCtx& turn) {
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

}  // namespace

const char* railVerdictSlug(RailVerdict v) {
  switch (v) {
    case V::Allow: return "allow";
    case V::Pending: return "pending";
    case V::Refuse: return "refuse";
  }
  return "refuse";
}

const RailRow* taintExemptions(size_t& count) {
  count = kRowCount;
  return kTaintExemptions;
}

bool itemOutside(const tx::Item& it) {
  switch (it.kind) {
    case tx::ItemKind::Result: return it.cls == tx::ResultClass::Outside;
    case tx::ItemKind::DeviceNote: return it.note == tx::NoteClass::Agent;
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
  if (deserializeJson(doc, argsJson, ArduinoJson::DeserializationOption::NestingLimit(8)) ||
      !doc.is<ArduinoJson::JsonObjectConst>())
    return false;
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  return outsideFolder(orch::callPath(tool, a)) || outsideFolder(str(a, "prefix"));
}

bool turnTainted(const TurnCtx& turn) { return turn.tainted || turn.trigger == Trigger::AgentResult; }

Status railFor(const TurnCtx& turn, const ToolCall& call, bool declaredEffect, RailVerdict& out, size_t* row) {
  out = V::Allow;
  if (row) *row = kRowCount;
  if (!turnTainted(turn)) return Status::success();
  out = V::Refuse;   // fail closed from here on
  ArduinoJson::JsonDocument doc;
  if (deserializeJson(doc, call.argsJson, ArduinoJson::DeserializationOption::NestingLimit(64)) ||
      !doc.is<ArduinoJson::JsonObjectConst>())
    return Status::success();
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  U use = orch::toolUseFor(call.name, a);
  if (use == U::Control && !declaredEffect) use = U::Read;   // toolUseFor's contract: no row, a declared read
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
  RailVerdict v = V::Refuse;
  if (!railFor(*dc.turn, call, dc.effect, v).ok()) return d;
  if (v == V::Allow) return DispatchDecision{};
  if (v == V::Refuse) return d;
  d.text = texts::tool_results::kRailNoPending;
  if (!deps.pending || !deps.pending(*dc.turn, call).ok()) return d;
  d.verdict = DispatchVerdict::Answer;
  d.text = texts::tool_results::kRailPending;
  return d;
}

}  // namespace

Status installRails(PlainHooks& hooks, const RailsDeps& deps) {
  return hooks.beforeDispatch(HookSlot::Rails, [deps](const DispatchCtx& dc) { return decide(deps, dc); });
}

}  // namespace plain
}  // namespace agent
