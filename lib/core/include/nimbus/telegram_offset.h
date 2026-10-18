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
// Persisting: one write attempt per newly acked page and, while a write is owed and
// failing, at most one retry per kRetryQuietMs. The cap keeps the persist-failure
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
    tried_ = false;
  }

  int32_t offset() const { return offset_; }

  // Move forward to a drain's ack offset. Never backward. True when it moved (a new
  // page was acked, so a write is owed and attempted on the next persist()).
  bool advance(int32_t ackOffset) {
    if (ackOffset <= offset_) return false;
    offset_ = ackOffset;
    newAck_ = true;
    return true;
  }

  // Live token swap: the stored offset belongs to the old bot, and a stale high
  // offset on a new bot skips its messages forever. Start the new bot at 0; the 0 is
  // persisted as an erase, which lands even on a full NVS.
  void reset() {
    offset_ = 0;
    newAck_ = true;
  }

  // The stored copy matches RAM (false while a write is owed).
  bool saved() const { return offset_ == stored_; }

  // Called after every drain and on every idle cycle. When a write is owed and due,
  // calls write(offset) -> bool (true when it landed) and returns true. A write is due
  // at once for a newly acked page, otherwise at most once per kRetryQuietMs.
  template <class WriteFn>
  bool persist(uint32_t nowMs, WriteFn write) {
    if (offset_ == stored_) {
      newAck_ = false;
      return false;
    }
    const bool due = newAck_ || !tried_ || (uint32_t)(nowMs - lastTryMs_) >= kRetryQuietMs;
    if (!due) return false;
    newAck_ = false;
    tried_ = true;
    lastTryMs_ = nowMs;
    ++writes_;
    if (write(offset_)) {
      stored_ = offset_;
    } else {
      ++writeFails_;
    }
    return true;
  }

  // Drop guard: false (and counted) for an update id below the cursor, which Telegram
  // has already confirmed. Never expected while the cursor rule holds; the count is
  // the evidence that it does.
  bool admit(int32_t updateId) {
    if (updateId >= offset_) return true;
    ++staleDropped_;
    return false;
  }

  uint32_t writeAttempts() const { return writes_; }
  uint32_t writeFails() const { return writeFails_; }
  uint32_t staleDropped() const { return staleDropped_; }

 private:
  int32_t  offset_ = 0;        // RAM truth: the offset the next getUpdates carries
  int32_t  stored_ = 0;        // the last value known to be in NVS
  bool     newAck_ = false;    // a newly acked page owes a write now
  bool     tried_ = false;     // lastTryMs_ is valid
  uint32_t lastTryMs_ = 0;
  uint32_t writes_ = 0;
  uint32_t writeFails_ = 0;
  uint32_t staleDropped_ = 0;
};

}  // namespace core
}  // namespace nimbus
