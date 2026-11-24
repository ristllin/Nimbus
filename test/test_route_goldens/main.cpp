// test_route_goldens - the request and parse goldens of the engine-2 wire as it is (lane E2-R2a;
// router design section 11, layers L1 and L1p). Before the router moves the generic half of
// lib/harness/src/providers/plain_wire_anthropic.cpp into a shared planner, the bytes that wire
// sends for a fixed set of stored transcripts, and what it and the exchange read from every
// recorded answer, are pinned here. The provider's prompt cache holds only while those bytes do
// not move, so every later lane leaves these goldens byte-identical or names each difference in
// its PR (tools/router/refresh_goldens.py rewrites them and prints what moved).
//
// The class rules:
//   - every request fixture renders to exactly its golden test/support/goldens/wire/<signature>/
//     <fixture>.req, and every recorded answer reads to exactly its <answer>.parse; a difference
//     names the golden, the first differing byte offset and both sides around it;
//   - the goldens hold no live prompt text: the fixtures carry their own small system text and two
//     tools (transcripts.h), so a prompt or tool-description change moves nothing here;
//   - a render is the same bytes every time and in any order (no clock, no random value, no state
//     left by an earlier render);
//   - MANIFEST lists every golden and the folders hold no other file, so a fixture cannot vanish
//     silently;
//   - the comparator itself is counter-tested: a one-byte change is caught and located.
#include <dirent.h>
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "cases.h"
#include "golden_io.h"
#include "parse_case.h"
#include "render_case.h"

namespace rg = route_goldens;

void setUp() {}
void tearDown() {}

namespace {

// Print every report (a run that moves several goldens names them all), fail on the first.
void failOn(const std::vector<std::string>& reports) {
  for (const std::string& r : reports) std::printf("%s\n", r.c_str());
  if (!reports.empty())
    TEST_FAIL_MESSAGE((std::to_string(reports.size()) + " golden(s) differ; first: " + reports.front()).c_str());
}

std::vector<std::string> goldenNames() {
  std::vector<std::string> out;
  for (const rg::Case& c : rg::requestCases()) out.push_back(c.signature + "/" + c.name + ".req");
  for (const rg::Answer& a : rg::answers()) out.push_back(std::string(rg::kDirect) + "/" + rg::stem(a.file) + ".parse");
  std::sort(out.begin(), out.end());
  return out;
}

std::string manifestText() {
  std::string o = "# route goldens: every file under test/support/goldens/wire/, written by test_route_goldens\n";
  for (const std::string& n : goldenNames()) o += n + "\n";
  return o;
}

// The files under each signature folder.
std::set<std::string> filesOnDisk() {
  std::set<std::string> out;
  for (const char* sig : {rg::kDirect, rg::kRouted}) {
    DIR* d = opendir((std::string(rg::kRoot) + sig).c_str());
    if (!d) continue;
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') out.insert(std::string(sig) + "/" + e->d_name);
    closedir(d);
  }
  return out;
}

}  // namespace

static void test_every_request_renders_to_its_golden() {
  std::vector<std::string> reports;
  const std::vector<rg::Case> cases = rg::requestCases();
  TEST_ASSERT_TRUE(cases.size() >= 14);
  for (const rg::Case& c : cases) {
    const std::string rendered = rg::renderCase(c);
    TEST_ASSERT_TRUE_MESSAGE(rendered.find("refused") == std::string::npos, rendered.c_str());
    const std::string r = rg::checkGolden(rg::goldenPath(c), rendered);
    if (!r.empty()) reports.push_back(r);
  }
  failOn(reports);
}

static void test_every_recorded_answer_reads_to_its_golden() {
  std::vector<std::string> reports;
  for (const rg::Answer& a : rg::answers()) {
    const std::string r = rg::checkGolden(rg::parsePath(a), rg::parseCase(a));
    if (!r.empty()) reports.push_back(r);
  }
  failOn(reports);
}

static void test_the_manifest_names_every_golden_and_no_other_file_is_there() {
  const std::string m = rg::checkGolden(std::string(rg::kRoot) + "MANIFEST", manifestText());
  TEST_ASSERT_TRUE_MESSAGE(m.empty(), m.c_str());
  const std::vector<std::string> names = goldenNames();
  const std::set<std::string> want(names.begin(), names.end());
  TEST_ASSERT_EQUAL_MESSAGE(names.size(), want.size(), "two fixtures share a golden name");
  if (rg::writeMode()) return;   // refresh_goldens.py removes what the manifest no longer names
  for (const std::string& f : filesOnDisk())
    TEST_ASSERT_TRUE_MESSAGE(want.count(f), ("a golden no fixture renders (refresh_goldens.py removes it): " + f).c_str());
}

// No clock, no random value and no state an earlier render left: three renders of each case, the
// second pass in reverse order, give the same bytes.
static void test_a_render_is_the_same_bytes_every_time_and_in_any_order() {
  const std::vector<rg::Case> cases = rg::requestCases();
  std::vector<std::string> first;
  for (const rg::Case& c : cases) first.push_back(rg::renderCase(c));
  for (size_t i = cases.size(); i-- > 0;) {
    TEST_ASSERT_TRUE_MESSAGE(rg::renderCase(cases[i]) == first[i], cases[i].name.c_str());
    TEST_ASSERT_TRUE_MESSAGE(rg::renderCase(cases[i]) == first[i], cases[i].name.c_str());
  }
  for (const rg::Answer& a : rg::answers()) TEST_ASSERT_TRUE_MESSAGE(rg::parseCase(a) == rg::parseCase(a), a.file);
}

// The comparator's counter-test: a golden with one byte changed, one byte added and one byte
// cut is caught every time, and the report names the golden and the exact offset.
static void test_a_one_byte_change_is_caught_and_located() {
  const rg::Case c = rg::requestCases().front();
  const std::string path = rg::goldenPath(c);
  const std::string good = rg::renderCase(c);
  const size_t at = good.find("\"max_tokens\":") + 13;
  TEST_ASSERT_TRUE(at > 13 && at < good.size());
  std::string changed = good;
  changed[at] = changed[at] == '9' ? '8' : '9';
  std::string added = good;
  added.insert(at, 1, ' ');
  const std::string cut = good.substr(0, good.size() - 1);
  for (const std::string* bad : {&changed, &added}) {
    const std::string r = rg::diffReport(path, good, *bad);
    TEST_ASSERT_TRUE_MESSAGE(r.find(path) != std::string::npos, r.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.find("differs at byte " + std::to_string(at) + " ") != std::string::npos, r.c_str());
  }
  const std::string r = rg::diffReport(path, good, cut);
  TEST_ASSERT_TRUE_MESSAGE(r.find("differs at byte " + std::to_string(good.size() - 1) + " ") != std::string::npos, r.c_str());
  TEST_ASSERT_TRUE(rg::diffReport(path, good, good).empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_request_renders_to_its_golden);
  RUN_TEST(test_every_recorded_answer_reads_to_its_golden);
  RUN_TEST(test_the_manifest_names_every_golden_and_no_other_file_is_there);
  RUN_TEST(test_a_render_is_the_same_bytes_every_time_and_in_any_order);
  RUN_TEST(test_a_one_byte_change_is_caught_and_located);
  return UNITY_END();
}
