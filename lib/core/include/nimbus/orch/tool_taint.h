#pragma once
// tool_taint - one table, one row per registered tool: where its OUTPUT comes
// from (taint) and what calling it DOES (effect). The single source three rails
// read, so they can never disagree:
//   - the leak repair's gate (lane L): fields recovered from leaked tool markup
//     carry actions, so they apply only on a turn that read no external content;
//   - the effect ledger and claim check (lane S): which tool backs which claim;
//   - owner preferences and skills (lanes J, I): what a tainted turn may change.
// Unknown names (a connector's remote tools, registered at runtime) read as
// External with no known effect - fail closed. A host test scans every tool the
// firmware registers by name and fails when one has no row.
//
// Portable, Arduino-free, host-tested (test_tool_taint).

#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace orch {

enum class Taint : uint8_t {
  Internal = 0,   // content the device, the owner or this turn's head wrote
  External,       // content from outside: the web, connectors, sub-agents, fetched files
};

enum class ToolEffect : uint8_t {
  Read = 0,    // looks something up; changes nothing
  MemWrite,    // stores a long-term memory
  MemUpdate,   // changes or replaces a stored memory
  MemPin,      // pins a memory against decay
  MemDelete,   // removes or archives memories
  Scratch,     // updates the scratchpad
  FileWrite,   // writes or downloads a file into the store
  Share,       // changes who can read a stored file
  SkillSave,   // creates, edits or deletes a skill
  Spawn,       // starts a sub-agent
  Cancel,      // stops a sub-agent
  Send,        // delivers something to a person (Telegram message, file)
  Speak,       // speaks aloud on the device
  Schedule,    // creates or cancels a routine or a wakeup
  Config,      // changes device or memory settings
  Admin,       // changes another person's role or quota
  Media,       // plays or stops audio
};

struct ToolTaintRow {
  const char* name;   // the registry name, dotted ("memory.write")
  Taint       taint;
  ToolEffect  effect;
};

// The row for a registry name, or nullptr when the tool has none.
const ToolTaintRow* toolTaintRow(const std::string& name);
// Taint of a tool's output; unknown tools read as External.
Taint toolTaint(const std::string& name);
// The tool's effect; unknown tools read as Read (nothing the device can vouch for).
ToolEffect toolEffect(const std::string& name);
// The whole table, for the static tests and the readers that walk it.
const ToolTaintRow* toolTaintTable(size_t& count);

}  // namespace orch
}  // namespace nimbus
