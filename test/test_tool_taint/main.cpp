// The tool taint table (one row per registered tool, taint + effect): a tool the
// firmware registers by name without a row fails here, so a new tool can never
// slip past the leak-repair gate, the effect ledger or the preference rails.
#include <unity.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/tool_taint.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

void walk(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) walk(path, out);
    else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cpp") == 0) out.push_back(path);
  }
  closedir(d);
}

bool toolNameShape(const std::string& s) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || c == '_' || c == '.')) return false;
  return true;
}

// Every string literal right after ".add(" that looks like a tool name.
std::set<std::string> registeredNames() {
  std::vector<std::string> files;
  walk("lib", files);
  walk("src", files);
  // JSON-array .add() calls that pass a dotted string but register no tool.
  const std::set<std::string> notTools = {"reasoning.encrypted_content"};
  std::set<std::string> names;
  for (const std::string& f : files) {
    const std::string src = readFile(f);
    for (size_t p = src.find(".add("); p != std::string::npos; p = src.find(".add(", p + 1)) {
      size_t q = p + 5;
      while (q < src.size() && (src[q] == ' ' || src[q] == '\n' || src[q] == '\r' || src[q] == '\t')) q++;
      if (q >= src.size() || src[q] != '"') continue;
      const size_t end = src.find('"', q + 1);
      if (end == std::string::npos) continue;
      const std::string lit = src.substr(q + 1, end - q - 1);
      if (toolNameShape(lit) && !notTools.count(lit)) names.insert(lit);
    }
  }
  return names;
}

}  // namespace

static void test_every_registered_tool_has_a_taint_row() {
  const std::set<std::string> names = registeredNames();
  // The scan must actually see the registrations, or it proves nothing.
  TEST_ASSERT_TRUE_MESSAGE(names.size() >= 40, "the source scan found too few tool registrations");
  TEST_ASSERT_TRUE(names.count("memory.write") && names.count("device.control") && names.count("web.search"));
  for (const std::string& n : names)
    TEST_ASSERT_NOT_NULL_MESSAGE(toolTaintRow(n), ("no taint row for registered tool " + n).c_str());
  TEST_MESSAGE(("property: " + std::to_string(names.size()) + " registered tools, each with a taint row").c_str());
}

// No stale rows: every row names a tool the firmware still registers.
static void test_every_row_names_a_registered_tool() {
  const std::set<std::string> names = registeredNames();
  size_t n = 0;
  const ToolTaintRow* rows = toolTaintTable(n);
  std::set<std::string> seen;
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(names.count(rows[i].name) == 1, rows[i].name);
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(rows[i].name).second, rows[i].name);   // no duplicates
  }
}

// Fail closed: a tool with no row (a connector's remote tool) is external content.
static void test_unknown_tools_are_external() {
  TEST_ASSERT_EQUAL(int(Taint::External), int(toolTaint("notion.search_pages")));
  TEST_ASSERT_EQUAL(int(Taint::External), int(toolTaint("")));
  TEST_ASSERT_EQUAL(int(ToolEffect::Read), int(toolEffect("notion.search_pages")));
  // The content readers that matter for the leak gate are External.
  for (const char* t : {"web.search", "files.read", "files.fetch", "results.get", "session.poll"})
    TEST_ASSERT_EQUAL_MESSAGE(int(Taint::External), int(toolTaint(t)), t);
  TEST_ASSERT_EQUAL(int(ToolEffect::MemWrite), int(toolEffect("memory.write")));
  TEST_ASSERT_EQUAL(int(ToolEffect::Spawn), int(toolEffect("session.spawn")));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_registered_tool_has_a_taint_row);
  RUN_TEST(test_every_row_names_a_registered_tool);
  RUN_TEST(test_unknown_tools_are_external);
  return UNITY_END();
}
