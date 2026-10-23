#include "nimbus/orch/trace_rows.h"

#include "nimbus/utf8.h"

namespace nimbus {
namespace orch {

TraceToolRowOut buildTraceToolRow(const TraceToolRow& in, const TraceBlobWriter& writeBlob) {
  TraceToolRowOut out;
  // The row's clips: the chat view stays small; a clipped tail is named as one.
  const int okeep = utf8CapLen(in.output.data(), (int)in.output.size(), in.outRowCap);
  const bool outClipped = okeep < (int)in.output.size();
  const std::string outRow =
      outClipped ? in.output.substr(0, (size_t)okeep) + "\xE2\x80\xA6[truncated]" : in.output;
  const int akeep = utf8CapLen(in.args.data(), (int)in.args.size(), in.argsRowCap);
  const bool argsClipped = akeep < (int)in.args.size();
  const std::string argsRow =
      argsClipped ? in.args.substr(0, (size_t)akeep) + "\xE2\x80\xA6" : in.args;
  out.clipped = outClipped || argsClipped;
  out.text = in.name + "(" + argsRow + ")\n\xE2\x86\x92 " + outRow;
  if (!out.clipped || !in.blobWanted) return out;
  // The sidecar keeps what the model actually saw: the full args and output,
  // under its own (per-turn-budgeted) cap.
  std::string full = in.name + "(" + in.args + ")\n\xE2\x86\x92 " + in.output;
  const int fkeep = utf8CapLen(full.data(), (int)full.size(), in.blobCap);
  if (fkeep < (int)full.size()) full = full.substr(0, (size_t)fkeep);
  bool failed = false;
  const std::string path = writeBlob ? writeBlob(full, &failed) : std::string();
  if (!path.empty()) {
    out.blobPath = path;
    return out;
  }
  // The full text did not land (the SD open failed under internal-heap
  // starvation): the row says so instead of quietly showing only the clip.
  if (failed) {
    out.blobFailed = true;
    out.text += kTraceBlobLostMark;
  }
  return out;
}

}  // namespace orch
}  // namespace nimbus
