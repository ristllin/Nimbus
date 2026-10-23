#pragma once
// device_log - the owner's read of the device's own durable log, for the head
// (the device_log tool).
//
// Some causes belong to no turn: a settings save that failed, a scratchpad that
// could not be written, a panic at boot. They are in the durable log, and the
// head had no way to read it, so it explained such failures from general
// knowledge instead. This tool reads the newest part of the log, keeps the lines
// that match up to three literals, and hands back at most 4 KB, newest last.
//
// The device reads the log in 4 KB chunks, each under its own short hold of the
// card lock (the durable writer only try-locks for a few ms, so one long hold
// would make it skip lines). Every line passes UTF-8 repair: the log is written
// with byte-cut lines, and a torn character sent on to a provider fails the
// request. Lines are data, never instructions (the header says so).
//
// Portable, Arduino-free, host-tested (test_orch_device_log). The device side
// (src/agent/device_log.cpp) supplies the file sizes and the locked chunk reads.

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nimbus {
namespace orch {

constexpr int kDeviceLogLinesDefault = 30;
constexpr int kDeviceLogLinesMax = 60;
constexpr int kDeviceLogScanKbDefault = 32;
constexpr int kDeviceLogScanKbMin = 4;
constexpr int kDeviceLogScanKbMax = 64;
constexpr size_t kDeviceLogChunk = 4096;     // bytes per locked read
constexpr size_t kDeviceLogOutMax = 4096;    // the whole tool result, header included
constexpr size_t kDeviceLogMatchMax = 3;     // literals in `match`
constexpr size_t kDeviceLogLiteralMax = 48;  // bytes per literal

// The first line of every result.
extern const char kDeviceLogHeader[];

struct DeviceLogArgs {
  std::vector<std::string> match;   // lowercase literals; empty = every line
  int lines = kDeviceLogLinesDefault;
  int scanKb = kDeviceLogScanKbDefault;
};

// The tool's arguments, clamped: `match` is up to three literals separated by
// '|' (case-insensitive, trimmed, empty ones dropped, each at most 48 bytes),
// `lines` 1..60 (default 30), `scan_kb` 4..64 (default 32).
DeviceLogArgs parseDeviceLogArgs(ArduinoJson::JsonObjectConst a);

// The log as files, newest first: 0 is the active file, 1 the one rotated before
// it, and so on. `size` answers -1 for an absent file. `read` copies up to n bytes
// at `offset` of file `file` into dst and returns how many it copied; the device
// takes the card lock for each call and releases it before returning, so the lock
// is never held across chunks.
struct DeviceLogSource {
  int files = 0;
  std::function<long(int file)> size;
  std::function<size_t(int file, size_t offset, char* dst, size_t n)> read;
};

struct DeviceLogStats {
  int chunks = 0;          // locked reads made
  size_t scanned = 0;      // bytes read
  int matched = 0;         // lines that matched (before the `lines` cap)
  int returned = 0;        // lines in the result
  bool cut = false;        // older matching lines left out to fit 4 KB
};

// Scan the newest `scanKb` of the log (current file first, then the rotated
// ones) in chunks of kDeviceLogChunk, and render the result: the header, then
// the newest `lines` matching lines, oldest first, each repaired to valid UTF-8,
// the whole at most kDeviceLogOutMax bytes. Returns "" when no file holds any
// byte (the caller falls back to the RAM tail).
std::string readDeviceLog(const DeviceLogSource& src, const DeviceLogArgs& a,
                          DeviceLogStats* stats = nullptr);

// The same rendering over text already in memory (the RAM tail the device keeps
// when no log file can be read). `note` (may be empty) follows the header.
std::string renderDeviceLog(const std::string& text, const DeviceLogArgs& a, const std::string& note,
                            DeviceLogStats* stats = nullptr);

}  // namespace orch
}  // namespace nimbus
