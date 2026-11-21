// Engine 2's notes (notes.h, lane RB-N1; PRD section 4 "Notes").
//
// Class rules proven here, each over a generated class rather than one instance:
//   - a note is kept on the first call with nothing but file operations (NotesIo has no other
//     seam), for the owner with no card at all; a member with no card is refused in one line;
//   - one value per key: over every sequence of sets of one key across both shelves and both
//     kinds, the key is stored exactly once, with the last value;
//   - pinned full stores unpinned and says so; both full is an error naming the largest keys,
//     and no stored byte changes (nothing evicted), for every fill;
//   - every origin that is not a person's own message, for set and remove alike, changes no
//     note and keeps the call on the pending shelf; approve runs it, reject drops it;
//   - each person reads and writes only their own files, chosen by the Principal alone;
//   - every write is atomic at the seam: a refused write changes nothing; a line the parser
//     does not know is kept by every rewrite; a refused allocation is an error result;
//   - the owner's files take at most 80 KB of internal flash with the atomic copy, and the
//     pinned block always fits the request's pinned-notes ceiling.
#include <unity.h>

#include <ArduinoJson.h>

#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/plain/request.h"   // kPinnedNotesCeiling, toolDecl("note")
#include "nimbus/orch/notes.h"
#include "nimbus/orch/rbac.h"

using namespace nimbus::orch;
using namespace nimbus::orch::notes;

namespace {

// In-memory storage; per person, per shelf. Records every op so a test can prove what ran.
struct MemIo : NotesIo {
  std::map<std::string, std::string> files;   // "<person>/<shelf>" -> bytes
  std::set<std::string> absent;               // persons whose storage is not there
  std::vector<std::string> ops;               // "read <file>", "write <file>"
  bool failWrites = false;
  bool throwOnRead = false;
  std::map<std::string, Caps> capsFor;
  static std::string name(const std::string& p, Shelf s) { return p + "/" + shelfSlug(s); }
  bool available(const std::string& p) override { return !absent.count(p); }
  bool read(const std::string& p, Shelf s, std::string& out) override {
    if (throwOnRead) throw std::bad_alloc();
    ops.push_back("read " + name(p, s));
    auto it = files.find(name(p, s));
    out = it == files.end() ? std::string() : it->second;
    return true;
  }
  bool write(const std::string& p, Shelf s, const std::string& bytes) override {
    ops.push_back("write " + name(p, s));
    if (failWrites) return false;
    files[name(p, s)] = bytes;
    return true;
  }
  Caps caps(const std::string& p) override {
    auto it = capsFor.find(p);
    if (it != capsFor.end()) return it->second;
    return p == "owner" ? kOwnerCaps : kMemberCaps;
  }
};

Principal owner() { return principalForRole("111", Role::Admin); }
Principal member(const char* chat) { return principalForRole(chat, Role::User); }

ToolResult callJson(Notebook& nb, const std::string& json, const Principal& who, Origin o = Origin::Attended) {
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, json));
  return nb.call(d.as<JsonObjectConst>(), who, o);
}
std::string setJson(const std::string& key, const std::string& value, int pin = -1, const char* kind = nullptr) {
  JsonDocument d;
  d["op"] = "set";
  d["key"] = key;
  d["value"] = value;
  if (pin >= 0) d["pin"] = pin == 1;
  if (kind) d["kind"] = kind;
  std::string s;
  serializeJson(d, s);
  return s;
}
size_t countKey(const std::string& file, const std::string& key) {
  size_t n = 0, at = 0;
  while (at < file.size()) {
    size_t end = file.find('\n', at);
    if (end == std::string::npos) end = file.size();
    Note note;
    if (parseLine(file.substr(at, end - at), note) && note.key == key) ++n;
    at = end + 1;
  }
  return n;
}
std::string both(MemIo& io, const std::string& p) {
  return io.files[MemIo::name(p, Shelf::Pinned)] + "|" + io.files[MemIo::name(p, Shelf::More)];
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the first call: nothing but file operations -------------------------------------------
static void test_set_saves_first_call_with_file_ops_only() {
  MemIo io;
  Notebook nb(io);
  const ToolResult r = callJson(nb, setJson("tea", "green, no sugar", 1), owner());
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  TEST_ASSERT_EQUAL(1, (int)countKey(io.files["owner/pinned"], "tea"));
  for (const std::string& op : io.ops) TEST_ASSERT_TRUE(op.rfind("read ", 0) == 0 || op.rfind("write ", 0) == 0);
}

static void test_owner_saves_with_no_card_and_member_is_refused_in_one_line() {
  MemIo io;
  io.absent.insert("222");   // the card is out: a member's storage is not there; the owner's is flash
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("a", "b"), owner()).success);
  const ToolResult m = callJson(nb, setJson("a", "b"), member("222"));
  TEST_ASSERT_FALSE(m.success);
  TEST_ASSERT_EQUAL(std::string::npos, m.error.find('\n'));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, m.error.find("card"));
  for (const std::string& op : io.ops) TEST_ASSERT_EQUAL(std::string::npos, op.find("222/"));
  TEST_ASSERT_EQUAL_STRING("", nb.pinnedBlock(member("222")).c_str());
}

