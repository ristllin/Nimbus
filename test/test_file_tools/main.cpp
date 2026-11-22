// The engine-2 plain file tools (file_tools.h): read_file, write_file, append_file,
// edit_file, list_files.
//
// Class rules proven here, each over a generated class rather than one instance:
//   - paging (the test of record): following next_offset rebuilds every corpus file byte
//     for byte at every head budget from 16 B to 64 KB, every cut falls on a UTF-8 boundary
//     (on valid text) and after a line end whenever one sits in the page's second half;
//   - append only appends: over generated contents and texts (duplicates, same-key CSV rows,
//     a zero row, no newline) the file after is exactly the file before plus the text;
//   - an unknown argument is an error naming the accepted ones, for every tool and every
//     unknown key, and nothing is written;
//   - per-person scoping: every tool x 6 caller classes x 3 file states matches the rule
//     table, a refused read reads exactly like a missing file, and a refusal writes nothing;
//   - quotas and the store's room hold for every write tool;
//   - a write to every reserved family, in any letter case, names the tool that owns it;
//   - every card op runs inside a hold, each hold holds at most one op of bounded size, and
//     the modelled hold (RB-M2's bench figures) stays under 150 ms at the content caps; a
//     counter-test proves the model catches the old whole-file save;
//   - a card fault at every step of every write leaves the old file or the new one, never
//     a third, and no part file behind.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/plain/request.h"   // plain::toolDecl - the manifest the binding registers with
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/file_tools.h"
#include "nimbus/orch/path_rules.h"
#include "nimbus/orch/psram_alloc.h"
#include "../support/file_aids_cases.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/utf8.h"

using namespace nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

void setUp() {}
void tearDown() {}

namespace {

// The card here is case-sensitive (nimbusd on a POSIX volume): a card op in the wrong
// letter case would reach another file.
std::string lowerKey(const std::string& project, const std::string& name) { return project + "/" + name; }

// The card and its lock, recorded. Each hold lists its ops: r read, a append, w part
// write, s swap, d discard, i index rewrite.
struct Op {
  char kind;
  size_t bytes;
};

struct FakeIo : FileToolsIo {
  FileStore store;
  std::map<std::string, std::string> card, parts, skills;
  std::vector<std::vector<Op>> holds;
  std::string persisted;
  bool inHold = false, present = true;
  int ops = 0, failAt = -1;
  uint32_t clock = 1763380000;   // 17 Nov 2026
  std::function<void(size_t)> afterHold;

  bool locked(const std::function<void()>& fn) override {
    if (!present) return false;
    TEST_ASSERT_FALSE_MESSAGE(inHold, "a hold inside a hold");
    inHold = true;
    holds.emplace_back();
    fn();
    inHold = false;
    if (afterHold) afterHold(holds.size());
    return true;
  }
  FileStore& index() override {
    TEST_ASSERT_TRUE_MESSAGE(inHold, "the index read outside a hold");
    return store;
  }
  bool op(char kind, size_t bytes) {
    TEST_ASSERT_TRUE_MESSAGE(inHold, "a card op outside a hold");
    holds.back().push_back({kind, bytes});
    return ++ops != failAt;
  }
  bool readAt(const std::string& p, const std::string& n, uint64_t off, size_t len, std::string& out) override {
    if (!op('r', len)) return false;
    const auto it = card.find(lowerKey(p, n));
    if (it == card.end() || off > it->second.size()) return false;
    out = it->second.substr(size_t(off), len);
    return true;
  }
  bool append(const std::string& p, const std::string& n, std::string_view b) override {
    if (!op('a', b.size())) return false;
    card[lowerKey(p, n)].append(b.data(), b.size());
    return true;
  }
  bool partWrite(const std::string& p, const std::string& n, std::string_view b, bool first) override {
    if (!op('w', b.size())) return false;
    std::string& part = parts[lowerKey(p, n)];
    if (first) part.clear();
    part.append(b.data(), b.size());
    return true;
  }
  bool partSwap(const std::string& p, const std::string& n) override {
    const std::string k = lowerKey(p, n);
    if (!op('s', 0) || !parts.count(k)) return false;
    card[k] = parts[k];
    parts.erase(k);
    return true;
  }
  void partDiscard(const std::string& p, const std::string& n) override {
    holds.back().push_back({'d', 0});
    parts.erase(lowerKey(p, n));
  }
  bool persistIndex() override {
    if (!op('i', 0)) return false;
    persisted = store.dump();
    return true;
  }
  bool removable = false;   // delete_file's removal (FileToolsIo::remove; a binding opts in)
  bool remove(const std::string& p, const std::string& n) override {
    if (!removable || !op('x', 0)) return false;
    card.erase(lowerKey(p, n));
    return true;
  }
  uint32_t now() override { return clock++; }
  bool skillBody(const std::string& id, std::string& out) override {
    const auto it = skills.find(id);
    if (it == skills.end()) return false;
    out = it->second;
    return true;
  }
  size_t writeOps() const {
    size_t n = 0;
    for (const auto& h : holds)
      for (const Op& o : h) n += (o.kind == 'a' || o.kind == 'w' || o.kind == 's' || o.kind == 'i');
    return n;
  }
};

void seed(FakeIo& io, const std::string& project, const std::string& name, const std::string& bytes,
          const std::string& owner = "owner", bool shared = false) {
  const uint32_t at = 1763000000;
  FileEntry e;
  e.project = project;
  e.name = name;
  e.bytes = uint32_t(bytes.size());
  e.createdAt = at;
  e.kind = fileKindForName(name);
  e.hash = strtoull(blobHash(bytes).c_str(), nullptr, 16);
  e.owner = owner;
  e.shared = shared;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(io.store.add(e, err), err.c_str());
  io.card[lowerKey(project, name)] = bytes;
}

void stamp(FakeIo& io, const std::string& project, const std::string& name, uint32_t at) {
  FileEntry e = *io.store.find(project, name);
  e.createdAt = at;
  std::string err;
  io.store.add(e, err);
}

std::string cardOf(FakeIo& io, const std::string& path) {
  const size_t s = path.find('/');
  const auto it = io.card.find(lowerKey(path.substr(0, s), path.substr(s + 1)));
  return it == io.card.end() ? std::string("<none>") : it->second;
}

const Principal kOwner = principalForRole("1001", Role::Admin);
const Principal kMemberA = principalForRole("2002", Role::User);
const Principal kMemberB = principalForRole("3003", Role::User);
const Principal kGuest = principalForRole("4004", Role::Guest);
const Principal kUnknown = principalForRole("5005", Role::Unknown);
const Principal kNobody{};

// The manifest's declarations (RB-M0a's measured shapes, as lane RB-E2's plain/texts.h
// carries them): the binding registers with these bytes.
FileToolDecl measuredDecl(const char* tool, size_t cap) {
  const std::string t = tool;
  if (t == "read_file")
    return {"Read a file by path (project/name) with exact paging.",
            R"({"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"integer"},"limit":{"type":"integer"},"column":{"type":"string"},"from":{"type":"string"},"to":{"type":"string"}},"required":["path"]})"};
  if (t == "write_file")
    return {"Create or replace a file (project/name).",
            R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string","maxLength":)" +
                std::to_string(cap) +
                R"(},"if_exists":{"type":"string","enum":["fail","replace"]}},"required":["path","content"]})"};
  if (t == "append_file")
    return {"Add text to the end of a file.",
            R"({"type":"object","properties":{"path":{"type":"string"},"text":{"type":"string"}},"required":["path","text"]})"};
  if (t == "edit_file")
    return {"Exact find and replace in a file.",
            R"({"type":"object","properties":{"path":{"type":"string"},"old":{"type":"string"},"new":{"type":"string"},"all":{"type":"boolean"},"line":{"type":"integer"}},"required":["path","old","new"]})"};
  if (t == "delete_file")
    return {"Delete a file by path (project/name).", R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"};
  return {"List files newest first.", R"({"type":"object","properties":{"prefix":{"type":"string"},"after":{"type":"string"}}})"};
}

struct Rig {
  FakeIo io;
  FileToolsConfig cfg;
  FileTools tools;
  ToolRegistry reg;
  explicit Rig(const FileToolsConfig& c = FileToolsConfig()) : cfg(c), tools(io, cfg) {
    const size_t cap = cfg.contentCap;
    tools.registerInto(reg, [cap](const char* t) { return measuredDecl(t, cap); });
  }
  ToolResult call(const char* tool, const std::string& json, const Principal& who = kOwner) {
    JsonDocument doc;
    TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, json), json.c_str());
    return reg.dispatch(tool, doc.as<JsonObjectConst>(), who);
  }
};

std::string jsonStr(const std::string& s) {
  JsonDocument d;
  d.set(s);
  std::string out;
  serializeJson(d, out);
  return out;
}

// ---- read results -----------------------------------------------------------------

struct ReadPage {
  bool ok = false;
  std::string header, content;
  uint64_t size = 0, a = 0, b = 0, next = 0;
  bool more = false;
};

uint64_t numAfter(const std::string& s, const std::string& key) {
  const size_t p = s.find(key);
  TEST_ASSERT_TRUE_MESSAGE(p != std::string::npos, (key + " in " + s).c_str());
  return strtoull(s.c_str() + p + key.size(), nullptr, 10);
}

ReadPage parseRead(const ToolResult& r) {
  ReadPage p;
  p.ok = r.success;
  if (!r.success) return p;
  const std::string& t = r.output;
  const size_t nl = t.find('\n');
  TEST_ASSERT_TRUE_MESSAGE(nl != std::string::npos, t.c_str());
  p.header = t.substr(0, nl);
  p.size = numAfter(p.header, " size=");
  const size_t bp = p.header.find(" bytes ");
  TEST_ASSERT_TRUE(bp != std::string::npos);
  p.a = strtoull(p.header.c_str() + bp + 7, nullptr, 10);
  p.b = numAfter(p.header.substr(bp + 7), "-");
  TEST_ASSERT_TRUE(p.b >= p.a);
  p.content = t.substr(nl + 1, size_t(p.b - p.a));
  TEST_ASSERT_EQUAL(int(p.b - p.a), int(p.content.size()));
  const std::string tail = t.substr(nl + 1 + size_t(p.b - p.a));
  p.more = !tail.empty();
  if (p.more) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(("\nnext_offset=" + std::to_string(p.b)).c_str(), tail.c_str(), p.header.c_str());
    p.next = p.b;
  }
  return p;
}

