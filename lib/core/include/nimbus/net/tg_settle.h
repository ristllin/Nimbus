#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// tg_settle - when a getUpdates page is complete enough to answer (albums, bursts and a
// text with the photo sent right after it, as one turn).
//
// Telegram delivers an album as one update per photo, and a phone uploads them one
// by one, so the first poll after the owner taps Send often holds only the first
// photo (and its caption), the next poll the second, and so on. A text and the photo
// the owner sends a few seconds after it have the same shape (25 Oct 2026: "Without the
// mac and cheese. Just the Sunday roast", then the plate, 5 s later by Telegram's
// dates). Answering each page at once split them into separate turns, the first one
// answered without the rest. So before the device classifies a page it asks this rule:
// answer now (Dispatch), or wait a little for the rest (Hold).
//
// Dispatch when any of these holds (each pinned by test/test_tg_settle):
//   - the page holds a slash command from an allowed chat: commands never wait;
//   - the page holds no turn candidate from an allowed chat (only edits, strangers or
//     stickers): nothing would join a turn;
//   - the page is full (count >= limit): more would not fit this page anyway (the item
//     cap of a burst);
//   - the newest item's Telegram date is older than the quiet window plus
//     kSettleDateMarginS (the sender finished a while ago, for example a backlog after
//     an outage); skipped while the device clock is not set, and the margin covers
//     Telegram's whole-second dates and a little clock drift;
//   - the first item was seen kSettleMaxHoldMs ago (the total-wait cap of a burst: a
//     page that keeps growing is never held longer);
//   - the page has not grown for the quiet window. The window follows the page's newest
//     turn candidate: kSettleQuietTextMs after a plain text (the photo, file or voice
//     note the owner sends next), kSettleQuietMediaMs after a photo, file or voice note
//     (the rest of an album). Growth (a newer turn candidate from an allowed chat)
//     restarts the window; a stranger's message or an edit does not.
// Hold otherwise.
//
// kSettleQuietTextMs comes from the owner's traffic: in four days the one text and photo
// that belonged together were 5 s apart by Telegram's whole-second dates (4 to 6 s in
// fact), and the nearest pair that did not (two separate meals) was 10 s apart. 5 s is
// the longest window the release allows; it joins a follow-up that arrives within 5 s
// of the device first seeing the text (never earlier than the send) plus the last
// re-poll's round trip. The windows run from that first sight.
//
// A hold is a per-cycle state on the one poll task, never a wait: the device re-polls
// the same offset with timeout 0 after repollInMs(), keeping the keep-alive socket
// open, and classifies, acks and stores nothing in between (no settings write). Local
// turns, job polling and reply sends keep running between re-polls. repollInMs() is
// kSettleRepollMs, or less when the window ends sooner, so a page that stopped growing
// is answered at the end of its window, never up to a whole pause later: a plain text
// with nothing after it waits the text window and no longer. A page that does not grow
// re-polls at most ceil(quiet / repoll) + 1 times.
namespace nimbus::net {

constexpr uint32_t kSettleQuietMediaMs = 3000;
constexpr uint32_t kSettleQuietTextMs = 5000;
constexpr uint32_t kSettleMaxHoldMs = 15000;
constexpr uint32_t kSettleDateMarginS = 2;
constexpr uint32_t kSettleRepollMs = 1000;
// The shortest pause before a re-poll (the last one of a window can be shorter than
// kSettleRepollMs, never a spin).
constexpr uint32_t kSettleRepollMinMs = 50;
// The device clock counts as set from 2020-01-01 on.
constexpr int64_t kSettleClockSet = 1577836800LL;

// The page's newest turn candidate from an allowed chat: what may still follow it.
enum class SettleTail : uint8_t {
  None,    // no turn candidate (only edits, strangers, stickers)
  Text,    // a plain text: the photo, file or voice note sent next may follow
  Media,   // a photo, file or voice note: the rest of an album may follow
};
// Every kind, for the tests that walk them all. A new kind goes here too; settleQuietMs
// has no default, so the compiler flags a kind with no window.
constexpr SettleTail kSettleTailAll[] = {SettleTail::None, SettleTail::Text, SettleTail::Media};

// What the rule needs to know about one fetched page (no update contents).
struct SettlePage {
  int        count = 0;                 // whole updates in the page
  int        limit = 10;                // the getUpdates limit of this fetch
  SettleTail tail = SettleTail::None;   // the newest allowed turn candidate's kind
  bool       command = false;           // an allowed chat sent a slash command
  int32_t    newestId = 0;              // the newest allowed turn candidate's update id (growth)
  int64_t    newestDate = 0;            // the largest Telegram date in the page (0: none)
};

enum class Settle : uint8_t { Dispatch, Hold };
enum class SettleWhy : uint8_t { Empty, Full, OldDate, MaxHold, Quiet, Waiting, Command, NoTurn };

inline const char* settleWhyName(SettleWhy w) {
  switch (w) {
    case SettleWhy::Empty: return "empty";
    case SettleWhy::Full: return "full";
    case SettleWhy::OldDate: return "old";
    case SettleWhy::MaxHold: return "max-hold";
    case SettleWhy::Quiet: return "quiet";
    case SettleWhy::Waiting: return "waiting";
    case SettleWhy::Command: return "command";
    case SettleWhy::NoTurn: return "no-turn";
  }
  return "?";
}

// The quiet window after a page's newest turn candidate (0: nothing to wait for).
inline uint32_t settleQuietMs(SettleTail t) {
  switch (t) {
    case SettleTail::Text: return kSettleQuietTextMs;
    case SettleTail::Media: return kSettleQuietMediaMs;
    case SettleTail::None: break;
  }
  return 0;
}

// The hold state across poll cycles. One instance, owned by the poll task.
class TgSettle {
 public:
  // Observe the page fetched at `offset` at uptime `nowMs` (device clock `nowEpoch`,
  // 0 or anything before kSettleClockSet when unset) and decide.
  Settle observe(int32_t offset, const SettlePage& p, uint32_t nowMs, int64_t nowEpoch) {
    if (p.count <= 0) {   // nothing to hold
      reset();
      return finish(SettleWhy::Empty, nowMs);
    }
    track(offset, p.newestId, nowMs);
    quiet_ = settleQuietMs(p.tail);
    const SettleWhy w = rule(p, nowMs, nowEpoch);
    if (w != SettleWhy::Waiting) return finish(w, nowMs);
    why_ = SettleWhy::Waiting;
    return Settle::Hold;
  }