// ---- one value per key, over every sequence --------------------------------------------------
static void test_one_value_per_key_over_every_sequence() {
  // Every sequence of 4 sets of one key over (pin true, pin false, pin left out) x (fact, rule).
  const int pins[] = {1, 0, -1};
  const char* kinds[] = {"fact", "rule"};
  int sequences = 0;
  for (int a = 0; a < 6; ++a)
    for (int b = 0; b < 6; ++b)
      for (int c = 0; c < 6; ++c)
        for (int e = 0; e < 6; ++e) {
          MemIo io;
          Notebook nb(io);
          const int steps[] = {a, b, c, e};
          std::string last;
          for (int i = 0; i < 4; ++i) {
            last = "v" + std::to_string(i);
            const ToolResult r = callJson(nb, setJson("k", last, pins[steps[i] % 3], kinds[steps[i] / 3]), owner());
            TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
          }
          const size_t inPinned = countKey(io.files["owner/pinned"], "k");
          const size_t inMore = countKey(io.files["owner/more"], "k");
          TEST_ASSERT_EQUAL(1, (int)(inPinned + inMore));
          const std::string& file = inPinned ? io.files["owner/pinned"] : io.files["owner/more"];
          TEST_ASSERT_NOT_EQUAL(std::string::npos, file.find(": " + last + "\n"));
          ++sequences;
        }
  TEST_ASSERT_EQUAL(1296, sequences);
}

static void test_a_set_heals_a_key_left_on_both_shelves() {
  MemIo io;
  io.files["owner/pinned"] = "k: old\n";
  io.files["owner/more"] = "k: older\n";   // a move cut between its two writes
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("k", "new"), owner()).success);
  TEST_ASSERT_EQUAL(1, (int)(countKey(io.files["owner/pinned"], "k") + countKey(io.files["owner/more"], "k")));
}

// ---- full shelves -----------------------------------------------------------------------------
static void test_pinned_full_stores_unpinned_and_says_so() {
  for (size_t fill = 0; fill < 8; ++fill) {
    MemIo io;
    io.capsFor["owner"] = Caps{64 + fill, 4096, 512};
    Notebook nb(io);
    std::string filler(40 + fill, 'x');
    TEST_ASSERT_TRUE(callJson(nb, setJson("fill", filler, 1), owner()).success);
    const ToolResult r = callJson(nb, setJson("next", "a value of some length", 1), owner());
    TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
    TEST_ASSERT_EQUAL(0, (int)countKey(io.files["owner/pinned"], "next"));
    TEST_ASSERT_EQUAL(1, (int)countKey(io.files["owner/more"], "next"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, r.output.find("unpinned"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, r.output.find("next: a value of some length"));
    TEST_ASSERT_EQUAL(1, (int)countKey(io.files["owner/pinned"], "fill"));   // nothing evicted
  }
}

static void test_both_full_is_an_error_naming_the_largest_keys_and_changes_nothing() {
  for (size_t extra = 0; extra < 6; ++extra) {
    MemIo io;
    io.capsFor["owner"] = Caps{80, 80, 512};
    Notebook nb(io);
    TEST_ASSERT_TRUE(callJson(nb, setJson("small", "s", 1), owner()).success);
    TEST_ASSERT_TRUE(callJson(nb, setJson("big-one", std::string(50 + extra, 'b'), 1), owner()).success);
    TEST_ASSERT_TRUE(callJson(nb, setJson("mid", std::string(30, 'm'), 0), owner()).success);
    TEST_ASSERT_TRUE(callJson(nb, setJson("wide", std::string(30, 'w'), 0), owner()).success);
    const std::string before = both(io, "owner");
    const ToolResult r = callJson(nb, setJson("new", std::string(60, 'n')), owner());
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), both(io, "owner").c_str());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, r.error.find("big-one"));
    const size_t big = r.error.find("big-one"), mid = r.error.find("mid"), wide = r.error.find("wide");
    TEST_ASSERT_TRUE(mid != std::string::npos && wide != std::string::npos);
    TEST_ASSERT_TRUE(big < mid && big < wide);   // largest first
    TEST_ASSERT_EQUAL(std::string::npos, r.error.find("small"));   // only the largest kLargestNamed
  }
}