std::string corpusLine(uint32_t& seed, int kind) {
  seed = seed * 1103515245u + 12345u;
  const uint32_t r = seed >> 8;
  switch (kind % 5) {
    case 0: return "2026-11-17,lunch,rye bread " + std::to_string(r % 97) + " g," + std::to_string(r % 700) + "\n";
    case 1: return "p\xc3\xa4iv\xc3\xa4 \xe2\x82\xac" + std::to_string(r % 50) + " \xf0\x9f\x8d\x9e\n";
    case 2: return std::string(200 + r % 900, char('a' + r % 26)) + "\n";
    case 3: return "line with crlf " + std::to_string(r) + "\r\n";
    default: return "\xe6\x97\xa5\xe8\xae\xb0 " + std::string(r % 40, 'x') + "\n";
  }
}

std::vector<std::string> pagingCorpus() {
  std::vector<std::string> out = {"", "x", "\n", "\n\n\n", std::string(70000, 'a'), "no newline at all here"};
  std::string s;
  for (int i = 0; i < 400; i++) s += "\xe2\x82\xac";   // one long line of 3-byte characters
  out.push_back(s);
  s.clear();
  for (int i = 0; i < 300; i++) s += "\xf0\x9f\x98\x80";   // 4-byte characters, no line end
  out.push_back(s);
  uint32_t seed = 7;
  for (int f = 0; f < 6; f++) {
    s.clear();
    const int lines = 20 + f * 60;
    for (int i = 0; i < lines; i++) s += corpusLine(seed, i * (f + 1));
    if (f % 2) s.pop_back();   // some files end without a newline
    out.push_back(s);
  }
  out.push_back(std::string("bad \xff\xfe bytes \xc3 torn\n") + std::string(3000, 'z'));
  return out;
}

bool boundary(const std::string& s, size_t at) {
  return at == 0 || at >= s.size() || (((unsigned char)s[at]) & 0xC0) != 0x80;
}

// Pages from offset 0 to the end; returns the rebuilt bytes.
std::string followPages(Rig& rig, const std::string& file, const std::string& path, size_t limit, int& pages) {
  std::string out;
  uint64_t off = 0;
  for (pages = 0; pages < 100000; pages++) {
    std::string args = "{\"path\":" + jsonStr(path) + ",\"offset\":" + std::to_string(off);
    if (limit) args += ",\"limit\":" + std::to_string(limit);
    const ReadPage p = parseRead(rig.call("read_file", args + "}"));
    TEST_ASSERT_TRUE_MESSAGE(p.ok, path.c_str());
    TEST_ASSERT_EQUAL(int(off), int(p.a));
    TEST_ASSERT_EQUAL(int(file.size()), int(p.size));
    // The page floor (ruling 95): a limit under kFileReadFloor reads as kFileReadFloor.
    const size_t cap = limit ? std::min(std::max(limit, kFileReadFloor), rig.cfg.readCap) : rig.cfg.readCap;
    TEST_ASSERT_TRUE_MESSAGE(p.content.size() <= cap, p.header.c_str());
    out += p.content;
    if (!p.more) break;
    TEST_ASSERT_TRUE_MESSAGE(p.b > p.a, "a page makes progress");
    // The cut rules.
    if (nimbus::utf8Valid(file)) TEST_ASSERT_TRUE_MESSAGE(boundary(file, size_t(p.b)), p.header.c_str());
    const size_t lastNl = p.content.rfind('\n');
    const size_t window = std::min(cap, size_t(file.size() - p.a));
    const std::string win = file.substr(size_t(p.a), window);
    const size_t winNl = win.rfind('\n');
    if (winNl != std::string::npos && winNl + 1 >= cap / 2)
      TEST_ASSERT_TRUE_MESSAGE(lastNl != std::string::npos && lastNl + 1 == p.content.size(), p.header.c_str());
    off = p.next;
  }
  return out;
}

}  // namespace

// ---- paging (the test of record) -----------------------------------------------------

static void test_following_next_offset_rebuilds_every_file_at_every_budget() {
  const std::vector<std::string> corpus = pagingCorpus();
  const size_t budgets[] = {16, 17, 31, 64, 100, 257, 1000, 4096, 8191, 8192, 8193, 12000, 32768, 65536};
  int reads = 0, files = 0;
  for (size_t cap : budgets) {
    FileToolsConfig cfg;
    cfg.readCap = cap;
    Rig rig(cfg);
    for (size_t i = 0; i < corpus.size(); i++) {
      const std::string name = "f" + std::to_string(i) + ".txt";
      seed(rig.io, "notes", name, corpus[i]);
      for (size_t limit : {size_t(0), size_t(16), cap / 3 + 16, cap + 1000}) {
        const size_t page = std::min(limit ? std::max(limit, kFileReadFloor) : cap, cap);
        if (corpus[i].size() / std::max<size_t>(page, 1) > 400) continue;   // keep it fast
        int pages = 0;
        const std::string rebuilt = followPages(rig, corpus[i], "notes/" + name, limit, pages);
        TEST_ASSERT_TRUE_MESSAGE(rebuilt == corpus[i], ("byte for byte: " + name + " cap " + std::to_string(cap)).c_str());
        reads += pages + 1;
        files++;
      }
    }
  }
  TEST_MESSAGE(("property: " + std::to_string(files) + " file x budget x limit runs rebuilt byte for byte in " +
                std::to_string(reads) + " reads")
                   .c_str());
}

static void test_page_cut_rule() {
  // At the end of the file the whole window is the page.
  TEST_ASSERT_EQUAL(5, int(pageCut("abcde", 16, true)));
  // A line end in the second half wins.
  TEST_ASSERT_EQUAL(12, int(pageCut("0123456789a\nbcdef", 16, false)));
  // A line end in the first half does not; the cut is the UTF-8 boundary.
  TEST_ASSERT_EQUAL(14, int(pageCut("ab\ncdefghijklm\xe2\x82\xac", 16, false)));
  // Never zero.
  TEST_ASSERT_TRUE(pageCut("\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80", 16, false) > 0);
}

static void test_read_states_size_lines_and_sha8() {
  Rig rig;
  const std::string body = "one\ntwo\nthree";
  seed(rig.io, "notes", "a.md", body);
  const ToolResult r = rig.call("read_file", R"({"path":"notes/a.md"})");
  TEST_ASSERT_TRUE(r.success);
  // The last line has no line end: the head says a write may have been cut short (file_aids.h kTornTail).
  const std::string want = "notes/a.md size=13 lines=3 sha8=" + blobHash(body).substr(0, 8) +
                           " bytes 0-13; last line has no newline (a write may have been cut short)\n" + body;
  TEST_ASSERT_EQUAL_STRING(want.c_str(), r.output.c_str());
  // The stored path is stated when the asked one was made valid; letter case follows the store.
  seed(rig.io, "my-notes", "Shopping-List.md", "milk\n");
  const ToolResult m = rig.call("read_file", R"({"path":"my notes/shopping list.md"})");
  TEST_ASSERT_TRUE_MESSAGE(m.success, m.error.c_str());
  TEST_ASSERT_EQUAL(0, int(m.output.find("my-notes/Shopping-List.md size=5 lines=1 ")));
  TEST_ASSERT_TRUE(m.output.find("(asked \"my notes/shopping list.md\")") != std::string::npos);
  // A limit over the head's cap returns the cap and says so.
  FileToolsConfig small;
  small.readCap = 100;
  Rig r2(small);
  seed(r2.io, "notes", "big.txt", std::string(1000, 'q'));
  const ToolResult b = r2.call("read_file", R"({"path":"notes/big.txt","limit":5000})");
  TEST_ASSERT_TRUE(b.success);
  TEST_ASSERT_TRUE_MESSAGE(b.output.find("bytes 0-100 (limit 100 B here)") != std::string::npos, b.output.c_str());
  // Past the end, at the end.
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"notes/a.md","offset":14})").success);
  const ReadPage end = parseRead(rig.call("read_file", R"({"path":"notes/a.md","offset":13})"));
  TEST_ASSERT_TRUE(end.ok && end.content.empty() && !end.more);
  // Over the line-count size, lines are not counted.
  FileToolsConfig lc;
  lc.lineCountMax = 10;
  Rig r3(lc);
  seed(r3.io, "notes", "a.md", body);
  TEST_ASSERT_TRUE(r3.call("read_file", R"({"path":"notes/a.md"})").output.find(" lines=? ") != std::string::npos);
}

