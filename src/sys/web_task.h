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

// The current web request's network clock (the AsyncTCP task only): the server opens it
// around each request handler (a middleware, nimbus::net::beginWeb) and the request's
// first slot wait starts it, so every exchange the handler starts draws on one allowance
// (web_guard.h kWebNetAllowanceMs). Outside a handler (a filler, a body chunk) each
// exchange gets a whole allowance.
struct WebRequestClock {
  bool open = false;
  bool started = false;
  uint32_t firstMs = 0;
};

inline WebRequestClock& webRequestClock() {
  static WebRequestClock c;
  return c;
}

inline void openWebRequest() { webRequestClock() = WebRequestClock{true, false, 0}; }
inline void closeWebRequest() { webRequestClock() = WebRequestClock{}; }

// What is left of this web request's network allowance, starting its clock on the first ask.
inline uint32_t webNetLeftNow() {
  WebRequestClock& c = webRequestClock();
  if (!onWebTask() || !c.open) return kWebNetAllowanceMs;
  const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
  if (!c.started) {
    c.started = true;
    c.firstMs = now;
  }
  return webNetLeftMs(now - c.firstMs);
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
