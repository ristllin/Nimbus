#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// plain/waiting.h - the person's messages that arrive while a chat's earlier conversation is read
// in for the first time (lane E2-FIX13). Engine 2 does not start a chat before the earlier engine's
// history is indexed (migrate_device.h historyReady: one to four minutes after the first boot on a
// device with history). A message in that window used to be turned away with "send it again"; now
// it is kept here, in arrival order, and answered by itself when the read-in ends, each once.
//
// The file is the truth: /mem/tx/<chat>.wait.json on the card, written whole by temp and rename
// before the person is told anything, so a restart during the read-in loses nothing. A message
// leaves it only after its turn ran (or the person got one true line instead). A restart between
// the turn and that removal is caught by `taken`: the transcript already holds the message (its
// day-stream row m or its Telegram update ids), so it is not answered a second time.
//
// A file that cannot be read is not an empty one (ruling 116): nothing is answered, nothing is
// held over it, and the read is tried again. A file that reads but does not decode is set aside
// whole (renamed, never removed) and the chat starts with an empty list.
// Host-tested in test_plain_waiting with a real ChatBinding.
namespace agent {
namespace plain {

using Status = nimbus::orch::tx::Status;

// One message as the turn needs it (ChatMessage's fields, plus the sender's name for the record).
struct WaitingMessage {
  std::string text;
  std::string from;                                // the sender's name (the day-stream row's tag)
  std::string ch = "tg";                           // u.ch
  std::vector<uint64_t> src;                       // Telegram update ids (none for the web chat)
  std::vector<std::string> stored;                 // the files stored for it (TurnInput::stored)
  std::vector<nimbus::orch::tx::Part> parts;       // its photos (Image parts; bytes stay on the card)
  int16_t tzMinutes = 0;
  uint64_t m = 0;                                  // its day-stream row, written when it arrived
};

// The most messages one chat keeps waiting. A person sends a few in the minutes of a read-in; past
// this the device answers with a line instead of keeping more (nothing already kept is dropped).
inline constexpr size_t kWaitingMax = 32;

inline std::string waitingPathFor(const std::string& chatId) {
  return std::string(nimbus::orch::tx::kTxRoot) + "/" + chatId + ".wait.json";
}

enum class HoldVerdict : uint8_t {
  Kept,        // stored on the card
  KeptInRam,   // the card did not store it (or there is none): kept until a restart only
  Duplicate,   // a re-served update already kept: nothing changed
  Full,        // kWaitingMax messages wait already: not kept
  Unread,      // the list on the card could not be read: not kept (nothing is written over it)
};

class WaitingInbox {
 public:
  // `files` null: RAM only (no card fitted).
  WaitingInbox(nimbus::orch::tx::TxFiles* files, std::string chatId) : files_(files), chatId_(std::move(chatId)) {}

  // Read the list. Io when the card could not tell (the list stays unread: hold answers Unread and
  // nothing is answered until a later load succeeds).
  Status load();
  bool loaded() const { return loaded_; }
  const std::string& chatId() const { return chatId_; }
  const std::vector<WaitingMessage>& messages() const { return msgs_; }
  bool empty() const { return msgs_.empty(); }
  // Is any of these update ids kept already (a re-served update)?
  bool holds(const std::vector<uint64_t>& src) const;
  HoldVerdict hold(const WaitingMessage& m);
  // The first message is done. Io when the card did not store that: it stays first (in RAM too),
  // and the next pass finds it taken.
  Status pop();

 private:
  Status save(const std::vector<WaitingMessage>& msgs);
  bool listed(bool& present) const;
  bool onCard() const;
  nimbus::orch::tx::TxFiles* files_;
  std::string chatId_;
  std::vector<WaitingMessage> msgs_;
  bool loaded_ = false;
};

// What answering one kept message did.
enum class WaitingAnswer : uint8_t {
  Done,    // its turn ran, or the person got one true line instead: it leaves the list
  Later,   // nothing was done (the chat cannot run a turn yet): it stays first, tried again later
};

struct WaitingDrainFns {
  // The transcript already holds this message (answered before a restart): no second turn.
  std::function<bool(const WaitingMessage&)> taken;
  std::function<WaitingAnswer(const WaitingMessage&)> answer;
};

// Answer the kept messages in arrival order, each once. Stops at Later and at a removal the card
// refused (that message is found taken on the next pass). Returns how many turns ran or lines went.
size_t answerWaiting(WaitingInbox& in, const WaitingDrainFns& f);

// The list as stored, and back (exposed for the tests). fromText false: it does not decode.
std::string waitingToText(const std::vector<WaitingMessage>& msgs);
bool waitingFromText(const std::string& text, std::vector<WaitingMessage>& out);

}  // namespace plain
}  // namespace agent
