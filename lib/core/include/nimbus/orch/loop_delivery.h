#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/loops.h"

// Where a routine, reminder or wakeup delivers its output (CUM-505 / CUM-513). PURE,
// host-tested (test/test_loop_delivery). The device seam is src/agent/loops_subsystem.cpp.
//
// The bug this closes: loops were stored with chatId "" whenever the model (or a web
// turn) did not name a chat, and "" meant "the first Telegram allowlist entry" at fire
// time. On a device with no Telegram that resolved to "" again, the reply was handed to
// telegram::send and dropped: the reminder ran, was billed, and nobody saw it. A loop
// remembered from the web chat ("web", e.g. /remind typed there) was worse: "web" is not
// on the Telegram allowlist, so the fire-time check paused it as "chat revoked".
//
// The rules encoded here:
//   - A loop remembers the channel it was created from: an explicit chat wins, else the
//     creating turn's chat (web chat, Telegram chat, the device's hold-to-talk).
//   - At fire time it delivers there. The device's own channels (web chat, device screen,
//     serial console) always exist. A Telegram chat delivers while the bot token is set and
//     the chat is still allowed (allow-listed, tenant role not revoked).
//   - When the bot token is gone but the chat is still the OWNER's allowed chat, the loop
//     falls back to the web chat and the delivered reply says so. Anything else (a chat
//     removed from the allowlist, a revoked tenant, an emptied allowlist, a member's chat)
//     is a revoke and the loop pauses: a fallback turn runs as the web chat (admin), so it
//     is only ever granted to the owner's own loops.
//   - The owner default ("" - a loop with no remembered channel) is the owner's Telegram
//     chat (the same rule Telegram ownership uses: the explicit owners list, else the first
//     allowlist entry) when a bot token is set, else the web chat. No loop fires into "".

namespace nimbus {
namespace orch {

// The chat id a hosted instance's (nimbusd) web page runs its turns under (CUM-542).
// Like "web" it is the web chat, never a Telegram chat (a Telegram chat id is a number).
inline constexpr char kHostedWebChatId[] = "owner";

// Every chatId routing value the device uses. The slugs ("web", "voice", "serial", a
// Telegram chat id, "") are persisted in loops.json and are frozen.
enum class DeliveryChannel : uint8_t {
  OwnerDefault = 0,   // ""
  Web,                // "web" (or kHostedWebChatId) - the web chat (history-backed)
  Voice,              // "voice"   - the device screen (hold-to-talk replies)
  Serial,             // "serial"  - the developer console
  Telegram,           // anything else: a Telegram chat id
  Count,
};
DeliveryChannel deliveryChannelOf(const std::string& chatId);
// Short user-facing name ("the web chat", "Telegram", ...), used in the fallback sentence.
const char* deliveryChannelLabel(DeliveryChannel c);

bool isDeviceChannel(DeliveryChannel c);   // web, voice, serial: always deliverable

// The Telegram facts delivery depends on, read once per tick.
struct DeliveryWorld {
  bool                     botToken = false;   // a Telegram bot token is set
  std::string              ownerTelegram;      // the owner-default chat ("" when none)
  std::vector<std::string> ownerChats;         // every allow-listed owner chat
  bool isOwnerChat(const std::string& chatId) const;
};
// From the stored bot token, the comma-separated allowlist and the owners list. Owners
// follow telegram::isOwner exactly: an owner must be allow-listed; with no owners list the
// owner is the text before the allowlist's first comma (trimmed), else every allow-listed
// entry of the owners list, the first of which is the owner default.
DeliveryWorld deliveryWorld(const std::string& botToken, const std::string& allowlist,
                            const std::string& owners);

enum class DeliveryVerdict : uint8_t {
  Deliver = 0,   // deliver to target, nothing to explain
  FellBack,      // the owner's Telegram is off: target is the web chat; say so
  Revoked,       // the Telegram chat is no longer allowed: pause, do not fire
};

struct Delivery {
  DeliveryVerdict verdict = DeliveryVerdict::Deliver;
  std::string     target;                          // the chatId the turn replies to
  DeliveryChannel from = DeliveryChannel::OwnerDefault;   // what the loop remembered
  DeliveryChannel to   = DeliveryChannel::Web;            // where it actually goes
};

// chatAllowed: the stored Telegram chat is still allowed (allow-listed and its tenant role
// not revoked; only read for a Telegram chat). Never returns an empty target unless the
// verdict is Revoked.
Delivery resolveDelivery(const std::string& storedChatId, const DeliveryWorld& w,
                         bool chatAllowed);

// One-time repair for loops the pre-fix rule paused by mistake: a RECURRING loop that
// remembers a device channel (web, voice, serial) and was paused as "chat revoked" is
// resumed (true = changed). A one-shot reminder stays paused: its moment has passed.
bool liftDeviceChannelRevoke(LoopRecord& l);

// The chat a NEW loop remembers: an explicit request wins, else the creating turn's
// chat ("" when there is no creating turn, e.g. the web Routines page: owner default).
std::string originChat(const std::string& requested, const std::string& creatingTurnChat);

// The sentence a fallen-back reply opens with (user-facing copy), e.g.
// "Showing this in the web chat because Telegram isn't available right now."
std::string deliveryFallbackSentence(const Delivery& d);

// The fire request for a due loop: the resolved target, the turn framing flags, and,
// on a fallback, a [DELIVERY] instruction appended to the prompt asking the reply to
// open with deliveryFallbackSentence(). Never called for a Revoked delivery (the loop
// pauses instead).
LoopFireRequest buildFireRequest(const LoopRecord& l, const Delivery& d, uint64_t nowEpoch);

}  // namespace orch
}  // namespace nimbus