// Ruling 95, the page floor: a limit under kFileReadFloor (2,048 B) is raised to it on every
// read, plain or filtered, and the head says the page used 2048; a limit over the floor is
// honoured as asked, with no note.
static void test_a_limit_under_the_floor_returns_a_full_floor_page_and_says_so() {
  Rig rig;
  std::string text;
  for (int i = 0; text.size() < 6000; i++) text += "line " + std::to_string(i) + " of a long note\n";
  seed(rig.io, "notes", "long.txt", text);
  for (const char* lim : {"0", "16", "150", "2047"}) {
    const ToolResult r = rig.call("read_file", std::string(R"({"path":"notes/long.txt","limit":)") + lim + "}");
    TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
    const ReadPage p = parseRead(r);
    TEST_ASSERT_TRUE_MESSAGE(p.content.size() > kFileReadFloor / 2 && p.content.size() <= kFileReadFloor, r.output.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.output.find(" (limit 2048 B here)\n") != std::string::npos, r.output.c_str());
    TEST_ASSERT_TRUE_MESSAGE(p.more && p.next == p.b, r.output.c_str());
  }
  // The counter-test: a limit over the floor is honoured, and no note is added.
  const ToolResult over = rig.call("read_file", R"({"path":"notes/long.txt","limit":3000})");
  const ReadPage q = parseRead(over);
  TEST_ASSERT_TRUE_MESSAGE(q.content.size() > 1500 && q.content.size() <= 3000, over.output.c_str());
  TEST_ASSERT_TRUE_MESSAGE(q.content.size() > kFileReadFloor, over.output.c_str());
  TEST_ASSERT_TRUE_MESSAGE(over.output.find("(limit ") == std::string::npos, over.output.c_str());
  // A filtered read has the same floor: a limit of 150 shows 2,048 B of matching rows.
  std::string csv = "date,item,amount\n";
  for (int i = 0; i < 200; i++) csv += "2026-11-19,item number " + std::to_string(i) + ",1\n";
  seed(rig.io, "log", "big.csv", csv);
  const ToolResult f = rig.call("read_file", R"({"path":"log/big.csv","column":"date","from":"2026-11-19","limit":150})");
  TEST_ASSERT_TRUE_MESSAGE(f.success, f.error.c_str());
  TEST_ASSERT_TRUE_MESSAGE(f.output.find(" (limit 2048 B here)\n") != std::string::npos, f.output.c_str());
  TEST_ASSERT_TRUE_MESSAGE(f.output.find("\n2026-11-19,item number 60,1\n") != std::string::npos, f.output.c_str());
  TEST_ASSERT_TRUE_MESSAGE(f.output.find("\nnext_offset=") != std::string::npos, f.output.c_str());
}

static void test_read_refuses_by_kind_never_by_guess() {
  Rig rig;
  seed(rig.io, "photos", "cat.jpg", "\xff\xd8\xff\xe0 jpeg");
  seed(rig.io, "inbox", "1-report.pdf", "%PDF-1.4 ...");
  seed(rig.io, "notes", "blob.bin", std::string("ab\0cd", 5));
  seed(rig.io, "notes", "odd.txt", "caf\xc3\xa9 \xff ok\n");
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"photos/cat.jpg"})").success);
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"inbox/1-report.pdf"})").success);
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"notes/blob.bin"})").success);
  // The rule reads the file's first 4 KB whatever the page: a small first page is not let through.
  seed(rig.io, "notes", "late.txt", std::string(100, 'a') + std::string("\0", 1) + std::string(100, 'b'));
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"notes/late.txt","limit":16})").success);
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"notes/late.txt","offset":150})").success);
  // A stray bad byte in text is still text: the wire layer repairs it.
  TEST_ASSERT_TRUE(rig.call("read_file", R"({"path":"notes/odd.txt"})").success);
}

// ---- append only appends -------------------------------------------------------------

