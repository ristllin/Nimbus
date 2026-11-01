#include "speaker_turn.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sfx {

namespace {
SemaphoreHandle_t line() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}
}  // namespace

bool SpeakerTurn::take(uint32_t waitMs) {
  if (held_) return true;
  SemaphoreHandle_t m = line();
  if (!m) return true;   // no memory for the lock: the driver's own mutex still serializes the clips
  held_ = xSemaphoreTake(m, pdMS_TO_TICKS(waitMs)) == pdTRUE;
  return held_;
}

SpeakerTurn::~SpeakerTurn() {
  if (held_) xSemaphoreGive(line());
}

}  // namespace sfx
