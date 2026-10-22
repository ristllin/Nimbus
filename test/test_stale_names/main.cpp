// test_stale_names - the class guard for stale console names (wf_plan 4b, critique B6).
//
// "La Plateforme" (Mistral's old console name) reached the owner from a web page
// hint after Mistral had renamed it, and console.anthropic.com now redirects to
// platform.claude.com. Every user-facing surface - the web page fragments, the
// firmware strings, the docs - is scanned; a new hit fails here instead of reaching
// the owner. docs/changelog.md is history and keeps its old wording; recorded
// provider data under test/support/fixtures is byte-frozen.
#include <unity.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

static const char* const kStale[] = {"Plateforme", "console.anthropic.com"};

static std::string readFile(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static bool scanned(const std::string& name) {
  for (const char* ext : {".h", ".cpp", ".md", ".mjs", ".py"}) {
    const std::string e = ext;
    if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) return true;
  }
  return false;
}

static void walk(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == ".." || name == "node_modules" || name == "fixtures") continue;
    const std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) walk(path, out);
    else if (scanned(name)) out.push_back(path);
  }
  closedir(d);
}

static void test_no_stale_console_names() {
  std::vector<std::string> files;
  for (const char* root : {"include", "src", "lib", "docs", "nimbusd/src", "tools/webui_harness"})
    walk(root, files);
  TEST_ASSERT_GREATER_THAN_MESSAGE(200, (int)files.size(), "the scan found too few files");
  int hits = 0;
  for (const std::string& f : files) {
    if (f == "docs/changelog.md") continue;   // history keeps the name it had
    const std::string text = readFile(f);
    for (const char* bad : kStale)
      if (text.find(bad) != std::string::npos) {
        std::printf("STALE: %s names %s\n", f.c_str(), bad);
        hits++;
      }
  }
  TEST_ASSERT_EQUAL_MESSAGE(0, hits, "a stale console name is back (see the STALE lines)");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_stale_console_names);
  return UNITY_END();
}