static void test_append_only_appends() {
  const std::vector<std::string> befores = {"", "date,meal,items,kcal\n", "2026-11-17,dinner,soup,300\n",
                                            "no newline", std::string(5000, 'x') + "\n"};
  const std::vector<std::string> texts = {"2026-11-17,dinner,soup,300\n",   // the same row again
                                          "2026-11-17,dinner,pasta,600\n",  // the same key, a new item
                                          "2026-11-17,dinner,soup,0\n",     // a zero row
                                          "x",
                                          "\n",
                                          "p\xc3\xa4iv\xc3\xa4\n",
                                          std::string(12000, 'y')};
  int checked = 0;
  for (const std::string& before : befores) {
    for (const std::string& text : texts) {
      Rig rig;
      if (!before.empty()) seed(rig.io, "food", "meals.csv", before);
      std::string want = before;
      for (int rep = 0; rep < 2; rep++) {   // twice: a repeat appends again
        const ToolResult r =
            rig.call("append_file", "{\"path\":\"food/meals.csv\",\"text\":" + jsonStr(text) + "}");
        TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
        // The newline fence: a file that ends without a line end gets one before the text.
        const bool fence = !want.empty() && want.back() != '\n';
        want += (fence ? "\n" : "") + text;
        TEST_ASSERT_TRUE_MESSAGE(cardOf(rig.io, "food/meals.csv") == want, r.output.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(fence, r.output.find("; added a newline before the text") != std::string::npos,
                                  r.output.c_str());
        const std::string head = "appended " + std::to_string(text.size() + fence) + " B to food/meals.csv; file is " +
                                 std::to_string(want.size()) + " B";
        TEST_ASSERT_EQUAL_MESSAGE(0, int(r.output.find(head)), r.output.c_str());
        TEST_ASSERT_EQUAL(want.back() != '\n', r.output.find("ends without a newline") != std::string::npos);
        TEST_ASSERT_EQUAL(before.empty() && rep == 0, r.output.find("new file") != std::string::npos);
        // The index keeps the true size and content hash.
        rig.io.locked([&] {
          const FileEntry* e = rig.io.store.find("food", "meals.csv");
          TEST_ASSERT_NOT_NULL(e);
          TEST_ASSERT_EQUAL(int(want.size()), int(e->bytes));
          TEST_ASSERT_TRUE(e->hash == strtoull(blobHash(want).c_str(), nullptr, 16));
        });
        checked++;
      }
    }
  }
  TEST_MESSAGE(("property: " + std::to_string(checked) +
                " appends, each the file before plus the text (with a line end first when it had none)").c_str());
}

static void test_append_reports_lines() {
  Rig rig;
  seed(rig.io, "notes", "log.md", "a\nb\n");
  const ToolResult r = rig.call("append_file", R"({"path":"notes/log.md","text":"c\n"})");
  TEST_ASSERT_EQUAL_STRING("appended 2 B to notes/log.md; file is 6 B, 3 lines", r.output.c_str());
  TEST_ASSERT_FALSE(rig.call("append_file", R"({"path":"notes/log.md","text":""})").success);
}

static void test_hash_continuation() {
  const std::string a = "date,meal\n", b = "2026-11-17,lunch\n\xc3\xa4";
  const uint64_t whole = strtoull(blobHash(a + b).c_str(), nullptr, 16);
  TEST_ASSERT_TRUE(whole == fnvContinue(fnvContinue(kFnvBasis, a), b));
  TEST_ASSERT_TRUE(fnvContinue(kFnvBasis, "") == kFnvBasis);
  TEST_ASSERT_EQUAL_STRING(blobHash(a).substr(0, 8).c_str(), sha8Of(fnvContinue(kFnvBasis, a)).c_str());
  TEST_ASSERT_EQUAL_STRING("unknown", sha8Of(0).c_str());
}

// ---- unknown and wrong arguments --------------------------------------------------------

namespace {
struct ToolCase {
  const char* tool;
  const char* args;   // a valid call without the extra key
  std::vector<const char*> accepted;
};
const std::vector<ToolCase> kTools = {
    {"read_file", R"("path":"notes/a.md")", {"path", "offset", "limit", "column", "from", "to"}},
    {"write_file", R"("path":"notes/new.md","content":"hi")", {"path", "content", "if_exists"}},
    {"append_file", R"("path":"notes/a.md","text":"more")", {"path", "text"}},
    {"edit_file", R"("path":"notes/a.md","old":"hello","new":"bye")", {"path", "old", "new", "all", "line"}},
    {"list_files", R"("prefix":"notes")", {"prefix", "after"}},
};
}  // namespace

static void test_an_unknown_argument_is_an_error_naming_the_accepted_ones() {
  int checked = 0;
  for (const ToolCase& t : kTools) {
    for (const char* extra : {"foo", "save_to", "key", "mode", "append", "row", "Path", "content_b64", "replace", "sum"}) {
      if (std::find(t.accepted.begin(), t.accepted.end(), std::string(extra)) != t.accepted.end()) continue;
      Rig rig;
      seed(rig.io, "notes", "a.md", "hello world\n");
      const std::string before = cardOf(rig.io, "notes/a.md");
      const ToolResult r = rig.call(t.tool, std::string("{") + t.args + ",\"" + extra + "\":1}");
      const std::string msg = std::string(t.tool) + " + " + extra + ": " + r.error;
      TEST_ASSERT_FALSE_MESSAGE(r.success, msg.c_str());
      for (const char* a : t.accepted) TEST_ASSERT_TRUE_MESSAGE(r.error.find(a) != std::string::npos, msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.error.find(extra) != std::string::npos, msg.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(0, int(rig.io.writeOps()), msg.c_str());
      TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == before);
      TEST_ASSERT_TRUE(cardOf(rig.io, "notes/new.md") == "<none>");
      checked++;
    }
    // The same call without the extra key succeeds: the refusal was the unknown key alone.
    Rig rig;
    seed(rig.io, "notes", "a.md", "hello world\n");
    const ToolResult ok = rig.call(t.tool, std::string("{") + t.args + "}");
    TEST_ASSERT_TRUE_MESSAGE(ok.success, (std::string(t.tool) + ": " + ok.error).c_str());
  }
  TEST_MESSAGE(("property: " + std::to_string(checked) + " calls with an unknown key, each an error that writes nothing")
                   .c_str());
}

static void test_wrong_types_and_missing_arguments_are_errors() {
  const char* const bad[][2] = {
      {"read_file", R"({})"},
      {"read_file", R"({"path":5})"},
      {"read_file", R"({"path":"notes/a.md","offset":-1})"},
      {"read_file", R"({"path":"notes/a.md","offset":"3"})"},
      {"read_file", R"({"path":"notes/a.md","limit":1.5})"},
      {"write_file", R"({"path":"notes/x.md"})"},
      {"write_file", R"({"path":"notes/x.md","content":7})"},
      {"write_file", R"({"path":"notes/x.md","content":"a","if_exists":"append"})"},
      {"append_file", R"({"path":"notes/a.md"})"},
      {"edit_file", R"({"path":"notes/a.md","old":"hello"})"},
      {"edit_file", R"({"path":"notes/a.md","old":"hello","new":"x","all":"yes"})"},
      {"edit_file", R"({"path":"notes/a.md","old":"","new":"x"})"},
      {"edit_file", R"({"path":"notes/a.md","old":"hello","new":"hello"})"},
      {"list_files", R"({"prefix":3})"},
      {"read_file", R"({"path":"/etc/passwd"})"},
      {"read_file", R"({"path":"../a/b"})"},
      {"write_file", R"({"path":"a/b/c","content":"x"})"},
  };
  for (const auto& b : bad) {
    Rig rig;
    seed(rig.io, "notes", "a.md", "hello world\n");
    const ToolResult r = rig.call(b[0], b[1]);
    TEST_ASSERT_FALSE_MESSAGE(r.success, b[1]);
    TEST_ASSERT_FALSE_MESSAGE(r.error.empty(), b[1]);
    TEST_ASSERT_EQUAL_MESSAGE(0, int(rig.io.writeOps()), b[1]);
  }
}

// ---- write_file and edit_file --------------------------------------------------------------

static void test_write_file_creates_or_replaces_only_when_asked() {
  Rig rig;
  const ToolResult w = rig.call("write_file", R"({"path":"notes/plan.md","content":"step 1\n"})");
  TEST_ASSERT_EQUAL_STRING(("wrote notes/plan.md size=7 sha8=" + blobHash("step 1\n").substr(0, 8) + "; new file").c_str(),
                           w.output.c_str());
  const ToolResult again = rig.call("write_file", R"({"path":"notes/plan.md","content":"other"})");
  TEST_ASSERT_FALSE(again.success);
  TEST_ASSERT_TRUE(again.error.find("if_exists=replace") != std::string::npos);
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/plan.md") == "step 1\n");
  const ToolResult rep =
      rig.call("write_file", R"({"path":"notes/plan.md","content":"step 2\n","if_exists":"replace"})");
  TEST_ASSERT_EQUAL_STRING(("wrote notes/plan.md size=7 sha8=" + blobHash("step 2\n").substr(0, 8) + "; replaced 7 B").c_str(),
                           rep.output.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/plan.md") == "step 2\n");
  // A made-valid name: the result states the stored path and the asked one.
  const ToolResult v = rig.call("write_file", R"({"path":"my notes/päivä.md","content":""})");
  TEST_ASSERT_TRUE_MESSAGE(v.success, v.error.c_str());
  TEST_ASSERT_EQUAL(0, int(v.output.find("wrote my-notes/paiva.md size=0 ")));
  TEST_ASSERT_TRUE(v.output.find("asked \"my notes/p\xc3\xa4iv\xc3\xa4.md\"") != std::string::npos);
  TEST_ASSERT_TRUE(cardOf(rig.io, "my-notes/paiva.md").empty());
  // The content cap of this host, stated in the schema.
  FileToolsConfig c60;
  c60.contentCap = 4096;
  Rig r60(c60);
  TEST_ASSERT_FALSE(r60.call("write_file", "{\"path\":\"notes/x.md\",\"content\":" + jsonStr(std::string(4097, 'a')) + "}").success);
  TEST_ASSERT_TRUE(r60.call("write_file", "{\"path\":\"notes/x.md\",\"content\":" + jsonStr(std::string(4096, 'a')) + "}").success);
  TEST_ASSERT_EQUAL(0, int(r60.io.parts.size()));
}

static void test_edit_file_exact_find_and_replace() {
  Rig rig;
  seed(rig.io, "notes", "a.md", "milk\neggs\nmilk\nbread\n");
  const ToolResult none = rig.call("edit_file", R"({"path":"notes/a.md","old":"butter","new":"x"})");
  TEST_ASSERT_FALSE(none.success);
  const ToolResult two = rig.call("edit_file", R"({"path":"notes/a.md","old":"milk","new":"oat milk"})");
  TEST_ASSERT_FALSE(two.success);
  TEST_ASSERT_EQUAL_STRING("old is in notes/a.md 2 times, at lines 1, 3; line picks one, all=true replaces all",
                           two.error.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == "milk\neggs\nmilk\nbread\n");
  const ToolResult all = rig.call("edit_file", R"({"path":"notes/a.md","old":"milk","new":"oat milk","all":true})");
  TEST_ASSERT_TRUE_MESSAGE(all.success, all.error.c_str());
  const std::string want = "oat milk\neggs\noat milk\nbread\n";
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == want);
  TEST_ASSERT_EQUAL_STRING(
      ("replaced 2 in notes/a.md at lines 1, 3; file is 29 B (was 21 B), 4 lines, sha8=" + blobHash(want).substr(0, 8) +
       "; old is still at lines 1, 3")   // "oat milk" holds "milk"
          .c_str(),
      all.output.c_str());
  const ToolResult one = rig.call("edit_file", R"({"path":"notes/a.md","old":"eggs\n","new":""})");
  TEST_ASSERT_TRUE(one.success);
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == "oat milk\noat milk\nbread\n");
  TEST_ASSERT_FALSE(rig.call("edit_file", R"({"path":"notes/missing.md","old":"a","new":"b"})").success);
  // Too large to rewrite.
  FileToolsConfig c;
  c.editMax = 100;
  Rig small(c);
  seed(small.io, "notes", "big.md", std::string(101, 'a'));
  TEST_ASSERT_FALSE(small.call("edit_file", R"({"path":"notes/big.md","old":"a","new":"b","all":true})").success);
  // A result larger than edit_file takes is refused before it is built.
  seed(small.io, "notes", "many.md", std::string(90, 'a'));
  const ToolResult grow = small.call("edit_file", R"({"path":"notes/many.md","old":"a","new":"bb","all":true})");
  TEST_ASSERT_FALSE(grow.success);
  TEST_ASSERT_TRUE_MESSAGE(grow.error.find("180 B") != std::string::npos, grow.error.c_str());
  TEST_ASSERT_EQUAL(0, int(small.io.writeOps()));
  // A NUL inside a string argument is kept, never a cut.
  Rig nul;
  TEST_ASSERT_TRUE(nul.call("write_file", R"({"path":"notes/n.txt","content":"ab\u0000cd"})").success);
  TEST_ASSERT_EQUAL(5, int(cardOf(nul.io, "notes/n.txt").size()));
  // A letter-case variant edits the stored file, never a second one.
  Rig cased;
  seed(cased.io, "notes", "a.md", "x y\n");
  TEST_ASSERT_TRUE(cased.call("write_file", R"({"path":"Notes/A.md","content":"z\n","if_exists":"replace"})").success);
  TEST_ASSERT_TRUE(cased.call("append_file", R"({"path":"NOTES/a.MD","text":"w\n"})").success);
  TEST_ASSERT_TRUE(cardOf(cased.io, "notes/a.md") == "z\nw\n");
  TEST_ASSERT_EQUAL(1, int(cased.io.card.size()));
}

static void test_edit_refuses_a_file_that_changed_while_it_was_edited() {
  Rig rig;
  seed(rig.io, "notes", "a.md", std::string(20000, 'a') + "needle" + std::string(20000, 'b'));
  // Another writer (the web page) replaces the file between edit_file's read and its write.
  bool fired = false;
  rig.io.afterHold = [&](size_t) {
    if (fired || rig.io.holds.back().empty() || rig.io.holds.back()[0].kind != 'r' ||
        rig.io.holds.back()[0].bytes == kFileReadSlice)
      return;   // the last (short) read slice of the file
    fired = true;
    rig.io.afterHold = nullptr;
    rig.io.locked([&] {
      seed(rig.io, "notes", "a.md", "someone else's text with needle\n");
    });
  };
  // seed() inside a hold uses store.add directly; the store keeps the newer entry.
  const ToolResult r = rig.call("edit_file", R"({"path":"notes/a.md","old":"needle","new":"pin"})");
  TEST_ASSERT_TRUE(fired);
  TEST_ASSERT_FALSE_MESSAGE(r.success, r.output.c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.error.find("changed") != std::string::npos, r.error.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == "someone else's text with needle\n");
  TEST_ASSERT_EQUAL(0, int(rig.io.parts.size()));
}

// ---- list_files ---------------------------------------------------------------------------