// ---- pending: every origin that is not a person's own message ---------------------------------
static void test_tainted_or_unattended_set_and_remove_go_pending() {
  const Origin origins[] = {Origin::Attended, Origin::Tainted, Origin::Unattended};
  const char* calls[] = {R"({"op":"set","key":"k","value":"changed","pin":true})", R"({"op":"remove","key":"k"})"};
  for (Origin o : origins)
    for (const char* c : calls) {
      for (const Principal& who : {owner(), member("333")}) {
        MemIo io;
        Notebook nb(io);
        TEST_ASSERT_TRUE(callJson(nb, setJson("k", "orig", 1), who).success);
        const std::string person = personKey(who);
        const std::string before = both(io, person);
        const ToolResult r = callJson(nb, c, who, o);
        std::vector<PendingNote> kept;
        TEST_ASSERT_TRUE(nb.pending(who, kept));
        if (o == Origin::Attended) {
          TEST_ASSERT_TRUE(r.success);
          TEST_ASSERT_TRUE(before != both(io, person));   // counter: it ran
          TEST_ASSERT_EQUAL(0, (int)kept.size());
        } else {
          TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
          TEST_ASSERT_EQUAL_STRING(before.c_str(), both(io, person).c_str());
          TEST_ASSERT_EQUAL(1, (int)kept.size());
          TEST_ASSERT_EQUAL_STRING(o == Origin::Tainted ? "outside" : "unattended", kept[0].why.c_str());
          TEST_ASSERT_NOT_EQUAL(std::string::npos, r.output.find("approval"));
        }
      }
    }
}

static void test_pending_approve_runs_it_and_reject_drops_it() {
  MemIo io;
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("k", "kept value", 1), owner(), Origin::Tainted).success);
  TEST_ASSERT_TRUE(callJson(nb, R"({"op":"remove","key":"gone"})", owner(), Origin::Unattended).success);
  std::vector<PendingNote> kept;
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_EQUAL(2, (int)kept.size());
  TEST_ASSERT_TRUE(kept[0].id != kept[1].id);
  TEST_ASSERT_TRUE(nb.approve(owner(), kept[0].id).success);
  TEST_ASSERT_EQUAL(1, (int)countKey(io.files["owner/pinned"], "k"));
  TEST_ASSERT_TRUE(nb.reject(owner(), kept[1].id).success);
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_EQUAL(0, (int)kept.size());
  TEST_ASSERT_FALSE(nb.approve(owner(), "p99").success);
  // A member's kept call is the member's, approved only on the member's own shelf.
  TEST_ASSERT_TRUE(callJson(nb, setJson("m", "v"), member("444"), Origin::Tainted).success);
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_EQUAL(0, (int)kept.size());
}

static void test_pending_full_is_an_error_and_changes_nothing() {
  MemIo io;
  io.capsFor["owner"] = Caps{4096, 4096, 120};
  Notebook nb(io);
  int kept = 0;
  ToolResult r;
  for (int i = 0; i < 10; ++i) {
    r = callJson(nb, setJson("k" + std::to_string(i), "a longer value here"), owner(), Origin::Tainted);
    if (!r.success) break;
    ++kept;
  }
  TEST_ASSERT_TRUE(kept > 0 && kept < 10);
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_TRUE(io.files["owner/pending"].size() <= 120);
  TEST_ASSERT_EQUAL(0, (int)io.files["owner/pinned"].size());
}

