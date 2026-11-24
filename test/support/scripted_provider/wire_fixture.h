#pragma once
#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "script.h"

// scripted_provider/wire_fixture.h - an answer fixture a script replays verbatim (lane E2-R3a; router
// design section 11, "Scripts can replay a fixture body verbatim").
//
// A wire fixture (test/support/fixtures/<provider>_wire_synthetic/*.txt) is three header lines, then the
// body exactly as the provider sends it:
//   synthetic: <the documentation URL>, read 2026-11-24     (or "recorded: <how>, <date>")
//   status: <HTTP status>
//   expect: <key=value ...>      what a reader of this body must conclude: stop=<end|tool|cut|refusal|
//                                context> complete=<true|false> calls=<n> for an answer; class=<error class
//                                of the design's section 9> retry=<yes|no> for an error
//   <body, to the end of the file>
// A fixture with any other first lines does not load, so a body without its source cannot slip in.
namespace harness_test {
namespace scripted {

struct WireFixture {
  std::string file;      // the path it was read from
  std::string source;    // the first line, whole
  int status = 0;
  std::string expect;    // the expect line's value
  std::string body;      // verbatim

  // The value of `key` in the expect line ("" when absent).
  std::string expected(const std::string& key) const {
    const std::string k = key + "=";
    for (size_t at = 0; at < expect.size();) {
      size_t end = expect.find(' ', at);
      if (end == std::string::npos) end = expect.size();
      if (expect.compare(at, k.size(), k) == 0) return expect.substr(at + k.size(), end - at - k.size());
      at = end + 1;
    }
    return "";
  }
};

inline bool readWhole(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

namespace detail {
inline bool takeLine(const std::string& s, size_t& at, std::string& line) {
  const size_t nl = s.find('\n', at);
  if (nl == std::string::npos) return false;
  line = s.substr(at, nl - at);
  at = nl + 1;
  return true;
}
inline bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}  // namespace detail

// Parse a fixture's text; false with the reason in `err` when its header is not the three lines.
inline bool parseWireFixture(const std::string& text, WireFixture& out, std::string& err) {
  size_t at = 0;
  std::string src, status, expect;
  if (!detail::takeLine(text, at, src) || !detail::takeLine(text, at, status) || !detail::takeLine(text, at, expect)) {
    err = "fewer than three header lines";
    return false;
  }
  const bool provenance = (detail::startsWith(src, "synthetic: ") || detail::startsWith(src, "recorded: ")) &&
                          src.find(", read ") != std::string::npos;
  if (!provenance) err = "first line is not 'synthetic: <url>, read <date>' or 'recorded: ...'";
  else if (!detail::startsWith(status, "status: ") || std::atoi(status.c_str() + 8) < 100) err = "no 'status: <n>' line";
  else if (!detail::startsWith(expect, "expect: ")) err = "no 'expect: ...' line";
  else if (at >= text.size()) err = "no body";
  if (!err.empty()) return false;
  out.source = src;
  out.status = std::atoi(status.c_str() + 8);
  out.expect = expect.substr(8);
  out.body = text.substr(at);
  while (!out.body.empty() && (out.body.back() == '\n' || out.body.back() == '\r')) out.body.pop_back();
  return true;
}

inline bool loadWireFixture(const std::string& path, WireFixture& out, std::string& err) {
  std::string text;
  if (!readWhole(path, text)) {
    err = "cannot read " + path;
    return false;
  }
  out.file = path;
  if (parseWireFixture(text, out, err)) return true;
  err = path + ": " + err;
  return false;
}

// Every *.txt fixture of a folder, sorted by name; a file that does not load is an error.
inline bool loadWireFixtures(const std::string& dir, std::vector<WireFixture>& out, std::string& err) {
  DIR* d = opendir(dir.c_str());
  if (!d) {
    err = "cannot open " + dir;
    return false;
  }
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0) names.push_back(n);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    WireFixture f;
    if (!loadWireFixture(dir + "/" + n, f, err)) return false;
    out.push_back(std::move(f));
  }
  return true;
}

// The step that serves a fixture's body verbatim at its status.
inline Step replay(const WireFixture& f) { return raw(f.status, f.body); }

}  // namespace scripted
}  // namespace harness_test
