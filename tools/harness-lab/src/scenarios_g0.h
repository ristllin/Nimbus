#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for the turn-structured history (histV2, conversation.h).
//
//   history-cache    a scripted chat through the real engine: facts told early
//                    are answered from the history later; a photo's description
//                    rides the history once; a routine sits between owner turns
//                    as its own exchange; 30 replayed copies of one message
//                    collapse to one "(x30)" row and push nothing out; the
//                    history block of each owner turn continues the previous
//                    one byte for byte until a /clear empties it. Per-turn
//                    prompt-cache tokens are printed (the cross-turn cache
//                    layout itself is the message-level caching lane's gate).
//   episodic-dates   L19-12: two photo rows, yesterday's described and today's
//                    not; "did any photo get read today?" is answered from the
//                    memory_episodic output alone (the rows carry their dates).
//
// Run on claude-haiku-5-5 with --no-embed, so long-term memory recall cannot
// answer in place of the history; see tools/harness-lab/README.md.
namespace lab {
namespace history {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void historyCache(LabRig& rig, const Report& r);
void episodicDates(LabRig& rig, const Report& r);

// The "## RECENT CONVERSATION" block of a prompt ("" when it has none).
std::string historyBlockOf(const std::string& prompt);

}  // namespace history
}  // namespace lab