  // Forget the hold (after a dispatch ran the page, or a fetch failed).
  void reset() {
    holding_ = false;
    repolls_ = 0;
  }

  // The pause before the next re-poll of the held page, at uptime `nowMs`: the rest of
  // the quiet window (or of the maximum hold) when that is shorter than
  // kSettleRepollMs, never below kSettleRepollMinMs. kSettleRepollMs when not holding.
  uint32_t repollInMs(uint32_t nowMs) const {
    if (!holding_) return kSettleRepollMs;
    const uint32_t left =
        std::min(remaining(quiet_, nowMs - lastGrowMs_), remaining(kSettleMaxHoldMs, nowMs - firstMs_));
    return std::max(kSettleRepollMinMs, std::min(left, kSettleRepollMs));
  }

  bool      holding() const { return holding_; }
  SettleWhy why() const { return why_; }
  // Re-polls of the current page so far (0 on its first sighting).
  int       repolls() const { return repolls_; }
  // How long the last dispatched page was held (0 for one dispatched at first sight).
  uint32_t  lastHeldMs() const { return lastHeldMs_; }

 private:
  // A new page starts its windows now; a page seen again restarts the quiet window
  // when it grew.
  void track(int32_t offset, int32_t newestId, uint32_t nowMs) {
    if (!holding_ || offset != offset_) {
      holding_ = true;
      offset_ = offset;
      newestId_ = newestId;
      firstMs_ = lastGrowMs_ = nowMs;
      repolls_ = 0;
      return;
    }
    ++repolls_;
    if (newestId != newestId_) {
      newestId_ = newestId;
      lastGrowMs_ = nowMs;
    }
  }

  // The first dispatch rule that holds, in the order of the header comment; Waiting
  // when none does.
  SettleWhy rule(const SettlePage& p, uint32_t nowMs, int64_t nowEpoch) const {
    if (p.command) return SettleWhy::Command;
    if (p.tail == SettleTail::None) return SettleWhy::NoTurn;
    if (p.count >= p.limit) return SettleWhy::Full;
    if (nowEpoch >= kSettleClockSet && p.newestDate > 0 &&
        (nowEpoch - p.newestDate) * 1000 > (int64_t)quiet_ + (int64_t)kSettleDateMarginS * 1000)
      return SettleWhy::OldDate;
    if ((uint32_t)(nowMs - firstMs_) >= kSettleMaxHoldMs) return SettleWhy::MaxHold;
    if ((uint32_t)(nowMs - lastGrowMs_) >= quiet_) return SettleWhy::Quiet;
    return SettleWhy::Waiting;
  }

  static uint32_t remaining(uint32_t window, uint32_t elapsed) { return window > elapsed ? window - elapsed : 0; }

  Settle finish(SettleWhy w, uint32_t nowMs) {
    why_ = w;
    lastHeldMs_ = holding_ ? nowMs - firstMs_ : 0;
    holding_ = false;   // a dispatched page starts fresh if it is seen again
    return Settle::Dispatch;
  }

  bool      holding_ = false;
  int32_t   offset_ = 0;
  int32_t   newestId_ = 0;
  uint32_t  firstMs_ = 0;
  uint32_t  lastGrowMs_ = 0;
  uint32_t  quiet_ = 0;
  int       repolls_ = 0;
  uint32_t  lastHeldMs_ = 0;
  SettleWhy why_ = SettleWhy::Empty;
};

// The turn after a page that missed an earlier turn of its chat: its earliest message
// was sent (Telegram date `earliestSent`) before that chat's previous answer went out
// (device epoch `answeredAt`), so the sender had not seen that answer yet. The turn is
// told the answer is already in the chat (kCrossedNote), so it answers with it in mind
// instead of as if nothing came before. Whole seconds on both sides: the same second
// is not counted (a note only when the order is certain); 0 on either side is unknown.
inline bool crossedAnswer(int64_t earliestSent, int64_t answeredAt) {
  return earliestSent > 0 && answeredAt > 0 && earliestSent < answeredAt;
}

// Model-facing (copy rules exempt; no em dash): appended to a crossed turn's text.
inline constexpr char kCrossedNote[] =
    "\n\n[sent while you were still answering the previous message; that answer is already in the "
    "chat above]";

// The length of a routed turn text without its kCrossedNote (`len` when it carries none):
// the turn's history row keeps what the sender sent, never the device's note.
inline size_t withoutCrossedNote(const char* text, size_t len) {
  const size_t n = sizeof(kCrossedNote) - 1;
  return len >= n && std::memcmp(text + len - n, kCrossedNote, n) == 0 ? len - n : len;
}

}  // namespace nimbus::net
