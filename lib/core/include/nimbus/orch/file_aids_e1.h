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

// Which form a files.read call takes: column, from or to -> Table; else sum -> DayTotal (with
// the tail view above it when tail is given); else tail -> Tail; else the plain paged read.
enum class ReadForm : uint8_t { Plain, Tail, DayTotal, Table };
ReadForm readForm(ArduinoJson::JsonObjectConst a);

// files.read {sum, date}: the day's total of the sum columns, as the scan's tail line ("rows m
// of n where <date column> D..D; sum <col>=<v>; ..."), then "\n". The date column is the
// header's (csvDateColumnOf over the file's first rows). Errors read "[no day total: ...]\n".
std::string dayTotal(AidSource& src, uint64_t size, const std::string& path, const std::vector<std::string>& sums,
                     const std::string& day);

// The general form's arguments: column, from, to (strings), sum (a list of column names, or
// one string of names separated by commas), offset (matching rows not shown); the page holds
// kE1PageBytes of rows. False with err (a wrong type, or checkCsvQuery's error).
inline constexpr size_t kE1PageBytes = 8192 - 64;
bool queryArgs(ArduinoJson::JsonObjectConst a, size_t offset, CsvQuery& q, std::string& err);

// files.read {column, from, to, sum[], offset, limit}: "<path> size=<B> lines=<n>", then the
// shown rows and the tail (renderCsvRead). False with out = the error.
bool tableRead(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q, std::string& out);

// files.read's plain form (RB-TR1): the part of a text file from byte `offset` that fits in a
// result of `cap` bytes, under one head line naming which part it is and how to get the next:
//   "bytes a-b of n, lines x-y of m; next part: \"offset\":b\n" then bytes [a, b), or
//   "...; end of file\n" on the last part ("bytes n-n of n; end of file\n" past the end).
// A part ends at a line end when one falls in its second half, else on a character boundary;
// an offset inside a character starts at the next one. Every part but the last holds at least
// one byte, so following "next part" always reaches the end. Each call reads the whole file
// once (the line count), 4 KB per read. The file is streamed kAidChunk at a
// time (any size); `size` is the stored size and a read that ends short of it is the card
// fault. False with out = the error.
bool readPart(AidSource& src, uint64_t size, uint64_t offset, size_t cap, std::string& out);
// files.read's offset argument: a number of 0 or more, held to 2^53 (a larger or a negative
// one never wraps); 0 when absent or not a number.
uint64_t offsetArg(ArduinoJson::JsonObjectConst a);
// The smallest cap readPart takes (a smaller one is raised to it).
inline constexpr size_t kPartMinCap = 512;

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
