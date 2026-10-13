// Class guard: no path pauses, stops, deletes or restarts the Telegram poll task for
// an OTA update (lane L32, release gate P2J).
//
// Lane L31 paused the poll task (24 KB stack, it also runs every turn) to lend its
// stack to the OTA task, then restarted it. Nothing guaranteed the 24 KB block
// re-formed, so the restart could wait forever and Telegram plus every local turn
// stayed down until a reboot, even after the unattended daily check. The design now
// hands the work to the poll task itself, which runs it between its own work. These
// pin the CLASS over the firmware sources and lib/core (comments stripped, so prose
// that NAMES a call never counts as one; whitespace squeezed out, so a call split
// across lines or spaced out still counts):
//   - the OTA module never reaches the poll task, and controls only its own task;
//   - nothing in the firmware suspends or resumes a task, and the poll task is created
//     in exactly one place (telegram::begin) and deleted only by itself;
//   - the only callers of telegram::quiesce are the two factory-reset steps, and
//     nothing calls telegram::stop; the removed restart API stays removed;
//   - the staged work runs only from the poll loop's top, before any turn of that
//     cycle, and nothing else calls the OTA run.
// A new pause added anywhere for OTA fails here. The decision and the single-flight
// staging are host-tested in test/test_ota_headroom.

#include <unity.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

static std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Index just past the raw string R"delim( ... )delim" whose quote is at `q`.
static size_t skipRawString(const std::string& s, size_t q) {
  const size_t open = s.find('(', q);
  if (open == std::string::npos) return s.size();
  const std::string close = ")" + s.substr(q + 1, open - q - 1) + "\"";
  const size_t end = s.find(close, open);
  return end == std::string::npos ? s.size() : end + close.size();
}

// True when the ' at `i` is a C++14 digit separator (24'576, 0xFF'FF): the token it
// sits in starts with a digit. A character literal never does (u8'a', case 'a').
static bool isDigitSeparator(const std::string& s, size_t i) {
  size_t t = i;
  while (t > 0 && (std::isalnum((unsigned char)s[t - 1]) || s[t - 1] == '\'')) --t;
  return t < i && std::isdigit((unsigned char)s[t]);
}

// Index of the newline that ends the // comment starting at `at`: a backslash right
// before a newline splices the next line into the comment (translation phase 2).
static size_t lineCommentEnd(const std::string& s, size_t at) {
  size_t nl = s.find('\n', at);
  while (nl != std::string::npos && nl > 0 &&
         (s[nl - 1] == '\\' || (s[nl - 1] == '\r' && nl > 1 && s[nl - 2] == '\\')))
    nl = s.find('\n', nl + 1);
  return nl;
}

// Index just past the literal that opens at `i`: a "..." string (raw or not) or a
// '...' character.
static size_t skipLiteral(const std::string& s, size_t i) {
  const char q = s[i];
  if (q == '"' && i > 0 && s[i - 1] == 'R') return skipRawString(s, i);
  for (++i; i < s.size(); ++i) {
    if (s[i] == '\\') ++i;
    else if (s[i] == q) return i + 1;
  }
  return s.size();
}

// C++ source with every comment replaced by a space; string and char literals are kept
// whole (a "https://" inside a string is not a comment).
static std::string stripComments(const std::string& src) {
  std::string out;
  size_t i = 0;
  while (i < src.size()) {
    const size_t at = src.find_first_of("\"'/", i);
    if (at == std::string::npos) { out.append(src, i, std::string::npos); break; }
    out.append(src, i, at - i);
    if (src[at] == '\'' && isDigitSeparator(src, at)) {
      out += '\'';
      i = at + 1;
    } else if (src[at] != '/') {   // a literal: copy it whole
      i = skipLiteral(src, at);
      out.append(src, at, i - at);
    } else if (src.compare(at, 2, "//") == 0) {
      out += ' ';
      i = lineCommentEnd(src, at);   // the newline itself stays
      if (i == std::string::npos) break;
    } else if (src.compare(at, 2, "/*") == 0) {
      out += ' ';
      const size_t end = src.find("*/", at + 2);
      i = end == std::string::npos ? src.size() : end + 2;
    } else {
      out += '/';
      i = at + 1;
    }
  }
  return out;
}

