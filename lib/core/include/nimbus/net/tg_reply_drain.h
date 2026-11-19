#pragma once
// tg_reply_drain - the reply queue's send rules (lane RB-TS1): what one sendMessage
// attempt proved, when a part is sent again, and a reply's parts queued all or none.
//
// The device sends each reply part as one sendMessage POST (src/agent/telegram.cpp).
// Before v5.2.0 any answer other than "ok":true counted as a failure: a part whose
// answer was lost (the request written, then a timeout) was resent plain at once and
// then up to five more times, so one part could reach the chat a dozen times. And
// send() queued a long reply part by part, so a full queue kept the first parts and
// refused the rest. The rules here bound both, and they are the same for the drain
// that runs inside the turn and the one at the bottom of the poll loop. Portable and
// host-tested with a fake transport (test_tg_reply_drain).

#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace net {

// What one sendMessage POST proved.
enum class TgSend : uint8_t {
  Ok,           // Telegram answered 2xx: the message is in the chat
  Refused,      // Telegram answered with a 4xx other than 429: it did not take this text
                // (for HTML: the entities); nothing was delivered
  NotSent,      // the request never reached Telegram whole (no connection, a short write),
                // or Telegram answered 429 (it refuses before it sends): try again later
  Unconfirmed,  // the request was written whole and no status line came back, or a 5xx
                // came back (a gateway may answer after Telegram took it): the message
                // may be in the chat
};

// What the device saw of one POST.
struct TgSendSeen {
  bool connected = false;   // the TLS session to api.telegram.org opened
  bool wroteAll = false;    // every byte of the request was written
  int status = 0;           // the HTTP status of the answer; 0 when no status line was read
};

inline TgSend classifyTgSend(const TgSendSeen& s) {
  if (!s.connected || !s.wroteAll) return TgSend::NotSent;
  if (s.status <= 0) return TgSend::Unconfirmed;
  if (s.status >= 200 && s.status < 300) return TgSend::Ok;
  if (s.status == 429) return TgSend::NotSent;
  if (s.status >= 500) return TgSend::Unconfirmed;
  return TgSend::Refused;
}

// One part out: HTML first, the plain text only after Telegram refused the HTML. Any
// other HTML outcome is final for this attempt: a part that may already be in the chat
// (Unconfirmed) is never followed by its plain copy. `raw(html)` makes one POST.
template <class Raw>
TgSend sendTgPart(Raw&& raw) {
  const TgSend h = raw(true);
  return h == TgSend::Refused ? raw(false) : h;
}

// Attempts for one part (as before v5.2.0), and the copies at most of a part whose
// answer was lost: one resend, so an owner sees a reply at most twice and a network
// break never drops it silently.
constexpr uint8_t kTgSendTries = 6;
constexpr uint8_t kTgUnconfirmedCopies = 2;

// The drain's bookkeeping for the part at the front of the queue.
struct TgPartTries {
  uint8_t tries = 0;         // failed attempts so far
  uint8_t unconfirmed = 0;   // of them, the ones whose answer was lost
};

enum class TgDrainStep : uint8_t {
  Sent,        // delivered: take it off the queue
  Retry,       // leave it at the front; the next drain sends it again
  GiveUp,      // Telegram kept refusing or could not be reached: dropped, logged
  LeaveAsSent, // its answer was lost kTgUnconfirmedCopies times: taken off as probably delivered
};

inline TgDrainStep afterTgSend(TgSend r, TgPartTries& t) {
  if (r == TgSend::Ok) return TgDrainStep::Sent;
  ++t.tries;
  if (r == TgSend::Unconfirmed && ++t.unconfirmed >= kTgUnconfirmedCopies) return TgDrainStep::LeaveAsSent;
  return t.tries < kTgSendTries ? TgDrainStep::Retry : TgDrainStep::GiveUp;
}

// One drain of the reply queue. The part stays IN the queue while it is sent (peek,
// then pop once it is done with), so a failed part keeps its place in front of the
// parts after it and a producer filling the queue meanwhile can never push it out.
// Stops at the first part to retry (the next drain resends it). `tries` belongs to
// the part at the front; it is reset whenever that part leaves the queue.
//   peek(Item&) -> bool    the oldest part, left in the queue (false: empty)
//   pop(TgDrainStep)       remove that part (Sent, GiveUp or LeaveAsSent: why it leaves)
//   send(const Item&) -> TgSend
//   note(const Item&, TgDrainStep, const TgPartTries&)  the log line of a part that did
//                          not simply go out (called for every step but Sent)
// Returns the parts taken off the queue.
template <class Item, class Peek, class Pop, class Send, class Note>
size_t drainTgReplies(Item& slot, TgPartTries& tries, Peek&& peek, Pop&& pop, Send&& send, Note&& note) {
  size_t taken = 0;
  while (peek(slot)) {
    const TgDrainStep step = afterTgSend(send(slot), tries);
    if (step != TgDrainStep::Sent) note(slot, step, tries);
    if (step == TgDrainStep::Retry) break;
    pop(step);
    tries = TgPartTries();
    ++taken;
  }
  return taken;
}

// A reply's parts go into the queue together or not at all. The caller holds the
// producers' lock across each room check and the pushes after it, and the one
// consumer only takes parts out, so the room it saw is there for every push.
//   room() -> size_t       free slots now
//   push(size_t i) -> bool queue part i
//   wait() -> bool         wait a little for room (false: stop waiting); on the device
//                          it gives the lock up while it waits, so other producers
//                          never wait behind a full queue
inline bool tgPartsFit(size_t room, size_t parts) { return parts <= room; }

template <class Room, class Push, class Wait>
bool queueTgParts(size_t parts, Room&& room, Push&& push, Wait&& wait) {
  if (parts == 0) return true;
  while (!tgPartsFit(room(), parts))
    if (!wait()) return false;
  for (size_t i = 0; i < parts; ++i)
    if (!push(i)) return false;   // not reached under the lock (the room was checked)
  return true;
}

}  // namespace net
}  // namespace nimbus
