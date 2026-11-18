#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/rails.h - the safety rails on the plain engine (PRD section 8): outside-content
// taint per item; in a tainted turn note, skill_save and routine create or update go to
// pending; send to another chat, device configuration, tenant changes and agent_stop
// are refused with the reason; a fixed-text reminder to the same chat is allowed. None
// reads the reply for words. Registered in HookSlot::Rails.
//
// Seam declared by lane RB-B0a; the body (plain_rails.cpp) is lane RB-RL's.
namespace agent {
namespace plain {

// What a rail does with one call in a turn.
enum class RailVerdict : uint8_t { Allow = 0, Pending = 1, Refuse = 2 };

struct RailsDeps {
  bool enabled = true;
};

Status railFor(const TurnCtx& turn, const ToolCall& call, RailVerdict& out);
Status installRails(PlainHooks& hooks, const RailsDeps& deps);

}  // namespace plain
}  // namespace agent