static std::vector<size_t> findAll(const std::string& hay, const std::string& needle) {
  std::vector<size_t> at;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1))
    at.push_back(p);
  return at;
}

static size_t countOf(const std::string& hay, const std::string& needle) {
  return findAll(hay, needle).size();
}

// Code with every whitespace character removed: "xTaskCreate(\n    pollTask," and
// "vTaskDelete (g_task)" read as "xTaskCreate(pollTask," and "vTaskDelete(g_task)".
static std::string squeeze(const std::string& code) {
  std::string out;
  out.reserve(code.size());
  for (char c : code)
    if (!std::isspace((unsigned char)c)) out += c;
  return out;
}

struct Source {
  std::string path;
  std::string code;   // comments stripped, whitespace squeezed out
};

// Every firmware source under src/ and include/, plus lib/core (the portable policy the
// firmware links). Bring-up sketches (src/bringup/) are bench diagnostics, not the
// product image; the generated docs pack is prose data.
static const std::vector<Source>& firmware() {
  static std::vector<Source> all;
  if (!all.empty()) return all;
  for (const char* root : {"src", "include", "lib/core/src", "lib/core/include"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string path = e.path().generic_string();
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") continue;
      if (path.rfind("src/bringup/", 0) == 0) continue;
      if (path == "lib/core/include/nimbus/docs_pack_data.h") continue;
      all.push_back({path, squeeze(stripComments(readFile(path)))});
    }
  }
  return all;
}

// Position of `needle` in `hay` at or after `from`, failing the test when it is absent:
// an order check against a missing anchor would compare against npos and pass.
static size_t anchor(const std::string& hay, const std::string& needle, size_t from = 0) {
  const size_t at = hay.find(needle, from);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, needle.c_str());
  return at;
}

static const std::string& codeOf(const std::string& path) {
  for (const Source& s : firmware())
    if (s.path == path) return s.code;
  TEST_FAIL_MESSAGE(path.c_str());
  static const std::string none;
  return none;
}

