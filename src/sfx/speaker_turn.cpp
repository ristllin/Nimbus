#include "speaker_turn.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

namespace sfx {

namespace {
// The turn's mutex, created on first use and RETRIED while creation fails: a
// heap-exhausted first attempt must not refuse every sound forever. Two tasks
// (sfx, music) may race the retry; the compare_exchange makes one mutex win and
// the loser's creation leaks (a few dozen bytes, once, only in the exhausted
// moment that caused the race). Creation runs OUTSIDE any critical section.
std::atomic<SemaphoreHandle_t>& line() {
  static std::atomic<SemaphoreHandle_t> mutex{nullptr};
  return mutex;
}
}  // namespace

bool SpeakerTurn::take(uint32_t waitMs) {
  if (held_) return true;
  SemaphoreHandle_t m = line().load(std::memory_order_acquire);
  if (!m) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return false;   // still exhausted: refuse, the next take retries
    SemaphoreHandle_t expected = nullptr;
    if (!line().compare_exchange_strong(expected, created)) m = expected;
    else m = created;
  }
  // No phantom turn: the caller is told it holds the turn only when the mutex
  // really is held. A null lock REFUSES the sound instead - the turn exists to
  // serialize the DMA-memory reads and the speaker open between the sfx and
  // music tasks, so "true with nothing held" let both open the speaker at once
  // (the 1 Nov 2026 Lumi failure class: a failed open left the radio's buffers
  // with nothing).
  held_ = xSemaphoreTake(m, pdMS_TO_TICKS(waitMs)) == pdTRUE;
  return held_;
}

SpeakerTurn::~SpeakerTurn() {
  if (held_) xSemaphoreGive(line().load(std::memory_order_acquire));
}

}  // namespace sfx
