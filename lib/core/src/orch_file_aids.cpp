// orch_file_aids - the line-picked edit, the append facts and the shared list rules
// (file_aids.h).
#include "nimbus/orch/file_aids.h"

namespace nimbus::orch::aids {

std::string checkEdit(const EditRequest&) { return ""; }
EditOutcome editFile(AidSource&, AidSink&, uint64_t, uint64_t, const std::string&, const EditRequest&) { return {}; }
std::string renderEdit(const std::string&, const EditOutcome&) { return ""; }
AppendFacts appendFacts(std::string_view, bool, uint64_t, uint64_t, std::string_view) { return {}; }
std::string renderLines(const LineList&) { return ""; }
uint64_t fnv64(uint64_t h, std::string_view) { return h; }
std::string sha8Hex(uint64_t) { return ""; }

}  // namespace nimbus::orch::aids
