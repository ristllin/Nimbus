// test_file_card_io - engine 2's file tools over the old engine's file store (lib/core file_card_io.h; lane
// E2-TOOLS), run as the device runs them, on a fake card with FAT's rule that a rename never overwrites.
//   - ordinary behavior through the registry with the manifest's own declarations: write, read (17 KB in one
//     call), append a CSV row and read it back with a column total, edit one line, list, delete;
//   - member isolation: a member sees, reads, lists, writes and deletes only its own files, hostile paths
//     included (.., absolute, the card's own names, a list cursor naming the owner's file);
//   - the storage rule (ruling 112): a file longer on the card than its index entry is appended to at its real
//     end and loses nothing; a read and an edit see the whole stored file; a file on the card with no index
//     entry is never written over or appended to; nothing is ever cut (the fake card has no truncate);
//   - a replace uses the old engine's swap paths and leaves no backup, note or part behind;
//   - lane E2-FSAFE: a short append is undone in its hold (the file byte-identical, the failure said) and a
//     failed cut-back says how many bytes landed (S2); a web upload in progress refuses every change (S4);
//     a power cut before every card operation of write, replace, append, edit and delete leaves the old file
//     or the new one with the index agreeing after the boot recovery (the swap's, then the journal note's;
//     S6, S8); an append whose file another writer replaced between its holds decides its fence again (S7).
#include <unity.h>

#include <ArduinoJson.h>

#include <functional>
#include <cstdio>
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

// The power going off before a card operation (thrown from the fake card; nothing after it runs).
struct PowerCut {};

// A card: files by absolute path, directories, FAT's rename (never over an existing file).
struct FakeCard : orch::FileCard {
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  long shortAppendAt = -1;   // the next append lands only this many bytes
  bool truncateFails = false;
  bool tornCut = false;      // a cut that falls on an append lands the first half of its bytes, then the power goes
  std::set<std::string> refuseRemove;      // paths whose remove the card refuses
  std::set<std::string> refuseOverwrite;   // paths whose writeText the card refuses while the file exists
  int ops = 0;
  int cutAt = -1;            // the power goes off before the changing operation with this number (1 = the first)

