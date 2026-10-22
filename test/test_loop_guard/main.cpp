// test_loop_guard - the main loop never waits for the card lock (nimbus/sys/loop_guard.h).
//
// The owner's device reset through the task watchdog every 3.5 h (21-22 Oct 2026):
// a turn's tool dispatch held the shared card lock (agent::memory::lock) for more than
// 8 s, and the main loop, the only task the watchdog watches, sat waiting for it in
// tickSdHealth. These tests pin the class:
//   * the portable rules (wait budget, pass and hold monitors) over every case;
//   * at source level, every agent::memory call the loop can reach (loop() and the
//     main.cpp functions it calls, transitively) is classified, and each one that
//     takes the card lock opens with a LoopLock before any other lock or call. A new
//     loop call into the memory subsystem with no classification fails here.
#include <unity.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/sys/loop_guard.h"

using namespace nimbus::sys;

void setUp() {}
void tearDown() {}

// ---- the wait budget ----------------------------------------------------------------

static void test_wait_budget_every_case() {
  for (int watched = 0; watched <= 1; ++watched) {
    for (int owns = 0; owns <= 1; ++owns) {
      const uint32_t w = lockWaitMs(watched != 0, owns != 0);
      if (watched && !owns) {
        TEST_ASSERT_EQUAL_UINT32(kLoopLockWaitMs, w);
        TEST_ASSERT_TRUE(w * 8 <= nimbus::cloud::kTaskWdtTimeoutMs / 4);
      } else {
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, w);   // other tasks, and an owner's re-take
      }
    }
  }
}

// A caller that retries every pass asks for less; nobody on a watched task gets more.
static void test_requested_wait_is_capped_never_raised() {
  const uint32_t asks[] = {0u, 1u, kLoopLockWaitMs - 1, kLoopLockWaitMs, kLoopLockWaitMs + 1, 60000u, UINT32_MAX};
  for (uint32_t ask : asks) {
    const uint32_t w = lockWaitMs(true, false, ask);
    TEST_ASSERT_TRUE(w <= kLoopLockWaitMs);
    TEST_ASSERT_EQUAL_UINT32(ask < kLoopLockWaitMs ? ask : kLoopLockWaitMs, w);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, lockWaitMs(false, false, ask));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, lockWaitMs(true, true, ask));
  }
}

static void test_a_pass_of_timed_out_tries_stays_far_inside_the_watchdog() {
  TEST_ASSERT_TRUE(kLoopLockWaitMs * kLoopLockTriesPerPass * 20 <= nimbus::cloud::kTaskWdtTimeoutMs);
  TEST_ASSERT_TRUE(kSlowPassMs < nimbus::cloud::kTaskWdtTimeoutMs / 2);   // logged before a reset
}

// ---- the pass monitor ---------------------------------------------------------------

static void test_pass_monitor_quiet_below_the_bar() {
  LoopPassMonitor m;
  uint32_t now = 0;
  for (uint32_t ms = 0; ms < kSlowPassMs; ms += 37) {
    now += ms;
    TEST_ASSERT_FALSE(m.note(ms, now));
  }
  TEST_ASSERT_EQUAL_UINT32(0, m.slowPasses());
  TEST_ASSERT_TRUE(m.maxPassMs() < kSlowPassMs);
}

static void test_pass_monitor_logs_first_slow_pass_and_new_maxima() {
  LoopPassMonitor m;
  TEST_ASSERT_TRUE(m.note(kSlowPassMs, 10000));                              // first: logged
  TEST_ASSERT_FALSE(m.note(kSlowPassMs + 10, 11000));                        // same level: quiet
  TEST_ASSERT_FALSE(m.note(kSlowPassMs + kSlowPassRelogStepMs - 1, 12000));  // under the step
  TEST_ASSERT_TRUE(m.note(kSlowPassMs + kSlowPassRelogStepMs, 13000));       // a step higher
  TEST_ASSERT_EQUAL_UINT32(4, m.slowPasses());
  TEST_ASSERT_EQUAL_UINT32(kSlowPassMs + kSlowPassRelogStepMs, m.maxPassMs());
  TEST_ASSERT_EQUAL_UINT32(13000, m.maxAtMs());
  TEST_ASSERT_TRUE(m.note(kSlowPassMs, 13000 + kSlowPassRelogQuietMs));       // quiet window over
}

