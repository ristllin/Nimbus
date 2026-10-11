#include "nimbus/harness/conversation.h"

#include "nimbus/harness/dream.h"   // buildEpisodicDigest - the window's line renderer
#include "nimbus/harness/jobs.h"    // isJobDelivery - captured job rows are not conversation
#include "nimbus/orch/caps.h"       // kRecentTurnsMax / kRecentConvBytes

namespace agent {

const char kClearBoundaryTag[] = "ev:clear";
const char kClearBoundaryText[] = "Conversation cleared.";

bool isClearBoundary(const nimbus::orch::EpisodicMessage& m) {
  // Tags are a comma list: match the whole item, never a prefix of another tag.
  // (A sender cannot forge it - user rows carry only a sanitized "from:" tag.)
  return ("," + m.tags + ",").find(std::string(",") + kClearBoundaryTag + ",") !=
         std::string::npos;
}

std::vector<nimbus::orch::EpisodicMessage> liveConversation(
    const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
    const std::string& inFlightId, size_t maxRows) {
  std::vector<nimbus::orch::EpisodicMessage> out;
  for (const auto& m : newestFirst) {
    if (isClearBoundary(m)) break;   // everything older was cleared
    if (!inFlightId.empty() && m.id == inFlightId) continue;
    if (isJobDelivery(m.tags)) continue;   // job output renders in the chat, never here
    if (maxRows && out.size() >= maxRows) break;
    out.push_back(m);
  }
  return out;
}

std::string recentConversationBlock(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
                                    const std::string& inFlightId) {
  const auto rows = liveConversation(newestFirst, inFlightId, (size_t)nimbus::orch::kRecentTurnsMax);
  if (rows.empty()) return std::string();
  std::string block =
      "\n## RECENT CONVERSATION (this chat, oldest first; the current message "
      "is NOT here - it is in [USER])\n";
  block += dream::buildEpisodicDigest(rows, nimbus::orch::kRecentConvBytes);
  return block;
}

}  // namespace agent
