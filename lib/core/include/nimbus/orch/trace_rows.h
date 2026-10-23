#pragma once
// trace_rows - the glass-box row one tool call leaves, assembled in one portable
// place so its honesty marks are host-tested (L19-24, CUM-676). The device hook
// (orchestrator.cpp) supplies the call's full name/args/output and a writer that
// parks the full text as a content-addressed sidecar; this builds the row the
// episodic store keeps: the short clips for the chat view, the sidecar's path
// when the write landed, and - when the write failed - the lost-sidecar mark,
// so the trace view and the model's results_get learn the full text is missing
// instead of silently showing only the clipped text.
#include <cstddef>
#include <functional>
#include <string>

namespace nimbus {
namespace orch {

// Appended to the row when the full text was clipped and the sidecar write
// failed (the SD open fails under internal-heap starvation): the row is
// captured with the clipped text only, and nothing else would tell anyone.
inline constexpr char kTraceBlobLostMark[] = "\n[full text not kept: low memory]";

// Parks the already cap-trimmed full call+result text as a sidecar; returns its
// path, "" when none was written. *failed (may be null) is true only when a
// sidecar SHOULD have been written and the write failed; a caller that has no
// card (degraded mode keeps media ephemeral) is not a failure.
using TraceBlobWriter = std::function<std::string(const std::string& full, bool* failed)>;

struct TraceToolRow {
  std::string name;                    // the tool's name
  std::string args;                    // the FULL args, as the model sent them
  std::string output;                  // the FULL result text
  int  argsRowCap = 256;               // the row's args clip (ORCH_TRACE_ARGS_MAX)
  int  outRowCap = 1024;               // the row's output clip (ORCH_TRACE_OUT_MAX)
  int  blobCap = 65536;                // the sidecar's own cap (ORCH_TRACE_BLOB_MAX)
  bool blobWanted = true;              // the per-turn sidecar budget is not spent
};

struct TraceToolRowOut {
  std::string text;                // "name(args)\n<arrow> output" (+ the mark)
  std::string blobPath;            // "" when no sidecar was written
  bool blobFailed = false;         // a wanted sidecar write failed (L19-24)
  bool clipped = false;            // anything was clipped (a sidecar was wanted)
};

TraceToolRowOut buildTraceToolRow(const TraceToolRow& in, const TraceBlobWriter& writeBlob);

}  // namespace orch
}  // namespace nimbus
