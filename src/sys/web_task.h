#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

}  // namespace nimbus::sys