static void test_list_newest_first_with_a_true_remainder() {
  Rig rig;
  std::set<std::string> visible;
  for (int i = 0; i < 95; i++) {
    const std::string project = i % 3 ? "notes" : "food";
    const std::string name = "f" + std::to_string(i) + ".md";
    const bool mine = i % 5 != 0;   // every fifth file is the owner's, private to it
    seed(rig.io, project, name, std::string(size_t(i), 'x'), mine ? "chat:2002" : "owner");
    stamp(rig.io, project, name, 1763000000 + (i * 7919) % 500);
    if (mine) visible.insert(project + "/" + name);
  }
  std::vector<std::string> seen;
  std::string after;
  uint32_t lastTime = UINT32_MAX;
  for (int page = 0; page < 10; page++) {
    const std::string args = after.empty() ? "{}" : "{\"after\":" + jsonStr(after) + "}";
    const ToolResult r = rig.call("list_files", args, kMemberA);
    TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
    std::vector<std::string> lines;
    for (size_t at = 0; at < r.output.size();) {
      const size_t nl = r.output.find('\n', at);
      lines.push_back(r.output.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
      at = nl == std::string::npos ? r.output.size() : nl + 1;
    }
    const std::string last = lines.back();
    TEST_ASSERT_EQUAL(0, int(last.find("more: ")));
    const size_t shownBefore = seen.size();
    for (size_t i = 0; i + 1 < lines.size(); i++) {
      const std::string path = lines[i].substr(0, lines[i].find(' '));
      TEST_ASSERT_TRUE_MESSAGE(visible.count(path) == 1, lines[i].c_str());
      seen.push_back(path);
      rig.io.locked([&] {
        const FileEntry* e = rig.io.store.find(path.substr(0, path.find('/')), path.substr(path.find('/') + 1));
        TEST_ASSERT_TRUE(e->createdAt <= lastTime);
        lastTime = e->createdAt;
      });
    }
    TEST_ASSERT_TRUE(seen.size() - shownBefore <= rig.cfg.listMax);
    const size_t remaining = visible.size() - seen.size();
    TEST_ASSERT_EQUAL_MESSAGE(int(remaining), int(strtoul(last.c_str() + 6, nullptr, 10)), last.c_str());
    if (!remaining) break;
    after = seen.back();
    TEST_ASSERT_TRUE(last.find("(after=" + after + ")") != std::string::npos);
  }
  TEST_ASSERT_EQUAL(int(visible.size()), int(seen.size()));
  TEST_ASSERT_EQUAL(int(visible.size()), int(std::set<std::string>(seen.begin(), seen.end()).size()));
  // A prefix narrows; letter case does not matter.
  const ToolResult food = rig.call("list_files", R"({"prefix":"FOOD/"})", kOwner);
  TEST_ASSERT_TRUE(food.output.find("notes/") == std::string::npos);
  TEST_ASSERT_TRUE(food.output.find("food/f0.md") != std::string::npos);
  TEST_ASSERT_FALSE(rig.call("list_files", R"({"after":"notes/none.md"})").success);
  Rig empty;
  TEST_ASSERT_TRUE(empty.call("list_files", "{}").output.find("more: 0") != std::string::npos);
}

// ---- people --------------------------------------------------------------------------------

namespace {
enum FileState { OwnersPrivate = 0, MembersPrivate, MembersShared, kStates };
struct Caller {
  const char* label;
  const Principal* who;
  bool owner, memberA, reads, writes;
};
const Caller kCallers[] = {
    {"owner", &kOwner, true, false, true, true},     {"member A", &kMemberA, false, true, true, true},
    {"member B", &kMemberB, false, false, true, true}, {"guest", &kGuest, false, false, true, true},
    {"unknown role", &kUnknown, false, false, false, false}, {"unattributed", &kNobody, false, false, false, false},
};
bool mayRead(const Caller& c, FileState s) {
  if (!c.reads) return false;
  if (c.owner) return true;
  if (s == MembersShared) return true;
  return s == MembersPrivate && c.memberA;
}
bool mayWrite(const Caller& c, FileState s) {
  if (!c.writes) return false;
  if (c.owner) return true;
  return s != OwnersPrivate && c.memberA;
}
}  // namespace

namespace {

const std::string kSecret = "secret 42 kcal\n";
const char* holderOf(int s) { return s == OwnersPrivate ? "owner" : "chat:2002"; }

// One (tool, caller, file state) cell for the tools that name an existing file.
void scopingCell(const ToolCase& t, const Caller& c, int s) {
  Rig rig;
  seed(rig.io, "notes", "a.md", "hello world\n" + kSecret, holderOf(s), s == MembersShared);
  const ToolResult r = rig.call(t.tool, std::string("{") + t.args + "}", *c.who);
  const bool reads = std::string(t.tool) == "read_file";
  const bool want = reads ? mayRead(c, FileState(s)) : mayWrite(c, FileState(s));
  const std::string msg =
      std::string(t.tool) + " by " + c.label + " state " + std::to_string(s) + ": " + r.error + r.output;
  TEST_ASSERT_EQUAL_MESSAGE(want, r.success, msg.c_str());
  if (r.success) return;
  TEST_ASSERT_TRUE_MESSAGE(r.error.find("secret") == std::string::npos, msg.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(0, int(rig.io.writeOps()), msg.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == "hello world\n" + kSecret);
  // A file the caller may not read reads exactly like a missing one.
  if (!c.reads || mayRead(c, FileState(s))) return;
  Rig miss;
  const ToolResult missing = miss.call(t.tool, std::string("{") + t.args + "}", *c.who);
  if (!missing.success) TEST_ASSERT_EQUAL_STRING_MESSAGE(missing.error.c_str(), r.error.c_str(), msg.c_str());
}

// write_file onto the path, then the listing, for one caller and file state.
void scopingWriteAndList(const Caller& c, int s) {
  Rig rig;
  seed(rig.io, "notes", "a.md", kSecret, holderOf(s), s == MembersShared);
  const ToolResult w =
      rig.call("write_file", R"({"path":"notes/a.md","content":"mine","if_exists":"replace"})", *c.who);
  TEST_ASSERT_EQUAL_MESSAGE(mayWrite(c, FileState(s)), w.success, (std::string(c.label) + " " + w.error).c_str());
  if (!w.success) TEST_ASSERT_TRUE(cardOf(rig.io, "notes/a.md") == kSecret);
  // A replace keeps whose file it is.
  if (w.success)
    rig.io.locked([&] {
      TEST_ASSERT_EQUAL_STRING(holderOf(s), plainCopy(rig.io.store.find("notes", "a.md")->owner).c_str());
    });
  // A listing shows exactly what the caller may read.
  const ToolResult l = rig.call("list_files", "{}", *c.who);
  TEST_ASSERT_EQUAL_MESSAGE(c.reads, l.success, c.label);
  if (l.success) TEST_ASSERT_EQUAL(mayRead(c, FileState(s)), l.output.find("notes/a.md") != std::string::npos);
}

}  // namespace

static void test_per_person_scoping_class() {
  int checked = 0;
  for (int s = 0; s < kStates; s++) {
    for (const Caller& c : kCallers) {
      for (const ToolCase& t : kTools) {
        if (std::string(t.tool) == "list_files" || std::string(t.tool) == "write_file") continue;
        scopingCell(t, c, s);
        checked++;
      }
      scopingWriteAndList(c, s);
      checked += 2;
    }
  }
  // A new file belongs to the person who wrote it.
  Rig rig;
  TEST_ASSERT_TRUE(rig.call("write_file", R"({"path":"notes/b.md","content":"b"})", kMemberB).success);
  rig.io.locked(
      [&] { TEST_ASSERT_EQUAL_STRING("chat:3003", plainCopy(rig.io.store.find("notes", "b.md")->owner).c_str()); });
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"notes/b.md"})", kMemberA).success);
  TEST_MESSAGE(("property: " + std::to_string(checked) + " (tool, caller, file state) cells match the rule table").c_str());
}

static void test_quotas_and_room_hold_for_every_write() {
  Principal tight = kMemberA;
  tight.quota.maxBytes = 1000;
  for (int k = 0; k < 3; k++) {
    Rig rig;
    seed(rig.io, "notes", "a.md", std::string(900, 'a'), "chat:2002");
    ToolResult r;
    if (k == 0) r = rig.call("write_file", "{\"path\":\"notes/b.md\",\"content\":" + jsonStr(std::string(101, 'b')) + "}", tight);
    if (k == 1) r = rig.call("append_file", "{\"path\":\"notes/a.md\",\"text\":" + jsonStr(std::string(101, 'b')) + "}", tight);
    if (k == 2) r = rig.call("edit_file", R"({"path":"notes/a.md","old":"a","new":"bb","all":true})", tight);
    TEST_ASSERT_FALSE_MESSAGE(r.success, r.output.c_str());
    TEST_ASSERT_TRUE(r.error.find("1000 B") != std::string::npos);
    TEST_ASSERT_EQUAL(0, int(rig.io.writeOps()));
    // Replacing one's own file counts only the difference.
    TEST_ASSERT_TRUE(rig.call("write_file", "{\"path\":\"notes/a.md\",\"if_exists\":\"replace\",\"content\":" +
                                                jsonStr(std::string(1000, 'c')) + "}", tight).success);
  }
  // The store's own room (the binding may reclaim copies through roomFor).
  Rig full;
  FileStore::Limits lim;
  lim.maxTotalBytes = 50;
  full.io.store.setLimits(lim);
  seed(full.io, "notes", "a.md", std::string(40, 'a'));
  TEST_ASSERT_FALSE(full.call("write_file", "{\"path\":\"notes/b.md\",\"content\":" + jsonStr(std::string(11, 'b')) + "}").success);
  TEST_ASSERT_FALSE(full.call("append_file", "{\"path\":\"notes/a.md\",\"text\":" + jsonStr(std::string(11, 'b')) + "}").success);
  TEST_ASSERT_EQUAL(0, int(full.io.writeOps()));
  // No card at all.
  Rig none;
  none.io.present = false;
  for (const ToolCase& t : kTools) TEST_ASSERT_FALSE(none.call(t.tool, std::string("{") + t.args + "}").success);
}

// ---- reserved paths ----------------------------------------------------------------------

