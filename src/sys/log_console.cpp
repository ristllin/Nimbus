#include "log_console.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>

namespace nimbus::logconsole {
namespace {

constexpr size_t kLineBytes = 192;   // most IDF log lines fit; a longer one goes to PSRAM
constexpr char kCut[] = " [log line cut: no memory for the rest]\n";

void out(const char* p, size_t n) {
  while (n > 0) {
    const ssize_t w = write(STDOUT_FILENO, p, n);
    if (w <= 0) return;   // the console is gone: nothing to tell
    p += w;
    n -= (size_t)w;
  }
}

int consoleVprintf(const char* fmt, va_list ap) {
  char buf[kLineBytes];
  va_list again;
  va_copy(again, ap);
  const int n = vsnprintf(buf, sizeof buf, fmt, ap);
  if (n < 0) {
    va_end(again);
    return n;
  }
  if ((size_t)n < sizeof buf) {
    va_end(again);
    out(buf, (size_t)n);
    return n;
  }
  char* big = (char*)heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!big) {
    va_end(again);
    out(buf, sizeof buf - 1);
    out(kCut, sizeof kCut - 1);
    return n;
  }
  vsnprintf(big, (size_t)n + 1, fmt, again);
  va_end(again);
  out(big, (size_t)n);
  heap_caps_free(big);
  return n;
}

}  // namespace

void install() { esp_log_set_vprintf(consoleVprintf); }

}  // namespace nimbus::logconsole
