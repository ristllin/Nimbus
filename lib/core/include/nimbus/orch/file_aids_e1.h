#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/file_edit.h"   // EditPolicy, CsvEditRow: files.edit's own rules and texts

// file_aids_e1 - the old engine's files.read and files.edit on the shared aids (RB-REC4 part
// 2b, rulings 47, 49 and 62). The device (files_subsystem.cpp) and nimbusd (posix_files.h)
// move bytes through AidSource/AidSink and call these, so both give the same reply bytes.
//
// files.read keeps its call shape {tail, sum (one column), date} and gains the general one
// beside it {column, from, to, sum (a list), offset, limit}; both run the one CSV scan
// (comment lines skipped and counted, a wrong-field-count row placed by its key, every row
// left out named by line), so the same day gives the same total either way.
namespace nimbus::orch::aids::e1 {

// files.read {sum, date}: the day's total of one column, as the scan's tail line ("rows m
// of n where <date column> D..D; sum <col>=<v>; ..."), then "\n". The date column is the
// header's (csvDateColumnOf over the file's first rows). Errors read "[no day total: ...]\n".
std::string dayTotal(AidSource& src, uint64_t size, const std::string& path, const std::string& sumCol,
                     const std::string& day);

// The general form's arguments: column, from, to (strings), sum (a list of column names, or
// one string of names separated by commas), offset (matching rows not shown); the page holds
// kE1PageBytes of rows. False with err (a wrong type, or checkCsvQuery's error).
inline constexpr size_t kE1PageBytes = 8192 - 64;
bool queryArgs(ArduinoJson::JsonObjectConst a, size_t offset, CsvQuery& q, std::string& err);

// files.read {column, from, to, sum[], offset, limit}: "<path> size=<B> lines=<n>", then the
// shown rows and the tail (renderCsvRead). False with out = the error.
bool tableRead(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q, std::string& out);

// The rows an edit removed, kept to name those a later re-add skips (one file at a time).
struct RemovedRows {
  std::string project, name;
  std::vector<CsvEditRow> rows;
};

// One files.edit call. header: the file's first row when it is a CSV ("" otherwise).
struct EditCall {
  std::string project, name, find, replace;
  EditPolicy pol;
  uint32_t line = 0;   // 0 = no pick
  std::string header;
};

// files.edit through the shared editFile: the first match by default, the first `count`, every
// one with all, or the whole-line match at `line`. The binding's sink admits quota and room
// (its admit); this checks files.edit's own caps first. reply: "edited p/n: replaced A of B
// match(es) (now N bytes)" + renderEditLines + the keyed-CSV row report; or the error (false).
bool edit(AidSource& src, AidSink& sink, uint64_t size, const EditCall& c, RemovedRows& memory, std::string& reply);

// "YYYY-MM-DD" of the running day (day_start.h) for `epoch` at `offsetMin`, "" with no clock.
std::string runningDate(uint64_t epoch, int32_t offsetMin, uint16_t startMin);

}  // namespace nimbus::orch::aids::e1
