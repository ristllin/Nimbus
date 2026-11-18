#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/wire.h"

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

struct RequestFacts {
  const TurnCtx* turn = nullptr;
  std::string pinnedNotes;     // the person's pinned notes
  std::string skillsIndex;     // one line per approved skill
  std::string digestBlock;     // tx::digestBlock
  std::string recentFiles;     // tx::recentFilesList
  bool food = false;           // the food capability is on
  bool admin = false;          // the person is the owner
};

Status composeSystem(const RequestFacts& facts, SystemBlocks& out);
// The tools array for this person and head; it never changes inside a turn.
Status toolManifest(const RequestFacts& facts, std::vector<ToolSpec>& out);

}  // namespace plain
}  // namespace agent