  void step() {
    if (++ops == cutAt) throw PowerCut{};
  }
  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool rename(const std::string& a, const std::string& b) override {
    step();
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override {
    if (!files.count(p) || refuseRemove.count(p)) return false;
    step();
    return files.erase(p) > 0;
  }
  bool writeText(const std::string& p, const std::string& t) override {
    if (refuseOverwrite.count(p) && files.count(p)) return false;
    step();
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
    if (tornCut && ops + 1 == cutAt) {   // the power goes mid-write: part of the row is on the card
      ++ops;
      files[p].append(bytes.data(), bytes.size() / 2);
      throw PowerCut{};
    }
    step();
    landed = shortAppendAt >= 0 && size_t(shortAppendAt) < bytes.size() ? size_t(shortAppendAt) : bytes.size();
    shortAppendAt = -1;
    files[p].append(bytes.data(), landed);
    return true;
  }
  bool write(const std::string& p, std::string_view bytes, bool first) override {
    step();
    if (first) files[p].clear();
    files[p].append(bytes.data(), bytes.size());
    return true;
  }
  bool mkdir(const std::string& d) override {
    dirs.insert(d);
    return true;
  }
  bool truncate(const std::string& p, uint64_t size) override {
    if (!files.count(p) || truncateFails) return false;
    step();
    files[p].resize(size_t(size));
    return true;
  }
};

const char* const kRoot = "/mem/files";

orch::CardFilesLayout layout() {
  return {kRoot, "/mem/files/.tools-part", {"/mem/files/.tools-part", "/mem/files/.bak", "/mem/files/.swapnote"},
          "/mem/files/.tools-note"};
}

const orch::Principal& owner() {
  static const orch::Principal p = orch::principalForRole("1001", orch::Role::Admin);
  return p;
}
const orch::Principal& member() {
  static const orch::Principal p = orch::principalForRole("2002", orch::Role::User);
  return p;
}

std::string projectOf(const std::string& path) { return path.substr(0, path.find('/')); }
std::string nameOf(const std::string& path) { return path.substr(path.find('/') + 1); }

// The device: one index (g_store), the card, the hooks files_subsystem.cpp gives, the tools as registered.
struct Device {
  FakeCard card;
  orch::FileStore store;
  int holds = 0, persists = 0;
  bool inHold = false, uploading = false;
  std::function<void(int)> afterHold;   // another writer between two holds
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
      if (afterHold) afterHold(holds);
      return true;
    };
    h.persistIndex = [this] {
      TEST_ASSERT_TRUE_MESSAGE(inHold || booting, "the index written outside a hold");
      card.step();   // the index rewrite is a card operation too (the device's tmp and rename: one here)
      ++persists;
      card.files["/mem/files/.index"] = store.dump();
      return true;
    };
    h.now = [] { return uint32_t(1795000000u); };
    h.skillBody = [](const std::string& id, std::string& out) {   // the device: skillsdev::skillBody (E2-CHAT)
      if (id != "bench-skill") return false;
      out = "Use the bench word.";
      return true;
    };
    h.busy = [this] { return uploading; };
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
  // The next boot over this card: the old engine's swap recovery, the index loaded from the card, then
  // the journal note's (files_subsystem.cpp begin, engine 2).
  // Ruling 115 item 1, asserted at every boot: the note's recovery leaves every owner file it found at least
  // as long, starting with the same bytes (at most one line end added), no file missing, and no entry it found
  // under another person.
  std::string bootFrom(const FakeCard& before) {
    card.files = before.files;
    card.dirs = before.dirs;
    orch::recoverSwap(card, layout().swap);
    const auto it = card.files.find("/mem/files/.index");
    if (it != card.files.end()) TEST_ASSERT_TRUE(store.load(it->second));
    const std::map<std::string, std::string> held = ownerFiles();
    std::map<std::string, std::string> whose;
    for (const orch::FileEntry* e : store.list()) whose[std::string(e->project) + "/" + std::string(e->name)] = e->owner;
    booting = true;
    const std::string did = io->recoverNote();
    booting = false;
    for (const auto& f : held) {
      const auto now = card.files.find(f.first);
      TEST_ASSERT_TRUE_MESSAGE(now != card.files.end(), ("the boot removed " + f.first).c_str());
      const bool same = now->second == f.second || now->second == f.second + "\n";
      TEST_ASSERT_TRUE_MESSAGE(same, ("the boot changed the stored bytes of " + f.first).c_str());
    }
    for (const auto& w : whose) {
      const orch::FileEntry* e = store.find(projectOf(w.first), nameOf(w.first));
      if (e) TEST_ASSERT_EQUAL_STRING_MESSAGE(w.second.c_str(), std::string(e->owner).c_str(), w.first.c_str());
    }
    return did;
  }
  // The owner's files on the card: <root>/<project>/<name> (the card's own files sit at the root).
  std::map<std::string, std::string> ownerFiles() const {
    std::map<std::string, std::string> out;
    const std::string root = std::string(kRoot) + "/";
    for (const auto& f : card.files) {
      if (f.first.rfind(root, 0) == 0 && f.first.find('/', root.size()) != std::string::npos) out.insert(f);
    }
    return out;
  }
  bool booting = false;
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

bool noTraces(Device& d) {
  return !d.card.files.count("/mem/files/.tools-note") && !d.card.files.count("/mem/files/.bak") &&
         !d.card.files.count("/mem/files/.swapnote");
}

// S2: the card took part of an append. The tool cuts the file back to the size it measured in the same
// hold (only its own bytes) and says so truthfully; a file the call created goes again.
void test_a_short_append_leaves_the_file_byte_identical_and_says_it_failed() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/d.csv"}, {"content", "h\n"}}, owner()));
  d.card.shortAppendAt = 2;
  const orch::ToolResult r = d.call("append_file", {{"path", "logs/d.csv"}, {"text", "1234\n"}}, owner());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("card write failed; nothing changed", r.error.c_str());
  TEST_ASSERT_EQUAL_STRING("h\n", d.onCard("logs/d.csv").c_str());   // byte-identical
  TEST_ASSERT_EQUAL_UINT32(2, d.indexed("logs", "d.csv"));
  TEST_ASSERT_TRUE(noTraces(d));
  // The retry lands one clean row, no half row before it.
  assertOk(d.call("append_file", {{"path", "logs/d.csv"}, {"text", "1234\n"}}, owner()));
  TEST_ASSERT_EQUAL_STRING("h\n1234\n", d.onCard("logs/d.csv").c_str());
  d.card.shortAppendAt = 1;
  TEST_ASSERT_FALSE(d.call("append_file", {{"path", "logs/new.csv"}, {"text", "abc\n"}}, owner()).success);
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/logs/new.csv"));
  TEST_ASSERT_NULL(d.store.find("logs", "new.csv"));
}

