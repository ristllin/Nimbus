#include "nimbus/harness/turn_contract.h"

#include <cctype>
#include <set>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/reply_markup.h"
#include "nimbus/orch/spawn_claim.h"

namespace agent {

namespace orch = nimbus::orch;

namespace {

// At most this many salvaged items are held per turn (bounded memory: each task
// may be 4 KB); the merge then applies the per-turn spawn cap.
constexpr size_t kPseudoItemsMax = 2 * (size_t)orch::kAgentMaxJobs;

const std::string& taskOf(const orch::SessionOp& op) { return op.task.empty() ? op.message : op.task; }

size_t spawnCount(const orch::Turn& t) {
  size_t n = t.spawn.size();
  for (const auto& op : t.session_ops) n += op.op == "spawn";
  return n;
}

// A spawn the leak gate drops is reported like every other refusal (the
// conservation law): the head gets a [SPAWN REFUSED] note, the owner one line.
void reportGatedSpawn(orch::Turn& t, const std::string& name, const std::string& task) {
  orch::SpawnDrop d;
  d.name = name;
  size_t n = task.size() < 200 ? task.size() : 200;
  while (n > 0 && n < task.size() && ((unsigned char)task[n] & 0xC0) == 0x80) n--;   // UTF-8 safe
  d.task = task.substr(0, n);
  d.reason = "it came from tool markup that leaked into the reply text, and recovered actions run only "
             "on the owner's own live message with no outside content";
  t.spawnDrops.push_back(std::move(d));
}

std::string joinReasons(const std::vector<orch::SpawnDrop>& drops) {
  std::string out;
  for (const auto& d : drops) {
    if (!out.empty()) out += "; ";
    out += d.reason;
  }
  return out;
}

}  // namespace

std::string spawnDedupeKey(const std::string& provider, const std::string& task) {
  std::string key;
  for (char c : provider) key += (char)std::tolower((unsigned char)c);
  key += '|';
  bool space = false;
  size_t taken = 0;
  for (char c : task) {
    if (taken >= 200) break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = true;
      continue;
    }
    if (space && key.back() != '|') key += ' ';
    space = false;
    key += (char)std::tolower((unsigned char)c);
    taken++;
  }
  return key;
}

orch::HeadToolResult takePseudoSpawns(const orch::HeadToolCall& call, PseudoSpawns& sink) {
  sink.calls++;
  orch::Turn got;
  const int n = orch::spawnsFromToolArgs(call.argsJson, got);
  orch::HeadToolResult r;
  r.id = call.id;
  r.name = call.name;
  for (auto& d : got.spawnDrops) sink.drops.push_back(d);
  if (n == 0) return r;   // nothing usable: the registry's recovery text answers
  int taken = 0;
  for (auto& op : got.session_ops) {
    if (sink.items.size() >= kPseudoItemsMax) break;
    sink.items.push_back(std::move(op));
    taken++;
  }
  r.output = call.name + " is not a tool, but the " + std::to_string(taken) +
             " spawn item(s) in this call were taken as your orch_turn session_ops: they start "
             "when you finish this turn with orch_turn. Do not repeat them in session_ops; "
             "their results arrive by themselves in [FRESH RESULTS].";
  if (!got.spawnDrops.empty())
    r.output += " " + std::to_string(got.spawnDrops.size()) +
                " item(s) in the call could not be used: " + joinReasons(got.spawnDrops) + ".";
  return r;
}

int mergePseudoSpawns(orch::Turn& t, const PseudoSpawns& p) {
  std::set<std::string> keys;
  for (const auto& op : t.session_ops)
    if (op.op == "spawn") keys.insert(spawnDedupeKey(op.provider, taskOf(op)));
  for (const auto& s : t.spawn) keys.insert(spawnDedupeKey(s.provider, s.task));
  size_t spawns = spawnCount(t);
  int added = 0;
  for (const auto& item : p.items) {
    if (!keys.insert(spawnDedupeKey(item.provider, taskOf(item))).second) continue;   // already there
    if (spawns >= (size_t)orch::kAgentMaxJobs || t.session_ops.size() >= (size_t)orch::kAgentMaxJobs) {
      orch::SpawnDrop d;
      d.name = item.name;
      d.task = taskOf(item).substr(0, 200);
      d.reason = orch::spawnOverCapReason();
      t.spawnDrops.push_back(std::move(d));
      continue;
    }
    t.session_ops.push_back(item);
    spawns++;
    added++;
  }
  // A call's unusable items count only when the turn ends with no spawn at all:
  // a head that sent session_ops({}) and then a proper orch_turn fixed it.
  if (spawns == 0)
    for (const auto& d : p.drops) t.spawnDrops.push_back(d);
  return added;
}