static void test_keep_is_the_rails_pending_store() {
  MemIo io;
  Notebook nb(io);
  JsonDocument d;
  deserializeJson(d, R"({"op":"set","key":"r","value":"from a page","kind":"rule"})");
  TEST_ASSERT_TRUE(nb.keep(d.as<JsonObjectConst>(), owner(), Origin::Tainted).success);
  std::vector<PendingNote> kept;
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_EQUAL(1, (int)kept.size());
  TEST_ASSERT_EQUAL(Kind::Rule, kept[0].note.kind);
  TEST_ASSERT_EQUAL(0, (int)io.files["owner/pinned"].size());
  deserializeJson(d, R"({"op":"set","key":"BAD KEY","value":"x"})");
  TEST_ASSERT_FALSE(nb.keep(d.as<JsonObjectConst>(), owner(), Origin::Tainted).success);   // invalid: not kept
}

// Review fixes (lane RB-N1 four-lens review): an id is never given twice, a kept call that left
// kind out keeps the old kind when approved, a refused approval waits again, and an approval is
// dropped before it runs (a cut loses it, never runs it twice).
static void test_pending_ids_are_never_reused() {
  MemIo io;
  Notebook nb(io);
  std::vector<std::string> given;
  for (int round = 0; round < 4; ++round) {
    TEST_ASSERT_TRUE(callJson(nb, setJson("k", "v" + std::to_string(round)), owner(), Origin::Tainted).success);
    std::vector<PendingNote> kept;
    TEST_ASSERT_TRUE(nb.pending(owner(), kept));
    TEST_ASSERT_EQUAL(1, (int)kept.size());
    for (const std::string& old : given) TEST_ASSERT_TRUE(old != kept[0].id);
    given.push_back(kept[0].id);
    TEST_ASSERT_TRUE((round % 2 ? nb.approve(owner(), kept[0].id) : nb.reject(owner(), kept[0].id)).success);
  }
  // An answer to an old id reaches nothing.
  TEST_ASSERT_TRUE(callJson(nb, setJson("other", "new call"), owner(), Origin::Tainted).success);
  TEST_ASSERT_FALSE(nb.approve(owner(), given.back()).success);
  TEST_ASSERT_EQUAL(0, (int)countKey(io.files["owner/pinned"], "other"));
}

static void test_approved_call_that_left_kind_out_keeps_the_old_kind() {
  for (Kind before : {Kind::Fact, Kind::Rule}) {
    MemIo io;
    Notebook nb(io);
    TEST_ASSERT_TRUE(callJson(nb, setJson("k", "old", 1, before == Kind::Rule ? "rule" : "fact"), owner()).success);
    TEST_ASSERT_TRUE(callJson(nb, setJson("k", "new"), owner(), Origin::Unattended).success);
    std::vector<PendingNote> kept;
    TEST_ASSERT_TRUE(nb.pending(owner(), kept));
    TEST_ASSERT_EQUAL(1, (int)kept.size());
    TEST_ASSERT_FALSE(kept[0].kindGiven);
    TEST_ASSERT_TRUE(nb.approve(owner(), kept[0].id).success);
    const std::string want = before == Kind::Rule ? "k [rule]: new\n" : "k: new\n";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), io.files["owner/pinned"].c_str());
  }
}

