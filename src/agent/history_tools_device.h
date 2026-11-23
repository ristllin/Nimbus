#pragma once
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/tool_registry.h"

// history_tools_device - the device binding of engine 2's search and view (lane E2-SRCH,
// nimbus/harness/plain/history.h): the transcripts on the card (txdev::cardTxFiles), the earlier
// engine's day rows of a chat through memory::episodic() (newest first, a small read budget per store
// query so each card-lock hold stays short), the person's unpinned notes (notesdev::notebook) and the
// photo blobs the intake keeps (/mem/blobs/<ref>.jpg or .bin). Engine 2 only
// (tools/engine/engine2_only.txt). Turn task only: the tools run inside the turn like every other tool.
namespace agent::historydev {

// The declarations of search and view on `reg` (their registry handlers refuse: no turn).
void registerHistory(nimbus::orch::ToolRegistry& reg);
// ChatBindingDeps::turnTool: search and view of the turn's own conversation, under the PSRAM-first
// scope; one log line per call with the card time, bytes, the poll task's free stack and the largest
// internal block after it.
bool answer(const agent::plain::ToolCall& call, const agent::plain::TurnCtx& turn, agent::plain::ToolOutput& out);

}  // namespace agent::historydev
