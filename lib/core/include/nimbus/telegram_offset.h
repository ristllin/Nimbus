#pragma once
#include <cstdint>

// Ported verbatim from Nuage-Solide lib/core/telegram_offset.h.
//
// Telegram long-poll offset arithmetic, isolated here so it is host-testable (the
// rest of the telegram client pulls in WiFiClientSecure and can't build natively).
//
// getUpdates?offset=N returns every update with update_id >= N and CONFIRMS
// (deletes server-side) those with id < N. So after handling an update with id
// `updateId`, the next offset must be updateId + 1 - otherwise the same update is
// re-delivered on every poll forever (re-running the decider and flooding TLS).
namespace nimbus {
namespace core {

inline int32_t nextTelegramOffset(int32_t currentOffset, int32_t updateId) {
  int32_t next = updateId + 1;
  return next > currentOffset ? next : currentOffset;  // never move the offset backwards
}

// TgOffsetCursor - the getUpdates offset the poll loop works from.
//
// The replay loop it ends (14 Oct 2026): the settings storage (NVS) reached 0 free
// entries, the offset write after each drain failed without anyone looking at its
// result, and the poll loop re-read the stale stored offset after every drain. Every
// poll then asked Telegram for the same page again: one photo was taken in 17 times,
// one batch of 4 messages was handled 11 times, and tool side effects repeated.
//
// The rule: RAM is the source of truth while the device runs. The cursor is seeded
// ONCE from the stored value when the poll loop starts and then only moves forward.
// Telegram confirms (deletes) every update below the offset a getUpdates carries, so
// the next poll confirms the drained page whether or not the write landed. The stored
// copy matters only across a restart: a restart between a drain and the next poll
// re-serves that one page (at-least-once, as before), and every earlier page was
// already confirmed server-side by a later poll, so at most one page ever replays.
//
// Persisting: one write attempt per new ack (a page, or each bucket of a page that
// holds a command, while writes land) and, while a write is owed and failing, at most
// one retry per kRetryQuietMs. The cap keeps the persist-failure
// count and the device log readable on a full NVS (one failure per page instead of
// one per poll cycle). It is not about flash wear: a refused one-entry int write
// cannot force a page erase at 0 free entries.
//
// Single owner: the poll task. Other tasks read the published counters through the
// device seam (telegram::pollStatus), never this object.
class TgOffsetCursor {
 public:
  static constexpr uint32_t kRetryQuietMs = 60000;

  // Seed from the stored offset (an absent key reads 0). Called once, when the poll
  // loop starts; a negative value (never written by this firmware) reads as 0.
  void seed(int32_t stored) {
    offset_ = stored > 0 ? stored : 0;
    stored_ = offset_;
    newAck_ = false;
    pageFailed_ = false;
  }

  int32_t offset() const { return offset_; }

  // Move forward to an ack offset (a page's, or a bucket's inside a page). Never
  // backward. True when it moved: a write is owed, and attempted on the next persist().
  bool advance(int32_t ackOffset) {
    if (ackOffset <= offset_) return false;
    offset_ = ackOffset;
    if (!pageFailed_) newAck_ = true;   // a page that already failed a write waits
    ++acks_;
    return true;
  }

  // Live token swap: the stored offset belongs to the old bot, and a stale high
  // offset on a new bot skips its messages forever. Start the new bot at 0; the 0 is
  // persisted as an erase, which lands even on a full NVS.
  void reset() {
    offset_ = 0;
    newAck_ = true;
    pageFailed_ = false;
    ++acks_;
  }

  // The stored copy matches RAM (false while a write is owed).
  bool saved() const { return offset_ == stored_; }

  // A new page is about to run (drainCycle calls this after a good fetch). A page gets
  // at most one failed write: once a write failed inside it (a checkpoint on a full
  // NVS), its later acks owe no immediate write and wait for the once-a-minute retry.
  void pageStart() { pageFailed_ = false; }

  // Called after every drain and on every idle cycle. When a write is owed and due,
  // calls write(offset) -> bool (true when it landed) and returns true. A write is due
  // at once for a new ack, otherwise at most once per kRetryQuietMs. (Whenever a write
  // is owed without a new ack, an attempt already happened, so lastTryMs_ is valid.)
  template <class WriteFn>
  bool persist(uint32_t nowMs, WriteFn write) {
    if (offset_ == stored_) {
      newAck_ = false;
      return false;
    }
    if (!newAck_ && (uint32_t)(nowMs - lastTryMs_) < kRetryQuietMs) return false;
    attempt(nowMs, write);
    return true;
  }

  // Inside a page with a command: store the offset after each bucket, so a restart
  // re-runs at most the bucket in flight (a command whose turn finished, and whose
  // checkpoint landed, is not run again). Every
  // command page tries, even after an earlier failure (room may have come back); once
  // a write failed inside this page (a full NVS) it stops, so the page costs at most
  // one failed write and the retry falls back to persist()'s once-a-minute pace.
  template <class WriteFn>
  bool checkpoint(uint32_t nowMs, WriteFn write) {
    if (offset_ == stored_ || pageFailed_) return false;
    attempt(nowMs, write);
    return true;
  }

  // The long-poll timeout (seconds) for the next cycle. While a write is owed, it is
  // cut so the cycle comes round when the retry is due: an owed write then lands
  // within kRetryQuietMs of room appearing even on an idle bot (a 30 s long-poll would
  // otherwise add up to 30 s), for at most one extra getUpdates a minute.
  int longPollS(int wantS, uint32_t nowMs) const {
    if (offset_ == stored_ || newAck_) return wantS;
    const uint32_t since = nowMs - lastTryMs_;
    const uint32_t left = since >= kRetryQuietMs ? 0 : kRetryQuietMs - since;
    const int s = (int)((left + 999) / 1000);
    if (s < 1) return 1;
    return s < wantS ? s : wantS;
  }

  // Drop guard: false (and counted) for an update id below the cursor, which Telegram
  // has already confirmed. Never expected while the cursor rule holds; the count is
  // the evidence that it does.
  bool admit(int32_t updateId) {
    if (updateId >= offset_) return true;
    ++staleDropped_;
    return false;
  }

  uint32_t acks() const { return acks_; }   // times the offset moved (or was reset)
  uint32_t writeAttempts() const { return writes_; }
  uint32_t writeFails() const { return writeFails_; }
  uint32_t staleDropped() const { return staleDropped_; }

 private:
  template <class WriteFn>
  void attempt(uint32_t nowMs, WriteFn write) {
    newAck_ = false;
    lastTryMs_ = nowMs;
    ++writes_;
    if (write(offset_)) {
      stored_ = offset_;
    } else {
      ++writeFails_;
      pageFailed_ = true;
    }
  }

  int32_t  offset_ = 0;          // RAM truth: the offset the next getUpdates carries
  int32_t  stored_ = 0;          // the last value known to be in NVS
  bool     newAck_ = false;      // a new ack owes a write now
  bool     pageFailed_ = false;  // a write failed inside the current page
  uint32_t lastTryMs_ = 0;
  uint32_t acks_ = 0;
  uint32_t writes_ = 0;
  uint32_t writeFails_ = 0;
  uint32_t staleDropped_ = 0;
};

}  // namespace core
}  // namespace nimbus