// S2: when the card refuses the cut back too, the result states how many bytes landed (never "nothing
// changed"); the bytes stay and the index follows the card (ruling 112: nothing else is cut). E2-INTEG:
// the line is the append's own (kAppended) as an error, since the class had no room for a new one.
void test_a_failed_cut_back_states_the_bytes_that_landed() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/d.csv"}, {"content", "h\n"}}, owner()));
  d.card.shortAppendAt = 2;
  d.card.truncateFails = true;
  const orch::ToolResult r = d.call("append_file", {{"path", "logs/d.csv"}, {"text", "1234\n"}}, owner());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL_STRING("appended 2 B to logs/d.csv; file is 4 B", r.error.c_str());
  TEST_ASSERT_EQUAL_STRING("h\n12", d.onCard("logs/d.csv").c_str());
  assertOk(d.call("read_file", {{"path", "logs/d.csv"}}, owner()));
  TEST_ASSERT_EQUAL_UINT32(4, d.indexed("logs", "d.csv"));
  TEST_ASSERT_TRUE(noTraces(d));
}

// S4: while a web upload is in progress no tool changes a file, as the old engine refuses its own writes.
// E2-INTEG: the refusal is the existing "card write failed; nothing changed" (no room for a new line).
void test_an_upload_in_progress_refuses_every_change() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/u.csv"}, {"content", "h\n1\n"}}, owner()));
  d.uploading = true;
  const char kBusy[] = "card write failed; nothing changed";
  ArduinoJson::JsonDocument w;
  w["path"] = "logs/u.csv";
  w["content"] = "x\n";
  w["if_exists"] = "replace";
  ArduinoJson::JsonDocument e;
  e["path"] = "logs/u.csv";
  e["old"] = "1";
  e["new"] = "2";
  const orch::ToolResult rs[] = {
      d.call("append_file", {{"path", "logs/u.csv"}, {"text", "2\n"}}, owner()), d.call("write_file", w, owner()),
      d.call("write_file", {{"path", "logs/v.csv"}, {"content", "v\n"}}, owner()), d.call("edit_file", e, owner()),
      d.call("delete_file", {{"path", "logs/u.csv"}}, owner())};
  for (const orch::ToolResult& r : rs) {
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_EQUAL_STRING(kBusy, r.error.c_str());
  }
  TEST_ASSERT_EQUAL_STRING("h\n1\n", d.onCard("logs/u.csv").c_str());
  TEST_ASSERT_FALSE(d.card.files.count("/mem/files/logs/v.csv"));
  assertOk(d.call("read_file", {{"path", "logs/u.csv"}}, owner()));   // reads go on
  d.uploading = false;
  assertOk(d.call("append_file", {{"path", "logs/u.csv"}, {"text", "2\n"}}, owner()));
}

// S6, S8: one changing call, and the power going off before each of its card operations in turn.
struct CutCase {
  const char* label;
  std::string path;               // project/name
  const orch::Principal* who;
  std::function<void(Device&)> setup;
  std::function<orch::ToolResult(Device&)> run;
};