// Absence of a flood: a loop stuck at one slow level for a whole day logs once per
// quiet window, never once per pass.
static void test_pass_monitor_never_floods() {
  LoopPassMonitor m;
  uint32_t now = 0, logged = 0;
  const uint32_t pass = kSlowPassMs + 500;
  const uint32_t day = 24u * 3600u * 1000u;
  while (now < day) {
    now += pass;
    if (m.note(pass, now)) ++logged;
  }
  TEST_ASSERT_TRUE(logged >= 1);
  TEST_ASSERT_TRUE(logged <= day / kSlowPassRelogQuietMs + 1);
  TEST_ASSERT_EQUAL_UINT32(day / pass + (day % pass ? 1 : 0), m.slowPasses());
}

static void test_slow_pass_line() {
  LoopPassMonitor m;
  m.note(2400, 5000);
  TEST_ASSERT_EQUAL_STRING("loop: one pass took 2400 ms (longest 2400 ms, 1 slow pass this boot)",
                           slowPassLine(2400, m).c_str());
  m.note(3500, 9000);
  TEST_ASSERT_EQUAL_STRING("loop: one pass took 3500 ms (longest 3500 ms, 2 slow passes this boot)",
                           slowPassLine(3500, m).c_str());
}

// ---- the hold monitor ---------------------------------------------------------------

