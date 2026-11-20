#pragma once
// The old engine's files.read and files.edit on the shared aids (file_aids_e1.h): the same
// calls on the same files, run (a) through the shared functions over a fake card, exactly
// as the device's files_subsystem.cpp calls them (test_file_aids_e1), and (b) through
// nimbusd's PosixFiles tools on its disk (nimbusd/tests/test_hosted_file_aids.cpp). Both
// transcripts must equal test/golden/file_aids_e1_results.txt byte for byte.
//
// The files are test/support/file_aids_cases.h's three equal domains in the owner file's
// shape (ruling 49). files.read totals nothing (ruling 95 removed sum): its CSV form keeps the
// matching rows.
#include <ArduinoJson.h>

#include <cstdio>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <utility>
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

// RB-TR1: the files a continued read is checked on (the class, not one instance): prose of
// about 40 KB with two- and three-byte characters, a CSV with comment lines and a quoted line
// end, a file with no last line end, one line of 30 KB, a file over 256 KB, an empty file and a
// file of one line end. Invented content.
inline std::string proseFile(size_t target, unsigned seed) {
  static const char* words[] = {"garden", "caf\xc3\xa9", "rota", "na\xc3\xaf" "ve", "shed", "price \xe2\x82\xac" "4",
                                "water", "compost", "gate", "key"};
  std::string out;
  unsigned r = seed;
  for (int n = 1; out.size() < target; ++n) {
    out += "Section " + std::to_string(n) + ".";
    for (int w = 0; w < 14 + n % 9; ++w) {
      r = r * 1103515245u + 12345u;
      out += std::string(" ") + words[(r >> 16) % 10];
    }
    out += ".\n";
  }
  return out;
}
inline std::string oneLine() {   // 30 KB with no line end before its last byte, three-byte characters
  std::string s(12000, 'a');
  for (int i = 0; i < 6000; ++i) s += "\xe2\x82\xac";
  return s + "\n";
}
inline std::string denseLines() {   // 20,000 two-byte lines: the head's numbers are all five digits
  std::string s;
  for (int i = 0; i < 20000; ++i) s += "x\n";
  return s;
}
inline std::vector<std::pair<std::string, std::string>> partFiles() {
  std::string csv = "date,note,amount\n";
  for (int i = 0; i < 900; ++i) {
    if (i % 50 == 7) csv += "#was: 2026-11-0" + std::to_string(1 + i % 9) + ",old,1\n";
    csv += "2026-11-" + std::to_string(10 + i % 20) + ",\"line " + std::to_string(i) + (i % 97 == 3 ? "\nsecond" : "") +
           "\"," + std::to_string(i % 13) + "." + std::to_string(i % 10) + "\n";
  }
  std::string noEnd = proseFile(12000, 7);
  noEnd.pop_back();
  return {{"docs/prose.txt", proseFile(40000, 1)},
          {"log/rows.csv", csv},
          {"docs/no-end.txt", noEnd},
          {"docs/one-line.txt", oneLine()},
          {"docs/big.txt", proseFile(300000, 3)},
          {"docs/dense.txt", denseLines()},
          {"docs/empty.txt", ""},
          {"docs/blank.txt", "\n"}};
}

// One part checked against the file: "" when its head's byte and line numbers are true and its
// body is those bytes of the file, starting where the parts before it ended.
inline std::string checkPart(const std::smatch& m, const std::string& r, const std::string& file,
                             const std::vector<size_t>& ends, size_t joined) {
  const size_t from = std::stoul(m[1]), to = std::stoul(m[2]), body = m.length(0);
  const size_t lines = ends[file.size()] + (!file.empty() && file.back() != '\n' ? 1 : 0);
  if (std::stoul(m[3]) != file.size() || from != joined || to - from != r.size() - body ||
      file.compare(from, to - from, r, body, std::string::npos) != 0)
    return "the part " + std::to_string(from) + "-" + std::to_string(to) + " does not hold those bytes of the file";
  const bool linesTrue = !m[4].matched || (std::stoul(m[5]) == ends[from] + 1 &&
                                           std::stoul(m[6]) == ends[to - 1] + 1 && std::stoul(m[7]) == lines);
  if (!linesTrue) return "the line numbers are wrong: " + m.str(0);
  if (to > from && (static_cast<unsigned char>(r[body]) & 0xC0) == 0x80) return "a part starts inside a character";
  return std::string();
}

