#pragma once
// tool_groups - the per-tool class of the registration spec and the `more` dispatcher.
//
// Every tool has a class: the GROUP it is reached through (empty: always on, in the
// tools array; otherwise behind `more`), the TAINT of its output (does it read outside
// content) and its EFFECT class (pure, read or effect). The engine's rails read the
// class instead of matching words in a tool's name:
//   - taint gating: a turn that saw an External result is tainted (PRD section 8.1);
//   - identical-call suppression: only Pure tools (PRD section 1);
//   - intents before effects: only Effect tools (PRD section 7).
// One query, toolClassFor(name), with a fail-closed default: a tool with no row reads
// as group "connectors", taint External, effect Effect (Ruling 16). Taint has its own
// single source, tool_taint.h; this file adds the group and the effect class.
//
// `more` (PRD section 4): one always-on tool that lists a group's tools with their
// arguments, or calls one by name. A call through `more` is a full
// ToolRegistry::dispatch with the caller's own Principal, so policy, access rules,
// live gates, the handler's own checks and the unknown-argument note all apply exactly
// as on a direct call. A tool hidden from a caller (admin-only for a non-admin, a
// closed gate, refused by the access rules) is neither listed nor callable through it.
//
// Portable, Arduino-free, host-tested (test_tool_groups).

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/tool_taint.h"   // Taint, toolTaint - the output-taint single source

namespace nimbus {
namespace orch {

class ToolRegistry;
struct Principal;
struct ToolResult;

enum class EffectClass : uint8_t {
  Pure = 0,   // reads stored state only; a repeat with the same arguments returns the same
              // result while no write has succeeded since (suppression may answer it)
  Read,       // changes nothing, but a repeat may differ (the web, the clock, the device)
  Effect,     // changes something; an intent item precedes it (PRD section 7)
};

const char* effectClassName(EffectClass e);   // "pure" | "read" | "effect"

// The class a registered tool is declared with.
struct ToolClass {
  const char* group = kConnectorsGroup;   // "" = always on; else the group behind more
  Taint       taint = Taint::External;
  EffectClass effect = EffectClass::Effect;

  static constexpr const char* kConnectorsGroup = "connectors";
  bool alwaysOn() const { return group && !group[0]; }
};

// One row of the class table: a registry name, its group and its effect class.
struct ToolClassRow {
  const char* name;     // the registry name ("files.read", "read_file")
  const char* group;    // "" always on, else one of toolGroupTable()
  EffectClass effect;
};

// The row for a registry name, or nullptr when it has none.
const ToolClassRow* toolClassRow(const std::string& name);
// The declared class: group and effect from the row, taint from tool_taint.h. No row:
// group "connectors", taint External, effect Effect.
ToolClass toolClassFor(const std::string& name);
// The whole table, for the class tests and the readers that walk it.
const ToolClassRow* toolClassTable(size_t& count);

// The groups behind more, in index order, each with what it is for (not model text:
// the index line names the group and its tools).
struct ToolGroup {
  const char* name;
  const char* about;
};
const ToolGroup* toolGroupTable(size_t& count);
bool isToolGroup(const std::string& name);
// The table's own pointer for a group name: "" for always on, a listed group, else
// "connectors" (so a declared class never holds a caller's temporary string).
const char* toolGroupName(const char* name);

// ---- more ---------------------------------------------------------------------------
constexpr const char kMoreTool[] = "more";
constexpr size_t kMoreListMax = 2048;      // one group's listing (PRD section 4 / RB-N4)
constexpr size_t kToolsIndexMax = 512;     // the tools index in the system text (PRD section 4)
constexpr size_t kMoreNamesMax = 640;      // the callable names an unknown name gets back

// The model-facing description and schema of `more`, the shape RB-M0a measured.
extern const char kMoreDescription[];
extern const char kMoreSchema[];

// Register `more` on `reg`. Its handler holds `reg`'s address: `reg` must outlive it and
// must not be copied after this (a copy's `more` would still reach the original).
// Nothing in the old engine calls this; the engine that puts `more` in its tools array does.
void registerMore(ToolRegistry& reg);

// What a `more` call does with these arguments, resolved without running anything, for
// the engine's before-dispatch seam (taint, suppression, intent): the registry name it
// calls (empty when it lists, or the name is unknown or refused) and that tool's class.
struct MoreTarget {
  std::string name;
  ToolClass   cls;   // the target's class; a listing reads Read, Internal for a named group
                     // other than connectors and External otherwise
  bool        lists = false;
};
MoreTarget moreTarget(const ToolRegistry& reg, ArduinoJson::JsonObjectConst args, const Principal& who);

// The class of one call as the rails must see it: the registry class of `name`, or for
// a `more` call the class of the tool it reaches.
ToolClass callClass(const ToolRegistry& reg, const std::string& name, ArduinoJson::JsonObjectConst args,
                    const Principal& who);

// The tools index: one line per group with a tool this caller may call, naming each
// with its required arguments ("agents: agent_start(name, task), agent_list()"), at
// most kToolsIndexMax bytes (Ruling 11). Groups that do not fit are named on one last
// line. Empty when nothing is behind more for this caller.
std::string toolsIndex(const ToolRegistry& reg, const Principal& who);

// One group's listing (at most kMoreListMax bytes): "name(req, opt?): description" per
// tool this caller may call; "" when the group has none for this caller.
std::string groupListing(const ToolRegistry& reg, const std::string& group, const Principal& who);

// The `more` handler itself: lists, or dispatches through reg.dispatch with `who`.
ToolResult moreCall(const ToolRegistry& reg, ArduinoJson::JsonObjectConst args, const Principal& who);

// The name a caller sees for a registry name: dotted names fold '.' to '_' as the head
// loop advertises them, except for the LAN MCP endpoint, which uses them dotted.
std::string spelledFor(const Principal& who, const std::string& regName);

}  // namespace orch
}  // namespace nimbus
