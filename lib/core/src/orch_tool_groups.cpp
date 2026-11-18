#include "nimbus/orch/tool_groups.h"

#include "nimbus/orch/tool_registry.h"

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonPairConst;
using ArduinoJson::JsonVariantConst;

namespace nimbus {
namespace orch {

namespace {

using EC = EffectClass;

// The groups behind more, in tools-index order (PRD section 4). "memory" holds the old
// engine's memory administration, which engine 2 deletes; "connectors" is where a tool
// nobody declared lands (a connected server's remote tool).
const ToolGroup kGroups[] = {
    {"device", "device state, control, updates, self test, health, log"},
    {"agents", "sub-agent jobs"},
    {"files_admin", "file deletion, moves, downloads, sharing, stat, PDFs"},
    {"media", "images, speech, music"},
    {"food", "targets and portions"},
    {"people", "who may use the device (admin only)"},
    {"models", "the providers' model lists"},
    {"docs", "the device's own documentation"},
    {"skills", "saved skills"},
    {"memory", "old-engine memory administration"},
    {ToolClass::kConnectorsGroup, "connected tool servers"},
};

// One row per tool: its group ("" always on) and its effect class. Taint is not here: it
// has one source, tool_taint.h. A lane registering a tool adds its row here (and its
// taint row there); a name with no row reads as connectors, outside, effect.
const ToolClassRow kRows[] = {
    // ---- engine 2, PRD section 4 (the plan; a row only lists once its tool registers) ----
    {"read_file", "", EC::Pure},
    {"write_file", "", EC::Effect},
    {"append_file", "", EC::Effect},
    {"edit_file", "", EC::Effect},
    {"list_files", "", EC::Pure},
    {"note", "", EC::Effect},
    {"search", "", EC::Pure},
    {"web_search", "", EC::Read},
    {"view", "", EC::Read},
    {"send", "", EC::Effect},
    {"schedule", "", EC::Effect},
    {"ledger_write", "", EC::Effect},
    {"ledger_day", "", EC::Pure},
    {"more", "", EC::Read},   // a router: callClass reads the class of the tool it reaches
    {"device_status", "device", EC::Read},
    {"device_control", "device", EC::Effect},
    {"ota_status", "device", EC::Read},
    {"selftest", "device", EC::Read},
    {"system_health", "device", EC::Read},
    {"device_log", "device", EC::Read},
    {"agent_start", "agents", EC::Effect},
    {"agent_list", "agents", EC::Read},
    {"agent_stop", "agents", EC::Effect},
    {"delete_file", "files_admin", EC::Effect},
    {"move_file", "files_admin", EC::Effect},
    {"fetch_url", "files_admin", EC::Effect},   // saves to path (Ruling 11)
    {"share_file", "files_admin", EC::Effect},
    {"file_stat", "files_admin", EC::Read},
    {"read_pdf", "files_admin", EC::Read},
    {"pdf_write", "files_admin", EC::Effect},
    {"image_generate", "media", EC::Effect},
    {"speak", "media", EC::Effect},
    {"ledger_target", "food", EC::Effect},
    {"food_portions", "food", EC::Read},
    {"skill_list", "skills", EC::Read},
    {"skill_save", "skills", EC::Effect},
    {"skill_delete", "skills", EC::Effect},
    // ---- the old engine's registered tools, grouped by their PRD section 4 fate ----
    {"files.read", "", EC::Pure},
    {"files.list", "", EC::Pure},
    {"files.search", "", EC::Pure},
    {"files.append", "", EC::Effect},
    {"files.edit", "", EC::Effect},
    {"artifact.save", "", EC::Effect},
    {"files.send", "", EC::Effect},
    {"reply.telegram", "", EC::Effect},
    {"memory.write", "", EC::Effect},
    {"prefs.edit", "", EC::Effect},
    {"memory.search", "", EC::Pure},
    {"memory.episodic", "", EC::Pure},
    {"web.search", "", EC::Read},
    {"results.get", "", EC::Read},
    {"image.look", "", EC::Read},
    {"image.note", "", EC::Read},
    {"loop.create", "", EC::Effect},
    {"loop.cancel", "", EC::Effect},
    {"loop.list", "", EC::Read},
    {"wakeup.set", "", EC::Effect},
    {"device.status", "device", EC::Read},
    {"device.control", "device", EC::Effect},
    {"device.log", "device", EC::Read},
    {"device.selftest", "device", EC::Read},
    {"system.health", "device", EC::Read},
    {"ota.status", "device", EC::Read},
    {"session.spawn", "agents", EC::Effect},
    {"session.tell", "agents", EC::Effect},
    {"session.poll", "agents", EC::Read},
    {"session.terminate", "agents", EC::Effect},
    {"session.list", "agents", EC::Read},
    {"files.delete", "files_admin", EC::Effect},
    {"files.fetch", "files_admin", EC::Effect},
    {"files.share", "files_admin", EC::Effect},
    {"files.stat", "files_admin", EC::Read},
    {"files.pdf", "files_admin", EC::Effect},
    {"image.generate", "media", EC::Effect},
    {"reply.speak", "media", EC::Effect},
    {"media.list", "media", EC::Read},
    {"media.play", "media", EC::Effect},
    {"media.pause", "media", EC::Effect},
    {"media.stop", "media", EC::Effect},
    {"food.portions", "food", EC::Read},
    {"tenant.list", "people", EC::Read},
    {"tenant.set_role", "people", EC::Effect},
    {"tenant.set_quota", "people", EC::Effect},
    {"models.list", "models", EC::Read},
    {"docs.list", "docs", EC::Read},
    {"docs.read", "docs", EC::Read},
    {"docs.search", "docs", EC::Read},
    {"skill.list", "skills", EC::Read},
    {"skill.get", "skills", EC::Read},
    {"skill.save", "skills", EC::Effect},
    {"skill.delete", "skills", EC::Effect},
    {"memory.update", "memory", EC::Effect},
    {"memory.pin", "memory", EC::Effect},
    {"memory.config", "memory", EC::Effect},
    {"memory.archive", "memory", EC::Effect},
    {"memory.scratchpad", "memory", EC::Effect},
    {"openq.edit", "memory", EC::Effect},
    {"results.list", "memory", EC::Read},
};

constexpr size_t kDescMax = 240;   // a listed description (PRD section 4)
constexpr size_t kEchoMax = 64;    // a model-written name echoed in an error

}  // namespace

const char kMoreDescription[] = "List a group's tools with their arguments (group), or call one (name, args).";
const char kMoreSchema[] =
    R"({"type":"object","properties":{"group":{"type":"string"},"name":{"type":"string"},"args":{"type":"object"}}})";

const char* effectClassName(EffectClass e) {
  switch (e) {
    case EffectClass::Pure: return "pure";
    case EffectClass::Read: return "read";
    default:                return "effect";
  }
}

const ToolClassRow* toolClassRow(const std::string& name) {
  for (const ToolClassRow& r : kRows)
    if (name == r.name) return &r;
  return nullptr;
}

ToolClass toolClassFor(const std::string& name) {
  ToolClass c;
  c.taint = toolTaint(name);
  if (const ToolClassRow* r = toolClassRow(name)) {
    c.group = r->group;
    c.effect = r->effect;
  }
  return c;
}

const ToolClassRow* toolClassTable(size_t& count) {
  count = sizeof(kRows) / sizeof(kRows[0]);
  return kRows;
}

const ToolGroup* toolGroupTable(size_t& count) {
  count = sizeof(kGroups) / sizeof(kGroups[0]);
  return kGroups;
}

bool isToolGroup(const std::string& name) {
  for (const ToolGroup& g : kGroups)
    if (name == g.name) return true;
  return false;
}

std::string spelledFor(const Principal& who, const std::string& regName) {
  if (who.ns == kMcpNs) return regName;
  std::string out = regName;
  for (char& c : out)
    if (c == '.') c = '_';
  return out;
}

// ---- what a caller may reach -----------------------------------------------------------

namespace {

// Listed, and callable through more, for this caller: registered, not a router, its gate
// open, no policy refusal, not admin-only for a non-admin, and not refused by the access
// rules for a call with no arguments. A direct call checks all of these but admin-only
// (the handler refuses there), so more is never wider than a direct call.
bool reachable(const ToolRegistry& reg, const std::string& name, const Principal& who) {
  if (!reg.has(name) || reg.isRouter(name) || !reg.gateOpen(name)) return false;
  if (reg.isAdminOnly(name) && !who.perms().manageTenants) return false;
  if (reg.policyFor(name).kind != ToolRegistry::Verdict::Allow) return false;
  return reg.accessFor(name, JsonObjectConst(), who).kind == ToolRegistry::Verdict::Allow;
}

// "name(path, limit?)": every argument with '?' on the optional ones, or with
// requiredOnly the required ones alone.
std::string signature(const std::string& spelled, const std::string& schemaJson, bool requiredOnly) {
  JsonDocument d;
  std::string out = spelled + "(";
  if (!deserializeJson(d, schemaJson)) {
    JsonArrayConst req = d["required"].as<JsonArrayConst>();
    auto required = [&req](const char* k) {
      for (JsonVariantConst v : req)
        if (v.is<const char*>() && std::string(v.as<const char*>()) == k) return true;
      return false;
    };
    bool first = true;
    for (JsonPairConst kv : d["properties"].as<JsonObjectConst>()) {
      const bool r = required(kv.key().c_str());
      if (requiredOnly && !r) continue;
      out += (first ? "" : ", ") + std::string(kv.key().c_str()) + (r ? "" : "?");
      first = false;
    }
  }
  return out + ")";
}

// Cut at most `max` bytes, never inside a UTF-8 character.
std::string clipUtf8(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  return s.substr(0, n);
}

// Lines in order while they fit `cap` with room for a tail naming the rest; the tail is
// `tailLead` and the names that did not fit, itself clipped to the cap.
std::string fitLines(const std::vector<std::string>& lines, const std::vector<std::string>& names,
                     const std::string& tailLead, size_t cap) {
  std::string all;
  for (const std::string& l : lines) all += (all.empty() ? "" : "\n") + l;
  if (all.size() <= cap) return all;
  size_t reserve = tailLead.size() + 1;
  for (const std::string& n : names) reserve += n.size() + 2;
  if (reserve > cap / 2) reserve = cap / 2;
  std::string out, tail;
  for (size_t i = 0; i < lines.size(); i++) {
    if (tail.empty() && out.size() + lines[i].size() + 1 <= cap - reserve) {
      out += (out.empty() ? "" : "\n") + lines[i];
      continue;
    }
    tail += (tail.empty() ? tailLead : std::string(", ")) + names[i];
  }
  out += (out.empty() ? "" : "\n") + tail;
  return clipUtf8(out, cap);
}

const char* groupOf(const ToolRegistry& reg, const std::string& name) {
  const ToolClass c = reg.classOf(name);
  return c.group ? c.group : "";
}

}  // namespace

std::string groupListing(const ToolRegistry& reg, const std::string& group, const Principal& who) {
  std::vector<std::string> lines, names;
  for (const ToolRegistry::Spec& s : reg.toolSpecs()) {
    if (group != groupOf(reg, s.name) || !reachable(reg, s.name, who)) continue;
    const std::string spelled = spelledFor(who, s.name);
    lines.push_back(signature(spelled, s.schemaJson, false) + ": " + clipUtf8(s.description, kDescMax));
    names.push_back(spelled);
  }
  return fitLines(lines, names, "not shown: ", kMoreListMax);
}

std::string toolsIndex(const ToolRegistry& reg, const Principal& who) {
  const std::vector<ToolRegistry::Spec> specs = reg.toolSpecs();
  std::vector<std::string> lines, names;
  for (const ToolGroup& g : kGroups) {
    std::string line;
    for (const ToolRegistry::Spec& s : specs) {
      if (groupOf(reg, s.name) != std::string(g.name) || !reachable(reg, s.name, who)) continue;
      line += (line.empty() ? std::string(g.name) + ": " : std::string(", ")) +
              signature(spelledFor(who, s.name), s.schemaJson, true);
    }
    if (line.empty()) continue;
    lines.push_back(line);
    names.push_back(g.name);
  }
  return fitLines(lines, names, "also behind more: ", kToolsIndexMax);
}

// ---- more ---------------------------------------------------------------------------------

namespace {

std::string callableNames(const ToolRegistry& reg, const Principal& who) {
  std::string list;
  int more = 0;
  for (const ToolRegistry::Spec& s : reg.toolSpecs()) {
    if (!*groupOf(reg, s.name) || !reachable(reg, s.name, who)) continue;
    const std::string n = spelledFor(who, s.name);
    if (list.size() + n.size() + 2 > kMoreNamesMax) { more++; continue; }
    list += (list.empty() ? "" : ", ") + n;
  }
  if (list.empty()) return "Nothing is behind more here.";
  return "Callable through more: " + list + (more ? " (+" + std::to_string(more) + " more)" : "") + ".";
}

std::string groupNames() {
  std::string out;
  for (const ToolGroup& g : kGroups) out += (out.empty() ? "" : ", ") + std::string(g.name);
  return out;
}

// The arguments more does not take, comma separated.
std::string strayKeys(JsonObjectConst args) {
  std::string out;
  for (JsonPairConst kv : args) {
    const std::string k = kv.key().c_str();
    if (k != "group" && k != "name" && k != "args") out += (out.empty() ? "" : ", ") + k;
  }
  return out;
}

// The registry name a reachable `name` stands for, or "" (unknown, hidden or a router).
std::string targetName(const ToolRegistry& reg, const std::string& spelled, const Principal& who) {
  const std::string reg_name = reg.resolveName(spelled);
  if (reg_name.empty() || reg.isRouter(reg_name)) return std::string();
  if (reg.isAdminOnly(reg_name) && !who.perms().manageTenants) return std::string();
  return reg_name;
}

std::string specSchema(const ToolRegistry& reg, const std::string& name) {
  for (const ToolRegistry::Spec& s : reg.toolSpecs())
    if (s.name == name) return s.schemaJson;
  return "{}";
}

ToolResult listFor(const ToolRegistry& reg, JsonVariantConst group, const Principal& who) {
  if (group.isNull()) {
    const std::string idx = toolsIndex(reg, who);
    return ToolResult::ok(idx.empty() ? std::string("Nothing is behind more here.") : idx);
  }
  if (!group.is<const char*>()) return ToolResult::fail("group must be a group name: " + groupNames() + ".");
  const std::string g = group.as<const char*>();
  if (!isToolGroup(g)) return ToolResult::fail("no group " + clipUtf8(g, kEchoMax) + ". Groups: " + groupNames() + ".");
  const std::string l = groupListing(reg, g, who);
  return ToolResult::ok(l.empty() ? "group " + g + ": nothing callable here." : l);
}

// One more call resolved without running anything: the registry name it calls, or an
// error. Listing leaves both empty. moreCall and moreTarget share it, so the class the
// rails see is the class of what really runs.
struct Resolved {
  std::string target;
  std::string error;
  bool lists = false;
};

Resolved resolveMore(const ToolRegistry& reg, JsonObjectConst args, const Principal& who) {
  Resolved r;
  const std::string stray = strayKeys(args);
  if (!stray.empty()) {
    r.error = "more takes group, name and args (the tool's arguments go in args); not read: " +
              clipUtf8(stray, kEchoMax) + ".";
    return r;
  }
  JsonVariantConst name = args["name"];
  if (name.isNull()) {
    r.lists = true;
    return r;
  }
  if (!name.is<const char*>()) {
    r.error = "name must be a tool name. " + callableNames(reg, who);
    return r;
  }
  const std::string spelled = name.as<const char*>();
  if (reg.isRouter(reg.resolveName(spelled))) {
    r.error = "more calls other tools, not itself.";
    return r;
  }
  r.target = targetName(reg, spelled, who);
  if (r.target.empty()) {
    r.error = "no tool " + clipUtf8(spelled, kEchoMax) + " behind more. " + callableNames(reg, who);
    return r;
  }
  JsonVariantConst inner = args["args"];
  if (!inner.isNull() && !inner.is<JsonObjectConst>()) {
    r.error = "args must be an object: " + signature(spelledFor(who, r.target), specSchema(reg, r.target), false) + ".";
    r.target.clear();
  }
  return r;
}

}  // namespace

ToolResult moreCall(const ToolRegistry& reg, JsonObjectConst args, const Principal& who) {
  const Resolved r = resolveMore(reg, args, who);
  if (!r.error.empty()) return ToolResult::fail(r.error);
  if (r.lists) return listFor(reg, args["group"], who);
  return reg.dispatch(r.target, args["args"].as<JsonObjectConst>(), who);
}

void registerMore(ToolRegistry& reg) {
  const ToolRegistry* r = &reg;
  reg.add(
      kMoreTool, kMoreDescription,
      [r](JsonObjectConst args, const Principal& who) { return moreCall(*r, args, who); }, kMoreSchema);
  reg.setRouter(kMoreTool);
}

MoreTarget moreTarget(const ToolRegistry& reg, JsonObjectConst args, const Principal& who) {
  const Resolved r = resolveMore(reg, args, who);
  MoreTarget t;
  t.lists = r.lists;
  if (r.lists) t.cls = ToolClass{"", Taint::Internal, EffectClass::Pure};
  else if (!r.target.empty()) t.cls = reg.classOf(t.name = r.target);
  return t;   // refused without running: the fail-closed class
}

ToolClass callClass(const ToolRegistry& reg, const std::string& name, JsonObjectConst args, const Principal& who) {
  const std::string regName = reg.resolveName(name);
  if (!regName.empty() && reg.isRouter(regName)) return moreTarget(reg, args, who).cls;
  return reg.classOf(regName.empty() ? name : regName);
}

}  // namespace orch
}  // namespace nimbus