// The class check of a continued read. read(offset) is one files.read result for that offset
// (0: no offset given). Follows the next part each head names until "end of file". "" when the
// parts put together are the file byte for byte, every result fits in cap, every head's byte
// and line numbers are true, and it took at most 2 * size / cap + 2 calls; else what failed.
inline std::string followParts(const std::function<std::string(uint64_t)>& read, const std::string& file, size_t cap) {
  static const std::regex head(
      R"(^bytes (\d+)-(\d+) of (\d+)(, lines (\d+)-(\d+) of (\d+))?; (next part: "offset":(\d+)|end of file)\n)");
  std::vector<size_t> ends(file.size() + 1, 0);   // ends[i]: line ends in file[0, i)
  for (size_t i = 0; i < file.size(); ++i) ends[i + 1] = ends[i] + (file[i] == '\n');
  std::string joined;
  uint64_t offset = 0;
  const size_t most = 2 * file.size() / cap + 2;
  for (size_t calls = 1; calls <= most; ++calls) {
    const std::string r = read(offset);
    std::smatch m;
    if (r.size() > cap) return "a result of " + std::to_string(r.size()) + " B passes the cap";
    if (!std::regex_search(r, m, head)) return "no part head in: " + r.substr(0, 120);
    if (const std::string bad = checkPart(m, r, file, ends, joined.size()); !bad.empty()) return bad;
    joined += r.substr(m.length(0));
    if (!m[9].matched) return joined == file ? std::string() : "end of file before the whole file was read";
    if (std::stoul(m[9]) != std::stoul(m[2])) return "the next part does not start where this one ends";
    offset = std::stoul(m[9]);
  }
  return "more than " + std::to_string(most) + " calls to read " + std::to_string(file.size()) + " B";
}

inline const char* kProse() {   // 20 KB of prose for the plain read's parts (RB-TR1)
  static const std::string p = proseFile(20000, 5);
  return p.c_str();
}

inline const std::vector<Call>& calls() {
  static const std::vector<Call> c = {
      {"plain read: the first part names the next", "docs/p.txt", kProse(), "files.read", R"("offset":0)"},
      {"plain read: the next part, as named", "docs/p.txt", nullptr, "files.read", R"("offset":8082)"},
      {"plain read: an offset of the model's own, inside a line", "docs/p.txt", nullptr, "files.read", R"("offset":17001)"},
      {"plain read: past the end", "docs/p.txt", nullptr, "files.read", R"("offset":30000)"},
      {"a day's rows", "log/f.csv", kFood, "files.read", R"("column":"date","from":"2026-11-19")"},
      {"a week, paged", "log/t.csv", kTraining, "files.read",
       R"("column":"date","from":"2026-11-13","to":"2026-11-19","offset":1)"},
      {"a month", "log/e.csv", kExpense, "files.read", R"("column":"date","from":"2026-11")"},
      {"a day with an empty cell", "log/e.csv", nullptr, "files.read", R"("column":"date","from":"2026-11-19")"},
      {"an unknown column", "log/e.csv", nullptr, "files.read", R"("column":"day","from":"2026-11-19")"},
      {"identical rows: the first by default", "log/t.csv", nullptr, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"2026-11-13,am,squat,5,110")"},
      {"identical rows: line picks the second", "log/t2.csv", kTraining, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"","line":3)"},
      {"a stale line number", "log/t2.csv", nullptr, "files.edit",
       R"("find":"2026-11-13,am,squat,5,100","replace":"x","line":3)"},
      {"part of a line", "log/e2.csv", kExpense, "files.edit", R"("find":"train ticket","replace":"bus","line":7)"},
      {"a correction by line", "log/e2.csv", kExpense, "files.edit",
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

// One call the way the device's files_subsystem.cpp makes it: files.read's CSV form,
// files.edit's precheck, no-op and shared edit. "ok <text>" or "error <text>".
inline std::string deviceCall(MemCard& card, const std::string& path, const Call& c,
                              nimbus::orch::aids::e1::RemovedRows& memory) {
  using namespace nimbus::orch;
  ArduinoJson::JsonDocument doc;
  deserializeJson(doc, std::string("{") + c.args + "}");
  const ArduinoJson::JsonObjectConst a = doc.as<ArduinoJson::JsonObjectConst>();
  std::string out;
  if (std::string(c.tool) == "files.read") {
    if (aids::e1::readForm(a) == aids::e1::ReadForm::Plain) {   // the part that fits the 8,192 B result
      const uint64_t offset = a["offset"] | 0u;
      return aids::e1::readPart(card, card.file.size(), offset, 8192, out) ? "ok " + out : "error " + out;
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

// The transcript: for each call, its reply and the file after it.
inline std::string transcript(const std::function<std::string(const Call&, const std::string& path)>& run,
                              const std::function<std::string(const std::string& path)>& fileOf) {
  std::string t;
  for (const Call& c : calls()) t += std::string("== ") + c.name + "\n" + run(c, c.path) + "\n-- " + c.path + "\n" + fileOf(c.path) + "\n";
  return t;
}

}  // namespace aidcases::e1
