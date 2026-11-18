#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"

namespace nimbus {
namespace orch {
class ToolRegistry;
}  // namespace orch
}  // namespace nimbus

// plain/request.h - request composition (PRD section 4): the system text in its three
// cache blocks and the always-on tool manifest (at most 14 tools in 10,240 B, fixed
// order, descriptions at most 240 B). Notes, the recent-files list and the system text
// are fixed for the turn. Every model-facing string lives in plain/texts.h (lane RB-E2).
//
// Seam declared by lane RB-B0a; the bodies (plain_request.cpp) are lane RB-E2's.
namespace agent {
namespace plain {

inline constexpr size_t kMaxAlwaysOnTools = 14;
inline constexpr size_t kToolDescriptionMax = 240;
inline constexpr size_t kCoreCeiling = 3072;        // core, including the 512 B tools index
inline constexpr size_t kSkillsIndexCeiling = 1024;
inline constexpr size_t kPinnedNotesCeiling = 4096;

inline constexpr size_t kToolsBytesCeiling = 10240;  // the tools array, rendered (toolsBytes)
inline constexpr size_t kSystemBytesCeiling = 8192;  // core + skills index + pinned notes
inline constexpr uint32_t kWriteContentMax = 12000;  // write_file content on a 120 s host
inline constexpr uint32_t kWriteContentMaxShort = 4096;  // on a 60 s host
inline constexpr size_t kTimeZoneMax = 48;           // longer names are clipped
inline constexpr size_t kModelNameMax = 64;

// Everything composition reads. The same facts give the same bytes (byte-stable): no
// clock, no turn number, no attempt, nothing random is read.
struct RequestFacts {
  const TurnCtx* turn = nullptr;   // reads turn->model and turn->who only
  const nimbus::orch::ToolRegistry* reg = nullptr;  // the registered tools (with `more`)
  std::string pinnedNotes;     // the person's pinned notes, one 'key: value' per line
  std::string skillsIndex;     // one line per approved skill, newest used first
  std::string digestBlock;     // tx::digestBlock
  std::string recentFiles;     // tx::recentFilesList
  std::string timeZone;        // IANA name ("Europe/London"); "" when not set
  bool food = false;           // the food capability is on (ledger tools, food paragraph)
  bool admin = false;          // the person is the owner
  bool shortHost = false;      // the head host answers within 60 s (write_file 4,096 B)
};

// The three system blocks: core (identity ... tools index, at most kCoreCeiling), notes
// (skills index, at most kSkillsIndexCeiling, then pinned notes, at most
// kPinnedNotesCeiling, clipped at whole lines so core + notes <= kSystemBytesCeiling),
// digests (digest block and recent files, as given). InvalidArgument without turn,
// turn->who or reg; TooLarge if the core would pass its ceiling.
Status composeSystem(const RequestFacts& facts, SystemBlocks& out);
// The tools array for this person and head, in the fixed order of
// texts::tool_descriptions::kTools: slots 1-11, the pack (food: ledger_write and
// ledger_day; else agent_start and delete_file), then more. A tool is listed only when
// it is registered and the caller may call it. It never changes inside a turn.
// InvalidArgument without turn, turn->who or reg; TooLarge over 14 tools or
// kToolsBytesCeiling.
Status toolManifest(const RequestFacts& facts, std::vector<ToolSpec>& out);

// The declaration of an always-on tool (description and schema as the manifest shows
// them), or nullptr. A binding registers its tool with these bytes:
//   reg.add(d->name, d->description, handler, plain::declSchema(*d, shortHost));
const texts::tool_descriptions::ToolDecl* toolDecl(const std::string& name);
// A declaration's schema, with write_file's content cap for this host.
std::string declSchema(const texts::tool_descriptions::ToolDecl& d, bool shortHost);
// Declare the pack on the registry: without food, agent_start and delete_file are
// always on (so the tools index does not list them again); with food they go back to
// their groups. Call it when tools are registered and when the capability changes.
void applyToolPack(nimbus::orch::ToolRegistry& reg, bool food);
// The tools array as the request carries it ([{"name","description","input_schema"}]):
// the figure the 10,240 B ceiling is measured on.
size_t toolsBytes(const std::vector<ToolSpec>& tools);
// Tool names a text mentions that this caller cannot call: a known tool name (the
// class table, the registry) standing as a word, or any name written as a call
// ("name("), that is neither in `tools` nor reachable through `more`. Empty: every
// tool-like name in the text is callable.
std::vector<std::string> uncallableNames(const std::string& text, const std::vector<ToolSpec>& tools,
                                         const nimbus::orch::ToolRegistry& reg,
                                         const nimbus::orch::Principal& who);

}  // namespace plain
}  // namespace agent