static void test_refused_approval_waits_again_and_a_cut_never_runs_twice() {
  MemIo io;
  io.capsFor["owner"] = Caps{16, 16, 512};
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("big", "a value too long for either shelf"), owner(), Origin::Tainted).success);
  std::vector<PendingNote> kept;
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_FALSE(nb.approve(owner(), kept[0].id).success);   // no room: refused
  std::vector<PendingNote> again;
  TEST_ASSERT_TRUE(nb.pending(owner(), again));
  TEST_ASSERT_EQUAL(1, (int)again.size());
  TEST_ASSERT_EQUAL_STRING(kept[0].id.c_str(), again[0].id.c_str());
  // The pending write is the first write of an approval: when it is refused nothing ran.
  io.capsFor["owner"] = kOwnerCaps;
  io.ops.clear();
  io.failWrites = true;
  TEST_ASSERT_FALSE(nb.approve(owner(), kept[0].id).success);
  size_t firstWrite = io.ops.size();
  for (size_t i = 0; i < io.ops.size(); ++i)
    if (io.ops[i].rfind("write ", 0) == 0) {
      firstWrite = i;
      break;
    }
  TEST_ASSERT_TRUE(firstWrite < io.ops.size());
  TEST_ASSERT_EQUAL_STRING("write owner/pending", io.ops[firstWrite].c_str());
  TEST_ASSERT_EQUAL(0, (int)io.files["owner/pinned"].size());
}

static void test_blank_values_c1_controls_and_crlf_lines() {
  TEST_ASSERT_FALSE(validValue(" "));
  TEST_ASSERT_FALSE(validValue("   "));
  TEST_ASSERT_TRUE(validValue(" a "));
  TEST_ASSERT_FALSE(validValue("a\xc2\x85" "b"));      // U+0085
  TEST_ASSERT_FALSE(validValue("a\xe2\x80\xa8" "b"));  // U+2028
  TEST_ASSERT_FALSE(validValue("a\xe2\x80\xa9" "b"));  // U+2029
  TEST_ASSERT_TRUE(validValue("a\xc2\xa0" "b"));       // U+00A0 is text
  TEST_ASSERT_TRUE(validValue("a\xe2\x80\x99" "b"));   // U+2019 is text
  MemIo io;
  io.files["owner/pinned"] = "tea: green\r\ncoffee: none\r\n";   // a hand edit with CRLF
  Notebook nb(io);
  TEST_ASSERT_EQUAL_STRING("tea: green\ncoffee: none\n", nb.pinnedBlock(owner()).c_str());
}

// ---- per person ---------------------------------------------------------------------------------
static void test_each_person_reads_and_writes_only_their_own() {
  MemIo io;
  Notebook nb(io);
  const Principal people[] = {owner(), member("501"), member("-100502")};
  for (const Principal& p : people) {
    const std::string pk = personKey(p);
    TEST_ASSERT_TRUE(callJson(nb, setJson("who", "I am " + pk, 1), p).success);
    TEST_ASSERT_TRUE(callJson(nb, setJson("where", "more of " + pk, 0), p).success);
  }
  for (const Principal& p : people) {
    const std::string pk = personKey(p);
    const std::string block = nb.pinnedBlock(p);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, block.find("I am " + pk));
    std::vector<Note> more;
    TEST_ASSERT_TRUE(nb.unpinned(p, more));
    TEST_ASSERT_EQUAL(1, (int)more.size());
    TEST_ASSERT_EQUAL_STRING(("more of " + pk).c_str(), more[0].value.c_str());
    for (const Principal& q : people) {
      if (personKey(q) == pk) continue;
      TEST_ASSERT_EQUAL(std::string::npos, block.find("I am " + personKey(q) + "\n"));
    }
  }
  for (const auto& f : io.files) {   // every file is under one of the three persons
    const std::string person = f.first.substr(0, f.first.find('/'));
    TEST_ASSERT_TRUE(person == "owner" || person == "501" || person == "-100502");
  }
  // A Principal that names no person a file can be keyed on reads and writes nothing.
  Principal odd;
  odd.ns = "chat:../owner";
  TEST_ASSERT_EQUAL_STRING("", personKey(odd).c_str());
  const size_t ops = io.ops.size();
  TEST_ASSERT_FALSE(callJson(nb, setJson("x", "y"), odd).success);
  TEST_ASSERT_EQUAL_STRING("", nb.pinnedBlock(odd).c_str());
  TEST_ASSERT_EQUAL(ops, io.ops.size());
  // No argument names a person or a path.
  TEST_ASSERT_FALSE(callJson(nb, R"({"op":"set","key":"x","value":"y","person":"owner"})", member("501")).success);
}

