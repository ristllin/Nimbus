// test_file_card_io - engine 2's file tools over the old engine's file store (lib/core file_card_io.h; lane
// E2-TOOLS), run as the device runs them, on a fake card with FAT's rule that a rename never overwrites.
//   - ordinary behavior through the registry with the manifest's own declarations: write, read (17 KB in one
//     call), append a CSV row and read it back with a column total, edit one line, list, delete;
//   - member isolation: a member sees, reads, lists, writes and deletes only its own files, hostile paths
//     included (.., absolute, the card's own names, a list cursor naming the owner's file);
//   - the storage rule (ruling 112): a file longer on the card than its index entry is appended to at its real
//     end and loses nothing; a read and an edit see the whole stored file; a file on the card with no index
//     entry is never written over or appended to; nothing is ever cut (the fake card has no truncate);
//   - a short append keeps what landed in an existing file, and removes a file the call itself created;
//   - a replace uses the old engine's swap paths and leaves no backup, note or part behind.
#include <unity.h>

#include <ArduinoJson.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema - the manifest the device registers with
#include "nimbus/orch/file_card_io.h"
#include "nimbus/orch/tool_registry.h"

namespace orch = nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

// A card: files by absolute path, directories, FAT's rename (never over an existing file).
struct FakeCard : orch::FileCard {
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  long shortAppendAt = -1;   // the next append lands only this many bytes
  int ops = 0;

  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool rename(const std::string& a, const std::string& b) override {
    ++ops;
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override {
    ++ops;
    return files.erase(p) > 0;
  }
  bool writeText(const std::string& p, const std::string& t) override {
    ++ops;
    files[p] = t;
    return true;
  }
  std::string readText(const std::string& p) override { return files.count(p) ? files[p] : std::string(); }
  long long size(const std::string& p) override { return files.count(p) ? (long long)files[p].size() : -1; }
  bool readAt(const std::string& p, uint64_t offset, size_t len, std::string& out) override {
    out.clear();
    if (!files.count(p)) return false;
    const std::string& f = files[p];
    if (offset < f.size()) out = f.substr(size_t(offset), len);
    return true;
  }
  bool append(const std::string& p, std::string_view bytes, size_t& landed) override {
    ++ops;
    landed = shortAppendAt >= 0 && size_t(shortAppendAt) < bytes.size() ? size_t(shortAppendAt) : bytes.size();
    shortAppendAt = -1;
    files[p].append(bytes.data(), landed);
    return true;
  }
  bool write(const std::string& p, std::string_view bytes, bool first) override {
    ++ops;
    if (first) files[p].clear();
    files[p].append(bytes.data(), bytes.size());
    return true;
  }
  bool mkdir(const std::string& d) override {
    dirs.insert(d);
    return true;
  }
};

const char* const kRoot = "/mem/files";

orch::CardFilesLayout layout() {
  return {kRoot, "/mem/files/.tools-part", {"/mem/files/.tools-part", "/mem/files/.bak", "/mem/files/.swapnote"}};
}

const orch::Principal& owner() {
  static const orch::Principal p = orch::principalForRole("1001", orch::Role::Admin);
  return p;
}
const orch::Principal& member() {
  static const orch::Principal p = orch::principalForRole("2002", orch::Role::User);
  return p;
}

// The device: one index (g_store), the card, the hooks files_subsystem.cpp gives, the tools as registered.
struct Device {
  FakeCard card;
  orch::FileStore store;
  int holds = 0, persists = 0;
  bool inHold = false;
  std::unique_ptr<orch::CardFilesIo> io;
  std::unique_ptr<orch::FileTools> tools;
  orch::ToolRegistry reg;

