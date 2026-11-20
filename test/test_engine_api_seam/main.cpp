// test_engine_api_seam - the device seam's split (lane RB-SEAM1b, PRD section 10), as a class
// over the sources (comments stripped), so a call that lands on the wrong side fails here:
//   - every call src/agent/engine_api.h declares is defined in EACH engine seam (the old engine's
//     src/agent/orchestrator_e1.cpp and the plain engine's src/agent/orchestrator_plain.cpp, RB-I0;
//     a device image links exactly one) and never in the engine-neutral half
//     (src/agent/orchestrator_common.cpp);
//   - every call src/agent/engine_tools.h declares is defined in each engine's tool file
//     (tools_e1.cpp, tools_plain.cpp, RB-I0);
//   - every call src/agent/orchestrator.h itself declares is defined in the common half and
//     never in an engine seam, so both engines share one tenant table, session snapshot,
//     results ring and set of reply sinks;
//   - orchestrator_e1.cpp is the only device file that includes nimbus/harness/engine.h.
// The scanners are checked on synthetic sources first (the counter-tests).
#include <unity.h>

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

void setUp() {}
void tearDown() {}

static const char* const kApi = "src/agent/engine_api.h";
static const char* const kPublic = "src/agent/orchestrator.h";
static const char* const kCommon = "src/agent/orchestrator_common.cpp";
static const char* const kEngine1 = "src/agent/orchestrator_e1.cpp";
// Every engine seam an env links (RB-I0: the engine-2 envs link orchestrator_plain.cpp).
static const char* const kSeams[] = {"src/agent/orchestrator_e1.cpp", "src/agent/orchestrator_plain.cpp"};
static const char* const kToolsApi = "src/agent/engine_tools.h";
static const char* const kToolFiles[] = {"src/agent/tools_e1.cpp", "src/agent/tools_plain.cpp"};
static const char* const kEngineHeader = "nimbus/harness/engine.h";

static std::string readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Drops // and /* */ comments outside string and character literals.
static std::string stripComments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const char n = i + 1 < s.size() ? s[i + 1] : 0;
    if (c == '"' || c == '\'') {
      out += c;
      for (++i; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        if (s[i] == c) break;
      }
    } else if (c == '/' && n == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      out += '\n';
    } else if (c == '/' && n == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) break;
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

static std::string code(const std::string& path) {
  const std::string s = readFile(path);
  TEST_ASSERT_FALSE_MESSAGE(s.empty(), path.c_str());
  return stripComments(s);
}

static bool isIdent(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// The function a namespace-level statement declares ("" when it declares none): the name
// right before its first '(', unless it is a `using` alias.
static std::string declaredName(const std::string& stmt) {
  const std::string s = trim(stmt);
  const size_t paren = s.find('(');
  if (paren == std::string::npos || s.rfind("using ", 0) == 0) return std::string();
  size_t e = paren;
  while (e > 0 && s[e - 1] == ' ') --e;
  size_t b = e;
  while (b > 0 && isIdent(s[b - 1])) --b;
  return s.substr(b, e - b);
}

// The names of the functions a header declares at namespace depth `depth` (2 for
// agent::orchestrator), from comment-stripped text. Preprocessor lines are skipped (an
// #ifdef'd declaration counts).
static std::set<std::string> declaredFunctions(const std::string& src, int depth = 2) {
  std::set<std::string> out;
  int d = 0;
  std::string stmt;
  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    if (c == '#' && (i == 0 || src[i - 1] == '\n')) {   // a preprocessor line
      i = src.find('\n', i);
      if (i == std::string::npos) break;
      continue;
    }
    if (c == '{' || c == '}') {
      d += c == '{' ? 1 : -1;
      stmt.clear();
    } else if (d == depth && c == ';') {
      const std::string name = declaredName(stmt);
      if (!name.empty()) out.insert(name);
      stmt.clear();
    } else if (d == depth) {
      stmt += c;
    }
  }
  return out;
}

static size_t skipBlanks(const std::string& src, size_t p) {
  while (p < src.size() && (src[p] == ' ' || src[p] == '\n')) ++p;
  return p;
}