// ---- the rules of a line ----------------------------------------------------------------------
static void test_key_and_value_rules() {
  TEST_ASSERT_TRUE(validKey("a"));
  TEST_ASSERT_TRUE(validKey("abc-123"));
  TEST_ASSERT_TRUE(validKey(std::string(kKeyMax, 'k')));
  TEST_ASSERT_FALSE(validKey(""));
  TEST_ASSERT_FALSE(validKey(std::string(kKeyMax + 1, 'k')));
  for (char c : std::string("ABZ _:./\\[]\n\t\x01"))
    TEST_ASSERT_FALSE(validKey(std::string("ab") + c));
  TEST_ASSERT_TRUE(validValue("x"));
  TEST_ASSERT_TRUE(validValue(std::string(kValueMax, 'v')));
  TEST_ASSERT_TRUE(validValue("caf\xc3\xa9 au lait"));
  TEST_ASSERT_FALSE(validValue(""));
  TEST_ASSERT_FALSE(validValue(std::string(kValueMax + 1, 'v')));
  for (int c = 0; c < 32; ++c) TEST_ASSERT_FALSE(validValue(std::string("a") + char(c) + "b"));
  TEST_ASSERT_FALSE(validValue("a\x7f"));
  TEST_ASSERT_FALSE(validValue("bad \xc3"));
  // Every line formatLine writes parses back to the same note, both kinds.
  for (Kind k : {Kind::Fact, Kind::Rule}) {
    Note n{"key-1", "value: with colon [rule] inside", k}, back;
    TEST_ASSERT_TRUE(parseLine(formatLine(n), back));
    TEST_ASSERT_EQUAL_STRING(n.key.c_str(), back.key.c_str());
    TEST_ASSERT_EQUAL_STRING(n.value.c_str(), back.value.c_str());
    TEST_ASSERT_EQUAL(k, back.kind);
  }
  TEST_ASSERT_EQUAL_STRING("a: b", formatLine(Note{"a", "b", Kind::Fact}).c_str());
  TEST_ASSERT_EQUAL_STRING("a [rule]: b", formatLine(Note{"a", "b", Kind::Rule}).c_str());
  // Calls: bad key, bad value, bad op, a set with no value, an unknown argument: errors, nothing written.
  const char* bad[] = {R"({"op":"set","key":"No","value":"v"})",   R"({"op":"set","key":"k","value":""})",
                       R"({"op":"put","key":"k","value":"v"})",    R"({"op":"set","key":"k"})",
                       R"({"op":"set","key":"k","value":"v","x":1})", R"({"op":"set","key":"k","value":"v","kind":"x"})",
                       R"({"key":"k","value":"v"})",                R"({"op":"set","key":"k","value":"v","pin":"yes"})"};
  for (const char* b : bad) {
    MemIo io;
    Notebook nb(io);
    const ToolResult r = callJson(nb, b, owner());
    TEST_ASSERT_FALSE_MESSAGE(r.success, b);
    TEST_ASSERT_EQUAL(0, (int)io.files.size());
  }
  MemIo io;
  Notebook nb(io);
  const ToolResult r = callJson(nb, R"({"op":"set","key":"k","value":"v","colour":"red"})", owner());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, r.error.find("colour"));
  for (const char* a : noteArgs()) TEST_ASSERT_NOT_EQUAL(std::string::npos, r.error.find(a));
}

static void test_the_schema_offers_exactly_the_arguments_read() {
  const auto* d = agent::plain::toolDecl("note");
  TEST_ASSERT_NOT_NULL(d);
  JsonDocument s;
  TEST_ASSERT_FALSE(deserializeJson(s, agent::plain::declSchema(*d, false)));
  std::set<std::string> offered, read;
  for (JsonPairConst p : s["properties"].as<JsonObjectConst>()) offered.insert(p.key().c_str());
  for (const char* a : noteArgs()) read.insert(a);
  TEST_ASSERT_TRUE(offered == read);
}