  Device() {
    orch::CardFilesHost h;
    h.locked = [this](const std::function<void()>& fn) {
      TEST_ASSERT_FALSE_MESSAGE(inHold, "a hold inside a hold");
      inHold = true;
      ++holds;
      fn();
      inHold = false;
      return true;
    };
    h.persistIndex = [this] {
      TEST_ASSERT_TRUE_MESSAGE(inHold, "the index written outside a hold");
      ++persists;
      card.files["/mem/files/.index"] = store.dump();
      return true;
    };
    h.now = [] { return uint32_t(1795000000u); };
    io.reset(new orch::CardFilesIo(card, store, layout(), h));
    tools.reset(new orch::FileTools(*io, orch::FileToolsConfig{}));
    tools->registerInto(reg, [](const char* t) {
      const auto* d = agent::plain::toolDecl(t);
      TEST_ASSERT_NOT_NULL_MESSAGE(d, t);
      return orch::FileToolDecl{d->description, agent::plain::declSchema(*d, false)};
    });
  }
  orch::ToolResult call(const char* tool, const ArduinoJson::JsonDocument& args, const orch::Principal& who) {
    return reg.dispatch(tool, args.as<ArduinoJson::JsonObjectConst>(), who);
  }
  orch::ToolResult call(const char* tool, std::initializer_list<std::pair<const char*, std::string>> kv,
                        const orch::Principal& who) {
    ArduinoJson::JsonDocument d;
    d.to<ArduinoJson::JsonObject>();
    for (const auto& p : kv) d[p.first] = p.second;
    return call(tool, d, who);
  }
  std::string onCard(const std::string& rel) { return card.files[std::string(kRoot) + "/" + rel]; }
  uint32_t indexed(const std::string& project, const std::string& name) {
    const orch::FileEntry* e = store.find(project, name);
    return e ? e->bytes : 0;
  }
};

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void assertOk(const orch::ToolResult& r) { TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str()); }

// ---- ordinary behavior ----------------------------------------------------------------

