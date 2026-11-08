// The poll task's liveness guard, wired into the firmware (lane TGSTALL).
//
// The guard's rules are host-tested in test_poll_guard, but they only protect the device
// while the firmware calls them: the loop task must check at every pass, the poll task
// must be armed when it starts and disarmed when it stops on purpose, the restart must
// be a panic (so the poison guard counts a try), and every path that legitimately keeps
// the poll task busy for long (an update download, a provider exchange, an archive
// download fetched whole, a turn's rounds and tools) must beat, or a slow link turns a
// healthy install into a restart in the middle of it. A later edit that drops any of
// these keeps every behavior test green; this suite fails instead. It reads the firmware
// sources with comments stripped (prose that names a call never counts) and whitespace
// squeezed out (a call split over lines still counts), the same way test_ota_poll_guard
// does.

#include <unity.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

namespace {

std::string readFile(const std::string& path) {
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
size_t skipRawString(const std::string& s, size_t q) {
  const size_t open = s.find('(', q);
  if (open == std::string::npos) return s.size();
  const std::string close = ")" + s.substr(q + 1, open - q - 1) + "\"";
  const size_t end = s.find(close, open);
  return end == std::string::npos ? s.size() : end + close.size();
}

// True when the ' at `i` is a C++14 digit separator (24'576): its token starts with a digit.
bool isDigitSeparator(const std::string& s, size_t i) {
  size_t t = i;
  while (t > 0 && (std::isalnum((unsigned char)s[t - 1]) || s[t - 1] == '\'')) --t;
  return t < i && std::isdigit((unsigned char)s[t]);
}

// Index just past the literal that opens at `i` ("...", raw or not, or '...').
size_t skipLiteral(const std::string& s, size_t i) {
  const char q = s[i];
  if (q == '"' && i > 0 && s[i - 1] == 'R') return skipRawString(s, i);
  for (++i; i < s.size(); ++i) {
    if (s[i] == '\\') ++i;
    else if (s[i] == q) return i + 1;
  }
  return s.size();
}

// The newline that ends the // comment at `at` (a backslash before it splices the next line).
size_t lineCommentEnd(const std::string& s, size_t at) {
  size_t nl = s.find('\n', at);
  while (nl != std::string::npos && nl > 0 &&
         (s[nl - 1] == '\\' || (s[nl - 1] == '\r' && nl > 1 && s[nl - 2] == '\\')))
    nl = s.find('\n', nl + 1);
  return nl;
}

// Every comment replaced by a space; literals kept whole.
std::string stripComments(const std::string& src) {
  std::string out;
  size_t i = 0;
  while (i < src.size()) {
    const size_t at = src.find_first_of("\"'/", i);
    if (at == std::string::npos) { out.append(src, i, std::string::npos); break; }
    out.append(src, i, at - i);
    if (src[at] == '\'' && isDigitSeparator(src, at)) {
      out += '\'';
      i = at + 1;
    } else if (src[at] != '/') {
      i = skipLiteral(src, at);
      out.append(src, at, i - at);
    } else if (src.compare(at, 2, "//") == 0) {
      out += ' ';
      i = lineCommentEnd(src, at);
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

std::string squeeze(const std::string& code) {
  std::string out;
  out.reserve(code.size());
  for (char c : code)
    if (!std::isspace((unsigned char)c)) out += c;
  return out;
}

const std::string& codeOf(const std::string& path) {
  static std::vector<std::pair<std::string, std::string>> cache;
  for (const auto& e : cache)
    if (e.first == path) return e.second;
  cache.emplace_back(path, squeeze(stripComments(readFile(path))));
  return cache.back().second;
}

size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// Index just past the brace that closes the one at `open`, skipping literals.
size_t closeOf(const std::string& c, size_t open) {
  int depth = 0;
  for (size_t i = open; i < c.size(); ++i) {
    const char ch = c[i];
    if (ch == '"' || (ch == '\'' && !isDigitSeparator(c, i))) {
      i = skipLiteral(c, i) - 1;
      continue;
    }
    if (ch == '{') ++depth;
    if (ch == '}' && --depth == 0) return i + 1;
  }
  return c.size();
}

// The body (between the braces) of the function or lambda whose head is `head` (squeezed,
// up to and including its opening brace). Fails the test when the head is absent or
// appears more than once in the file.
std::string bodyOf(const std::string& path, const std::string& head) {
  const std::string& c = codeOf(path);
  const std::string h = squeeze(head);
  const size_t at = c.find(h);
  const std::string msg = path + ": " + head;
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, msg.c_str());
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, countOf(c, h), msg.c_str());
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('{', h.back(), msg.c_str());
  const size_t open = at + h.size() - 1;
  const size_t end = closeOf(c, open);
  return c.substr(open + 1, end - open - 2);
}

void assertHas(const std::string& hay, const std::string& needle, const std::string& where) {
  const std::string n = squeeze(needle);
  const std::string msg = where + " lost: " + needle;
  TEST_ASSERT_TRUE_MESSAGE(hay.find(n) != std::string::npos, msg.c_str());
}

std::string beat(const char* step) { return squeeze(std::string("nimbus::pollwatch::beat(\"") + step + "\");"); }

}  // namespace

// The loop task checks at every pass: tick() is a plain statement at the top level of
// loop(), before anything that can return early, never under a condition.
void test_loop_checks_every_pass() {
  const std::string body = bodyOf("src/main.cpp", "void loop() {");
  const std::string call = squeeze("nimbus::pollwatch::tick();");
  const size_t at = body.find(call);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "loop() no longer calls pollwatch::tick()");
  TEST_ASSERT_EQUAL_size_t(1, countOf(body, call));
  const std::string before = body.substr(0, at);
  TEST_ASSERT_TRUE_MESSAGE(before.find("return") == std::string::npos, "loop() can return before the check");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(countOf(before, "{"), countOf(before, "}"), "the check sits inside a block");
  TEST_ASSERT_TRUE_MESSAGE(at == 0 || before.back() == ';' || before.back() == '}',
                           "the check is under a condition");
  // Nothing else in the firmware calls it (one checker, one cadence).
  TEST_ASSERT_EQUAL_size_t(1, countOf(codeOf("src/main.cpp"), call));
}

// setup() reads the streak record before it plans the crash notice (the notice needs to
// know this boot follows a guard restart).
void test_setup_reads_the_record_before_the_notice() {
  const std::string body = bodyOf("src/main.cpp", "void setup() {");
  const size_t boot = body.find(squeeze("nimbus::pollwatch::boot();"));
  const size_t notice = body.find(squeeze("nimbus::crash::queueCrashNotice();"));
  TEST_ASSERT_TRUE_MESSAGE(boot != std::string::npos, "setup() no longer calls pollwatch::boot()");
  TEST_ASSERT_TRUE(notice != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(boot < notice, "the crash notice is planned before the record is read");
}

// The poll task is armed the moment it exists, beats at its loop top, and is disarmed
// when it stops on purpose (its own exit, a quiesce).
void test_poll_task_armed_beating_and_disarmed() {
  const std::string tg = "src/agent/telegram.cpp";
  const std::string begin = bodyOf(tg, "void begin(const String& token, const String& allowlist, MessageCallback cb) {");
  const size_t create = begin.find(squeeze("xTaskCreatePinnedToCore(pollTask,"));
  const size_t arm = begin.find(squeeze("nimbus::pollwatch::arm(g_task);"));
  TEST_ASSERT_TRUE(create != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(arm != std::string::npos, "telegram::begin() no longer arms the guard");
  TEST_ASSERT_TRUE(create < arm);

  const std::string task = bodyOf(tg, "void pollTask(void*) {");
  assertHas(task, "while (g_running) {" + std::string("nimbus::pollwatch::beat(\"poll loop\");"), "pollTask loop top");
  assertHas(task, "nimbus::pollwatch::disarm(); g_task = nullptr; vTaskDelete(nullptr);", "pollTask exit");
  // The staged update work runs inline on this task: it beats before it starts.
  assertHas(task, "nimbus::pollwatch::beat(\"staged work\"); g_stagedRun();", "pollTask staged work");

  const std::string quiesce = bodyOf(tg, "bool quiesce(uint32_t timeoutMs) {");
  TEST_ASSERT_TRUE_MESSAGE(quiesce.find(squeeze("nimbus::pollwatch::disarm();")) == 0,
                           "quiesce() must disarm before it stops the task");
}

// The guard restarts by a panic (the poison guard counts a try only for a crash reset:
// test_poll_guard test_guard_restart_counts_a_poison_try), never by esp_restart.
void test_restart_is_a_panic() {
  const std::string pw = "src/sys/poll_watch.cpp";
  const std::string restart = bodyOf(pw, "[[noreturn]] void restartFor(const PollStall& s, TaskHandle_t t, uint32_t limitMs) {");
  assertHas(restart, "esp_system_abort(why.c_str());", "restartFor");
  for (const char* banned : {"esp_restart(", "ESP.restart(", "vTaskDelete(", "vTaskSuspend("})
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, countOf(codeOf(pw), banned), banned);
  const std::string tick = bodyOf(pw, "void tick() {");
  assertHas(tick, "restartFor(s, t, limitMs);", "tick");
  // The beat counts only the watched task's own calls.
  assertHas(bodyOf(pw, "void beat(const char* step) {"), "xTaskGetCurrentTaskHandle() == t", "beat");
}

// The boot after a guard restart plans the stall notice with what Telegram will serve
// again (a token and the poll up, the poison tries of the work in flight).
void test_notice_is_told_what_telegram_serves_again() {
  const std::string body = bodyOf("src/sys/crash_report.cpp", "void queueCrashNotice() {");
  assertHas(body, "nimbus::orch::planStallNotice(", "queueCrashNotice");
  assertHas(body, "agent::telegram::enabled()", "queueCrashNotice");
  assertHas(body, "agent::telegram::guardStatus()", "queueCrashNotice");
}

// The long paths beat. Each row is a place the poll task can legitimately stay for long,
// with the beat that keeps the guard from restarting a healthy device there.
void test_long_paths_beat() {
  struct Row {
    const char* file;
    const char* head;   // the function (or lambda) the beat must stay inside
    const char* code;   // the beat with its neighbours: it stays in that loop
  };
  const Row kRows[] = {
      // The staged update check and install download on the poll task: a slow link can
      // take longer than 5 minutes for an image, so every chunk beats.
      {"src/sys/ota_update.cpp", nullptr,
       "while (before(deadline)) { int n = client.read(buf, sizeof(buf)); nimbus::pollwatch::beat(\"update download\");"},
      // A provider exchange (capped at 120 s): every read wait, every write slice and
      // pacing sleep, every connect attempt.
      {"src/agent/transport_tls.cpp", "int read() override {", "nimbus::pollwatch::beat(\"provider read\"); delay(1);"},
      {"src/agent/transport_tls.cpp", "int readStatus() {", "else { nimbus::pollwatch::beat(\"provider read\"); delay(2); }"},
      {"src/agent/transport_tls.cpp", "bool readHeaders(int code) {", "else { nimbus::pollwatch::beat(\"provider read\"); delay(2); }"},
      {"src/agent/transport_tls.cpp", "bool readBody() {", "else { nimbus::pollwatch::beat(\"provider read\"); delay(2); }"},
      {"src/agent/transport_tls.cpp", "void paceForHeadroom() {",
       "auto sleep = [](uint32_t ms) { nimbus::pollwatch::beat(\"provider write\"); vTaskDelay("},
      {"src/agent/transport_tls.cpp", "size_t writeBody(size_t off, size_t want) {", "nimbus::pollwatch::beat(\"provider write\");"},
      {"src/agent/transport_tls.cpp", "bool connect() {",
       "for (int a = 0; a < 3 && !expired(); a++) { nimbus::pollwatch::beat(\"provider connect\");"},
      // An archive download: a file the server will not resume is fetched whole (up to
      // 20 MB, no time budget), so every read beats, also while bytes keep coming.
      {"src/agent/tg_archive.cpp", "int read(uint8_t* buf, size_t cap) override {",
       "nimbus::pollwatch::beat(\"archive download\");"},
      // Telegram's own socket waits.
      {"src/agent/telegram.cpp", "static void idlePause(uint32_t pauseMs) {", "nimbus::pollwatch::beat(\"idle\"); vTaskDelay("},
      {"src/agent/telegram.cpp", "int readLine(WiFiClientSecure& sc, char* buf, int bufLen, uint32_t deadline) {",
       "nimbus::pollwatch::beat(\"telegram read\"); vTaskDelay(1);"},
      {"src/agent/telegram.cpp", "int readBody(WiFiClientSecure& sc, char* buf, int bufLen, uint32_t deadline) {",
       "nimbus::pollwatch::beat(\"telegram read\"); vTaskDelay(1);"},
      {"src/agent/telegram.cpp", "static int readBodyN(WiFiClientSecure& sc, char* buf, int bufLen, long want, uint32_t deadline) {",
       "nimbus::pollwatch::beat(\"telegram read\"); vTaskDelay(1);"},
      {"src/agent/telegram.cpp", "static nimbus::net::TgWait awaitPollResponse(uint32_t deadline, int timeoutS) {",
       "nimbus::pollwatch::beat(\"long-poll\"); vTaskDelay(1);"},
      {"src/agent/telegram.cpp", "bool tlsConnect(WiFiClientSecure& sc) {", "nimbus::pollwatch::beat(\"telegram connect\");"},
      {"src/agent/telegram.cpp", nullptr, "else { nimbus::pollwatch::beat(\"media download\"); delay(2); }"},
      // A turn: its start and end, every round, every tool call and result.
      {"src/agent/orchestrator.cpp", "d.hooks.onTurnStart = [](const TurnStartEv& ev) {", "nimbus::pollwatch::beat(\"turn\");"},
      {"src/agent/orchestrator.cpp", "d.hooks.onToolCall = [](const orch::HeadToolCall& call) {", "nimbus::pollwatch::beat(\"tool\");"},
      {"src/agent/orchestrator.cpp", "d.hooks.onToolResult = [](const orch::HeadToolResult& r) {", "nimbus::pollwatch::beat(\"tool done\");"},
      {"src/agent/orchestrator.cpp", "d.hooks.onTurnProgress = [](const TurnProgressEv& ev) {", "nimbus::pollwatch::beat(\"round\");"},
      {"src/agent/orchestrator.cpp", "d.hooks.onTurnEnd = [](const TurnEndEv& ev) {", "nimbus::pollwatch::beat(\"turn end\");"},
      // Every TLS session any adapter opens or closes.
      {"src/sys/net_util.h", "inline void tlsClose(WiFiClientSecure& c) {", "nimbus::pollwatch::beat(\"close\");"},
  };
  for (const Row& r : kRows) {
    const std::string where = std::string(r.file) + (r.head ? std::string(" ") + r.head : std::string());
    const std::string scope = r.head ? bodyOf(r.file, r.head) : codeOf(r.file);
    assertHas(scope, r.code, where);
  }
  // Every connectHost overload beats first: every adapter's connect is a beat.
  const std::string& nu = codeOf("src/sys/net_util.h");
  const std::string head = squeeze("inline int connectHost(");
  size_t n = 0;
  for (size_t at = nu.find(head); at != std::string::npos; at = nu.find(head, at + 1), ++n) {
    const size_t open = nu.find('{', at);
    TEST_ASSERT_TRUE(open != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(nu.compare(open + 1, beat("connect").size(), beat("connect")) == 0,
                             "a connectHost overload does not beat first");
  }
  TEST_ASSERT_EQUAL_size_t(3, n);
}

// The name lookup every connect runs is the host-tested one (nimbus/net/dns_lookup.h,
// test_late_wait), not a copy of it.
void test_resolver_is_the_tested_one() {
  const std::string body =
      bodyOf("src/sys/net_util.h", "inline bool resolveHostLocked(const char* host, IPAddress& out, uint32_t waitMs = 10000) {");
  assertHas(body, "nimbus::net::DnsLookup<nimbus_net_detail::LwipDnsOps>::resolve(host, a, waitMs)", "resolveHostLocked");
  TEST_ASSERT_EQUAL_size_t(0, countOf(body, "dns_gethostbyname"));
  TEST_ASSERT_EQUAL_size_t(1, countOf(codeOf("src/sys/net_util.h"), "dns_gethostbyname("));
}

// Every beat names its step with a string literal: the guard keeps the pointer (it is the
// step the panic reason and /api/state report), so a temporary would dangle.
void test_every_beat_step_is_a_literal() {
  size_t beats = 0;
  for (const char* root : {"src", "include", "lib/core/src", "lib/core/include"}) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") continue;
      const std::string path = e.path().generic_string();
      const std::string& c = codeOf(path);
      const std::string call = "pollwatch::beat(";
      for (size_t at = c.find(call); at != std::string::npos; at = c.find(call, at + 1)) {
        ++beats;
        const std::string msg = path + ": a beat whose step is not a string literal";
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('"', c[at + call.size()], msg.c_str());
      }
    }
  }
  TEST_ASSERT_TRUE(beats >= 30);   // the walk found the firmware's beats
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_loop_checks_every_pass);
  RUN_TEST(test_setup_reads_the_record_before_the_notice);
  RUN_TEST(test_poll_task_armed_beating_and_disarmed);
  RUN_TEST(test_restart_is_a_panic);
  RUN_TEST(test_notice_is_told_what_telegram_serves_again);
  RUN_TEST(test_long_paths_beat);
  RUN_TEST(test_resolver_is_the_tested_one);
  RUN_TEST(test_every_beat_step_is_a_literal);
  return UNITY_END();
}