static void test_a_write_to_a_reserved_path_names_its_owner() {
  const char* const paths[][2] = {
      {"skills/food-coach.md", "skill_save"},   {"SKILLS/Food-Coach.MD", "skill_save"},
      {"skills/notes.txt", "skill_save"},       {"export-pre-v2/x.md", "no tool"},
      {"inbox/12-report.txt", "the device"},
      {"agents/research-a1.md", "agent_start"},
  };
  int checked = 0;
  for (const auto& p : paths) {
    for (int k = 0; k < 3; k++) {
      Rig rig;
      const std::string path = jsonStr(p[0]);
      ToolResult r;
      if (k == 0) r = rig.call("write_file", "{\"path\":" + path + ",\"content\":\"x\",\"if_exists\":\"replace\"}");
      if (k == 1) r = rig.call("append_file", "{\"path\":" + path + ",\"text\":\"x\"}");
      if (k == 2) r = rig.call("edit_file", "{\"path\":" + path + ",\"old\":\"x\",\"new\":\"y\"}");
      const std::string msg = std::string(p[0]) + ": " + r.error;
      TEST_ASSERT_FALSE_MESSAGE(r.success, msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.error.find(p[1]) != std::string::npos, msg.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.error.find("read-only") != std::string::npos, msg.c_str());
      TEST_ASSERT_EQUAL(0, int(rig.io.writeOps()));
      checked++;
    }
  }
  // Readable: an archive, the inbox and agent reports read like any file.
  Rig rig;
  seed(rig.io, "export-pre-v2", "openq.txt", "an old question\n");
  seed(rig.io, "inbox", "12-report.txt", "from a document\n");
  TEST_ASSERT_TRUE(rig.call("read_file", R"({"path":"export-pre-v2/openq.txt"})").success);
  TEST_ASSERT_TRUE(rig.call("read_file", R"({"path":"inbox/12-report.txt"})").success);
  // A person's own log or table, whatever it tracks, is an ordinary file: written, appended
  // and edited like any other (ruling 41; three use cases as equal fixtures).
  for (const char* p : {"food/food-log.csv", "expenses/2026.csv", "training/log.csv"}) {
    Rig own;
    const std::string path = jsonStr(p);
    const ToolResult w = own.call("write_file", "{\"path\":" + path + ",\"content\":\"date,item,qty\\n\"}");
    TEST_ASSERT_TRUE_MESSAGE(w.success, (std::string(p) + ": " + w.error).c_str());
    const ToolResult a = own.call("append_file", "{\"path\":" + path + ",\"text\":\"2026-11-19,x,1\\n\"}");
    TEST_ASSERT_TRUE_MESSAGE(a.success, (std::string(p) + ": " + a.error).c_str());
    const ToolResult e = own.call("edit_file", "{\"path\":" + path + ",\"old\":\",x,1\",\"new\":\",x,2\"}");
    TEST_ASSERT_TRUE_MESSAGE(e.success, (std::string(p) + ": " + e.error).c_str());
    TEST_ASSERT_TRUE(own.call("read_file", "{\"path\":" + path + "}").output.find("2026-11-19,x,2") != std::string::npos);
  }
  TEST_MESSAGE(("property: " + std::to_string(checked) + " writes to reserved paths, each naming its owner").c_str());
}

static void test_skills_read_the_approved_version() {
  Rig rig;
  std::string body = "---\ntitle: Food coach\n---\n";
  for (int i = 0; i < 300; i++) body += "step " + std::to_string(i) + " of the approved version\n";
  rig.io.skills["food-coach"] = body;
  // A file in the store under skills/ is never what is read.
  seed(rig.io, "skills", "food-coach.md", "a stale copy\n");
  const ReadPage p = parseRead(rig.call("read_file", R"({"path":"skills/food-coach.md","limit":1000})"));
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_TRUE(p.more);
  TEST_ASSERT_TRUE(p.header.find("(approved skill)") != std::string::npos);
  int pages = 0;
  TEST_ASSERT_TRUE(followPages(rig, body, "SKILLS/food-coach.md", 1000, pages) == body);
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"skills/unknown.md"})").success);
  TEST_ASSERT_FALSE(rig.call("read_file", R"({"path":"skills/notes.txt"})").success);
  TEST_ASSERT_EQUAL(0, int(rig.io.holds.size() > 0 && rig.io.writeOps()));
}

// ---- card holds -------------------------------------------------------------------------

namespace {
// RB-M2's bench figures (coverage/LINK-2026-11.md), worst observed: an append or part
// write of n bytes 12 ms at 4 KB and 39 ms at 16 KB (15 + 1.5 ms per KB covers both); an
// 8 KB read slice 6 ms (3 + 0.5 ms per KB); a rename 25 ms (the 20 KB part plus rename
// took 52 ms at worst, of which the 16 KB-class write was 39); the index rewrite with its
// room check, hash and log line 89 ms median on a 70-file store (110 ms at worst).
double holdMs(const std::vector<Op>& h) {
  double ms = 0;
  for (const Op& o : h) {
    const double kb = double(o.bytes) / 1024.0;
    if (o.kind == 'r') ms += 3 + 0.5 * kb;
    if (o.kind == 'a' || o.kind == 'w') ms += 15 + 1.5 * kb;
    if (o.kind == 's') ms += 25;
    if (o.kind == 'i') ms += 110;
    if (o.kind == 'd') ms += 5;
  }
  return ms;
}
double worstHold(const FakeIo& io, size_t& opsInHold) {
  double worst = 0;
  opsInHold = 0;
  for (const auto& h : io.holds) {
    worst = std::max(worst, holdMs(h));
    opsInHold = std::max(opsInHold, h.size());
  }
  return worst;
}
}  // namespace

static void test_every_hold_is_short_at_the_caps() {
  const std::string cap(12000, 'k');
  struct Path {
    const char* label;
    std::string args;
    const char* tool;
  };
  const std::vector<Path> paths = {
      {"write_file 12,000 B", "{\"path\":\"notes/new.md\",\"content\":" + jsonStr(cap) + "}", "write_file"},
      {"write_file replace", "{\"path\":\"notes/big.md\",\"if_exists\":\"replace\",\"content\":" + jsonStr(cap) + "}",
       "write_file"},
      {"append_file 12,000 B", "{\"path\":\"notes/big.md\",\"text\":" + jsonStr(cap) + "}", "append_file"},
      {"edit_file on 250 KB", "{\"path\":\"notes/big.md\",\"old\":\"needle\",\"new\":" + jsonStr(cap) + "}", "edit_file"},
      {"read_file 64 KB", R"({"path":"notes/big.md"})", "read_file"},
      {"list_files", "{}", "list_files"},
  };
  for (const Path& p : paths) {
    FileToolsConfig cfg;
    cfg.readCap = 65536;
    Rig rig(cfg);
    seed(rig.io, "notes", "big.md", std::string(130000, 'z') + "needle" + std::string(120000, 'z'));
    const ToolResult r = rig.call(p.tool, p.args);
    TEST_ASSERT_TRUE_MESSAGE(r.success, (std::string(p.label) + ": " + r.error).c_str());
    size_t opsInHold = 0;
    const double worst = worstHold(rig.io, opsInHold);
    char line[160];
    snprintf(line, sizeof line, "%s: %u holds, worst modelled hold %.1f ms, at most %u card op per hold", p.label,
             unsigned(rig.io.holds.size()), worst, unsigned(opsInHold));
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE_MESSAGE(worst <= 150.0, line);
    TEST_ASSERT_TRUE_MESSAGE(opsInHold <= 1, line);
    for (const auto& h : rig.io.holds)
      for (const Op& o : h) {
        if (o.kind == 'r') TEST_ASSERT_TRUE(o.bytes <= kFileReadSlice);
        if (o.kind == 'w') TEST_ASSERT_TRUE(o.bytes <= kFileWriteSlice);
        if (o.kind == 'a') TEST_ASSERT_TRUE(o.bytes <= rig.cfg.contentCap + 1);   // + the newline fence
      }
  }
}

static void test_the_hold_model_catches_the_old_whole_file_save() {
  // files::saveText: room check, hash, the 20 KB write, the rename and the index rewrite in
  // one hold (131 ms median, 162 worst on the bench).
  FakeIo io;
  io.holds.push_back({{'w', 20480}, {'s', 0}, {'i', 0}});
  size_t ops = 0;
  TEST_ASSERT_TRUE(worstHold(io, ops) > 150.0);
  TEST_ASSERT_TRUE(ops > 1);
}

// ---- faults ---------------------------------------------------------------------------------

