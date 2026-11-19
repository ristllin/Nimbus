#pragma once
// The shared fixtures of the general file aids through the plain file tools (RB-REC4): the
// same calls on the same files, run on the device's card seam (test_file_tools, a fake card)
// and on nimbusd's POSIX disk (nimbusd/tests/test_hosted_file_aids.cpp). Both transcripts must
// equal test/golden/file_aids_results.txt byte for byte, so both hosts give the same results.
//
// Three equal domains (a food log, an expense log, a training log) in the owner file's shape
// (ruling 49: comment lines, rows with more fields than the header, empty summed cells, a
// stray blank line, a free-text key), with invented content.
#include <ArduinoJson.h>

#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/request.h"   // the manifest the bindings register with
#include "nimbus/orch/file_tools.h"
#include "nimbus/orch/rbac.h"

namespace aidcases {

struct Case {
  const char* name;
  const char* path;   // project/name the call reads or writes
  const char* seed;   // the file before the call (nullptr: no file)
  const char* tool;
  const char* args;   // the call's JSON
};

inline const char* kFood =
    "date,occasion,item,grams,kcal\n"
    "2026-11-18,dinner,soup,300,180\n"
    "2026-11-19,breakfast,oats,60,230\n"
    "#was: 2026-11-19,breakfast,oats,40,150\n"
    "2026-11-19,lunch 12:30,\"rice, beans\",250,410\n"
    "2026-11-19,lunch 12:30,toast, butter, jam,80,260\n"
    "2026-11-19,snack 2,apple,150,\n"
    "\n"
    "2026-11-20,breakfast,oats,60,230\n";
inline const char* kExpense =
    "date,when,note,amount,tip\n"
    "2026-10-31,evening,books,24.90,0\n"
    "2026-11-02,morning,grocer,31.15,0\n"
    "#gone: 2026-11-02,morning,grocer,13.15,0\n"
    "2026-11-14,lunch 12:30,\"cafe, two cups\",7.40,1\n"
    "2026-11-14,lunch 12:30,cafe, two cups, cake,11.20,1.5\n"
    "2026-11-19,errand 2,train ticket,,0\n"
    "2026-11-30,evening,books,12.00,0\n";
inline const char* kTraining =
    "date,session,exercise,reps,kg\n"
    "2026-11-13,am,squat,5,100\n"
    "2026-11-13,am,squat,5,100\n"
    "#was: 2026-11-13,am,squat,5,10\n"
    "2026-11-15,pm,\"rows, cable\",12,40.5\n"
    "2026-11-17,pm,rows, cable, slow,12,40\n"
    "2026-11-19,set 2,bench press,8,\n"
    "2026-11-20,am,squat,5,105\n";

inline const std::vector<Case>& cases() {
  static const std::vector<Case> c = {
      {"food day total", "log/food.csv", kFood, "read_file",
       R"({"path":"log/food.csv","column":"date","from":"2026-11-19","sum":["grams","kcal"]})"},
      {"expense month total", "log/expense.csv", kExpense, "read_file",
       R"({"path":"log/expense.csv","column":"date","from":"2026-11","sum":["amount","tip"]})"},
      {"training week total", "log/training.csv", kTraining, "read_file",
       R"({"path":"log/training.csv","column":"date","from":"2026-11-13","to":"2026-11-19","sum":["reps","kg"]})"},
      {"every row, paged", "log/food2.csv", kFood, "read_file",
       R"({"path":"log/food2.csv","sum":["kcal"],"limit":16,"offset":1})"},
      {"an unknown column", "log/food3.csv", kFood, "read_file",
       R"({"path":"log/food3.csv","column":"day","from":"2026-11-19"})"},
      {"comment lines only", "log/notes.csv", "# a\n#b\n", "read_file", R"({"path":"log/notes.csv","sum":["kcal"]})"},
      {"a torn last line", "log/torn.csv", "date,amount\n2026-11-19,5\n2026-11-19,3", "read_file",
       R"({"path":"log/torn.csv"})"},
      {"identical rows: the match lines", "log/training2.csv", kTraining, "edit_file",
       R"({"path":"log/training2.csv","old":"2026-11-13,am,squat,5,100","new":"2026-11-13,am,squat,5,110"})"},
      {"identical rows: line picks one", "log/training3.csv", kTraining, "edit_file",
       R"({"path":"log/training3.csv","old":"2026-11-13,am,squat,5,100","new":"","line":3})"},
      {"a stale line number", "log/training4.csv", kTraining, "edit_file",
       R"({"path":"log/training4.csv","old":"2026-11-13,am,squat,5,100","new":"x","line":4})"},
      {"part of a line", "log/expense2.csv", kExpense, "edit_file",
       R"({"path":"log/expense2.csv","old":"train ticket","new":"bus","line":7})"},
      {"a correction by line", "log/expense3.csv", kExpense, "edit_file",
       R"({"path":"log/expense3.csv","old":"2026-11-19,errand 2,train ticket,,0","new":"2026-11-19,errand 2,train ticket,8.60,0","line":7})"},
      {"a comment line by line", "log/food4.csv", kFood, "edit_file",
       R"({"path":"log/food4.csv","old":"#was: 2026-11-19,breakfast,oats,40,150","new":"","line":4})"},
      {"every match", "log/food5.csv", kFood, "edit_file",
       R"({"path":"log/food5.csv","old":"oats","new":"oat flakes","all":true})"},
      {"an append after a torn line", "log/torn2.csv", "date,amount\n2026-11-19,5\n2026-11-19,3", "append_file",
       R"({"path":"log/torn2.csv","text":"2026-11-19,5\n"})"},
      {"an identical append", "log/food6.csv", kFood, "append_file",
       R"({"path":"log/food6.csv","text":"2026-11-20,breakfast,oats,60,230\n"})"},
      {"an append to a new file", "log/new.csv", nullptr, "append_file",
       R"({"path":"log/new.csv","text":"date,amount\n2026-11-19,5\n"})"},
  };
  return c;
}

// Run every case through FileTools on a binding. seed(project, name, bytes) stores a file
// with its index entry; card(project, name) returns its bytes now ("<none>" when missing).
inline std::string transcript(nimbus::orch::FileToolsIo& io,
                              const std::function<void(const std::string&, const std::string&, const std::string&)>& seed,
                              const std::function<std::string(const std::string&, const std::string&)>& card) {
  using namespace nimbus::orch;
  FileToolsConfig cfg;
  FileTools tools(io, cfg);
  ToolRegistry reg;
  tools.registerInto(reg, [](const char* t) {
    const auto* d = agent::plain::toolDecl(t);
    return FileToolDecl{d->description, agent::plain::declSchema(*d, false)};
  });
  const Principal owner = principalForRole("1001", Role::Admin);
  std::string out;
  for (const Case& c : cases()) {
    const std::string path = c.path;
    const std::string project = path.substr(0, path.find('/')), name = path.substr(path.find('/') + 1);
    if (c.seed) seed(project, name, c.seed);
    ArduinoJson::JsonDocument doc;
    deserializeJson(doc, c.args);
    const ToolResult r = reg.dispatch(c.tool, doc.as<ArduinoJson::JsonObjectConst>(), owner);
    out += std::string("== ") + c.name + "\n" + (r.success ? "ok " + r.output : "error " + r.error) + "\n-- " + path +
           "\n" + card(project, name) + "\n";
  }
  return out;
}

}  // namespace aidcases