static void test_result_states_the_line_and_space_used() {
  MemIo io;
  Notebook nb(io);
  const ToolResult r = callJson(nb, setJson("tea", "green", 1, "rule"), owner());
  TEST_ASSERT_TRUE(r.success);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, r.output.find("tea [rule]: green"));
  const std::string used = "used " + groupedBytes(io.files["owner/pinned"].size()) + " of 4,096 B";
  TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, r.output.find(used), r.output.c_str());
  TEST_ASSERT_EQUAL_STRING("1,812", groupedBytes(1812).c_str());
  TEST_ASSERT_EQUAL_STRING("32,724", groupedBytes(32724).c_str());
  TEST_ASSERT_EQUAL_STRING("7", groupedBytes(7).c_str());
  TEST_ASSERT_EQUAL_STRING("1,000,000", groupedBytes(1000000).c_str());
  const ToolResult rm = callJson(nb, R"({"op":"remove","key":"tea"})", owner());
  TEST_ASSERT_TRUE(rm.success);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, rm.output.find("used 0 of 4,096 B"));
  TEST_ASSERT_FALSE(callJson(nb, R"({"op":"remove","key":"tea"})", owner()).success);
}

// ---- storage faults ------------------------------------------------------------------------------
static void test_a_refused_write_changes_nothing() {
  MemIo io;
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("k", "v1", 1), owner()).success);
  const std::string before = both(io, "owner");
  io.failWrites = true;
  const char* calls[] = {R"({"op":"set","key":"k","value":"v2"})", R"({"op":"set","key":"k","value":"v2","pin":false})",
                         R"({"op":"remove","key":"k"})", R"({"op":"set","key":"n","value":"v"})"};
  for (const char* c : calls) {
    const ToolResult r = callJson(nb, c, owner());
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), both(io, "owner").c_str());
  }
}

