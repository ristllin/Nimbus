// Lane E2-BENCH: engine 2's device binding installs the harness log seam (agent::hlog) into the device
// log, as engine 1 does. Without it every harness line (a failed compaction's cause, the wire's errors)
// is dropped on the device: the bench's failed /compact left only "The summary failed" and no cause.
// orchestrator_plain.cpp is device-only, so this reads its source: begin() must set the sink to alog.
#include <unity.h>

#include <cstdio>
#include <string>

namespace {

std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return std::string();
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// Code only: line comments blanked, so a word in a comment never counts.
std::string stripLineComments(const std::string& in) {
  std::string out = in;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '"') {
      for (++i; i < out.size() && out[i] != '"'; ++i)
        if (out[i] == '\\') ++i;
    } else if (out.compare(i, 2, "//") == 0) {
      while (i < out.size() && out[i] != '\n') out[i++] = ' ';
    }
  }
  return out;
}

// The text of the function whose signature starts at `sig`, up to its matching brace.
std::string bodyOf(const std::string& code, const char* sig) {
  const size_t at = code.find(sig);
  if (at == std::string::npos) return std::string();
  const size_t open = code.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < code.size(); ++i) {
    if (code[i] == '{') ++depth;
    if (code[i] == '}' && --depth == 0) return code.substr(open, i - open + 1);
  }
  return std::string();
}

}  // namespace

static void test_engine_two_begin_routes_the_harness_log_into_alog() {
  const std::string code = stripLineComments(readFile("src/agent/orchestrator_plain.cpp"));
  TEST_ASSERT_FALSE(code.empty());
  const std::string begin = bodyOf(code, "void begin(HeavyFabric* fabric, const Sinks& sinks)");
  TEST_ASSERT_FALSE(begin.empty());
  const size_t set = begin.find("hlog::setSink(");
  TEST_ASSERT_TRUE_MESSAGE(set != std::string::npos, "engine 2's begin() installs no hlog sink");
  TEST_ASSERT_TRUE(begin.find("alog(", set) != std::string::npos);
  // Installed before the engine is marked ready, so no turn or compaction runs without it.
  TEST_ASSERT_TRUE(set < begin.find("s_begun = true"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_engine_two_begin_routes_the_harness_log_into_alog);
  return UNITY_END();
}
