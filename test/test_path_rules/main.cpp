// The engine-2 file tools' path rule and reserved paths (path_rules.h).
//
// Class rules proven here, each over a generated class rather than one instance:
//   - no path escapes the store: for every hostile path (traversal, absolute paths, long
//     names, non-ASCII, empty segments, control bytes, broken UTF-8, FAT-refused
//     characters, and 20,000 generated mixes of them) the result is an error or exactly
//     two segments the file store accepts, with no '.' or '..' component, so the card path
//     /mem/files/<project>/<name> stays inside /mem/files;
//   - a valid path is never changed, and making a path valid twice changes nothing;
//   - two different long names never become one stored name;
//   - every reserved family is matched in every letter case, and nothing outside it is.
#include <unity.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/file_store.h"
#include "nimbus/orch/path_rules.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

// The FAT-refused printable characters (beyond the store's own '/', '\\', ':').
bool fatSafe(const std::string& s) {
  for (char c : s)
    if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  return s.empty() || s.back() != '.';
}

// The card path the device builds, resolved the way a file system would.
bool staysInStore(const StorePath& p) {
  const std::string abs = "/mem/files/" + p.project + "/" + p.name;
  std::vector<std::string> parts;
  size_t i = 1;
  while (i <= abs.size()) {
    const size_t k = abs.find('/', i);
    const std::string seg = abs.substr(i, k == std::string::npos ? std::string::npos : k - i);
    if (seg == "..") {
      if (parts.empty()) return false;
      parts.pop_back();
    } else if (!seg.empty() && seg != ".") {
      parts.push_back(seg);
    }
    if (k == std::string::npos) break;
    i = k + 1;
  }
  return parts.size() == 4 && parts[0] == "mem" && parts[1] == "files";
}