// After the boot recovery: the file is the old copy or the new one (or absent where that was one of them),
// the index entry exists exactly when the file does, with its size, a hash that is right or unknown, and
// the writer as its owner; nothing of the journal or the swap is left; the name takes the next write.
struct OldNew {
  bool hadOld = false, hasNew = false;
  std::string old, now;
};

void checkOldOrNew(Device& b, const CutCase& c, const OldNew& x, int cut) {
  const bool hadOld = x.hadOld, hasNew = x.hasNew;
  const std::string& old = x.old;
  const std::string& now = x.now;
  char where[160];
  snprintf(where, sizeof where, "%s, power off before card op %d", c.label, cut);
  const std::string full = std::string(kRoot) + "/" + c.path;
  const bool present = b.card.files.count(full) > 0;
  const std::string bytes = present ? b.card.files[full] : std::string();
  const bool isOld = hadOld ? (present && bytes == old) : !present;
  const bool isNew = hasNew ? (present && bytes == now) : !present;
  TEST_ASSERT_TRUE_MESSAGE(isOld || isNew, (std::string(where) + ": on the card \"" + bytes + "\"").c_str());
  const orch::FileEntry* e = b.store.find(projectOf(c.path), nameOf(c.path));
  TEST_ASSERT_EQUAL_MESSAGE(present, e != nullptr, where);
  if (e) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(bytes.size(), e->bytes, where);
    TEST_ASSERT_TRUE_MESSAGE(e->hash == 0 || e->hash == orch::fnvContinue(orch::kFnvBasis, bytes), where);
    TEST_ASSERT_TRUE_MESSAGE(b.store.ownedBy(*e, c.who->ns, false), where);
  }
  TEST_ASSERT_TRUE_MESSAGE(noTraces(b), where);
  // The name is not blocked: the next ordinary call on it succeeds.
  const orch::ToolResult next = present ? b.call("append_file", {{"path", c.path}, {"text", "z\n"}}, *c.who)
                                        : b.call("write_file", {{"path", c.path}, {"content", "z\n"}}, *c.who);
  TEST_ASSERT_TRUE_MESSAGE(next.success, (std::string(where) + ": " + next.error).c_str());
}

void runCutCase(const CutCase& c) {
  const std::string full = std::string(kRoot) + "/" + c.path;
  Device ref;
  c.setup(ref);
  OldNew x;
  x.hadOld = ref.card.files.count(full) > 0;
  if (x.hadOld) x.old = ref.card.files[full];
  assertOk(c.run(ref));
  x.hasNew = ref.card.files.count(full) > 0;
  if (x.hasNew) x.now = ref.card.files[full];
  int cuts = 0;
  for (int k = 1; k < 64; k++) {
    Device d;
    c.setup(d);
    d.card.cutAt = d.card.ops + k;
    bool cutHappened = false;
    try {
      (void)c.run(d);
    } catch (const PowerCut&) {
      cutHappened = true;
    }
    if (!cutHappened) break;
    ++cuts;
    Device b;
    b.bootFrom(d.card);
    checkOldOrNew(b, c, x, k);
  }
  char line[96];
  snprintf(line, sizeof line, "%s: %d cut points", c.label, cuts);
  TEST_MESSAGE(line);
  TEST_ASSERT_TRUE_MESSAGE(cuts >= 3, line);
}

ArduinoJson::JsonDocument replaceArgs(const std::string& path, const std::string& content) {
  ArduinoJson::JsonDocument w;
  w["path"] = path;
  w["content"] = content;
  w["if_exists"] = "replace";
  return w;
}