static void test_a_card_fault_at_every_step_leaves_old_or_new() {
  const std::string old = "line one\nline two\n" + std::string(9000, 'o');
  struct W {
    const char* tool;
    std::string args;
    std::string after;
  };
  const std::vector<W> writes = {
      {"write_file", "{\"path\":\"notes/a.md\",\"if_exists\":\"replace\",\"content\":" + jsonStr(std::string(10000, 'n')) + "}",
       std::string(10000, 'n')},
      {"append_file", R"({"path":"notes/a.md","text":"line three\n"})", old + "\nline three\n"},   // fenced
      {"edit_file", R"({"path":"notes/a.md","old":"two","new":"2"})", "line one\nline 2\n" + std::string(9000, 'o')},
      {"write_file", "{\"path\":\"notes/b.md\",\"content\":" + jsonStr(std::string(5000, 'n')) + "}", std::string(5000, 'n')},
      {"append_file", R"({"path":"notes/b.md","text":"first\n"})", "first\n"},
  };
  int checked = 0;
  for (const W& w : writes) {
    Rig clean;
    seed(clean.io, "notes", "a.md", old);
    TEST_ASSERT_TRUE(clean.call(w.tool, w.args).success);
    const int steps = clean.io.ops;
    const std::string target = w.args.find("notes/b.md") != std::string::npos ? "notes/b.md" : "notes/a.md";
    const std::string before = target == "notes/a.md" ? old : "<none>";
    for (int k = 1; k <= steps; k++) {
      Rig rig;
      seed(rig.io, "notes", "a.md", old);
      rig.io.failAt = k;
      const ToolResult r = rig.call(w.tool, w.args);
      const std::string now = cardOf(rig.io, target);
      const std::string msg = std::string(w.tool) + " fault at op " + std::to_string(k) + ": " + r.error + r.output;
      TEST_ASSERT_TRUE_MESSAGE(now == before || now == w.after, msg.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(0, int(rig.io.parts.size()), msg.c_str());
      // The result says what is true: success means the new file is on the card.
      TEST_ASSERT_EQUAL_MESSAGE(now == w.after, r.success, msg.c_str());
      // The index in memory matches the card.
      rig.io.locked([&] {
        const FileEntry* e = rig.io.store.find("notes", target.substr(6));
        TEST_ASSERT_EQUAL_MESSAGE(now != "<none>", e != nullptr, msg.c_str());
        if (!e) return;
        TEST_ASSERT_EQUAL_MESSAGE(int(now.size()), int(e->bytes), msg.c_str());
        TEST_ASSERT_TRUE_MESSAGE(e->hash == strtoull(blobHash(now).c_str(), nullptr, 16), msg.c_str());
      });
      checked++;
    }
  }
  TEST_MESSAGE(("property: " + std::to_string(checked) + " card faults, each leaving the old file or the new one").c_str());
}

// ---- registration -----------------------------------------------------------------------

static void test_registration_states_the_accepted_arguments() {
  Rig rig;
  for (const ToolCase& t : kTools) {
    TEST_ASSERT_TRUE_MESSAGE(rig.reg.has(t.tool), t.tool);
    // The list the unknown-key error names is the list the measured schema offers.
    const std::vector<const char*>& args = fileToolArgs(t.tool);
    TEST_ASSERT_EQUAL_MESSAGE(int(t.accepted.size()), int(args.size()), t.tool);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, measuredDecl(t.tool, 12000).schema));
    JsonObjectConst props = doc["properties"].as<JsonObjectConst>();
    size_t n = 0;
    for (auto kv : props) {
      TEST_ASSERT_TRUE_MESSAGE(std::find_if(args.begin(), args.end(), [&](const char* a) {
                                 return std::strcmp(a, kv.key().c_str()) == 0;
                               }) != args.end(),
                               kv.key().c_str());
      n++;
    }
    TEST_ASSERT_EQUAL_MESSAGE(int(args.size()), int(n), t.tool);
  }
  TEST_ASSERT_TRUE(fileToolArgs("files.read").empty());
  // The manifest the binding registers with (RB-E2's plain::toolDecl) offers exactly these arguments, and its
  // write_file schema states the content cap this config enforces, on both host classes.
  for (const ToolCase& t : kTools) {
    const auto* d = agent::plain::toolDecl(t.tool);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, t.tool);
    for (bool shortHost : {false, true}) {
      JsonDocument doc;
      TEST_ASSERT_FALSE(deserializeJson(doc, agent::plain::declSchema(*d, shortHost)));
      size_t n = 0;
      for (auto kv : doc["properties"].as<JsonObjectConst>()) {
        TEST_ASSERT_TRUE_MESSAGE(std::find(t.accepted.begin(), t.accepted.end(), std::string(kv.key().c_str())) !=
                                     t.accepted.end(),
                                 kv.key().c_str());
        n++;
      }
      TEST_ASSERT_EQUAL_MESSAGE(int(fileToolArgs(t.tool).size()), int(n), t.tool);
      if (std::string(t.tool) == "write_file")
        TEST_ASSERT_EQUAL(shortHost ? 4096 : 12000, doc["properties"]["content"]["maxLength"].as<int>());
    }
  }
}

// ---- the general file aids through the tools (RB-REC4, file_aids.h) ------------------------

namespace {
long gAidAllocs = 0, gAidRefuseAt = -1;
void* aidAlloc(size_t n) { return ++gAidAllocs == gAidRefuseAt ? nullptr : std::malloc(n ? n : 1); }
void aidFree(void* p) { std::free(p); }
const char* kLog =
    "# kept by hand\n"
    "date,occasion,note,qty,amount\n"
    "2026-11-18,dinner,soup,1,4.5\n"
    "2026-11-19,lunch,\"rice, beans\",2,7.25\n"
    "2026-11-19,lunch,rice, beans,2,7.25\n"
    "#was: 2026-11-19,lunch,rice,1,3\n"
    "2026-11-19,tea,green,1,\n"
    "2026-11-19,tea,green,1,\n"
    "2026-11-20,breakfast,oats,1,2\n";
}  // namespace

// Ruling 95: read_file totals nothing. A request for a total gets the rows and never a figure;
// a call that still sends sum is refused as an unknown argument, and reads nothing. Ruling 97
// (red first): the selection is a plain read of the selected rows: the same head as a plain
// read, the rows as stored (the malformed line 5 among them), no line numbers, and no word
// about rows; the counter-test below reads the same file plainly and gets the same head form.
static void test_read_file_filters_csv_rows_and_totals_nothing() {
  Rig rig;
  seed(rig.io, "log", "2026.csv", kLog);
  const ToolResult r = rig.call("read_file", R"({"path":"log/2026.csv","column":"date","from":"2026-11-19"})");
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  TEST_ASSERT_EQUAL_STRING(
      ("log/2026.csv size=" + std::to_string(std::strlen(kLog)) + " lines=9 sha8=" +
       blobHash(kLog).substr(0, 8) +
       " bytes 0-" + std::to_string(std::strlen(kLog)) +
       "\ndate,occasion,note,qty,amount\n2026-11-19,lunch,\"rice, beans\",2,7.25\n"
       "2026-11-19,lunch,rice, beans,2,7.25\n2026-11-19,tea,green,1,\n2026-11-19,tea,green,1,\n")
          .c_str(),
      r.output.c_str());
  // Rows, never a figure, and nothing said about rows: no sum clause, no total, no computed
  // value, none of the words of the old tail.
  for (const char* word : {"sum", "total", "14.5", "21.75", "skipped", "left out", "not counted", "not summed",
                           "field count", "not compared", "rows "})
    TEST_ASSERT_TRUE_MESSAGE(r.output.find(word) == std::string::npos, r.output.c_str());
  // The counter-test: a plain read of the file has the same head form.
  const ToolResult plain = rig.call("read_file", R"({"path":"log/2026.csv"})");
  TEST_ASSERT_EQUAL(0, int(plain.output.find(r.output.substr(0, r.output.find('\n')))));
  // Every read is 4 KB per hold, one card op each.
  for (const auto& h : rig.io.holds) {
    TEST_ASSERT_TRUE(h.size() <= 1);
    for (const Op& o : h)
      if (o.kind == 'r') TEST_ASSERT_TRUE(o.bytes <= aids::kAidChunk);
  }
  // A call that still sends sum: refused the way every unknown argument is, nothing read.
  const ToolResult old = rig.call("read_file", R"({"path":"log/2026.csv","column":"date","from":"2026-11-19","sum":["amount"]})");
  TEST_ASSERT_FALSE(old.success);
  TEST_ASSERT_TRUE_MESSAGE(old.error.find("sum") != std::string::npos, old.error.c_str());
  TEST_ASSERT_TRUE_MESSAGE(old.error.find("column, from, to") != std::string::npos, old.error.c_str());
  // The errors state the fact and read nothing more.
  TEST_ASSERT_EQUAL_STRING("column needs from", rig.call("read_file", R"({"path":"log/2026.csv","column":"date"})").error.c_str());
  TEST_ASSERT_EQUAL_STRING("column day is not in the header: date,occasion,note,qty,amount",
                           rig.call("read_file", R"({"path":"log/2026.csv","column":"day","from":"x"})").error.c_str());
  TEST_ASSERT_EQUAL_STRING("no file log/none.csv",
                           rig.call("read_file", R"({"path":"log/none.csv","column":"a","from":"x"})").error.c_str());
}

static void test_edit_file_line_picks_one_of_identical_rows() {
  Rig rig;
  seed(rig.io, "log", "2026.csv", kLog);
  const ToolResult many = rig.call("edit_file", R"({"path":"log/2026.csv","old":"2026-11-19,tea,green,1,","new":""})");
  TEST_ASSERT_EQUAL_STRING("old is in log/2026.csv 2 times, at lines 7, 8; line picks one, all=true replaces all",
                           many.error.c_str());
  const ToolResult one =
      rig.call("edit_file", R"({"path":"log/2026.csv","old":"2026-11-19,tea,green,1,","new":"","line":8})");
  TEST_ASSERT_TRUE_MESSAGE(one.success, one.error.c_str());
  const std::string now = cardOf(rig.io, "log/2026.csv");
  TEST_ASSERT_EQUAL(0, int(one.output.find("replaced 1 in log/2026.csv at line 8; file is " + std::to_string(now.size()))));
  TEST_ASSERT_TRUE_MESSAGE(one.output.find("; lines that were 9 and later are now 1 lower; old is still at line 7") !=
                               std::string::npos,
                           one.output.c_str());
  // A stale number fails with the current lines and changes nothing.
  const ToolResult stale =
      rig.call("edit_file", R"({"path":"log/2026.csv","old":"2026-11-19,tea,green,1,","new":"x","line":8})");
  TEST_ASSERT_EQUAL_STRING("no match on line 8; matches at line 7", stale.error.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "log/2026.csv") == now);
  TEST_ASSERT_EQUAL_STRING("line is not a whole number, 1 or more",
                           rig.call("edit_file", R"({"path":"log/2026.csv","old":"a","new":"b","line":0})").error.c_str());
  TEST_ASSERT_EQUAL_STRING("line picks one match and all=true replaces every match; not both",
                           rig.call("edit_file", R"({"path":"log/2026.csv","old":"a","new":"b","line":2,"all":true})")
                               .error.c_str());
  // The index holds the new size and hash.
  rig.io.locked([&] {
    const FileEntry* e = rig.io.store.find("log", "2026.csv");
    TEST_ASSERT_EQUAL(int(now.size()), int(e->bytes));
    TEST_ASSERT_TRUE(e->hash == strtoull(blobHash(now).c_str(), nullptr, 16));
  });
}

static void test_append_file_states_the_fence_and_identical_lines() {
  Rig rig;
  seed(rig.io, "log", "2026.csv", "date,amount\n2026-11-19,5\n2026-11-19,6");
  const ToolResult r = rig.call("append_file", R"({"path":"log/2026.csv","text":"2026-11-19,5\n"})");
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_EQUAL_STRING(
      "appended 14 B to log/2026.csv; file is 51 B, 4 lines; added a newline before the text; identical to line 2",
      r.output.c_str());
  TEST_ASSERT_TRUE(cardOf(rig.io, "log/2026.csv") == "date,amount\n2026-11-19,5\n2026-11-19,6\n2026-11-19,5\n");
}