void checkOne(const std::string& asked, int& errors, int& stored) {
  const StorePath p = storePathFor(asked);
  if (!p.ok()) {
    TEST_ASSERT_TRUE_MESSAGE(p.project.empty() && p.name.empty(), asked.c_str());
    errors++;
    return;
  }
  stored++;
  const std::string msg = "asked: " + asked + " stored: " + p.str();
  TEST_ASSERT_TRUE_MESSAGE(FileStore::validSegment(p.project, kStoreProjectMax), msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(FileStore::validSegment(p.name, kStoreNameMax), msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(fatSafe(p.project) && fatSafe(p.name), msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(staysInStore(p), msg.c_str());
  // Making valid twice changes nothing.
  const StorePath again = storePathFor(p.str());
  TEST_ASSERT_TRUE_MESSAGE(again.ok() && !again.changed && again.str() == p.str(), msg.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(p.str() != asked, p.changed, msg.c_str());
}

const std::vector<std::string> kHostile = {
    "", "/", "//", "///", "a", "a/", "/a", "a//b", "a/b/", "/a/b", "//a/b", "a/b/c", "a/b/c/d",
    ".", "..", "./a", "../a", "a/.", "a/..", "./a/b", "../a/b", "a/../b", "a/./b", "../../etc/passwd",
    "/etc/passwd", "/mem/files/notes/a.md", "notes/../../secret", "notes/..", "../notes/a.md",
    "a\\..\\b", "..\\..\\x/y", "notes/..\\..\\x", "C:/x", "C:\\x/y", "notes/a:b", "notes/a*?\"<>|b",
    "notes/.hidden", ".git/config", "notes/...", "notes/a.", "notes/a. .", "notes/ ", " /a", "notes/   ",
    "notes/\t", "notes/a\nb", "notes/a\rb", std::string("notes/a\0b", 9), "notes/\x7f",
    "ruoka/p\xc3\xa4iv\xc3\xa4.md", "\xc3\xa5\xc3\xa4\xc3\xb6/\xc3\x85\xc3\x84\xc3\x96.txt", "notes/stra\xc3\x9f" "e.md",
    "notes/\xe2\x80\xae" "gnp.exe", "notes/\xf0\x9f\x98\x80.md", "\xe6\x97\xa5\xe8\xae\xb0/\xe4\xbb\x8a\xe5\xa4\xa9.md",
    "notes/\xff\xfe", "notes/\xc3", "notes/\xe2\x80", "notes/\xc0\xaf..", "notes/%2e%2e%2f", "%2e%2e/x",
    "notes/" + std::string(300, 'a'), std::string(300, 'p') + "/x", "notes/" + std::string(200, 'a') + ".markdown",
    "notes/" + std::string(60, 'b') + ".csv", "notes/" + std::string(47, 'c') + ".md",
    "notes/" + std::string(100, '.') , "notes/" + std::string(100, '/'), std::string(5000, '/'),
    "notes/" + std::string(80, '\xe4'), "notes/a b c.md", "my notes/shopping list.md", "food/food-log.csv",
};

}  // namespace

static void test_no_hostile_path_escapes_the_store() {
  int errors = 0, stored = 0;
  for (const std::string& s : kHostile) checkOne(s, errors, stored);
  // Generated mixes: segments drawn from hostile pieces, joined by one to four slashes.
  const std::vector<std::string> pieces = {"..", ".", "", "a", "/", "\\", ":", " ", "\xc3\xa4", "\xff",
                                           "\x01", "*", "notes", std::string(30, 'z'), "x.md", "%2e"};
  uint32_t seed = 12345;
  auto next = [&seed] {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
  };
  for (int n = 0; n < 20000; n++) {
    std::string s;
    const int k = 1 + int(next() % 7);
    for (int j = 0; j < k; j++) s += pieces[next() % pieces.size()];
    checkOne(s, errors, stored);
  }
  TEST_ASSERT_TRUE(errors > 1000 && stored > 1000);   // both outcomes are exercised
  TEST_MESSAGE(("property: " + std::to_string(errors + stored) + " hostile paths, " + std::to_string(stored) +
                " stored inside the store, " + std::to_string(errors) + " named as errors")
                   .c_str());
}

static void test_errors_name_the_form() {
  TEST_ASSERT_EQUAL(int(PathError::Empty), int(storePathFor("").error));
  TEST_ASSERT_EQUAL(int(PathError::Absolute), int(storePathFor("/etc/passwd").error));
  TEST_ASSERT_EQUAL(int(PathError::Absolute), int(storePathFor("/mem/files/notes/a.md").error));
  for (const char* s : {"a", "a/b/c", "a//b", "a/", "/"})
    TEST_ASSERT_TRUE_MESSAGE(storePathFor(s).error == PathError::NotTwoParts || storePathFor(s).error == PathError::Absolute, s);
  for (const char* s : {"../a", "a/..", "a/../b", "./a/b", ".", "..", "a/."})
    TEST_ASSERT_EQUAL_MESSAGE(int(PathError::DotSegment), int(storePathFor(s).error), s);
}

static void test_valid_paths_are_never_changed() {
  for (const char* s : {"notes/a.md", "food/food-log.csv", "A-b_c/D.E.F", "x/y", "p/!#$%&'()+,-;=@[]^`{}~",
                        "projectname-is-24-chars-/name-of-exactly-forty-eight-characters-ok.md"}) {
    const StorePath p = storePathFor(s);
    TEST_ASSERT_TRUE_MESSAGE(p.ok(), s);
    TEST_ASSERT_FALSE_MESSAGE(p.changed, s);
    TEST_ASSERT_EQUAL_STRING(s, p.str().c_str());
  }
}

static void test_names_are_made_valid_one_for_one() {
  TEST_ASSERT_EQUAL_STRING("my-notes/shopping-list.md", storePathFor("my notes/shopping list.md").str().c_str());
  TEST_ASSERT_EQUAL_STRING("ruoka/paiva.md", storePathFor("ruoka/p\xc3\xa4iv\xc3\xa4.md").str().c_str());
  TEST_ASSERT_EQUAL_STRING("notes/strasse.md", storePathFor("notes/stra\xc3\x9f" "e.md").str().c_str());
  TEST_ASSERT_EQUAL_STRING("notes/-.md", storePathFor("notes/\xf0\x9f\x98\x80.md").str().c_str());
  TEST_ASSERT_EQUAL_STRING("notes/_hidden", storePathFor("notes/.hidden").str().c_str());
  TEST_ASSERT_EQUAL_STRING("notes/a", storePathFor("notes/a.").str().c_str());
  TEST_ASSERT_EQUAL_STRING("notes/a-b", storePathFor("notes/a:b").str().c_str());
  TEST_ASSERT_TRUE(storePathFor("notes/a.").changed);
  // A name that is all dots after the mapping has nothing left... except the leading '_'.
  TEST_ASSERT_EQUAL_STRING("notes/_", storePathFor("notes/...").str().c_str());
}

static void test_long_names_keep_the_extension_and_stay_distinct() {
  std::set<std::string> seen;
  for (int i = 0; i < 500; i++) {
    const std::string asked = "notes/" + std::string(60, 'a') + std::to_string(i) + ".csv";
    const StorePath p = storePathFor(asked);
    TEST_ASSERT_TRUE(p.ok() && p.changed);
    TEST_ASSERT_EQUAL(int(kStoreNameMax), int(p.name.size()));
    TEST_ASSERT_EQUAL_STRING(".csv", p.name.substr(p.name.size() - 4).c_str());
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(p.name).second, p.name.c_str());
  }
  const StorePath proj = storePathFor(std::string(40, 'p') + "/x.md");
  TEST_ASSERT_TRUE(proj.ok());
  TEST_ASSERT_EQUAL(int(kStoreProjectMax), int(proj.project.size()));
  // An extension too long to keep is cut with the stem.
  const StorePath ext = storePathFor("notes/a." + std::string(60, 'e'));
  TEST_ASSERT_TRUE(ext.ok());
  TEST_ASSERT_EQUAL(int(kStoreNameMax), int(ext.name.size()));
}

namespace {
std::string withCase(const std::string& s, uint32_t mask) {
  std::string out = s;
  for (size_t i = 0; i < out.size() && i < 32; i++)
    if ((mask >> i) & 1u) out[i] = char(toupper((unsigned char)out[i]));
  return out;
}
}  // namespace

static void test_reserved_families_in_every_case() {
  struct Row {
    const char* project;
    const char* name;
    Reserved kind;
    const char* owner;
  };
  const Row rows[] = {
      {"skills", "food-coach.md", Reserved::Skill, "skill_save"},
      {"skills", "notes.txt", Reserved::Skill, "skill_save"},
      {"food", "food-log.csv", Reserved::Ledger, "ledger_write"},
      {"food", "food-log-12345.csv", Reserved::Ledger, "ledger_write"},
      {"food", "food-logs.csv", Reserved::Ledger, "ledger_write"},
      {"food-pre-v2", "food-log.csv", Reserved::Archive, ""},
      {"export-pre-v2", "anything.md", Reserved::Archive, ""},
      {"inbox", "1234-report.pdf", Reserved::Inbox, ""},
      {"agents", "research-a1.md", Reserved::Agents, "agent_start"},
  };
  int checked = 0;
  for (const Row& r : rows) {
    for (uint32_t mask : {0u, 1u, 0xffffffffu, 0x55555555u, 0xaaaaaaaau}) {
      const std::string p = withCase(r.project, mask), n = withCase(r.name, mask >> 3);
      const PathRule rule = pathRuleFor(p, n);
      const std::string msg = p + "/" + n;
      TEST_ASSERT_EQUAL_MESSAGE(int(r.kind), int(rule.kind), msg.c_str());
      TEST_ASSERT_FALSE_MESSAGE(rule.writable, msg.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(r.owner, rule.owner, msg.c_str());
      checked++;
    }
  }
  // The made-valid path is what is matched: "skills /x.md" is not the skill store.
  TEST_ASSERT_EQUAL(int(Reserved::None), int(pathRuleFor(storePathFor("skills /x.md").project, "x.md").kind));
  TEST_MESSAGE(("property: " + std::to_string(checked) + " reserved paths in mixed case").c_str());
}

static void test_lookalikes_are_not_reserved() {
  const char* const paths[][2] = {
      {"food", "food-targets.csv"}, {"food", "food-log.txt"}, {"food", "my-food-log.csv"}, {"foods", "food-log.csv"},
      {"skill", "x.md"},           {"skills-old", "x.md"},   {"inboxes", "x"},           {"agent", "x.md"},
      {"food-pre-v3", "x"},        {"notes", "skills.md"},   {"notes", "inbox"},         {"export", "x"},
  };
  for (const auto& p : paths) {
    const PathRule r = pathRuleFor(p[0], p[1]);
    TEST_ASSERT_EQUAL_MESSAGE(int(Reserved::None), int(r.kind), p[1]);
    TEST_ASSERT_TRUE(r.writable);
  }
}

static void test_skill_ids() {
  TEST_ASSERT_EQUAL_STRING("food-coach", pathRuleFor("skills", "food-coach.md").skillId.c_str());
  TEST_ASSERT_EQUAL_STRING("Food_2", pathRuleFor("SKILLS", "Food_2.MD").skillId.c_str());
  TEST_ASSERT_EQUAL_STRING("", pathRuleFor("skills", "notes.txt").skillId.c_str());
  TEST_ASSERT_EQUAL_STRING("", pathRuleFor("skills", "a.b.md").skillId.c_str());
  TEST_ASSERT_EQUAL_STRING("", pathRuleFor("skills", (std::string(24, 'x') + ".md").c_str()).skillId.c_str());
  TEST_ASSERT_EQUAL_STRING("", pathRuleFor("skills", ".md").skillId.c_str());
  TEST_ASSERT_TRUE(skillIdValid(std::string(23, 'x')));
  TEST_ASSERT_FALSE(skillIdValid(std::string(24, 'x')));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_hostile_path_escapes_the_store);
  RUN_TEST(test_errors_name_the_form);
  RUN_TEST(test_valid_paths_are_never_changed);
  RUN_TEST(test_names_are_made_valid_one_for_one);
  RUN_TEST(test_long_names_keep_the_extension_and_stay_distinct);
  RUN_TEST(test_reserved_families_in_every_case);
  RUN_TEST(test_lookalikes_are_not_reserved);
  RUN_TEST(test_skill_ids);
  return UNITY_END();
}
