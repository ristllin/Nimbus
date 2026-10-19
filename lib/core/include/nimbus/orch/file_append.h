#pragma once
#include <cstddef>
#include <string>
#include <vector>

// file_append - the portable rules behind files.append and the files.read tail
// view (host-tested; the device and nimbusd only move bytes).
//
// Logging a row used to cost a whole-file read plus a whole-file rewrite
// (files.read, then artifact.save of the entire log), and every byte of both rode
// the turn's prompt. files.append writes only the new rows at the end of the
// file; the tail view reads only the end of the file. Neither ever rewrites a byte
// that is already stored.
namespace nimbus::orch {

// One append call carries at most this much text (a few rows, never a file).
inline constexpr size_t kAppendMaxBytes = 4096;
// The tail view: default and maximum row counts, and the window read from the
// end of the file (it fits one tool result with room for the header lines).
inline constexpr size_t kTailDefaultRows = 20;
inline constexpr size_t kTailMaxRows = 200;
inline constexpr size_t kTailWindowBytes = 7680;

// The bytes to write for one append, or an error. Rows are normalized to "\n"
// line ends and always end with one. When the file's last byte is not a line end
// (a row cut by a power loss or a hand edit) the new rows start on a fresh line
// and `repairedCut` says so - the cut row is reported, never silently joined to
// the new one.
struct AppendPlan {
  bool ok = false;
  std::string bytes;     // exactly what to append
  std::string err;       // why not, when !ok
  bool repairedCut = false;
  int rows = 0;          // rows in `bytes`
};
AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte);

// The tail view over the last bytes of a file. `window` is the end of the file;
// `windowStart` is its offset in the file (0 = the window is the whole file).
// A window that starts mid-file drops its first, partial row.
struct TailView {
  std::vector<std::string> rows;   // the last N complete rows, oldest first
  bool earlier = false;            // rows above these exist and are not shown
  bool lastCut = false;            // the file does not end in a line end: the
                                   // last row is incomplete (cut mid-write)
};
TailView tailRows(const std::string& window, size_t windowStart, size_t n);

// The tool text: a heading naming what is shown, the optional header row (a CSV's
// first line), the rows, and a closing note when the last row is incomplete.
std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow);

}  // namespace nimbus::orch
