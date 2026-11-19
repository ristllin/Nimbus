// The file replay driver: one JSON case per input line, one JSON result per output line.
// It runs the shared file aids (lib/core file_aids.h: read with column/from/to/sum, edit
// with line, the append facts) on the case's file, the code both engines and nimbusd call,
// so a tree's answer to a real file shape can be read without a device or a model.
// Built against any tree from RB-REC4 on (see README.md).
#include <ArduinoJson.h>

#include <iostream>
#include <string>

#include "fake_aid_card.h"
#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/psram_alloc.h"

namespace aids = nimbus::orch::aids;

namespace {

void putLines(JsonObject out, const char* key, const aids::LineList& l) {
  JsonObject o = out[key].to<JsonObject>();
  o["count"] = l.count;
  JsonArray a = o["lines"].to<JsonArray>();
  for (uint32_t n : l.lines) a.add(n);
}

// The refusing allocator for a case that asks for it: refuse the Nth working block.
struct Refuse {
  explicit Refuse(long at) : on(at > 0) {
    if (on) aidfake::arm(at);
  }
  ~Refuse() {
    if (on) aidfake::disarm();
  }
  bool on;
};

void runSum(JsonObjectConst in, JsonObject out) {
  aidfake::Card card;
  card.file = in["file"] | "";
  aids::CsvQuery q;
  q.column = in["column"] | "";
  q.from = in["from"] | "";
  q.to = in["to"] | "";
  for (JsonVariantConst s : in["sum"].as<JsonArrayConst>()) q.sum.push_back(s.as<std::string>());
  q.showRows = false;
  Refuse refuse(in["refuse_at"] | 0L);
  const aids::CsvScanOutcome o = aids::scanCsv(card, card.file.size(), in["path"] | "file.csv", q);
  out["ok"] = o.ok;
  if (!o.ok) {
    out["error"] = o.error;
    return;
  }
  const aids::CsvScanResult& r = o.result;
  out["matched"] = r.matched;
  out["records"] = r.records;
  JsonObject sums = out["sums"].to<JsonObject>();
  for (const aids::CsvColumnSum& c : r.sums) sums[c.name] = aids::renderDec3(c.total);
  putLines(out, "field_count", r.fieldCount);
  putLines(out, "not_compared", r.notCompared);
  putLines(out, "blank", r.blankCells);
  putLines(out, "not_number", r.notNumberCells);
  out["comments"] = r.commentLines;
  out["empty_lines"] = r.emptyLines;
  out["torn_line"] = r.tornLine;
  out["tail"] = aids::renderCsvTail(q, r);
}

void runEdit(JsonObjectConst in, JsonObject out) {
  aidfake::Card card;
  card.file = in["file"] | "";
  const std::string before = card.file;
  aids::EditRequest e;
  e.oldText = in["old"] | "";
  e.newText = in["new"] | "";
  e.line = in["line"] | 0u;
  e.all = in["all"] | false;
  const std::string path = in["path"] | "file.csv";
  Refuse refuse(in["refuse_at"] | 0L);
  const aids::EditOutcome o =
      aids::editFile(card, card, card.file.size(), aids::fnv64(aids::kFnv64Basis, card.file), path, e);
  out["ok"] = o.ok;
  out["result"] = aids::renderEdit(path, o);
  out["file"] = card.file;
  out["changed"] = card.file != before;
}

void runAppend(JsonObjectConst in, JsonObject out) {
  const std::string file = in["file"] | "", text = in["text"] | "";
  uint64_t lines = 0;
  for (char c : file) lines += c == '\n';
  if (!file.empty() && file.back() != '\n') ++lines;
  const bool whole = file.size() <= aids::kAppendTailBytes;
  const std::string tail = whole ? file : file.substr(file.size() - aids::kAppendTailBytes);
  const aids::AppendFacts f = aids::appendFacts(tail, whole, file.size(), lines, text);
  out["fence"] = f.fence;
  out["note"] = f.note;
  out["file"] = file + (f.fence ? "\n" : "") + text;
}

}  // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument in, out;
    if (deserializeJson(in, line) != DeserializationError::Ok) {
      std::cout << "{\"error\":\"bad json\"}\n";
      continue;
    }
    const std::string kind = in["kind"] | "";
    JsonObject o = out.to<JsonObject>();
    o["id"] = in["id"] | "";
    if (kind == "sum") runSum(in.as<JsonObjectConst>(), o);
    else if (kind == "edit") runEdit(in.as<JsonObjectConst>(), o);
    else if (kind == "append") runAppend(in.as<JsonObjectConst>(), o);
    else o["error"] = "unknown kind";
    serializeJson(out, std::cout);
    std::cout << "\n";
  }
  return 0;
}
