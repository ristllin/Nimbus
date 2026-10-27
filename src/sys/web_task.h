#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

#include "nimbus/sys/web_guard.h"

// The web server's task (nimbus/sys/web_guard.h). Every web handler runs on the
// AsyncTCP service task ("async_tcp"), which the task watchdog watches. The web server
// records it once the server has started (nimbus::net::beginWeb); before that no
// handler can run, so onWebTask() is false.
namespace nimbus::sys {

inline TaskHandle_t& webTaskSlot() {
  static TaskHandle_t h = nullptr;
  return h;
}

// Called once the AsyncTCP task exists (it starts with the first server begin()).
inline void setWebTask(TaskHandle_t h) { webTaskSlot() = h; }

inline TaskHandle_t webTask() { return webTaskSlot(); }

// True on the AsyncTCP task: the caller is a web handler, a response filler or a
// request's body or disconnect callback.
inline bool onWebTask() {
  const TaskHandle_t h = webTaskSlot();
  return h != nullptr && h == xTaskGetCurrentTaskHandle();
}

// Takes a state lock that a writer holds across a flash write (the spend limits, the
// usage ledger). Off the web task it waits as before; on it, it tries for
// kWebFillerWaitMs and reports false, and the caller answers from what it last read.
template <class M>
inline bool webTake(std::unique_lock<M>& lk) {
  if (!onWebTask()) {
    lk.lock();
    return true;
  }
  const TickType_t start = xTaskGetTickCount();
  while (!lk.try_lock()) {
    if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= kWebFillerWaitMs) return false;
    vTaskDelay(1);
  }
  return true;
}

}  // namespace nimbus::sys
