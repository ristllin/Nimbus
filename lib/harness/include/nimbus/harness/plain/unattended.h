#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/unattended.h - isolated routine runs and held delivery (PRD section 6): each fire
// runs on its own transcript routine:<id> with zero bytes of the owner's chat and only
// the tools its definition names; a note for quiet hours is held with an expiry and sent
// as stored when they end. Registered in HookSlot::Policy (tools) and HookSlot::Routines
// (idle).
//
// Seam declared by lane RB-B0a; the body (plain_unattended.cpp) is lane RB-U1a's.
namespace agent {
namespace plain {

inline constexpr uint8_t kRoutineRounds = 6;
inline constexpr uint32_t kRoutineSoftMs = 90000;
inline constexpr uint32_t kRoutineHardMs = 180000;
inline constexpr uint32_t kRoutineMaxMicroUsd = 50000;   // 0.05 USD a fire
inline constexpr uint32_t kHeldNoteExpiryS = 12 * 3600;

struct RoutineFire {
  std::string id;
  std::string name;
  std::string instruction;
  std::vector<std::string> tools;   // the definition's tools (default web_search, read_file, ledger_day, send)
  uint64_t fireAt = 0;
};

struct UnattendedDeps {
  bool quietHoursRespected = true;
};

Status installUnattended(PlainHooks& hooks, const UnattendedDeps& deps);
// The transcript key of a routine's own conversation: "routine:<id>".
Status routineChatKey(const std::string& routineId, std::string& out);

}  // namespace plain
}  // namespace agent