static void test_a_refused_allocation_in_a_filtered_read_or_a_line_edit_changes_nothing() {
  for (int tool = 0; tool < 2; ++tool) {
    const char* call = tool == 0 ? R"({"path":"log/2026.csv","column":"date","from":"2026-11-19"})"
                                 : R"({"path":"log/2026.csv","old":"2026-11-19,tea,green,1,","new":"","line":8})";
    const char* name = tool == 0 ? "read_file" : "edit_file";
    long need = 0;
    {
      Rig rig;
      seed(rig.io, "log", "2026.csv", kLog);
      gAidAllocs = 0;
      gAidRefuseAt = -1;
      setWorkingAllocators(aidAlloc, aidFree);
      TEST_ASSERT_TRUE(rig.call(name, call).success);
      need = gAidAllocs;
      setWorkingAllocators(nullptr, nullptr);
    }
    TEST_ASSERT_TRUE(need >= 2);
    for (long k = 1; k <= need; ++k) {
      Rig rig;
      seed(rig.io, "log", "2026.csv", kLog);
      gAidAllocs = 0;
      gAidRefuseAt = k;
      setWorkingAllocators(aidAlloc, aidFree);
      const ToolResult r = rig.call(name, call);
      setWorkingAllocators(nullptr, nullptr);
      TEST_ASSERT_FALSE(r.success);
      TEST_ASSERT_EQUAL_STRING(aids::kAidNoMemory, r.error.c_str());
      TEST_ASSERT_TRUE(cardOf(rig.io, "log/2026.csv") == kLog);
      TEST_ASSERT_EQUAL(0, int(rig.io.parts.size()));
    }
  }
}

// The shared fixtures (test/support/file_aids_cases.h) on the device's card seam give the
// golden transcript byte for byte; nimbusd's POSIX disk must give the same bytes
// (nimbusd/tests/test_hosted_file_aids.cpp). GOLDEN_UPDATE=1 re-blesses after a read of the diff.
static void test_shared_fixtures_match_the_golden_transcript() {
  FakeIo io;
  const std::string got = aidcases::transcript(
      io, [&](const std::string& p, const std::string& n, const std::string& b) { seed(io, p, n, b); },
      [&](const std::string& p, const std::string& n) {
        const auto it = io.card.find(lowerKey(p, n));
        return it == io.card.end() ? std::string("<none>") : it->second;
      });
  const char* path = "test/golden/file_aids_results.txt";
  if (std::getenv("GOLDEN_UPDATE")) {
    FILE* f = std::fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    std::fwrite(got.data(), 1, got.size(), f);
    std::fclose(f);
  }
  std::string want;
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "missing golden: GOLDEN_UPDATE=1 pio test -e native -f test_file_tools");
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) want.append(buf, n);
  std::fclose(f);
  TEST_ASSERT_EQUAL_STRING(want.c_str(), got.c_str());
}

// ---- E2-VERB: a stub is never content; delete_file -------------------------------------------

// A stub line a model copied from its history (its own earlier call shown as its stub) is refused as
// write_file content, append_file text and edit_file new, with a line that says to send the real
// text; nothing is written. Text that only mentions a stub, or holds one among other lines, is text.
void test_a_stub_line_is_refused_as_content_and_the_refusal_names_the_real_text() {
  Rig r;
  seed(r.io, "journal", "day-1", "Journal, day 1, Porto.\n");
  const char* stubs[] = {"[stub 1720B sha:c4091c8e: view id=r4]",
                         "[stub 1720B sha:c4091c8e: an argument sent earlier, not content; read_file path=journal/day-1 shows the file]",
                         "  [stub read_file r43 2221B 3L 20:44] journal/day-1 size=2507 ... (full: view id=r43)\n"};
  for (const char* st : stubs) {
    const size_t writes = r.io.writeOps();
    const ToolResult w = r.call("write_file", R"({"path":"journal/day-2","content":)" + jsonStr(st) + "}");
    TEST_ASSERT_FALSE_MESSAGE(w.success, st);
    TEST_ASSERT_TRUE_MESSAGE(w.error.find("stub line, not text") != std::string::npos && w.error.find("only real text is stored") != std::string::npos,
                             w.error.c_str());
    TEST_ASSERT_TRUE(r.call("append_file", R"({"path":"journal/day-1","text":)" + jsonStr(st) + "}").error.find("only real text is stored") != std::string::npos);
    TEST_ASSERT_TRUE(r.call("edit_file", R"({"path":"journal/day-1","old":"Porto","new":)" + jsonStr(st) + "}").error.find("only real text is stored") != std::string::npos);
    TEST_ASSERT_EQUAL_MESSAGE(int(writes), int(r.io.writeOps()), "a refused stub wrote something");
  }
  TEST_ASSERT_EQUAL_STRING("<none>", cardOf(r.io, "journal/day-2").c_str());
  TEST_ASSERT_EQUAL_STRING("Journal, day 1, Porto.\n", cardOf(r.io, "journal/day-1").c_str());
  TEST_ASSERT_TRUE(r.call("write_file", R"({"path":"journal/day-2","content":"[stub] marks a placeholder in my notes."})").success);
  TEST_ASSERT_TRUE(r.call("write_file", R"({"path":"journal/day-3","content":"line one\n[stub 1720B sha:c4091c8e: view id=r4]"})").success);
}

// delete_file removes the caller's own file, bytes and index entry in one hold, and persists the
// index; another person's file, a missing file and a reserved path are refused and nothing changes.
void test_delete_file_removes_only_the_callers_own_file() {
  Rig r;
  r.io.removable = true;
  seed(r.io, "docs", "old.csv", "a,b\n1,2\n");
  seed(r.io, "docs", "theirs.txt", "not yours", "chat:2002");
  const ToolResult d = r.call("delete_file", R"({"path":"docs/old.csv"})");
  TEST_ASSERT_TRUE_MESSAGE(d.success, d.error.c_str());
  TEST_ASSERT_EQUAL_STRING("deleted docs/old.csv (8 B)", d.output.c_str());
  TEST_ASSERT_EQUAL_STRING("<none>", cardOf(r.io, "docs/old.csv").c_str());
  TEST_ASSERT_NULL(r.io.store.find("docs", "old.csv"));
  TEST_ASSERT_TRUE_MESSAGE(r.io.persisted.find("old.csv") == std::string::npos, "the index was not persisted");
  TEST_ASSERT_FALSE(r.call("read_file", R"({"path":"docs/old.csv"})").success);
  TEST_ASSERT_FALSE(r.call("delete_file", R"({"path":"docs/old.csv"})").success);
  const ToolResult other = r.call("delete_file", R"({"path":"docs/theirs.txt"})", kMemberA);
  TEST_ASSERT_TRUE(other.success);   // member A owns it
  seed(r.io, "docs", "theirs.txt", "not yours", "chat:2002");
  TEST_ASSERT_FALSE(r.call("delete_file", R"({"path":"docs/theirs.txt"})", kMemberB).success);
  TEST_ASSERT_EQUAL_STRING("not yours", cardOf(r.io, "docs/theirs.txt").c_str());
  TEST_ASSERT_FALSE(r.call("delete_file", R"({"path":"docs/old.csv","force":true})").success);
  // A binding that cannot remove says so and changes nothing.
  r.io.removable = false;
  seed(r.io, "docs", "kept.txt", "kept");
  const ToolResult no = r.call("delete_file", R"({"path":"docs/kept.txt"})");
  TEST_ASSERT_FALSE(no.success);
  TEST_ASSERT_EQUAL_STRING("kept", cardOf(r.io, "docs/kept.txt").c_str());
  TEST_ASSERT_NOT_NULL(r.io.store.find("docs", "kept.txt"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_following_next_offset_rebuilds_every_file_at_every_budget);
  RUN_TEST(test_page_cut_rule);
  RUN_TEST(test_read_states_size_lines_and_sha8);
  RUN_TEST(test_a_limit_under_the_floor_returns_a_full_floor_page_and_says_so);
  RUN_TEST(test_read_refuses_by_kind_never_by_guess);
  RUN_TEST(test_append_only_appends);
  RUN_TEST(test_append_reports_lines);
  RUN_TEST(test_hash_continuation);
  RUN_TEST(test_an_unknown_argument_is_an_error_naming_the_accepted_ones);
  RUN_TEST(test_wrong_types_and_missing_arguments_are_errors);
  RUN_TEST(test_write_file_creates_or_replaces_only_when_asked);
  RUN_TEST(test_edit_file_exact_find_and_replace);
  RUN_TEST(test_edit_refuses_a_file_that_changed_while_it_was_edited);
  RUN_TEST(test_list_newest_first_with_a_true_remainder);
  RUN_TEST(test_per_person_scoping_class);
  RUN_TEST(test_quotas_and_room_hold_for_every_write);
  RUN_TEST(test_a_write_to_a_reserved_path_names_its_owner);
  RUN_TEST(test_skills_read_the_approved_version);
  RUN_TEST(test_every_hold_is_short_at_the_caps);
  RUN_TEST(test_the_hold_model_catches_the_old_whole_file_save);
  RUN_TEST(test_a_card_fault_at_every_step_leaves_old_or_new);
  RUN_TEST(test_registration_states_the_accepted_arguments);
  RUN_TEST(test_read_file_filters_csv_rows_and_totals_nothing);
  RUN_TEST(test_edit_file_line_picks_one_of_identical_rows);
  RUN_TEST(test_append_file_states_the_fence_and_identical_lines);
  RUN_TEST(test_a_refused_allocation_in_a_filtered_read_or_a_line_edit_changes_nothing);
  RUN_TEST(test_shared_fixtures_match_the_golden_transcript);
  RUN_TEST(test_a_stub_line_is_refused_as_content_and_the_refusal_names_the_real_text);
  RUN_TEST(test_delete_file_removes_only_the_callers_own_file);
  return UNITY_END();
}
