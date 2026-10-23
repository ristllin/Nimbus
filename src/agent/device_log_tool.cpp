#include "device_log_tool.h"

#include <FS.h>

#include <string>

#include "memory_subsystem.h"                      // the card lock, taken per chunk
#include "nimbus/orch/device_log.h"                // the portable chunked reader
#include "../sys/errlog.h"                         // the portable path policy (paths, caps)
#include "../sys/errlog_fs.h"                      // activeFs / onSdTier / the RAM tail

namespace agent {
namespace devicelog {

namespace {

// The log's file i, newest first (0 = the active file), through errlog's own
// path policy: /log/nimbus.log, /log/nimbus.log.1, and so on, bounded by the
// tier's file count. Sizes and reads take the card lock per call, released
// before returning (the durable writer only try-locks: one long hold would make
// it skip lines).
std::string logPath(int file) {
  return nimbus::errlog::pathFor(nimbus::errlog::rotatedName((size_t)file));
}

long logSize(int file) {
  memory::Lock lk;   // recursive: safe when the turn already holds it
  const std::string path = logPath(file);
  if (!nimbus::errlog::activeFs().exists(path.c_str())) return -1;
  File f = nimbus::errlog::activeFs().open(path.c_str(), FILE_READ);
  if (!f) return -1;
  const long sz = (long)f.size();
  f.close();
  return sz;
}

size_t logRead(int file, size_t offset, char* dst, size_t n) {
  memory::Lock lk;   // held for this chunk only, released before returning
  const std::string path = logPath(file);
  File f = nimbus::errlog::activeFs().open(path.c_str(), FILE_READ);
  if (!f) return 0;
  size_t got = 0;
  if (f.seek(offset)) got = f.read((uint8_t*)dst, n);
  f.close();
  return got;
}

}  // namespace

nimbus::orch::ToolResult run(ArduinoJson::JsonObjectConst args) {
  const nimbus::orch::DeviceLogArgs a = nimbus::orch::parseDeviceLogArgs(args);
  nimbus::orch::DeviceLogSource src;
  const nimbus::errlog::TierCaps caps = nimbus::errlog::capsFor(nimbus::errlog::onSdTier());
  src.files = (int)caps.maxFiles;
  src.size = logSize;
  src.read = logRead;
  std::string out = nimbus::orch::readDeviceLog(src, a);
  if (out.empty()) {
    // No log file could be read (no card, a card that failed): the RAM tail the
    // durable writer keeps is the honest fallback, and the result says so.
    const std::string tail = nimbus::errlog::readRecent(2048);
    out = nimbus::orch::renderDeviceLog(
        tail, a, "(no log file could be read; the in-memory tail follows)");
  }
  return nimbus::orch::ToolResult::ok(out);
}

}  // namespace devicelog
}  // namespace agent
