#pragma once
#include <cstdarg>
#include <cstdio>

// hlog - the harness's one logging seam. The portable library never talks to a
// device log ring or Serial directly: it formats into a bounded stack buffer and
// hands the line to an installed sink. The device installs agent_log's alog();
// host tests install a recorder (or nothing - a null sink drops lines, never
// crashes). Mirrors the injected-log convention head_loop.h already proved.
namespace agent {
namespace hlog {

using Sink = void (*)(const char* line);

inline Sink& sinkRef() { static Sink s = nullptr; return s; }
inline void setSink(Sink s) { sinkRef() = s; }

inline void log(const char* line) {
  Sink s = sinkRef();
  if (s && line) s(line);
}

inline void logf(const char* fmt, ...) {
  Sink s = sinkRef();
  if (!s || !fmt) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = 0;
  s(buf);
}

}  // namespace hlog
}  // namespace agent
