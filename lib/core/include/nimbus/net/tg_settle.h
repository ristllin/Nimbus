#pragma once
#include <cstdint>

// tg_settle - when a getUpdates page is complete enough to answer (albums and
// bursts as one turn).
//
// Telegram delivers an album as one update per photo, and a phone uploads them one
// by one, so the first poll after the owner taps Send often holds only the first
// photo (and its caption), the next poll the second, and so on. Answering each page
// at once split a 3-photo album into three turns, in the wrong order. A burst of
// short texts has the same shape. So before the device classifies a page it asks
// this rule: answer now (Dispatch), or wait a little for the rest (Hold).
//
// Dispatch when any of these holds (each pinned by test/test_tg_settle):
//   - the page is full (count >= limit): more would not fit this page anyway;
//   - the newest item's Telegram date is older than the quiet window plus
//     kSettleDateMarginS (the sender finished a while ago, for example a backlog after
//     an outage); skipped while the device clock is not set, and the margin covers
//     Telegram's whole-second dates and a little clock drift;
//   - the first item was seen kSettleMaxHoldMs ago (a slow upload never holds the
//     answer longer than that);
//   - the page has not grown for the quiet window: kSettleQuietMediaMs when the page
//     holds a photo, file or voice note, kSettleQuietTextMs for text only. Growth (a
//     new newest update id) restarts the window.
// Hold otherwise.
//
// A hold is a per-cycle state on the one poll task, never a wait: the device re-polls
// the same offset with timeout 0 after its normal kSettleRepollMs pause, keeping the
// keep-alive socket open, and classifies, acks and stores nothing in between (no
// settings write). Local turns, job polling and reply sends keep running between
// re-polls. A page that does not grow re-polls at most ceil(quiet / repoll) + 1 times.
// The numbers are starting points, tuned on the bench (compile-time constants).
namespace nimbus::net {

constexpr uint32_t kSettleQuietMediaMs = 3000;
constexpr uint32_t kSettleQuietTextMs = 1500;
constexpr uint32_t kSettleMaxHoldMs = 15000;
constexpr uint32_t kSettleDateMarginS = 2;
constexpr uint32_t kSettleRepollMs = 1000;
// The device clock counts as set from 2020-01-01 on.
constexpr int64_t kSettleClockSet = 1577836800LL;

// What the rule needs to know about one fetched page (no update contents).
struct SettlePage {
  int     count = 0;          // whole updates in the page
  int     limit = 10;         // the getUpdates limit of this fetch
  bool    hasMedia = false;   // some update is a photo, a file or a voice note
  int32_t newestId = 0;       // the largest update id in the page (growth)
  int64_t newestDate = 0;     // the largest Telegram date in the page (0: none)
};

enum class Settle : uint8_t { Dispatch, Hold };
enum class SettleWhy : uint8_t { Empty, Full, OldDate, MaxHold, Quiet, Waiting };

inline const char* settleWhyName(SettleWhy w) {
  switch (w) {
    case SettleWhy::Empty: return "empty";
    case SettleWhy::Full: return "full";
    case SettleWhy::OldDate: return "old";
    case SettleWhy::MaxHold: return "max-hold";
    case SettleWhy::Quiet: return "quiet";
    case SettleWhy::Waiting: return "waiting";
  }
  return "?";
}

// The quiet window for a page.
inline uint32_t settleQuietMs(bool hasMedia) { return hasMedia ? kSettleQuietMediaMs : kSettleQuietTextMs; }

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
    if (!holding_ || offset != offset_) {   // a new page: the windows start now
      holding_ = true;
      offset_ = offset;
      newestId_ = p.newestId;
      firstMs_ = lastGrowMs_ = nowMs;
      repolls_ = 0;
    } else {
      ++repolls_;
      if (p.newestId != newestId_) {   // grew: the quiet window restarts
        newestId_ = p.newestId;
        lastGrowMs_ = nowMs;
      }
    }
    const uint32_t quiet = settleQuietMs(p.hasMedia);
    if (p.count >= p.limit) return finish(SettleWhy::Full, nowMs);
    if (nowEpoch >= kSettleClockSet && p.newestDate > 0 &&
        (nowEpoch - p.newestDate) * 1000 > (int64_t)quiet + (int64_t)kSettleDateMarginS * 1000)
      return finish(SettleWhy::OldDate, nowMs);
    if ((uint32_t)(nowMs - firstMs_) >= kSettleMaxHoldMs) return finish(SettleWhy::MaxHold, nowMs);
    if ((uint32_t)(nowMs - lastGrowMs_) >= quiet) return finish(SettleWhy::Quiet, nowMs);
    why_ = SettleWhy::Waiting;
    return Settle::Hold;
  }

  // Forget the hold (after a dispatch ran the page, or a fetch failed).
  void reset() {
    holding_ = false;
    repolls_ = 0;
  }

  bool      holding() const { return holding_; }
  SettleWhy why() const { return why_; }
  // Re-polls of the current page so far (0 on its first sighting).
  int       repolls() const { return repolls_; }
  // How long the last dispatched page was held (0 for one dispatched at first sight).
  uint32_t  lastHeldMs() const { return lastHeldMs_; }

 private:
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
  int       repolls_ = 0;
  uint32_t  lastHeldMs_ = 0;
  SettleWhy why_ = SettleWhy::Empty;
};

}  // namespace nimbus::net