static void test_hold_monitor_names_long_holds_only() {
  LockHoldMonitor h;
  TEST_ASSERT_FALSE(h.note("memory.scratchpad", 40));
  TEST_ASSERT_FALSE(h.note("memory.search", kLongHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT32(0, h.longHolds());
  TEST_ASSERT_TRUE(h.note("memory.search", kLongHoldMs));
  TEST_ASSERT_TRUE(h.note("web.search", 9200));
  TEST_ASSERT_FALSE(h.note("docs.read", 12));
  TEST_ASSERT_EQUAL_UINT32(2, h.longHolds());
  TEST_ASSERT_EQUAL_UINT32(9200, h.longestMs());
  TEST_ASSERT_EQUAL_STRING("web.search", h.longestWhat().c_str());
}

static void test_long_hold_line_is_printable_and_capped() {
  TEST_ASSERT_EQUAL_STRING("memory: tool memory.search held the card lock for 9200 ms",
                           longHoldLine("memory.search", 9200).c_str());
  TEST_ASSERT_EQUAL_STRING("memory: a tool held the card lock for 3000 ms", longHoldLine("", 3000).c_str());
  // The name is model text: no spaces, '=' or control bytes reach the log, and it is capped.
  const std::string hostile = std::string("x=1 y\n") + std::string(80, 'a');
  const std::string line = longHoldLine(hostile, 3100);
  TEST_ASSERT_TRUE(line.find('\n') == std::string::npos);
  TEST_ASSERT_TRUE(line.find("x_1_y_") != std::string::npos);
  TEST_ASSERT_TRUE(line.size() <= std::string("memory: tool  held the card lock for 3100 ms").size() + 40);
}

// ---- source gate: the loop's storage calls ------------------------------------------

static std::string readSource(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The code without comments and literals (a literal becomes ""), so a brace or a name
// inside a string or a comment never counts. Each skip returns the index of the last
// character it consumed.
static size_t skipLineComment(const std::string& s, size_t i, std::string& out) {
  const size_t nl = s.find('\n', i);
  if (nl == std::string::npos) return s.size();
  out += '\n';
  return nl;
}
static size_t skipBlockComment(const std::string& s, size_t i) {
  const size_t end = s.find("*/", i);
  return end == std::string::npos ? s.size() : end + 1;
}
static size_t skipRawString(const std::string& s, size_t i, std::string& out) {
  const size_t open = s.find('(', i + 2);
  const std::string close = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
  out += "\"\"";
  return s.find(close, open) + close.size() - 1;
}
static size_t skipQuoted(const std::string& s, size_t i, std::string& out) {
  const char q = s[i];
  size_t j = i + 1;
  while (j < s.size() && s[j] != q) j += (s[j] == '\\') ? 2 : 1;
  out += q;
  out += q;
  return j;
}
static std::string codeOnly(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const char n = i + 1 < s.size() ? s[i + 1] : '\0';
    if (c == '/' && n == '/') i = skipLineComment(s, i, out);
    else if (c == '/' && n == '*') i = skipBlockComment(s, i);
    else if (c == 'R' && n == '"') i = skipRawString(s, i, out);
    else if (c == '"' || c == '\'') i = skipQuoted(s, i, out);
    else out += c;
  }
  return out;
}

static bool identChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static bool spaceChar(char c) { return c == ' ' || c == '\n' || c == '\t'; }

// The identifier that ends right before `end` (skipping spaces), or "".
static std::string identBefore(const std::string& s, size_t end) {
  size_t e = end;
  while (e > 0 && spaceChar(s[e - 1])) --e;
  size_t b = e;
  while (b > 0 && identChar(s[b - 1])) --b;
  return s.substr(b, e - b);
}

// Index of the '(' matching the ')' at `close`.
static size_t openParenOf(const std::string& code, size_t close) {
  int depth = 0;
  for (size_t r = close + 1; r-- > 0;) {
    if (code[r] == ')') ++depth;
    if (code[r] == '(' && --depth == 0) return r;
  }
  return 0;
}

// The function name a '{' at `brace` opens, or "" when it opens anything else: it must
// follow ")" (optionally "const"/"override"/"noexcept"), and the name before the "(" must
// not be a statement keyword.
static std::string functionNameAt(const std::string& code, size_t brace) {
  static const std::set<std::string> kKeywords = {"if", "for", "while", "switch", "catch", "return", "sizeof"};
  size_t p = brace;
  while (p > 0 && spaceChar(code[p - 1])) --p;
  const std::string word = identBefore(code, p);
  const bool qualifier = word == "const" || word == "override" || word == "noexcept";
  if (!word.empty() && !qualifier) return "";
  size_t q = p - word.size();
  while (q > 0 && spaceChar(code[q - 1])) --q;
  if (q == 0 || code[q - 1] != ')') return "";
  const std::string name = identBefore(code, openParenOf(code, q - 1));
  return kKeywords.count(name) ? std::string() : name;
}

// Does the line holding the '{' at `brace` open a namespace or an extern "C" block?
static bool opensNamespace(const std::string& code, size_t brace) {
  const size_t nl = code.rfind('\n', brace);
  const std::string head = code.substr(nl == std::string::npos ? 0 : nl + 1, brace - (nl == std::string::npos ? 0 : nl + 1));
  return head.find("namespace") != std::string::npos || head.find("extern \"\"") != std::string::npos;
}

// The body between the '{' at `brace` and its matching '}'.
static std::string bodyAt(const std::string& code, size_t brace) {
  int d = 1;
  size_t j = brace + 1;
  for (; j < code.size() && d; ++j) {
    if (code[j] == '{') ++d;
    if (code[j] == '}') --d;
  }
  return code.substr(brace + 1, j - brace - 2);
}

// Function bodies at namespace scope, by name (overloads concatenated); namespace and
// extern blocks are transparent, every other brace nests.
static std::map<std::string, std::string> functionBodies(const std::string& code) {
  std::map<std::string, std::string> out;
  std::vector<bool> transparent;   // one entry per open brace
  int depth = 0;                   // open braces that are not namespace blocks
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '}' && !transparent.empty()) {
      depth -= transparent.back() ? 0 : 1;
      transparent.pop_back();
    }
    if (code[i] != '{') continue;
    const bool ns = depth == 0 && opensNamespace(code, i);
    transparent.push_back(ns);
    if (ns) continue;
    if (depth == 0) {
      const std::string name = functionNameAt(code, i);
      if (!name.empty()) out[name] += bodyAt(code, i);
    }
    ++depth;
  }
  return out;
}

// Every NAME in "prefix NAME(" occurrences.
static std::set<std::string> callsWithPrefix(const std::string& body, const std::string& prefix) {
  std::set<std::string> out;
  for (size_t at = body.find(prefix); at != std::string::npos; at = body.find(prefix, at + 1)) {
    size_t b = at + prefix.size(), e = b;
    while (e < body.size() && identChar(body[e])) ++e;
    size_t k = e;
    while (k < body.size() && body[k] == ' ') ++k;
    if (e > b && k < body.size() && body[k] == '(') out.insert(body.substr(b, e - b));
  }
  return out;
}

// Identifiers called anywhere in `body` ("name(").
static std::set<std::string> calledNames(const std::string& body) {
  std::set<std::string> out;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '(') continue;
    const std::string n = identBefore(body, i);
    if (!n.empty()) out.insert(n);
  }
  return out;
}