void test_write_read_append_edit_list_delete() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/meals.csv"}, {"content", "date,item,kcal\n2026-11-22,soup,210\n"}},
                  owner()));
  assertOk(d.call("append_file", {{"path", "logs/meals.csv"}, {"text", "2026-11-22,bread,180\n"}}, owner()));
  TEST_ASSERT_EQUAL_STRING("date,item,kcal\n2026-11-22,soup,210\n2026-11-22,bread,180\n", d.onCard("logs/meals.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(d.onCard("logs/meals.csv").size(), d.indexed("logs", "meals.csv"));
  const orch::ToolResult whole = d.call("read_file", {{"path", "logs/meals.csv"}}, owner());
  assertOk(whole);
  TEST_ASSERT_TRUE(has(whole.output, "2026-11-22,bread,180"));
  ArduinoJson::JsonDocument q;
  q["path"] = "logs/meals.csv";
  q["column"] = "date";
  q["from"] = "2026-11-22";
  q["to"] = "2026-11-22";
  const orch::ToolResult rows = d.call("read_file", q, owner());
  assertOk(rows);
  TEST_ASSERT_TRUE_MESSAGE(has(rows.output, "2026-11-22,bread,180"), rows.output.c_str());   // the rows read back
  ArduinoJson::JsonDocument e;
  e["path"] = "logs/meals.csv";
  e["old"] = "soup,210";
  e["new"] = "soup,230";
  assertOk(d.call("edit_file", e, owner()));
  TEST_ASSERT_EQUAL_STRING("date,item,kcal\n2026-11-22,soup,230\n2026-11-22,bread,180\n", d.onCard("logs/meals.csv").c_str());
  assertOk(d.call("write_file", {{"path", "notes/b.md"}, {"content", "b\n"}}, owner()));
  const orch::ToolResult l = d.call("list_files", {}, owner());
  assertOk(l);
  TEST_ASSERT_TRUE(has(l.output, "logs/meals.csv"));
  TEST_ASSERT_TRUE(has(l.output, "notes/b.md"));
  const orch::ToolResult del = d.call("delete_file", {{"path", "notes/b.md"}}, owner());
  assertOk(del);
  TEST_ASSERT_TRUE_MESSAGE(has(del.output, "deleted notes/b.md (2 B)"), del.output.c_str());
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/notes/b.md"));
  TEST_ASSERT_NULL(d.store.find("notes", "b.md"));
  TEST_ASSERT_FALSE(d.call("read_file", {{"path", "notes/b.md"}}, owner()).success);
  TEST_ASSERT_FALSE(d.call("delete_file", {{"path", "notes/b.md"}}, owner()).success);
  // Nothing of the swap or the part is left on the card.
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.tools-part"));
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.bak"));
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.swapnote"));
}

void test_a_17_KB_file_reads_in_one_call() {
  Device d;
  std::string big;
  for (int i = 0; big.size() < 17 * 1024; i++) big += "row " + std::to_string(i) + " of the long file\n";
  // Two calls write it (the write cap is 12,000 B): a write, then an append.
  const size_t cut = big.find('\n', 9000) + 1;   // at a line end, so the append adds no fence
  assertOk(d.call("write_file", {{"path", "docs/long.txt"}, {"content", big.substr(0, cut)}}, owner()));
  assertOk(d.call("append_file", {{"path", "docs/long.txt"}, {"text", big.substr(cut)}}, owner()));
  TEST_ASSERT_EQUAL_STRING(big.c_str(), d.onCard("docs/long.txt").c_str());
  const orch::ToolResult r = d.call("read_file", {{"path", "docs/long.txt"}}, owner());
  assertOk(r);
  TEST_ASSERT_FALSE_MESSAGE(has(r.output, "next_offset"), "a 17 KB file is read whole in one call");
  TEST_ASSERT_TRUE(has(r.output, big.substr(big.size() - 40)));
}

void test_the_tools_are_the_daemon_names_and_arguments() {
  // The names and arguments nimbusd's binding registers (hosted_file_tools.h) plus delete_file (path).
  const char* const names[] = {"read_file", "write_file", "append_file", "edit_file", "list_files", "delete_file"};
  Device d;
  for (const char* n : names) TEST_ASSERT_TRUE_MESSAGE(d.reg.has(n), n);
  TEST_ASSERT_EQUAL(1, (int)orch::fileToolArgs("delete_file").size());
  TEST_ASSERT_EQUAL_STRING("path", orch::fileToolArgs("delete_file")[0]);
  const orch::ToolResult r = d.call("delete_file", {{"path", "a/b.md"}, {"force", "yes"}}, owner());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE_MESSAGE(has(r.error, "path"), r.error.c_str());
}

// ---- member isolation ----------------------------------------------------------------

void test_a_member_sees_only_its_own_files() {
  Device d;
  assertOk(d.call("write_file", {{"path", "private/diary.md"}, {"content", "owner text\n"}}, owner()));
  assertOk(d.call("write_file", {{"path", "mine/list.md"}, {"content", "member text\n"}}, member()));
  // Reads and lists.
  TEST_ASSERT_FALSE(d.call("read_file", {{"path", "private/diary.md"}}, member()).success);
  const orch::ToolResult l = d.call("list_files", {}, member());
  assertOk(l);
  TEST_ASSERT_FALSE_MESSAGE(has(l.output, "private"), l.output.c_str());
  TEST_ASSERT_TRUE(has(l.output, "mine/list.md"));
  TEST_ASSERT_FALSE(has(d.call("list_files", {{"prefix", "private"}}, member()).output, "diary"));
  TEST_ASSERT_FALSE(d.call("list_files", {{"after", "private/diary.md"}}, member()).success);
  // Writes: replace, append, edit, delete of the owner's file are refused and change nothing.
  ArduinoJson::JsonDocument w;
  w["path"] = "private/diary.md";
  w["content"] = "taken";
  w["if_exists"] = "replace";
  TEST_ASSERT_FALSE(d.call("write_file", w, member()).success);
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "private/diary.md"}, {"text", "more\n"}}, member()).success);
  ArduinoJson::JsonDocument e;
  e["path"] = "private/diary.md";
  e["old"] = "owner";
  e["new"] = "member";
  TEST_ASSERT_FALSE(d.call("edit_file", e, member()).success);
  TEST_ASSERT_FALSE(d.call("delete_file", {{"path", "private/diary.md"}}, member()).success);
  TEST_ASSERT_EQUAL_STRING("owner text\n", d.onCard("private/diary.md").c_str());
  TEST_ASSERT_TRUE(d.store.find("private", "diary.md") != nullptr);
  // Hostile paths: none reaches a file, the card's own names or outside the store.
  const char* const hostile[] = {"../private/diary.md", "/mem/files/private/diary.md", "private/../diary.md",
                                 "private/diary.md/", "./private/diary.md", ".index", "x/../../.swapnote",
                                 "private//diary.md", ""};
  for (const char* p : hostile) {
    const std::string before = d.onCard("private/diary.md");
    TEST_ASSERT_FALSE_MESSAGE(d.call("read_file", {{"path", p}}, member()).success, p);
    TEST_ASSERT_FALSE_MESSAGE(d.call("delete_file", {{"path", p}}, member()).success, p);
    ArduinoJson::JsonDocument h;
    h["path"] = p;
    h["content"] = "x";
    h["if_exists"] = "replace";
    TEST_ASSERT_FALSE_MESSAGE(d.call("write_file", h, member()).success, p);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), d.onCard("private/diary.md").c_str());
  }
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.index") && d.card.files["/mem/files/.index"] == "x");
  // The member's own file is its to change and delete.
  assertOk(d.call("append_file", {{"path", "mine/list.md"}, {"text", "two\n"}}, member()));
  assertOk(d.call("delete_file", {{"path", "mine/list.md"}}, member()));
  TEST_ASSERT_NULL(d.store.find("mine", "list.md"));
}