void test_a_power_cut_at_every_card_operation_leaves_old_or_new_with_the_index_agreeing() {
  const auto seeded = [](const char* path, const char* content, const orch::Principal& who) {
    return [path, content, &who](Device& d) { assertOk(d.call("write_file", {{"path", path}, {"content", content}}, who)); };
  };
  const auto nothing = [](Device&) {};
  const std::vector<CutCase> cases = {
      {"write_file new", "notes/n.md", &owner(), nothing,
       [](Device& d) { return d.call("write_file", {{"path", "notes/n.md"}, {"content", "new note\n"}}, owner()); }},
      {"write_file new by a member", "mine/m.md", &member(), nothing,
       [](Device& d) { return d.call("write_file", {{"path", "mine/m.md"}, {"content", "member note\n"}}, member()); }},
      {"write_file replace", "notes/f.md", &owner(), seeded("notes/f.md", "first copy\n", owner()),
       [](Device& d) { return d.call("write_file", replaceArgs("notes/f.md", "the second, longer copy\n"), owner()); }},
      {"write_file replace, same size", "notes/g.md", &owner(), seeded("notes/g.md", "tea,3\n", owner()),
       [](Device& d) { return d.call("write_file", replaceArgs("notes/g.md", "tea,4\n"), owner()); }},
      {"append_file", "logs/a.csv", &owner(), seeded("logs/a.csv", "h\n1\n", owner()),
       [](Device& d) { return d.call("append_file", {{"path", "logs/a.csv"}, {"text", "2\n"}}, owner()); }},
      {"append_file creating the file", "logs/c.csv", &owner(), nothing,
       [](Device& d) { return d.call("append_file", {{"path", "logs/c.csv"}, {"text", "h\n"}}, owner()); }},
      {"edit_file, same size", "logs/e.csv", &owner(), seeded("logs/e.csv", "d,item\n1,tea,3\n", owner()),
       [](Device& d) {
         ArduinoJson::JsonDocument e;
         e["path"] = "logs/e.csv";
         e["old"] = "tea,3";
         e["new"] = "tea,4";
         return d.call("edit_file", e, owner());
       }},
      {"delete_file", "logs/x.csv", &owner(), seeded("logs/x.csv", "x\n", owner()),
       [](Device& d) { return d.call("delete_file", {{"path", "logs/x.csv"}}, owner()); }},
      {"delete_file by a member", "mine/y.md", &member(), seeded("mine/y.md", "y\n", member()),
       [](Device& d) { return d.call("delete_file", {{"path", "mine/y.md"}}, member()); }},
  };
  for (const CutCase& c : cases) runCutCase(c);
}

