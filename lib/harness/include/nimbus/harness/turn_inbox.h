#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Declared, not included: msg_batch.h pulls telegram_offset.h, whose nimbus::core
// would shadow the global core:: in every file that includes this header.
namespace nimbus {
namespace orch {
class MessageBatcher;
}
}  // namespace nimbus

// turn_inbox - what a channel knows about an inbound turn that the turn text
// alone cannot carry, handed to the engine for the turn's history row (histV2).
//
// A photo reaches the engine as one routed string ("[image] <description>\n\n
// They wrote: <caption>"), and the engine used to store exactly that as the user
// row, next to the media row that already held the description: every photo and
// voice note was stored twice. The channel now says what the sender actually
// sent (bare labels where the items sat), which media rows belong to the turn
// (their blob refs) and where the turn came from (its source key), so the engine
// stores each item once and drops a delivery it already captured.
//
// One slot, written and taken on the poll task (no concurrency, no lock): the
// channel notes the turn just before it routes it, the engine takes it at the
// turn's capture. A note binds to the routed text it was written for, so a note
// whose turn never reached the engine (a refused caption, a command) is stale:
// the next turn of that chat, carrying other text, drops it instead of taking it.
namespace agent {
namespace history {

struct Inbound {
  std::string chatId;
  // What the sender sent, with a bare label ("[image]", "[file] name.pdf") where
  // each item sat; "" = the routed turn text is what they sent.
  std::string said;
  std::vector<std::string> refs;   // blob refs (16 hex) of the turn's media rows, in order
  std::string src;                 // source key, e.g. "tg:<last update_id>x<count>"; "" = unknown
  uint32_t sentUnix = 0;           // when the sender sent the newest item (0 = unknown)
};

// `routedText` is exactly the text the channel hands the engine for this turn.
void noteInbound(const Inbound& in, const std::string& routedText);

// The note for the turn of Telegram batch bucket `i` (nimbus/orch/msg_batch.h): its
// source ("tg:<newest update id>x<message count>", "" for an inject with no ids), the
// refs of its stored media in album order (`storedRefs[k]` for media k, "" = that item
// left no media row: its description stays in the owner row, its only home), and what
// the sender sent - the bucket's render with each stored item's segment cut back to
// its label ("[image 2/3 ref=...]"), since the media row keeps the description. A
// bucket with nothing stored keeps `said` empty (the routed text is what was sent).
// The send time is not set: a late item already carries it in its label or text.
Inbound inboundForBucket(const nimbus::orch::MessageBatcher& b, size_t i,
                         const std::vector<std::string>& storedRefs);
// The note for this chat's turn carrying `routedText`, consumed; false when there
// is none (the slot is emptied either way: a stale note never outlives a turn).
bool takeInbound(const std::string& chatId, const std::string& routedText, Inbound& out);

// The device's histV2 switch (RAM only, no settings key): on by default; the
// test console turns it off for the byte-identical leg. The engine reads it
// through TurnEngine::Deps::histV2, the channel directly.
bool v2Enabled();
void setV2Enabled(bool on);

}  // namespace history
}  // namespace agent