// From the '(' at `p`: the parameter list closes and a body opens (an optional `const` between).
static bool closesIntoBody(const std::string& src, size_t p) {
  int open = 0;
  for (; p < src.size(); ++p) {
    if (src[p] == ';' || src[p] == '{') return false;
    if (src[p] == '(') ++open;
    if (src[p] == ')' && --open == 0) break;
  }
  if (p >= src.size()) return false;
  p = skipBlanks(src, p + 1);
  if (src.compare(p, 5, "const") == 0) p = skipBlanks(src, p + 5);
  return p < src.size() && src[p] == '{';
}

// `name` at `at` on a column-0 line [line, end) is a definition's name: after a return type
// (not after `::`, so a member is not it), followed by its parameter list and a body.
static bool definesAt(const std::string& src, size_t line, size_t at, const std::string& name) {
  if (at == line || isIdent(src[at - 1]) || src[at - 1] == ':') return false;
  size_t p = at + name.size();
  while (p < src.size() && src[p] == ' ') ++p;
  return p < src.size() && src[p] == '(' && closesIntoBody(src, p);
}

// Whether comment-stripped `src` DEFINES a free function `name`, on a line that starts in
// column 0 (a call inside a body is indented).
static bool definesFunction(const std::string& src, const std::string& name) {
  for (size_t line = 0; line < src.size();) {
    const size_t eol = src.find('\n', line);
    const size_t end = eol == std::string::npos ? src.size() : eol;
    const char first = src[line];
    const bool candidate = line < end && first != ' ' && first != '\t' && first != '#' && first != '}';
    for (size_t at = src.find(name, line); candidate && at != std::string::npos && at < end; at = src.find(name, at + 1))
      if (definesAt(src, line, at, name)) return true;
    if (eol == std::string::npos) break;
    line = eol + 1;
  }
  return false;
}

static bool includesEngineHeader(const std::string& strippedSrc) {
  return strippedSrc.find(std::string("#include \"") + kEngineHeader + "\"") != std::string::npos ||
         strippedSrc.find(std::string("#include <") + kEngineHeader + ">") != std::string::npos;
}

// ---- the scanners, on synthetic sources --------------------------------------------------

static void test_the_declaration_scanner_reads_namespace_functions_only() {
  const std::string h = stripComments(
      "#pragma once\n#include \"x.h\"\nnamespace agent {\nnamespace orchestrator {\n"
      "using F = void (*)(int);\nstruct S {\n  int x;\n  void member();\n};\n"
      "// void commented();\nint f(int a = g());\nstd::vector<T> list(size_t* n = nullptr);\n"
      "#ifdef NIMBUS_TEST\nvoid testOnly();\n#endif\nuint32_t  spaced  ();\n}\n}\n");
  const std::set<std::string> got = declaredFunctions(h);
  const std::set<std::string> want = {"f", "list", "testOnly", "spaced"};
  TEST_ASSERT_TRUE(got == want);
}

static void test_the_definition_scanner_finds_bodies_not_calls_or_declarations() {
  const std::string src = stripComments(
      "namespace a {\nbool f(int x,\n       int y) {\n  if (g(x)) {\n    return h(y);\n  }\n}\n"
      "bool declared(int);\nint TurnEngine::member() { return 0; }\n"
      "String withDefault(const char* c) const {\n}\n// int commented() {}\nstatic void s() { k(); }\n}\n");
  TEST_ASSERT_TRUE(definesFunction(src, "f"));
  TEST_ASSERT_TRUE(definesFunction(src, "withDefault"));
  TEST_ASSERT_TRUE(definesFunction(src, "s"));
  TEST_ASSERT_FALSE(definesFunction(src, "g"));          // a call inside a body
  TEST_ASSERT_FALSE(definesFunction(src, "h"));
  TEST_ASSERT_FALSE(definesFunction(src, "declared"));   // a declaration
  TEST_ASSERT_FALSE(definesFunction(src, "member"));     // a member, not a free function
  TEST_ASSERT_FALSE(definesFunction(src, "commented"));
  TEST_ASSERT_FALSE(definesFunction(src, "k"));
}

static void test_the_include_scanner_ignores_comments() {
  TEST_ASSERT_TRUE(includesEngineHeader(stripComments("#include \"nimbus/harness/engine.h\"  // x\n")));
  TEST_ASSERT_FALSE(includesEngineHeader(stripComments("// #include \"nimbus/harness/engine.h\"\n")));
  TEST_ASSERT_FALSE(includesEngineHeader(stripComments("#include \"nimbus/harness/engine_fold.h\"\n")));
}