// S7: another writer replaces the file between the append's tail read and its commit (here: a copy whose
// last line has no newline). The append decides its fence again on the copy now stored: no glued row.
void test_an_append_decides_its_fence_on_the_file_as_it_is_at_the_commit() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/s.csv"}, {"content", "h\n1\n"}}, owner()));
  const int from = d.holds;
  d.afterHold = [&d, from](int held) {
    if (held != from + 2) return;   // after the tail read (hold 2 of the append)
    d.card.files["/mem/files/logs/s.csv"] = "h\n1\nx";   // the other writer's copy, indexed as it writes it
    orch::FileEntry e = *d.store.find("logs", "s.csv");
    e.bytes = 5;
    e.createdAt = 1795000100u;
    e.hash = orch::fnvContinue(orch::kFnvBasis, "h\n1\nx");
    std::string err;
    TEST_ASSERT_TRUE(d.store.add(e, err));
  };
  const orch::ToolResult r = d.call("append_file", {{"path", "logs/s.csv"}, {"text", "2\n"}}, owner());
  assertOk(r);
  TEST_ASSERT_EQUAL_STRING("h\n1\nx\n2\n", d.onCard("logs/s.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(8, d.indexed("logs", "s.csv"));
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


// ---- ruling 115: the boot recovery never removes, cuts or re-assigns a stored byte ----------

const char* const kNote = "/mem/files/.tools-note";

std::string noteText(char op, const std::string& person, const std::string& rel, long long before,
                     unsigned long long bytes) {
  return std::string(1, op) + "\n" + person + "\n" + projectOf(rel) + "\n" + nameOf(rel) + "\n" +
         std::to_string(before) + "\n" + std::to_string(bytes) + "\n";
}

// Another writer that keeps the index (the web panel, v5.2.0): the file and its entry at the new size.
void indexedWrite(Device& d, const std::string& rel, const std::string& bytes) {
  d.card.files[std::string(kRoot) + "/" + rel] = bytes;
  orch::FileEntry e = *d.store.find(projectOf(rel), nameOf(rel));
  e.bytes = uint32_t(bytes.size());
  e.hash = orch::fnvContinue(orch::kFnvBasis, bytes);
  std::string err;
  TEST_ASSERT_TRUE(d.store.add(e, err));
  d.card.files["/mem/files/.index"] = d.store.dump();
}

// P1: an append completed and was reported; its note stayed (the card refused its removal). The next boot
// leaves the row, and the file an append created stays with its entry.
void test_a_stale_append_note_after_a_completed_append_leaves_the_row() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/a.csv"}, {"content", "h\n1\n"}}, owner()));
  assertOk(d.call("append_file", {{"path", "logs/a.csv"}, {"text", "2\n"}}, owner()));
  d.card.files[kNote] = noteText('a', owner().ns, "logs/a.csv", 4, 2);
  Device b;
  b.bootFrom(d.card);
  TEST_ASSERT_EQUAL_STRING("h\n1\n2\n", b.onCard("logs/a.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(6, b.indexed("logs", "a.csv"));
  TEST_ASSERT_FALSE(b.card.files.count(kNote));

  Device n;
  assertOk(n.call("append_file", {{"path", "logs/n.csv"}, {"text", "h\n"}}, owner()));
  n.card.files[kNote] = noteText('a', owner().ns, "logs/n.csv", -1, 2);
  Device c;
  c.bootFrom(n.card);
  TEST_ASSERT_EQUAL_STRING("h\n", c.onCard("logs/n.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(2, c.indexed("logs", "n.csv"));
  TEST_ASSERT_FALSE(c.card.files.count(kNote));
}

// P2, P4: a note older than other writers. Engine 2's append of 600 B was cut before its bytes landed; then
// v5.2.0 (after a flash back) appended rows and kept its index, and one row the index never saw. The boot keeps
// every row and the index holds the full size. The same for a web upload over the file, and for a note of a new
// file whose name another person's upload took: nothing cut, removed or re-assigned.
void test_a_stale_note_and_rows_of_a_writer_that_does_not_journal_leave_every_row() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/l.csv"}, {"content", "h\n"}}, owner()));
  d.card.files[kNote] = noteText('a', owner().ns, "logs/l.csv", 2, 600);
  std::string rows = "h\n";
  for (int i = 0; i < 5; i++) {
    rows += "2026-11-2" + std::to_string(i) + ",row\n";
    indexedWrite(d, "logs/l.csv", rows);
  }
  behind(d, "logs/l.csv", "2026-11-25,row\n");
  rows += "2026-11-25,row\n";
  Device b;
  b.bootFrom(d.card);
  TEST_ASSERT_EQUAL_STRING(rows.c_str(), b.onCard("logs/l.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(rows.size(), b.indexed("logs", "l.csv"));
  TEST_ASSERT_FALSE(b.card.files.count(kNote));

  Device u;   // a web upload of U bytes, B < U <= B + L
  assertOk(u.call("write_file", {{"path", "logs/u.csv"}, {"content", "h\n1\n"}}, owner()));
  u.card.files[kNote] = noteText('a', owner().ns, "logs/u.csv", 4, 600);
  indexedWrite(u, "logs/u.csv", "another,file\nwith,rows\n");
  Device c;
  c.bootFrom(u.card);
  TEST_ASSERT_EQUAL_STRING("another,file\nwith,rows\n", c.onCard("logs/u.csv").c_str());
  TEST_ASSERT_EQUAL_UINT32(23, c.indexed("logs", "u.csv"));

  Device m;   // a note of the owner's new file; the member's own file now holds that name
  assertOk(m.call("write_file", {{"path", "mine/m.md"}, {"content", "member's\n"}}, member()));
  m.card.files[kNote] = noteText('a', owner().ns, "mine/m.md", -1, 600);
  Device e;
  e.bootFrom(m.card);
  TEST_ASSERT_EQUAL_STRING("member's\n", e.onCard("mine/m.md").c_str());
  TEST_ASSERT_TRUE(e.store.ownedBy(*e.store.find("mine", "m.md"), member().ns, false));
}

// P9: a short append whose cut-back the card refused said "2 of 5 B landed"; the note stayed as well (the card
// refused its removal, the marker and the cut to empty). The next boot keeps those bytes.
void test_a_failed_cut_back_keeps_its_bytes_over_a_note_the_card_kept() {
  Device d;
  assertOk(d.call("write_file", {{"path", "logs/d.csv"}, {"content", "h\n"}}, owner()));
  d.card.shortAppendAt = 2;
  d.card.truncateFails = true;
  d.card.refuseRemove.insert(kNote);
  d.card.refuseOverwrite.insert(kNote);
  const orch::ToolResult r = d.call("append_file", {{"path", "logs/d.csv"}, {"text", "1234\n"}}, owner());
  TEST_ASSERT_EQUAL_STRING("appended 2 B to logs/d.csv; file is 4 B", r.error.c_str());
  TEST_ASSERT_TRUE_MESSAGE(has(d.card.files[kNote], "logs"), "the card kept the live note");
  Device b;
  b.bootFrom(d.card);
  const std::string now = b.onCard("logs/d.csv");
  TEST_ASSERT_TRUE_MESSAGE(now == "h\n12" || now == "h\n12\n", now.c_str());
  TEST_ASSERT_EQUAL_UINT32(now.size(), b.indexed("logs", "d.csv"));
}

// Rule 2: a note whose clear the card refuses is overwritten by a marker (or cut to empty), so it causes no
// action at the next two boots: here, no re-adding of a file whose entry another writer dropped since.
void test_a_note_whose_clear_fails_causes_no_action_at_the_next_two_boots() {
  const auto refusing = [](Device& x, bool overwrite) {
    x.card.refuseRemove.insert(kNote);
    if (overwrite) x.card.refuseOverwrite.insert(kNote);
  };
  const auto dropEntry = [](Device& x, const char* project, const char* name) {
    TEST_ASSERT_TRUE(x.store.remove(project, name));
    x.card.files["/mem/files/.index"] = x.store.dump();
  };
  Device d;   // at runtime: the index landed, the removal refused
  refusing(d, false);
  assertOk(d.call("write_file", {{"path", "notes/k.md"}, {"content", "kept\n"}}, owner()));
  TEST_ASSERT_FALSE_MESSAGE(has(d.card.files[kNote], "notes"), "the note is no longer readable as a change");
  dropEntry(d, "notes", "k.md");
  Device b, c;
  refusing(b, false);
  refusing(c, false);
  TEST_ASSERT_EQUAL_STRING("", b.bootFrom(d.card).c_str());
  TEST_ASSERT_NULL(b.store.find("notes", "k.md"));
  TEST_ASSERT_EQUAL_STRING("", c.bootFrom(b.card).c_str());
  TEST_ASSERT_NULL(c.store.find("notes", "k.md"));

  Device e;   // at boot: a cut left a new file and its note; the boot adopts it once, then cannot clear the note
  e.card.files["/mem/files/notes/q.md"] = "new note\n";
  e.card.dirs.insert("/mem/files/notes");
  e.card.files[kNote] = noteText('w', member().ns, "notes/q.md", -1, 9);
  Device f, g, h;
  refusing(f, true);
  refusing(g, true);
  refusing(h, true);
  TEST_ASSERT_FALSE(f.bootFrom(e.card).empty());
  TEST_ASSERT_TRUE(f.store.ownedBy(*f.store.find("notes", "q.md"), member().ns, false));
  dropEntry(f, "notes", "q.md");
  TEST_ASSERT_EQUAL_STRING("", g.bootFrom(f.card).c_str());
  TEST_ASSERT_NULL(g.store.find("notes", "q.md"));
  TEST_ASSERT_EQUAL_STRING("", h.bootFrom(g.card).c_str());
  TEST_ASSERT_NULL(h.store.find("notes", "q.md"));
  TEST_ASSERT_EQUAL_STRING("new note\n", h.onCard("notes/q.md").c_str());
}

// Rule 1: an append the power cut mid-write leaves its part of the row; the boot ends it with a line end, so
// the next row starts cleanly, and the index holds it.
void test_a_torn_append_gets_its_line_end_at_boot() {
  bool torn = false;
  for (int k = 1; k < 16 && !torn; k++) {
    Device d;
    assertOk(d.call("write_file", {{"path", "logs/t.csv"}, {"content", "h\n1\n"}}, owner()));
    d.card.tornCut = true;
    d.card.cutAt = d.card.ops + k;
    try {
      (void)d.call("append_file", {{"path", "logs/t.csv"}, {"text", "22,tea\n"}}, owner());
    } catch (const PowerCut&) {
    }
    if (d.onCard("logs/t.csv") != "h\n1\n22,") continue;
    torn = true;
    Device b;
    b.bootFrom(d.card);
    TEST_ASSERT_EQUAL_STRING("h\n1\n22,\n", b.onCard("logs/t.csv").c_str());
    TEST_ASSERT_EQUAL_UINT32(8, b.indexed("logs", "t.csv"));
    TEST_ASSERT_FALSE(b.card.files.count(kNote));
    assertOk(b.call("append_file", {{"path", "logs/t.csv"}, {"text", "3,tea\n"}}, owner()));
    TEST_ASSERT_EQUAL_STRING("h\n1\n22,\n3,tea\n", b.onCard("logs/t.csv").c_str());
  }
  TEST_ASSERT_TRUE_MESSAGE(torn, "no cut fell inside the append");
}

}  // namespace

// Lane E2-CHAT: read_file on skills/<id>.md answers the skill's approved body through the host's reader.
static void test_read_file_on_a_skill_reads_the_skill_body() {
  Device d;
  const orch::ToolResult r = d.call("read_file", {{"path", "skills/bench-skill.md"}}, owner());
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  TEST_ASSERT_TRUE(r.output.find("Use the bench word.") != std::string::npos);
  TEST_ASSERT_FALSE(d.call("read_file", {{"path", "skills/none.md"}}, owner()).success);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_write_read_append_edit_list_delete);
  RUN_TEST(test_a_17_KB_file_reads_in_one_call);
  RUN_TEST(test_the_tools_are_the_daemon_names_and_arguments);
  RUN_TEST(test_a_member_sees_only_its_own_files);
  RUN_TEST(test_read_file_on_a_skill_reads_the_skill_body);
  RUN_TEST(test_an_append_lands_at_the_real_end_and_loses_nothing);
  RUN_TEST(test_a_torn_tail_gets_a_line_end_first_not_a_cut);
  RUN_TEST(test_read_and_edit_see_every_stored_byte);
  RUN_TEST(test_a_file_the_index_does_not_know_is_never_written);
  RUN_TEST(test_a_short_append_leaves_the_file_byte_identical_and_says_it_failed);
  RUN_TEST(test_a_failed_cut_back_states_the_bytes_that_landed);
  RUN_TEST(test_an_upload_in_progress_refuses_every_change);
  RUN_TEST(test_a_power_cut_at_every_card_operation_leaves_old_or_new_with_the_index_agreeing);
  RUN_TEST(test_an_append_decides_its_fence_on_the_file_as_it_is_at_the_commit);
  RUN_TEST(test_a_pending_replace_blocks_every_write_to_its_file);
  RUN_TEST(test_a_replace_is_the_old_engines_swap);
  RUN_TEST(test_a_stale_append_note_after_a_completed_append_leaves_the_row);
  RUN_TEST(test_a_stale_note_and_rows_of_a_writer_that_does_not_journal_leave_every_row);
  RUN_TEST(test_a_failed_cut_back_keeps_its_bytes_over_a_note_the_card_kept);
  RUN_TEST(test_a_note_whose_clear_fails_causes_no_action_at_the_next_two_boots);
  RUN_TEST(test_a_torn_append_gets_its_line_end_at_boot);
  return UNITY_END();
}