// The stripper itself: prose is gone, code and literals stay.
static void test_comment_stripper() {
  const std::string s = stripComments(
      "a(); // telegram::quiesce(1)\n/* vTaskSuspend(x) */ b(\"https://x\"); c('/');"
      " d(R\"j({\"q\":\"//\"})j\"); e('\\''); f(\"\\\"// no\");");
  TEST_ASSERT_EQUAL_size_t(0, countOf(s, "quiesce"));
  TEST_ASSERT_EQUAL_size_t(0, countOf(s, "vTaskSuspend"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(s, "\"https://x\""));
  TEST_ASSERT_EQUAL_size_t(1, countOf(s, "c('/');"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(s, "d(R\"j({\"q\":\"//\"})j\");"));   // raw string kept
  TEST_ASSERT_EQUAL_size_t(1, countOf(s, "e('\\'');"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(s, "f(\"\\\"// no\");"));            // escaped quote
  // A digit separator is not a character literal, and a spliced // comment goes on.
  const std::string t = stripComments(
      "int n = 24'576; g(\"https://y\"); // a \\\n quiesce(2)\nh(); u8'a'; k(\"//z\");");
  TEST_ASSERT_EQUAL_size_t(1, countOf(t, "24'576"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(t, "g(\"https://y\");"));
  TEST_ASSERT_EQUAL_size_t(0, countOf(t, "quiesce"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(t, "h();"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(t, "k(\"//z\");"));
  TEST_ASSERT_EQUAL_STRING("vTaskDelete(g_task);", squeeze("vTaskDelete (\n  g_task );").c_str());
  TEST_ASSERT_TRUE(firmware().size() > 100);   // the walk really covered the firmware
}

// The OTA module never reaches the poll task, and controls only its own task: one
// create (the dedicated otacheck / otainstall task) and self-deletes only.
static void test_ota_module_never_touches_the_poll_task() {
  for (const char* path : {"src/sys/ota_update.cpp", "src/sys/ota_update.h"}) {
    const std::string& c = codeOf(path);
    for (const char* banned : {"telegram", "quiesce", "resume(", "vTaskSuspend", "vTaskResume",
                               "vTaskPrioritySet", "PollHooks", "PollResumeLedger"})
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(c, banned), banned);
  }
  const std::string& ota = codeOf("src/sys/ota_update.cpp");
  TEST_ASSERT_EQUAL_size_t(1, countOf(ota, "xTaskCreate"));
  TEST_ASSERT_EQUAL_size_t(countOf(ota, "vTaskDelete("), countOf(ota, "vTaskDelete(nullptr)"));
}

// Nothing in the firmware suspends or resumes a task; the poll task is created in one
// place and only ever deletes itself.
static void test_poll_task_lifecycle_has_one_owner() {
  size_t pollNamed = 0;
  std::string offenders;
  for (const Source& s : firmware()) {
    if (countOf(s.code, "vTaskSuspend") || countOf(s.code, "vTaskResume")) offenders += s.path + " ";
    pollNamed += countOf(s.code, "pollTask");
  }
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", offenders.c_str(), "no task suspend/resume in the firmware");
  // The task function is named twice in all the code: its definition and its one
  // create, in begin(). Any other mention (a second create, a restart) fails.
  TEST_ASSERT_EQUAL_size_t(2, pollNamed);
  const std::string& tg = codeOf("src/agent/telegram.cpp");
  anchor(tg, "voidpollTask(void*){");
  anchor(tg, "(pollTask,", anchor(tg, "voidbegin("));
  TEST_ASSERT_EQUAL_size_t(countOf(tg, "vTaskDelete("), countOf(tg, "vTaskDelete(nullptr)"));
}

// The poll task's stop seams have exactly the callers the factory reset needs, and the
// restart API L31 added stays removed: a pause for OTA has nothing to call.
static void test_quiesce_is_factory_reset_only() {
  size_t quiesceCalls = 0, stopCalls = 0;
  for (const Source& s : firmware()) {
    quiesceCalls += countOf(s.code, "telegram::quiesce(");
    stopCalls += countOf(s.code, "telegram::stop(");
    for (const char* gone : {"telegram::resume", "telegram::onPollTask", "telegram::stackBytes",
                             "setPollHooks", "pollRestartStep", "PollResumeLedger"})
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(s.code, gone), (s.path + ": " + gone).c_str());
  }
  TEST_ASSERT_EQUAL_size_t(0, stopCalls);
  // "quiesce(" however it is reached (a namespace alias, a using-declaration): the
  // declaration, the definition and the two calls, nothing else.
  size_t bare = 0;
  for (const Source& s : firmware()) bare += countOf(s.code, "quiesce(");
  TEST_ASSERT_EQUAL_size_t(4, bare);
  TEST_ASSERT_EQUAL_size_t(1, countOf(codeOf("src/agent/telegram.h"), "boolquiesce(uint32_ttimeoutMs);"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(codeOf("src/agent/telegram.cpp"), "boolquiesce(uint32_ttimeoutMs){"));
  // Both calls live in main.cpp, each inside a factory-reset step: the full reset's
  // sweep (gated on g_factoryEraseSd) and the tail's internal file area erase.
  const std::string& m = codeOf("src/main.cpp");
  const std::vector<size_t> at = findAll(m, "telegram::quiesce(");
  TEST_ASSERT_EQUAL_size_t(2, at.size());
  TEST_ASSERT_EQUAL_size_t(2, quiesceCalls);
  for (size_t p : at) {
    const std::string before = m.substr(p > 400 ? p - 400 : 0, p > 400 ? 400 : p);
    const bool reset = countOf(before, "g_factoryEraseSd") || countOf(before, "ResetStep::");
    TEST_ASSERT_TRUE_MESSAGE(reset, m.substr(p, 40).c_str());
  }
  // telegram.h declares no restart, pause or suspend of its own task.
  const std::string& h = codeOf("src/agent/telegram.h");
  for (const char* banned : {"resume(", "pause(", "restart(", "suspend("})
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(h, banned), banned);
}

// The staged work runs only from the poll loop's top, before the Wi-Fi wait and any of
// that cycle's turns (the long-poll drain and the inbound drain both run turns), so
// never inside a turn or a helper dispatch; nothing else runs the OTA job.
static void test_staged_work_runs_only_at_the_poll_loop_top() {
  const std::string& tg = codeOf("src/agent/telegram.cpp");
  const std::vector<size_t> runs = findAll(tg, "g_stagedRun()");
  TEST_ASSERT_EQUAL_size_t(1, runs.size());
  const size_t body = anchor(tg, "voidpollTask(void*){");
  const size_t loop = anchor(tg, "while(g_running){", body);
  const size_t firstTurn = anchor(tg, "drainAndDispatch(", loop);
  const size_t inbound = anchor(tg, "g_cb(", loop);
  const size_t jobs = anchor(tg, "g_tick()", loop);
  // Before the Wi-Fi wait too: without Wi-Fi the run fails fast and frees the OTA
  // claim (USB updates share it) instead of holding it for the whole outage.
  const size_t wifiWait = anchor(tg, "if(!wifiReady()){", loop);
  TEST_ASSERT_TRUE(runs[0] > loop);
  TEST_ASSERT_TRUE(runs[0] < wifiWait);
  TEST_ASSERT_TRUE(runs[0] < firstTurn);
  TEST_ASSERT_TRUE(runs[0] < inbound);
  TEST_ASSERT_TRUE(runs[0] < jobs);
  // The job bodies run on their own task or from that run, nowhere else: no request
  // path (the /update hook runs inside a turn) can reach a check or install inline.
  const std::string& ota = codeOf("src/sys/ota_update.cpp");
  const size_t onPoll = anchor(ota, "voidrunOnPoll(){");
  for (const char* body : {"checkRun();", "installRun();"}) {
    const std::vector<size_t> calls = findAll(ota, body);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, calls.size(), body);
    TEST_ASSERT_TRUE_MESSAGE(calls[0] < onPoll && calls[1] > onPoll, body);   // task + run
  }
  TEST_ASSERT_TRUE(anchor(ota, "checkRun();") > anchor(ota, "staticvoidcheckTask(void*){"));
  TEST_ASSERT_TRUE(anchor(ota, "installRun();") > anchor(ota, "staticvoidinstallTask(void*){"));
  // The OTA run is reached only through that seam: main.cpp hands it over and never
  // calls it, and no other module names it.
  const std::string& m = codeOf("src/main.cpp");
  TEST_ASSERT_EQUAL_size_t(
      1, countOf(m, "agent::telegram::setStagedWork(otaupd::onPollPending,otaupd::runOnPoll)"));
  for (const Source& s : firmware()) {
    if (s.path == "src/sys/ota_update.cpp" || s.path == "src/sys/ota_update.h") continue;
    const size_t named = countOf(s.code, "runOnPoll");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(s.path == "src/main.cpp" ? 1 : 0, named, s.path.c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_comment_stripper);
  RUN_TEST(test_ota_module_never_touches_the_poll_task);
  RUN_TEST(test_poll_task_lifecycle_has_one_owner);
  RUN_TEST(test_quiesce_is_factory_reset_only);
  RUN_TEST(test_staged_work_runs_only_at_the_poll_loop_top);
  return UNITY_END();
}