// The memory functions the loop reaches: loop() and every main.cpp function it calls.
static std::map<std::string, std::string> loopReachedMemoryCalls(const std::map<std::string, std::string>& mainFns) {
  std::map<std::string, std::string> calls;   // memory function -> first main.cpp caller
  std::set<std::string> seen = {"loop"};
  std::vector<std::string> todo = {"loop"};
  while (!todo.empty()) {
    const std::string fn = todo.back();
    todo.pop_back();
    const std::string& body = mainFns.at(fn);
    for (const std::string& m : callsWithPrefix(body, "agent::memory::")) calls.emplace(m, fn);
    for (const std::string& n : calledNames(body)) {
      if (mainFns.count(n) && !seen.count(n)) {
        seen.insert(n);
        todo.push_back(n);
      }
    }
  }
  return calls;
}

// Names a guarded function may call before its LoopLock: plain reads, no lock.
static const std::set<std::string> kLockFreeHelpers = {"effHaveSd", "clockSynced", "sdLost"};

static void assertOpensWithLoopLock(const std::string& fn, const std::string& body) {
  const size_t at = body.find("LoopLock");
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, (fn + ": takes the card lock without a LoopLock").c_str());
  const std::string before = body.substr(0, at);
  TEST_ASSERT_TRUE_MESSAGE(before.find("Lock") == std::string::npos && before.find("lock(") == std::string::npos,
                           (fn + ": a lock is taken before the LoopLock").c_str());
  for (const std::string& n : calledNames(before))
    TEST_ASSERT_TRUE_MESSAGE(kLockFreeHelpers.count(n) || n == "if",
                             (fn + ": calls " + n + " before the LoopLock").c_str());
}

static void test_every_loop_storage_call_is_bounded() {
  const std::string mainSrc = codeOnly(readSource("src/main.cpp"));
  const std::string memSrc = codeOnly(readSource("src/agent/memory_subsystem.cpp"));
  TEST_ASSERT_FALSE(mainSrc.empty());
  TEST_ASSERT_FALSE(memSrc.empty());
  const std::map<std::string, std::string> mainFns = functionBodies(mainSrc);
  const std::map<std::string, std::string> memFns = functionBodies(memSrc);
  TEST_ASSERT_TRUE(mainFns.count("loop") && mainFns.count("setup"));
  TEST_ASSERT_TRUE(memFns.size() > 100);   // the parse really saw the subsystem

  // Open the card lock with a LoopLock (busy: skip or park).
  const std::set<std::string> kGuarded = {"tickSdHealth", "appendBatteryHistory", "captureEvent",
                                          "flushPendingEvents", "promoteSd"};
  // Try with a timeout of their own (the name: the call that does the timed try).
  const std::map<std::string, std::string> kBoundedTry = {{"pruneRetention", "tryLock"},
                                                          {"episodicBackfillTick", "runBackfillSlice"},
                                                          {"runBackfillSlice", "tryLock"}};
  // Read flags only.
  const std::set<std::string> kLockFree = {"sdLost", "haveSd", "nowHours", "consumeRetentionRequest"};
  // The factory erase: the owner confirmed it, the turn task is quiesced first, its
  // card work runs under its own timed tries, and the device restarts after it.
  const std::set<std::string> kFactoryErase = {"eraseAllPrivateData", "factoryEraseWouldRefuse",
                                               "eraseDurableStore", "finalSweepPass", "scrubInternalFs"};

  const std::map<std::string, std::string> reached = loopReachedMemoryCalls(mainFns);
  TEST_ASSERT_TRUE(reached.count("tickSdHealth"));   // the reach really walked loop()
  for (const auto& kv : reached) {
    const std::string& fn = kv.first;
    const bool known = kGuarded.count(fn) || kBoundedTry.count(fn) || kLockFree.count(fn) || kFactoryErase.count(fn);
    TEST_ASSERT_TRUE_MESSAGE(known, ("loop reaches agent::memory::" + fn + " (via " + kv.second +
                                     "): open it with a LoopLock and classify it here")
                                        .c_str());
  }
  for (const std::string& fn : kGuarded) {
    TEST_ASSERT_TRUE_MESSAGE(memFns.count(fn), (fn + ": no definition").c_str());
    assertOpensWithLoopLock(fn, memFns.at(fn));
  }
  for (const auto& kv : kBoundedTry) {
    TEST_ASSERT_TRUE_MESSAGE(memFns.count(kv.first), (kv.first + ": no definition").c_str());
    const std::string& body = memFns.at(kv.first);
    TEST_ASSERT_TRUE_MESSAGE(body.find(kv.second + "(") != std::string::npos, (kv.first + ": no " + kv.second).c_str());
    const std::string bare = body.substr(0, body.find(kv.second + "("));
    TEST_ASSERT_TRUE_MESSAGE(bare.find("Lock g") == std::string::npos && bare.find("lock()") == std::string::npos,
                             (kv.first + ": a blocking lock before its timed try").c_str());
  }
  for (const std::string& fn : kLockFree) {
    TEST_ASSERT_TRUE_MESSAGE(memFns.count(fn), (fn + ": no definition").c_str());
    const std::string& body = memFns.at(fn);
    TEST_ASSERT_TRUE_MESSAGE(body.find("Lock") == std::string::npos && body.find("lock(") == std::string::npos,
                             (fn + ": takes a lock").c_str());
  }
  for (const std::string& fn : kFactoryErase) TEST_ASSERT_TRUE_MESSAGE(memFns.count(fn), (fn + ": no definition").c_str());

  // Every loop log line reaches the durable log with a timed try (CUM-401).
  const std::map<std::string, std::string> logFns = functionBodies(codeOnly(readSource("src/sys/errlog_fs.cpp")));
  TEST_ASSERT_TRUE(logFns.count("append"));
  TEST_ASSERT_TRUE(logFns.at("append").find("tryLock(") != std::string::npos);
}

