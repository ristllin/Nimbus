// test_hosted_file_aids - RB-REC4: the plain file tools on nimbusd's POSIX disk give the
// device's results byte for byte. The shared fixtures (test/support/file_aids_cases.h: CSV
// totals with comment lines, rows with more fields than the header and empty summed cells in
// three domains, line-picked edits, the append fence and identical-line fact, the torn-tail
// note) run through FileTools over PosixFileToolsIo; the transcript must equal
// test/golden/file_aids_results.txt, which test_file_tools checks on the device's card seam.
// Offline: no provider traffic.
#include <algorithm>
#include <string>

#include "../../test/support/file_aids_cases.h"
#include "../../test/support/file_aids_e1_cases.h"
#include "posix_files.h"
#include "nimbus/orch/file_aids.h"
#include "posix_file_tools.h"
#include "test_util.h"

using namespace nimbusd;
namespace orch = nimbus::orch;

static std::string readAll(const std::string& path) {
  std::string out;
  return fsutil::readFile(path, out) ? out : std::string("<none>");
}

static void testSharedFixtures(ndtest::Ctx& c, const std::string& repo) {
  const std::string dir = ndtest::scratchDir("file-aids");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  PosixFileToolsIo io(dir);
  const std::string got = aidcases::transcript(
      io,
      [&](const std::string& p, const std::string& n, const std::string& b) {
        fsutil::mkdirs(dir + "/" + p);
        fsutil::writeFileAtomic(io.file(p, n), b);
        orch::FileEntry e;
        e.project = orch::workingCopy(p);
        e.name = orch::workingCopy(n);
        e.bytes = uint32_t(b.size());
        e.createdAt = 1763000000;
        e.kind = orch::fileKindForName(n);
        e.hash = orch::aids::fnv64(orch::aids::kFnv64Basis, b);
        e.owner = orch::workingCopy("owner");
        std::string err;
        io.locked([&] { io.index().add(e, err); });
      },
      [&](const std::string& p, const std::string& n) { return readAll(io.file(p, n)); });
  const std::string want = readAll(repo + "/test/golden/file_aids_results.txt");
  c.ok(want != "<none>", "the golden transcript exists");
  c.ok(got == want, "the POSIX transcript equals the device's golden byte for byte");
  if (got != want) {
    const size_t at = std::mismatch(got.begin(), got.end(), want.begin(), want.end()).first - got.begin();
    std::printf("    first difference at byte %zu:\n    got:  %s\n    want: %s\n", at, got.substr(at, 160).c_str(),
                want.substr(std::min(at, want.size()), 160).c_str());
  }
  // No part file is left behind, and the index on disk reloads to the same entries.
  c.ok(readAll(dir + "/log/.training3.csv.tools-part") == "<none>", "no part file left after an edit");
  PosixFileToolsIo again(dir);
  bool same = true;
  again.locked([&] {
    const orch::FileEntry* e = again.index().find("log", "expense3.csv");
    same = e && e->bytes == readAll(dir + "/log/expense3.csv").size();
  });
  c.ok(same, "the persisted index holds the edited file's new size");
  ndtest::rmTree(dir);
}

// The old engine's files.read and files.edit (PosixFiles, the hosted files.* tools) give the
// device's reply bytes: the transcript equals test/golden/file_aids_e1_results.txt.
static void testOldEngineTools(ndtest::Ctx& c, const std::string& repo) {
  const std::string dir = ndtest::scratchDir("file-aids-e1");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  PosixFiles files(dir);
  orch::ToolRegistry reg;
  files.registerTools(reg);
  orch::Principal owner = orch::principalForRole("1", orch::Role::Admin);
  owner.owner = true;
  const std::string got = aidcases::e1::transcript(
      [&](const aidcases::e1::Call& k, const std::string& path) {
        if (k.seed) files.seed(path, k.seed);
        const std::string project = path.substr(0, path.find('/')), name = path.substr(path.find('/') + 1);
        ArduinoJson::JsonDocument d;
        deserializeJson(d, "{\"project\":\"" + project + "\",\"name\":\"" + name + "\"," + k.args + "}");
        const orch::ToolResult r = reg.dispatch(k.tool, d.as<ArduinoJson::JsonObjectConst>(), owner);
        if (!r.success) return "error " + r.error;
        const bool tail = std::string(k.args).find("\"tail\"") != std::string::npos;
        return "ok " + (tail ? aidcases::e1::dayTotalPart(r.output) : r.output);
      },
      [&](const std::string& path) { return files.get(path); });
  const std::string want = readAll(repo + "/test/golden/file_aids_e1_results.txt");
  c.ok(want != "<none>", "the old engine's golden transcript exists");
  c.ok(got == want, "files.read and files.edit on the POSIX disk equal the device's golden byte for byte");
  if (got != want) {
    const size_t at = std::mismatch(got.begin(), got.end(), want.begin(), want.end()).first - got.begin();
    std::printf("    first difference at byte %zu:\n    got:  %s\n    want: %s\n", at, got.substr(at, 160).c_str(),
                want.substr(std::min(at, want.size()), 160).c_str());
  }
  ndtest::rmTree(dir);
}

// RB-TR1: files.read on nimbusd can be continued to the whole file, for every file of the class
// (aidcases::e1::partFiles), at the default cap and at a set one; the same check as the device's
// test_a_plain_read_can_be_continued_to_the_whole_file, through the registered tool.
static void testContinuedRead(ndtest::Ctx& c) {
  const std::string dir = ndtest::scratchDir("file-aids-parts");
  ndtest::rmTree(dir);
  fsutil::mkdirs(dir);
  PosixFiles files(dir);
  orch::ToolRegistry reg;
  files.registerTools(reg);
  orch::Principal owner = orch::principalForRole("1", orch::Role::Admin);
  owner.owner = true;
  size_t cap = PosixFiles::kDefaultResultCap;
  files.setResultCap([&cap] { return cap; });
  for (const auto& f : aidcases::e1::partFiles()) {
    files.seed(f.first, f.second);
    const std::string project = f.first.substr(0, f.first.find('/')), name = f.first.substr(f.first.find('/') + 1);
    for (size_t at : {PosixFiles::kDefaultResultCap, size_t(16384)}) {
      cap = at;
      auto read = [&](uint64_t off) {
        ArduinoJson::JsonDocument d;
        d["project"] = project;
        d["name"] = name;
        if (off) d["offset"] = off;
        const orch::ToolResult r = reg.dispatch("files.read", d.as<ArduinoJson::JsonObjectConst>(), owner);
        return r.success ? r.output : "error " + r.error;
      };
      const std::string why = aidcases::e1::followParts(read, f.second, at);
      c.ok(why.empty(), (f.first + " read to the end in parts at cap " + std::to_string(at) + (why.empty() ? "" : ": " + why)).c_str());
    }
  }
  ndtest::rmTree(dir);
}

int main(int argc, char** argv) {
  ndtest::Ctx c;
  c.suite = "hosted_file_aids";
  std::printf("=== hosted_file_aids (T1, offline) ===\n");
  // The repo root from this file's own path (nimbusd/tests/<this file>).
  std::string here = __FILE__;
  for (int up = 0; up < 3; ++up) here = here.substr(0, here.find_last_of('/'));
  testSharedFixtures(c, here);
  testOldEngineTools(c, here);
  testContinuedRead(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  (void)argc;
  (void)argv;
  return c.failures ? 1 : 0;
}
