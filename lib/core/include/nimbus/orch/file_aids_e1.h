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
// files.read keeps {tail} and its paged read and has the general selection {column, from, to,
// offset} on the one CSV scan (comment lines skipped and counted, a wrong-field-count row
// shown and named by line). It totals nothing: ruling 95 removed sum (and the {tail, sum,
// date} day total with it).
namespace nimbus::orch::aids::e1 {

// Which form a files.read call takes: column, from or to -> Table; else tail -> Tail; else the
// plain paged read.
enum class ReadForm : uint8_t { Plain, Tail, Table };
ReadForm readForm(ArduinoJson::JsonObjectConst a);

// The general form's arguments: column, from, to (strings), offset (a file byte, as a plain
// read's); the part holds kE1PageBytes of rows. False with err (a wrong type, or checkCsvQuery's error).
inline constexpr size_t kE1PageBytes = 8192 - 64;
bool queryArgs(ArduinoJson::JsonObjectConst a, size_t offset, CsvQuery& q, std::string& err);

// files.read {column, from, to, offset} (ruling 97): the same part head as a plain read
// (partHeadInto), then the header (in the part that holds it) and the selected rows as stored
// (file_aids.h CsvQuery). False with out = the error.
// The part head of every files.read, plain or a selection (RB-TR1's form). firstLine 0: no
// lines clause (an empty part). movedFrom: the asked offset when it fell inside a character.
struct PartHead {
  uint64_t from, to, size;
  uint64_t firstLine, lastLine, lines;
  bool more;
};
void partHeadInto(detail::Txt& t, const PartHead& h, const uint64_t* movedFrom);

bool tableRead(AidSource& src, uint64_t size, const std::string& path, const CsvQuery& q, std::string& out);

// files.read's plain form (RB-TR1): the part of a text file from byte `offset` that fits in a
// result of `cap` bytes, under one head line naming which part it is and how to get the next:
//   "bytes a-b of n, lines x-y of m; next part: \"offset\":b\n" then bytes [a, b), or
//   "...; end of file\n" on the last part ("bytes n-n of n; end of file\n" past the end).
// A part ends at a line end when one falls in its second half, else on a character boundary;
// an offset inside a character starts at the next one, and the head says so
// ("bytes a-b of n (\"offset\":X was inside a character), lines ..."). Every part but the last holds at least
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

}  // namespace nimbus::orch::aids::e1