// The gate itself: it must fail a loop storage call that waits, and see through the
// shapes the real sources use.
static void test_gate_rejects_a_blocking_loop_call() {
  const std::string mainSrc = codeOnly(
      "namespace {\nint x;\n}\nstatic void helper() {\n  agent::memory::slowThing();\n}\n"
      "void loop() {\n  if (a) { helper(); }\n  agent::memory::tickSdHealth(now);\n}\n"
      "void setup() {\n  agent::memory::begin();\n}\n");
  const std::map<std::string, std::string> fns = functionBodies(mainSrc);
  const std::map<std::string, std::string> reached = loopReachedMemoryCalls(fns);
  TEST_ASSERT_EQUAL(2, (int)reached.size());   // setup()'s call is not the loop's
  TEST_ASSERT_TRUE(reached.count("slowThing") && reached.count("tickSdHealth"));
  TEST_ASSERT_EQUAL_STRING("helper", reached.at("slowThing").c_str());

  const std::string base = codeOnly(
      "void tickSdHealth(uint32_t nowMs) {\n  flushPendingEvents();  // \"Lock\" in a comment\n"
      "  Lock g;\n  probe();\n}\n");
  const std::map<std::string, std::string> memFns = functionBodies(base);
  TEST_ASSERT_TRUE(memFns.count("tickSdHealth"));
  TEST_ASSERT_TRUE(memFns.at("tickSdHealth").find("LoopLock") == std::string::npos);   // v4.5.15 shape fails
  const std::string fixed = codeOnly(
      "bool flushPendingEvents() const {\n  if (!g_begun || !clockSynced()) return;\n  LoopLock g;\n"
      "  if (!g) return;\n}\n");
  const std::map<std::string, std::string> fixedFns = functionBodies(fixed);
  TEST_ASSERT_TRUE(fixedFns.count("flushPendingEvents"));
  assertOpensWithLoopLock("flushPendingEvents", fixedFns.at("flushPendingEvents"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wait_budget_every_case);
  RUN_TEST(test_requested_wait_is_capped_never_raised);
  RUN_TEST(test_a_pass_of_timed_out_tries_stays_far_inside_the_watchdog);
  RUN_TEST(test_pass_monitor_quiet_below_the_bar);
  RUN_TEST(test_pass_monitor_logs_first_slow_pass_and_new_maxima);
  RUN_TEST(test_pass_monitor_never_floods);
  RUN_TEST(test_slow_pass_line);
  RUN_TEST(test_hold_monitor_names_long_holds_only);
  RUN_TEST(test_long_hold_line_is_printable_and_capped);
  RUN_TEST(test_every_loop_storage_call_is_bounded);
  RUN_TEST(test_gate_rejects_a_blocking_loop_call);
  return UNITY_END();
}