// ---- the storage rule (ruling 112) ------------------------------------------------------

// Another writer (the web panel, an index rewrite that did not land) left the card longer than the index.
void behind(Device& d, const std::string& rel, const std::string& extra) {
  d.card.files[std::string(kRoot) + "/" + rel] += extra;
}

void test_an_append_lands_at_the_real_end_and_loses_nothing() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/a.csv"}, {"content", "h\n1\n"}}, owner()));
  behind(d, "logs/a.csv", "2\n3\n");
  TEST_ASSERT_EQUAL_UINT32(4, d.indexed("logs", "a.csv"));
  const orch::ToolResult r = d.call("append_file", {{"path", "logs/a.csv"}, {"text", "4\n"}}, owner());
  assertOk(r);
  TEST_ASSERT_EQUAL_STRING("h\n1\n2\n3\n4\n", d.onCard("logs/a.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(10, d.indexed("logs", "a.csv"));
  TEST_ASSERT_TRUE_MESSAGE(has(r.output, "file is 10 B"), r.output.c_str());
  // The index on the card follows.
  orch::FileStore reload;
  TEST_ASSERT_TRUE(reload.load(d.card.files["/mem/files/.index"]));
  TEST_ASSERT_EQUAL_UINT32(10, reload.find("logs", "a.csv")->bytes);
}

void test_a_torn_tail_gets_a_line_end_first_not_a_cut() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/b.csv"}, {"content", "h\n1\n"}}, owner()));
  behind(d, "logs/b.csv", "2,par");   // an append a cut stopped mid-row: its bytes stay
  assertOk(d.call("append_file", {{"path", "logs/b.csv"}, {"text", "3\n"}}, owner()));
  TEST_ASSERT_EQUAL_STRING("h\n1\n2,par\n3\n", d.onCard("logs/b.csv").c_str());
}

void test_read_and_edit_see_every_stored_byte() {
  Device d;
  assertOk(d.call("write_file", {{"path", "notes/c.md"}, {"content", "one\n"}}, owner()));
  behind(d, "notes/c.md", "two\nthree\n");
  const orch::ToolResult r = d.call("read_file", {{"path", "notes/c.md"}}, owner());
  assertOk(r);
  TEST_ASSERT_TRUE_MESSAGE(has(r.output, "three"), r.output.c_str());
  ArduinoJson::JsonDocument e;
  e["path"] = "notes/c.md";
  e["old"] = "one";
  e["new"] = "ONE";
  assertOk(d.call("edit_file", e, owner()));
  TEST_ASSERT_EQUAL_STRING("ONE\ntwo\nthree\n", d.onCard("notes/c.md").c_str());
  TEST_ASSERT_EQUAL_UINT32(14, d.indexed("notes", "c.md"));
}