static void test_lines_that_are_not_notes_are_kept_by_every_rewrite() {
  MemIo io;
  io.files["owner/pinned"] = "# my notes\nk: v\nnot a note line\n";
  Notebook nb(io);
  TEST_ASSERT_TRUE(callJson(nb, setJson("k", "v2"), owner()).success);
  TEST_ASSERT_TRUE(callJson(nb, setJson("n", "x", 1), owner()).success);
  TEST_ASSERT_TRUE(callJson(nb, R"({"op":"remove","key":"n"})", owner()).success);
  const std::string& f = io.files["owner/pinned"];
  TEST_ASSERT_NOT_EQUAL(std::string::npos, f.find("# my notes\n"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, f.find("not a note line\n"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, f.find("k: v2\n"));
}

static void test_a_refused_allocation_is_an_error_result() {
  MemIo io;
  Notebook nb(io);
  io.throwOnRead = true;
  const ToolResult r = callJson(nb, setJson("k", "v"), owner());
  TEST_ASSERT_FALSE(r.success);
  TEST_ASSERT_EQUAL(0, (int)io.files.size());
  TEST_ASSERT_EQUAL_STRING("", nb.pinnedBlock(owner()).c_str());
}

// ---- sizes ---------------------------------------------------------------------------------------
static void test_owner_files_take_at_most_80KB_of_flash_with_the_atomic_copy() {
  TEST_ASSERT_EQUAL(0, (int)fsBlocks(0));
  TEST_ASSERT_EQUAL(1, (int)fsBlocks(1));
  TEST_ASSERT_EQUAL(1, (int)fsBlocks(4096));
  TEST_ASSERT_EQUAL(2, (int)fsBlocks(4097));
  TEST_ASSERT_EQUAL(8, (int)fsBlocks(32724));   // CTZ: blocks 1..7 carry 11 pointers (44 B)
  TEST_ASSERT_EQUAL(9, (int)fsBlocks(32725));
  TEST_ASSERT_EQUAL(9, (int)fsBlocks(32768));
  TEST_ASSERT_TRUE(flashBound(kOwnerCaps) <= 80u * 1024u);
  TEST_ASSERT_EQUAL(20u * 4096u, flashBound(kOwnerCaps));
  // Counter-test: the PRD's 32,768 B unpinned would pass 80 KB.
  TEST_ASSERT_TRUE(flashBound(Caps{4096, 32768, 2048}) > 80u * 1024u);
  // The caps hold at the seam: no write passes them.
  MemIo io;
  Notebook nb(io);
  for (int i = 0; i < 400; ++i) (void)callJson(nb, setJson("k" + std::to_string(i), std::string(kValueMax, 'v')), owner());
  TEST_ASSERT_TRUE(io.files["owner/pinned"].size() <= kOwnerCaps.pinned);
  TEST_ASSERT_TRUE(io.files["owner/more"].size() <= kOwnerCaps.more);
}

static void test_pinned_block_fits_the_request_ceiling() {
  TEST_ASSERT_TRUE(kOwnerCaps.pinned <= agent::plain::kPinnedNotesCeiling);
  TEST_ASSERT_TRUE(kMemberCaps.pinned <= agent::plain::kPinnedNotesCeiling);
  TEST_ASSERT_TRUE(kHostedCaps.pinned <= agent::plain::kPinnedNotesCeiling);
  MemIo io;
  io.files["owner/pinned"] = std::string(6000, 'a') + "\nk: v\n";   // an oversized file (an older build)
  Notebook nb(io);
  TEST_ASSERT_TRUE(nb.pinnedBlock(owner()).size() <= kOwnerCaps.pinned);
  io.files["owner/pinned"] = "# heading\nk: v\nk2 [rule]: w\n";
  TEST_ASSERT_EQUAL_STRING("k: v\nk2 [rule]: w\n", nb.pinnedBlock(owner()).c_str());
}

static void test_registered_handler_reads_the_origin() {
  MemIo io;
  Notebook nb(io);
  ToolRegistry reg;
  Origin now = Origin::Attended;
  const auto* d = agent::plain::toolDecl("note");
  nb.registerInto(reg, d->description, agent::plain::declSchema(*d, false), [&] { return now; });
  JsonDocument a;
  deserializeJson(a, setJson("k", "v", 1));
  TEST_ASSERT_TRUE(reg.dispatch("note", a.as<JsonObjectConst>(), owner()).success);
  TEST_ASSERT_EQUAL(1, (int)countKey(io.files["owner/pinned"], "k"));
  now = Origin::Unattended;
  deserializeJson(a, setJson("k", "changed", 1));
  TEST_ASSERT_TRUE(reg.dispatch("note", a.as<JsonObjectConst>(), owner()).success);
  TEST_ASSERT_EQUAL(std::string::npos, io.files["owner/pinned"].find("changed"));
  std::vector<PendingNote> kept;
  TEST_ASSERT_TRUE(nb.pending(owner(), kept));
  TEST_ASSERT_EQUAL(1, (int)kept.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_set_saves_first_call_with_file_ops_only);
  RUN_TEST(test_owner_saves_with_no_card_and_member_is_refused_in_one_line);
  RUN_TEST(test_one_value_per_key_over_every_sequence);
  RUN_TEST(test_a_set_heals_a_key_left_on_both_shelves);
  RUN_TEST(test_pinned_full_stores_unpinned_and_says_so);
  RUN_TEST(test_both_full_is_an_error_naming_the_largest_keys_and_changes_nothing);
  RUN_TEST(test_tainted_or_unattended_set_and_remove_go_pending);
  RUN_TEST(test_pending_approve_runs_it_and_reject_drops_it);
  RUN_TEST(test_pending_full_is_an_error_and_changes_nothing);
  RUN_TEST(test_keep_is_the_rails_pending_store);
  RUN_TEST(test_each_person_reads_and_writes_only_their_own);
  RUN_TEST(test_key_and_value_rules);
  RUN_TEST(test_the_schema_offers_exactly_the_arguments_read);
  RUN_TEST(test_result_states_the_line_and_space_used);
  RUN_TEST(test_a_refused_write_changes_nothing);
  RUN_TEST(test_lines_that_are_not_notes_are_kept_by_every_rewrite);
  RUN_TEST(test_a_refused_allocation_is_an_error_result);
  RUN_TEST(test_owner_files_take_at_most_80KB_of_flash_with_the_atomic_copy);
  RUN_TEST(test_pinned_block_fits_the_request_ceiling);
  RUN_TEST(test_registered_handler_reads_the_origin);
  RUN_TEST(test_pending_ids_are_never_reused);
  RUN_TEST(test_approved_call_that_left_kind_out_keeps_the_old_kind);
  RUN_TEST(test_refused_approval_waits_again_and_a_cut_never_runs_twice);
  RUN_TEST(test_blank_values_c1_controls_and_crlf_lines);
  return UNITY_END();
}
