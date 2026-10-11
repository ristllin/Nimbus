#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"   // EpisodicMessage

// conversation - what the head reads as "this conversation", and the /clear
// boundary that ends it (CUM-511).
//
// /clear used to answer "Cleared this conversation" while the next turn still
// carried the previous 12 messages: the RECENT CONVERSATION window is rebuilt
// every turn straight from the chat's episodic history, and nothing marked
// where the owner asked to start over. Now /clear writes a BOUNDARY row into
// the chat's own history and every conversation read stops there. The history
// itself is never edited - it is the owner's record (and memory.episodic still
// searches all of it, like long-term memory recall).
namespace agent {

// The boundary row a /clear leaves in the chat's session: kind=message,
// role=system, this tag - so every conversation query (message kinds) already
// returns it and no extra read is needed to find it.
extern const char kClearBoundaryTag[];   // "ev:clear"
extern const char kClearBoundaryText[];  // the row's text (owner-visible in the history)
bool isClearBoundary(const nimbus::orch::EpisodicMessage& m);

// The live part of a chat's rows (`newestFirst` = MsgQuery order): rows NEWER
// than the newest /clear boundary, minus `inFlightId` (the turn's own user row,
// which arrives as [USER]) and minus captured job-engine deliveries (they render
// in the chat but are not conversation the model reads - see isJobDelivery),
// capped at `maxRows` (0 = no cap). Still newest-first.
std::vector<nimbus::orch::EpisodicMessage> liveConversation(
    const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
    const std::string& inFlightId, size_t maxRows);

// The head's "## RECENT CONVERSATION" block over the live conversation (up to
// kRecentTurnsMax rows, oldest first, newest rows kept whole within capBytes -
// CUM-485 derives capBytes from the head model's window; kRecentConvBytes is
// the floor) - "" when nothing is live, so a fresh or just-cleared chat (CUM-511
// boundary) carries no conversation at all.
std::string recentConversationBlock(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
                                    const std::string& inFlightId,
                                    size_t capBytes = 3000);

}  // namespace agent