void test_a_file_the_index_does_not_know_is_never_written() {
  Device d;
  d.card.files["/mem/files/old/kept.md"] = "stored before the index knew it\n";
  const std::string before = d.onCard("old/kept.md");
  TEST_ASSERT_FALSE(d.call("write_file", {{"path", "old/kept.md"}, {"content", "new"}}, owner()).success);
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "old/kept.md"}, {"text", "x\n"}}, member()).success);
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "old/kept.md"}, {"text", "x\n"}}, owner()).success);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), d.onCard("old/kept.md").c_str());
  TEST_ASSERT_NULL_MESSAGE(d.store.find("old", "kept.md"), "never adopted for whoever wrote first");
}

void test_a_short_append_keeps_what_landed_and_removes_only_its_own_new_file() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/d.csv"}, {"content", "h\n"}}, owner()));
  d.card.shortAppendAt = 2;
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "logs/d.csv"}, {"text", "1234\n"}}, owner()).success);
  TEST_ASSERT_EQUAL_STRING("h\n12", d.onCard("logs/d.csv").c_str());   // nothing stored is cut
  assertOk(d.call("read_file", {{"path", "logs/d.csv"}}, owner()));
  TEST_ASSERT_EQUAL_UINT32(4, d.indexed("logs", "d.csv"));             // the index follows the card
  d.card.shortAppendAt = 1;
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "logs/new.csv"}, {"text", "abc\n"}}, owner()).success);
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/logs/new.csv"));
  TEST_ASSERT_NULL(d.store.find("logs", "new.csv"));
}

void test_a_pending_replace_blocks_every_write_to_its_file() {
  Device d;
  assertOk(d.call("write_file", {{"path", "notes/e.md"}, {"content", "e\n"}}, owner()));
  // The old engine's swap note names the file (a double fault left its backup): nothing writes it until boot.
  d.card.files["/mem/files/.swapnote"] = "/mem/files/notes/e.md";
  d.card.files["/mem/files/.bak"] = "older e\n";
  d.card.files.erase("/mem/files/notes/e.md");
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "notes/e.md"}, {"text", "x\n"}}, owner()).success);
  TEST_ASSERT_FALSE(d.call("delete_file", {{"path", "notes/e.md"}}, owner()).success);
  TEST_ASSERT_EQUAL_STRING("older e\n", d.card.files["/mem/files/.bak"].c_str());
}

void test_a_replace_is_the_old_engines_swap() {
  Device d;
  assertOk(d.call("write_file", {{"path", "notes/f.md"}, {"content", "first\n"}}, owner()));
  ArduinoJson::JsonDocument w;
  w["path"] = "notes/f.md";
  w["content"] = "second\n";
  w["if_exists"] = "replace";
  assertOk(d.call("write_file", w, owner()));
  TEST_ASSERT_EQUAL_STRING("second\n", d.onCard("notes/f.md").c_str());
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.bak"));
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.swapnote"));
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.tools-part"));
  // A cut after the backup was taken and before the new copy landed: the old engine's boot recovery
  // (recoverSwap over the same note and backup) puts the old copy back.
  d.card.files["/mem/files/.swapnote"] = "/mem/files/notes/f.md";
  d.card.files["/mem/files/.bak"] = d.onCard("notes/f.md");
  d.card.files.erase("/mem/files/notes/f.md");
  orch::recoverSwap(d.card, layout().swap);
  TEST_ASSERT_EQUAL_STRING("second\n", d.onCard("notes/f.md").c_str());
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/.swapnote"));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_write_read_append_edit_list_delete);
  RUN_TEST(test_a_17_KB_file_reads_in_one_call);
  RUN_TEST(test_the_tools_are_the_daemon_names_and_arguments);
  RUN_TEST(test_a_member_sees_only_its_own_files);
  RUN_TEST(test_an_append_lands_at_the_real_end_and_loses_nothing);
  RUN_TEST(test_a_torn_tail_gets_a_line_end_first_not_a_cut);
  RUN_TEST(test_read_and_edit_see_every_stored_byte);
  RUN_TEST(test_a_file_the_index_does_not_know_is_never_written);
  RUN_TEST(test_a_short_append_keeps_what_landed_and_removes_only_its_own_new_file);
  RUN_TEST(test_a_pending_replace_blocks_every_write_to_its_file);
  RUN_TEST(test_a_replace_is_the_old_engines_swap);
  return UNITY_END();
}
