#pragma once
// device_log_tool - the device half of the device_log tool (CUM-646): it wires
// the portable chunked reader (nimbus/orch/device_log.h) to the durable log's
// public seams - errlog's path policy and activeFs(), never errlog_fs.cpp's
// internals - and falls back to the RAM tail when no log file can be read.
//
// The card lock is taken per chunk and released between chunks (the durable
// writer only try-locks, so one long hold would make it skip lines), and the
// chunk buffer comes from the working allocator, so a 4 KB scan never lands on
// the internal heap in small pieces.
//
// Registered on the shared tool registry beside device.status (main.cpp).
// Host-tested through the portable core (test_orch_device_log); the device
// half is a thin seam over that core.

#include "ArduinoJson.h"
#include "nimbus/orch/tool_registry.h"

namespace agent {
namespace devicelog {

// The tool's handler: `args` carries `match` (up to 3 '|' separated literals),
// `lines` (1..60) and `scan_kb` (4..64), clamped by the portable parser.
nimbus::orch::ToolResult run(ArduinoJson::JsonObjectConst args);

}  // namespace devicelog
}  // namespace agent
