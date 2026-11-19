#pragma once
// The old engine's files.read and files.edit on the shared aids (file_aids_e1.h): the same
// calls on the same files, run (a) through the shared functions over a fake card, exactly
// as the device's files_subsystem.cpp calls them (test_file_aids_e1), and (b) through
// nimbusd's PosixFiles tools on its disk (nimbusd/tests/test_hosted_file_aids.cpp). Both
// transcripts must equal test/golden/file_aids_e1_results.txt byte for byte.
//
// The files are test/support/file_aids_cases.h's three equal domains in the owner file's
// shape (ruling 49). files.read {tail, sum, date} is compared on its day-total line (the tail
// view above it is RB-REC1's renderTail, unchanged).
#include <ArduinoJson.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "file_aids_cases.h"
#include "nimbus/orch/file_aids_e1.h"
#include "nimbus/orch/file_append.h"

namespace aidcases::e1 {

struct Call {
  const char* name;
  const char* path;
  const char* seed;   // the file before the call (nullptr: the file as the last call left it)
  const char* tool;   // files.read or files.edit
  const char* args;   // the arguments past project and name
};

inline const std::vector<Call>& calls() {
  static const std::vector<Call> c = {
      {"old shape: a day's total", "log/f.csv", kFood, "files.read", R"("tail":3,"sum":"kcal","date":"2026-11-19")"},
      {"old shape with no tail: the day's total alone", "log/f.csv", nullptr, "files.read",
       R"("sum":"kcal","date":"2026-11-19")"},
      {"new shape: the same day", "log/f.csv", nullptr, "files.read", R"("column":"date","from":"2026-11-19","sum":"kcal")"},
      {"new shape: two columns", "log/f.csv", nullptr, "files.read",
       R"("column":"date","from":"2026-11-19","sum":"grams,kcal")"},
      {"new shape: a week, paged", "log/t.csv", kTraining, "files.read",
       R"("column":"date","from":"2026-11-13","to":"2026-11-19","sum":["reps","kg"],"offset":1)"},
      {"new shape: a month", "log/e.csv", kExpense, "files.read", R"("column":"date","from":"2026-11","sum":"amount,tip")"},
      {"old shape: the month's day with an empty cell", "log/e.csv", nullptr, "files.read",
       R"("tail":2,"sum":"amount","date":"2026-11-19")"},
      {"an unknown column", "log/e.csv", nullptr, "files.read", R"("column":"day","from":"2026-11-19")"},
      {"identical rows: the first by default", "log/t.csv", nullptr, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"2026-11-13,am,squat,5,110")"},
      {"identical rows: line picks the second", "log/t2.csv", kTraining, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"","line":3)"},
      {"a stale line number", "log/t2.csv", nullptr, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"x","line":3)"},
      {"part of a line", "log/e2.csv", kExpense, "files.edit", R"("find":"train ticket","replace":"bus","line":7)"},
      {"a correction by line", "log/e2.csv", nullptr, "files.edit",
       R"("find":"2026-11-19,errand 2,train ticket,,0","replace":"2026-11-19,errand 2,train ticket,8.60,0","line":7)"},
      {"the first two matches", "log/f2.csv", kFood, "files.edit", R"("find":"oats","replace":"oat flakes","count":2)"},
      {"every match", "log/f3.csv", kFood, "files.edit", R"("find":"lunch 12:30","replace":"lunch","all":true)"},
      {"a comment line by line", "log/f4.csv", kFood, "files.edit",
       R"("find":"#was: 2026-11-19,breakfast,oats,40,150","replace":"","line":4)"},
      {"no match", "log/f4.csv", nullptr, "files.edit", R"("find":"pears","replace":"plums")"},
  };
  return c;
}

// The fake card the shared functions run on (one file per path).
struct MemCard : nimbus::orch::aids::AidSource, nimbus::orch::aids::AidSink {
  std::string file, part;
  bool readAt(uint64_t off, size_t len, char* out, size_t& got) override {
    got = off < file.size() ? std::min(len, size_t(file.size() - off)) : 0;
    if (got) std::memcpy(out, file.data() + off, got);
    return true;
  }
  bool partWrite(std::string_view b, bool first) override {
    if (first) part.clear();
    part.append(b.data(), b.size());
    return true;
  }
  bool partSwap() override {
    file = part;
    return true;
  }
  void partDiscard() override { part.clear(); }
};

// One call the way the device's files_subsystem.cpp makes it: files.read's CSV form or its
// day total, files.edit's precheck, no-op and shared edit. "ok <text>" or "error <text>".
inline std::string deviceCall(MemCard& card, const std::string& path, const Call& c,
                              nimbus::orch::aids::e1::RemovedRows& memory) {
  using namespace nimbus::orch;
  ArduinoJson::JsonDocument doc;
  deserializeJson(doc, std::string("{") + c.args + "}");
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  std::string out;
  if (std::string(c.tool) == "files.read") {
    if (aids::e1::readForm(a) == aids::e1::ReadForm::DayTotal) {
      aids::CsvQuery q;   // the sum argument read as the CSV form reads it
      if (!aids::e1::queryArgs(a, 0, q, out)) return "error " + out;
      return "ok " + aids::e1::dayTotal(card, card.file.size(), path, q.sum, a["date"] | "");
    }
    aids::CsvQuery q;
    const size_t offset = a["offset"] | 0;
    if (!aids::e1::queryArgs(a, offset, q, out)) return "error " + out;
    return aids::e1::tableRead(card, card.file.size(), path, q, out) ? "ok " + out : "error " + out;
  }
  aids::e1::EditCall call;
  call.project = path.substr(0, path.find('/'));
  call.name = path.substr(path.find('/') + 1);
  call.find = a["find"] | "";
  call.replace = a["replace"] | "";
  call.pol.all = a["all"] | false;
  if (a["count"].is<int>()) call.pol.count = a["count"].as<int>();
  call.line = a["line"] | 0u;
  if (const std::string bad = validateEdit(call.find.size(), call.replace.size(), call.pol); !bad.empty())
    return "error " + bad;
  if (editChangesNothing(call.find, call.replace)) return "error no-op case not in the fixtures";
  if (isCsvName(call.name)) call.header = card.file.substr(0, card.file.find('\n'));
  return aids::e1::edit(card, card, card.file.size(), call, memory, out) ? "ok " + out : "error " + out;
}

// The day-total line of a files.read {tail, sum} reply: what follows the tail view.
inline std::string dayTotalPart(const std::string& reply) {
  if (reply.size() < 2) return reply;
  const size_t nl = reply.rfind('\n', reply.size() - 2);
  return nl == std::string::npos ? reply : reply.substr(nl + 1);
}

// The transcript: for each call, its reply and the file after it.
inline std::string transcript(const std::function<std::string(const Call&, const std::string& path)>& run,
                              const std::function<std::string(const std::string& path)>& fileOf) {
  std::string t;
  for (const Call& c : calls()) t += std::string("== ") + c.name + "\n" + run(c, c.path) + "\n-- " + c.path + "\n" + fileOf(c.path) + "\n";
  return t;
}

}  // namespace aidcases::e1
