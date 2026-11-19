// orch_csv_sum - the CSV scan behind read_file column/from/to/sum (file_aids.h).
#include "nimbus/orch/file_aids.h"

namespace nimbus::orch::aids {

struct CsvScanner::Impl {};

NumParse parseDec3(std::string_view, Dec3& out) {
  out.milli = 0;
  return NumParse::NotNumber;
}
std::string renderDec3(Dec3) { return ""; }
std::string checkCsvQuery(const CsvQuery&) { return ""; }
CsvScanner::CsvScanner(const CsvQuery&, const std::string&) {}
CsvScanner::~CsvScanner() {}
bool CsvScanner::feed(const char*, size_t) { return false; }
bool CsvScanner::finish() { return false; }
CsvScanOutcome scanCsv(AidSource&, uint64_t, const std::string&, const CsvQuery&) { return {}; }
std::string renderCsvTail(const CsvQuery&, const CsvScanResult&) { return ""; }
std::string renderCsvRead(const CsvQuery&, const CsvScanResult&) { return ""; }

}  // namespace nimbus::orch::aids