namespace {

// What in a parsed final turn would lose work its reply promises.
void shapeIssues(const orch::Turn& t, size_t salvaged, std::vector<std::string>& issues) {
  for (const auto& d : t.spawnDrops)
    issues.push_back("a spawn item" + (d.name.empty() ? std::string() : " '" + d.name + "'") +
                     " would not start: " + d.reason);
  for (const auto& note : t.parseNotes) {
    // A field the device could not use at all (spawn lists are reported above).
    const bool lost = note.find(" arrived as ") != std::string::npos &&
                      note.find("not used") != std::string::npos;
    const bool spawnList = note.rfind("session_ops", 0) == 0 || note.rfind("spawn", 0) == 0;
    if (lost && !spawnList) issues.push_back(note);
  }
  if (spawnCount(t) == 0 && salvaged == 0 && orch::replyClaimsSpawn(t.reply))
    issues.push_back(
        "your reply says sub-agents were started, but this orch_turn carries no spawn item, so "
        "none would start - add one session_ops spawn item per sub-agent, or change the reply "
        "to say what actually happened");
}

}  // namespace

std::string turnShapeCorrection(const std::string& finalTurn, const PseudoSpawns* pseudo) {
  orch::Turn t;
  orch::ParseError e;
  std::vector<std::string> issues;
  if (orch::parseHeadReply(finalTurn, t, e) == orch::ReplyShape::Unusable)
    issues.push_back("the orch_turn could not be read (" + e.detail +
                     "); it must be one JSON object whose reply is a string");
  else
    shapeIssues(t, pseudo ? pseudo->items.size() : 0, issues);
  if (issues.empty()) return std::string();
  std::string out =
      "[TURN CHECK] This orch_turn was not applied: nothing in it ran and the owner has not "
      "seen it. Send the whole orch_turn again, fixed: ";
  for (size_t i = 0; i < issues.size(); i++) out += (i ? "; " : "") + issues[i];
  out += ". session_ops is a JSON list of objects, for example "
         "[{\"op\":\"spawn\",\"task\":\"<the request>\",\"provider\":null,\"model\":null,"
         "\"skill\":null,\"name\":\"<short name>\",\"project\":null,\"attach\":null}]. Do not "
         "mention this check to the owner.";
  return out;
}

const char kCutNothingSalvaged[] =
    "That answer was cut off before it finished, so nothing in it was carried out. Ask again, "
    "or ask for a shorter answer.";
const char kUnreadableNothingSalvaged[] =
    "That answer came back in a form that could not be read, so nothing in it was carried "
    "out. Ask again.";

SalvageOutcome salvageForOwner(const std::string& raw, const std::string& parseDetail,
                               bool reportedCut) {
  SalvageOutcome o;
  orch::Salvage sv;
  orch::salvageTurnText(raw, sv);
  const bool truncated = parseDetail.find("truncated") != std::string::npos;
  std::string text = orch::cleanReplyMarkup(sv.reply);
  const std::string ask = sv.askComplete ? orch::cleanReplyMarkup(sv.ask) : std::string();
  if (!ask.empty()) text += (text.empty() ? "" : "\n") + ask;
  o.cut = reportedCut || truncated || (!sv.reply.empty() && !sv.replyComplete);
  if (text.empty()) {
    o.text = o.cut ? kCutNothingSalvaged : kUnreadableNothingSalvaged;
    return o;
  }
  o.salvaged = true;
  o.text = o.cut ? text + "\n\n" + orch::kSalvageCutMarker : text;
  return o;
}

bool dropRecoveredContainers(orch::Turn& t) {
  const uint16_t m = t.repair.recoveredMask | t.repair.containerMask;
  if (!m) return false;
  auto has = [m](orch::TurnField f) { return (m & (1u << (unsigned)f)) != 0; };
  // A recovered note or question is quoted text too: the running notes are read
  // back into every later prompt, so a planted one would persist.
  if (has(orch::TurnField::Memory)) t.memory.clear();
  if (has(orch::TurnField::Ask)) t.ask.clear();
  if (has(orch::TurnField::Device)) t.device.clear();
  if (has(orch::TurnField::MemWrite)) t.mem_write.clear();
  if (has(orch::TurnField::MemQuery)) t.mem_query.clear();
  if (has(orch::TurnField::SessionOps)) {
    for (const auto& op : t.session_ops)
      if (op.op == "spawn") reportGatedSpawn(t, op.name, taskOf(op));
    t.session_ops.clear();
  }
  if (has(orch::TurnField::Scratchpad)) t.scratchpad = orch::ScratchUpdate{};
  if (has(orch::TurnField::Spawn)) {
    for (const auto& sp : t.spawn) reportGatedSpawn(t, sp.name, sp.task);
    t.spawn.clear();
  }
  if (has(orch::TurnField::Await)) t.await_.clear();
  return true;
}

}  // namespace agent