// ---- the device seam ------------------------------------------------------------------------

static void test_every_engine_call_is_defined_in_the_engine_seam_only() {
  const std::set<std::string> api = declaredFunctions(code(kApi));
  // The calls the plan names (turn entry, status for the web, last-turn data, fold status,
  // clear conversation) are in the list.
  for (const char* named : {"begin", "handleMessage", "injectScheduledTurn", "pollJobs", "turnInFlight", "turnCount",
                            "lastTurnUsage", "lastTurnDebugPs", "lastFailJson", "foldStatusJsonFor", "foldStatusText",
                            "stageManualFold", "requestConvClear", "requestConvReset"})
    TEST_ASSERT_TRUE_MESSAGE(api.count(named) == 1, named);
  const std::string common = code(kCommon);
  for (const char* seam : kSeams) {
    const std::string src = code(seam);
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), seam);
    for (const std::string& fn : api)
      TEST_ASSERT_TRUE_MESSAGE(definesFunction(src, fn), (fn + " is not defined in " + seam).c_str());
  }
  for (const std::string& fn : api)
    TEST_ASSERT_FALSE_MESSAGE(definesFunction(common, fn), (fn + " is defined in the common half").c_str());
}

// engine_tools.h's hooks (the tool families an engine registers): each engine's tool file defines
// every one, so memory::begin and model_refresh::begin link on both engines.
static void test_every_tool_family_hook_is_defined_for_each_engine() {
  const std::set<std::string> hooks = declaredFunctions(code(kToolsApi));
  for (const char* named : {"registerMemoryFamilies", "registerModelsFamily"})
    TEST_ASSERT_TRUE_MESSAGE(hooks.count(named) == 1, named);
  for (const char* file : kToolFiles) {
    const std::string src = code(file);
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), file);
    for (const std::string& fn : hooks)
      TEST_ASSERT_TRUE_MESSAGE(definesFunction(src, fn), (fn + " is not defined in " + file).c_str());
  }
}

static void test_every_public_call_is_defined_in_the_common_half_only() {
  const std::string pub = code(kPublic);
  TEST_ASSERT_TRUE(pub.find("#include \"engine_api.h\"") != std::string::npos);   // callers see one API
  const std::set<std::string> own = declaredFunctions(pub);
  const std::set<std::string> api = declaredFunctions(code(kApi));
  TEST_ASSERT_TRUE(own.size() >= 30);
  const std::string common = code(kCommon);
  for (const std::string& fn : own) {
    TEST_ASSERT_TRUE_MESSAGE(api.count(fn) == 0, (fn + " is declared in both headers").c_str());
    TEST_ASSERT_TRUE_MESSAGE(definesFunction(common, fn), (fn + " is not defined in " + kCommon).c_str());
    for (const char* seam : kSeams)
      TEST_ASSERT_FALSE_MESSAGE(definesFunction(code(seam), fn), (fn + " is defined in " + seam).c_str());
  }
}

static void test_only_the_engine_seam_includes_the_old_engine() {
  std::vector<std::string> includers;
  for (const auto& e : std::filesystem::recursive_directory_iterator("src")) {
    if (!e.is_regular_file()) continue;
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h") continue;
    const std::string path = e.path().generic_string();
    if (includesEngineHeader(stripComments(readFile(path)))) includers.push_back(path);
  }
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, includers.size(), includers.empty() ? "none" : includers[0].c_str());
  TEST_ASSERT_EQUAL_STRING(kEngine1, includers[0].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_declaration_scanner_reads_namespace_functions_only);
  RUN_TEST(test_the_definition_scanner_finds_bodies_not_calls_or_declarations);
  RUN_TEST(test_the_include_scanner_ignores_comments);
  RUN_TEST(test_every_engine_call_is_defined_in_the_engine_seam_only);
  RUN_TEST(test_every_tool_family_hook_is_defined_for_each_engine);
  RUN_TEST(test_every_public_call_is_defined_in_the_common_half_only);
  RUN_TEST(test_only_the_engine_seam_includes_the_old_engine);
  return UNITY_END();
}
