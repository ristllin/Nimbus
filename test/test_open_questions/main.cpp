#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/open_questions.h"
#include "nimbus/orch/openq_tools.h"
#include "nimbus/orch/tool_registry.h"

// The owner's open-question list (lane POL2): the store at /data/openq.txt, the
// deterministic raise-once observation, and the openq.edit rails. Encodes the
// class from the 2 Nov signals: "an item the owner left open is raised at most
// once, then only when they return to it" - as properties, not one point case.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

// ---- a fake file (the same seam the rules use) --------------------------------
struct FakeIO : PrefsStoreIO {
  bool exists = false;
  std::string blob;
  bool failNext = false;
  int saves = 0;
  bool load(std::string& out) override {
    if (!exists) return false;
    out = blob;
    return true;
  }
  bool saveChecked(const std::string& b) override {
    if (failNext) {
      failNext = false;
      return false;
    }
    saves++;
    exists = true;
    blob = b;
    return true;
  }
};

struct World {
  FakeIO io;
  OpenQuestions q;
  bool scheduled = false;
  bool tainted = false;
  OpenQToolContext ctx() {
    OpenQToolContext c;
    c.withOpenQ = [this](const std::function<void(OpenQuestions&)>& fn) { fn(q); return true; };
    c.inScheduledTurn = [this] { return scheduled; };
    c.externalContentSeen = [this] { return tainted; };
    c.nowHours = [] { return 77u; };
    return c;
  }
};

ToolResult callTool(ToolRegistry& reg, const char* action, const char* id, const char* text,
                    const Principal& who) {
  JsonDocument d;
  d["action"] = action;
  if (id) d["id"] = id;
  if (text) d["text"] = text;
  return reg.dispatch("openq.edit", d.as<ArduinoJson::JsonObjectConst>(), who);
}

}  // namespace

// ---- 1. the store: fresh device, first add, reload -------------------------------
static void test_fresh_device_and_first_add() {
  World w;
  w.q.begin(&w.io);
  TEST_ASSERT_FALSE(w.q.fileExisted());
  TEST_ASSERT_EQUAL(0, (int)w.q.count());
  TEST_ASSERT_EQUAL_STRING("", w.q.renderBlock().c_str());
  std::string id;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(w.q.add("Photograph the skyr pot label next time", 5, &id)));
  TEST_ASSERT_EQUAL_STRING("q1", id.c_str());
  TEST_ASSERT_TRUE(w.io.blob.rfind("#nimbus-openq v1 ", 0) == 0);
  TEST_ASSERT_TRUE(w.io.blob.find("next=2\n") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("- (q1) Photograph the skyr pot label next time", w.q.renderBlock().c_str());
  // A reload sees the same item; ids are never reused while the file exists.
  OpenQuestions again;
  again.begin(&w.io);
  TEST_ASSERT_TRUE(again.fileExisted());
  TEST_ASSERT_EQUAL(1, (int)again.count());
  TEST_ASSERT_EQUAL_STRING("Photograph the skyr pot label next time", again.entries()[0].text.c_str());
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(again.remove(1)));
  std::string id2;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(again.add("Another open item", 6, &id2)));
  TEST_ASSERT_TRUE(id2 != id);
}

// ---- 2. caps, words and idempotent adds ------------------------------------------
static void test_caps_refuse_with_plain_words() {
  World w;
  w.q.begin(&w.io);
  std::string id;
  TEST_ASSERT_EQUAL(int(OpenQErr::Empty), int(w.q.add(" \n\t ", 0, nullptr)));
  TEST_ASSERT_EQUAL(int(OpenQErr::TooLong), int(w.q.add(std::string(161, 'a'), 0, nullptr)));
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(w.q.add(std::string(160, 'a'), 0, &id)));
  for (int i = 0; i < 4; i++) w.q.add("open item number " + std::to_string(i), 0, nullptr);
  TEST_ASSERT_EQUAL(5, (int)w.q.count());
  TEST_ASSERT_EQUAL(int(OpenQErr::Full), int(w.q.add("one more open item", 0, nullptr)));
  // Re-adding an existing item (same key, different words) is idempotent.
  std::string again;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(w.q.add("OPEN   item number 2 ", 0, &again)));
  TEST_ASSERT_EQUAL_STRING("q4", again.c_str());   // q1 was the 'a' rule, then number 0..4 = q2..q6
  TEST_ASSERT_EQUAL(5, (int)w.q.count());
  TEST_ASSERT_EQUAL(int(OpenQErr::NotFound), int(w.q.remove(99)));
  for (int e = 0; e <= int(OpenQErr::PersistFailed); e++) {
    const std::string words = openQErrWords(OpenQErr(e));
    TEST_ASSERT_TRUE(!words.empty());
    TEST_ASSERT_TRUE(words.find("\xE2\x80\x94") == std::string::npos);
  }
}

// ---- 3. a failed save changes nothing ---------------------------------------------
static void test_persist_failure_changes_nothing() {
  World w;
  w.q.begin(&w.io);
  w.q.add("Photograph the skyr pot label next time", 1, nullptr);
  const std::string before = w.q.serialize();
  w.io.failNext = true;
  TEST_ASSERT_EQUAL(int(OpenQErr::PersistFailed), int(w.q.add("Another item", 2, nullptr)));
  TEST_ASSERT_EQUAL_STRING(before.c_str(), w.q.serialize().c_str());
  TEST_ASSERT_EQUAL(1, (int)w.q.count());
}

// ---- 4. tolerant parse: a damaged file never crashes or passes the caps -----------
static void test_damaged_files_parse_within_the_caps() {
  World w;
  w.q.begin(&w.io);
  for (int i = 0; i < 3; i++) w.q.add("open item number " + std::to_string(i), i, nullptr);
  const std::string good = w.q.serialize();
  for (size_t cut = 0; cut <= good.size(); cut++) {
    OpenQuestions q;
    q.parse(good.substr(0, cut));
    TEST_ASSERT_TRUE(q.count() <= kOpenQMaxItems);
  }
  // Duplicate ids and duplicate keys skip; the cap holds; CRLF is tolerated;
  // the raised flag round-trips.
  std::string crlf;
  for (int i = 1; i <= 12; i++)
    crlf += "q" + std::to_string(i % 7 + 1) + "\t" + (i % 2 ? "R" : "-") + "\t" +
            std::to_string(i) + "\tline " + std::to_string(i % 9) + "\r\n";
  OpenQuestions q;
  q.parse(crlf);
  TEST_ASSERT_EQUAL(5, (int)q.count());   // the cap keeps the first five intact lines
  TEST_ASSERT_TRUE(q.find(2)->raised);
  TEST_ASSERT_FALSE(q.find(3)->raised);
  // next= is honored and never reuses an id the file already used.
  FakeIO nio;
  OpenQuestions n;
  n.begin(&nio);
  n.parse("#nimbus-openq v1 next=9\nq7\t-\t3\tan item\n");
  std::string id;
  TEST_ASSERT_EQUAL(int(OpenQErr::Ok), int(n.add("a new item", 4, &id)));
  TEST_ASSERT_EQUAL_STRING("q9", id.c_str());
  // A hand-written over-long item is cut to the cap, never kept whole.
  OpenQuestions l;
  l.parse("q1\t-\t0\t" + std::string(400, 'x') + "\n");
  TEST_ASSERT_TRUE(l.entries().size() == 0 || l.entries()[0].text.size() <= kOpenQItemMax);
}

// ---- 5. the raise-once observation -------------------------------------------------
static void test_a_reply_marks_an_item_raised_once() {
  World w;
  w.q.begin(&w.io);
  w.q.add("Photograph the skyr pot label next time", 1, nullptr);
  // A reply that mentions the subject marks it raised; the block says so.
  std::string log = observeOpenQ(w.q, "hi", "By the way, could you photograph the skyr pot label for me?");
  TEST_ASSERT_TRUE(log.find("marked raised") != std::string::npos);
  TEST_ASSERT_TRUE(w.q.entries()[0].raised);
  TEST_ASSERT_TRUE(w.q.renderBlock().find("[raised") != std::string::npos);
  // A second reply that mentions it changes nothing: raised is once.
  TEST_ASSERT_TRUE(observeOpenQ(w.q, "hi", "and the skyr pot label again").empty());
  TEST_ASSERT_TRUE(w.q.entries()[0].raised);
  // Persisted: a reload still knows it was raised.
  OpenQuestions again;
  again.begin(&w.io);
  TEST_ASSERT_TRUE(again.entries()[0].raised);
}

static void test_the_owner_returning_reopens_an_item() {
  World w;
  w.q.begin(&w.io);
  w.q.add("Photograph the skyr pot label next time", 1, nullptr);
  observeOpenQ(w.q, "hi", "could you photograph the skyr pot label?");
  TEST_ASSERT_TRUE(w.q.entries()[0].raised);
  // The owner's own message mentioning the subject re-opens it.
  const std::string log = observeOpenQ(w.q, "what about that skyr pot label then", "on it");
  TEST_ASSERT_TRUE(log.find("re-opened") != std::string::npos);
  TEST_ASSERT_FALSE(w.q.entries()[0].raised);
}

static void test_the_owner_withdrawing_removes_an_item() {
  World w;
  w.q.begin(&w.io);
  w.q.add("Photograph the skyr pot label next time", 1, nullptr);
  w.q.add("Check the shed roof", 1, nullptr);
  // A drop cue without the subject removes nothing.
  TEST_ASSERT_TRUE(observeOpenQ(w.q, "forget it all", "ok").empty());
  TEST_ASSERT_EQUAL(2, (int)w.q.count());
  // A drop cue paired with the subject removes that one item.
  const std::string log = observeOpenQ(w.q, "forget the skyr pot label, it is sorted", "ok");
  TEST_ASSERT_TRUE(log.find("removed") != std::string::npos);
  TEST_ASSERT_EQUAL(1, (int)w.q.count());
  TEST_ASSERT_TRUE(w.q.renderBlock().find("skyr") == std::string::npos);
  TEST_ASSERT_TRUE(w.q.renderBlock().find("shed roof") != std::string::npos);
}

// The dimension the matcher must not swallow: a reply that shares only furniture
// words does not mark the item raised, and a mention needs half the subject.
static void test_a_mention_needs_the_subject() {
  World w;
  w.q.begin(&w.io);
  w.q.add("Photograph the skyr pot label next time", 1, nullptr);
  TEST_ASSERT_TRUE(observeOpenQ(w.q, "hi", "The time is 06:00.").empty());
  TEST_ASSERT_FALSE(w.q.entries()[0].raised);
  TEST_ASSERT_FALSE(observeOpenQ(w.q, "hi", "Sure - what about the skyr pot?").empty());
  TEST_ASSERT_TRUE(w.q.entries()[0].raised);
}

// ---- 6. the openq.edit rails: role x turn kind x taint x action --------------------
static void test_openq_edit_rails_matrix() {
  static_assert(int(Role::Admin) == 3, "a new Role needs a row here");
  const bool ownerRole[] = {false, false, false, true};   // Unknown, Guest, User, Admin
  int cells = 0;
  for (int r = 0; r <= int(Role::Admin); r++)
    for (int sched = 0; sched < 2; sched++)
      for (int taint = 0; taint < 2; taint++)
        for (int action = 0; action < 3; action++) {
          World w;
          w.q.begin(&w.io);
          w.q.add("Photograph the skyr pot label next time", 1, nullptr);
          w.scheduled = sched == 1;
          w.tainted = taint == 1;
          const std::string before = w.q.serialize();
          ToolRegistry reg;
          registerOpenQTools(reg, w.ctx());
          const Principal who =
              principalForRole(ownerRole[r] ? "owner-chat" : "member-chat", Role(r));
          static const char* const kActions[] = {"add", "remove", "list"};
          const ToolResult res = callTool(reg, kActions[action], "q1", "Check the shed roof", who);
          const bool isList = action == 2;
          // Owner: list always; add/remove only attended and untainted. Anyone
          // else: nothing, not even the list.
          const bool expectOk = ownerRole[r] && (isList || (!w.scheduled && !w.tainted));
          TEST_ASSERT_EQUAL_MESSAGE(expectOk, res.success,
                                    (std::string(kActions[action]) + " role " + roleName(Role(r))).c_str());
          if (!expectOk)
            TEST_ASSERT_EQUAL_STRING(before.c_str(), w.q.serialize().c_str());   // nothing changed
          cells++;
        }
  TEST_ASSERT_EQUAL(4 * 2 * 2 * 3, cells);
}

// The tool is owner-only in the advertisement too.
static void test_openq_edit_is_never_advertised_to_a_member() {
  World w;
  w.q.begin(&w.io);
  ToolRegistry reg;
  registerOpenQTools(reg, w.ctx());
  TEST_ASSERT_TRUE(reg.isAdminOnly("openq.edit"));
  bool memberSees = false, ownerSees = false;
  for (const auto& s : reg.toolSpecsFor(principalForRole("m", Role::User))) memberSees |= s.name == "openq.edit";
  for (const auto& s : reg.toolSpecsFor(principalForRole("o", Role::Admin))) ownerSees |= s.name == "openq.edit";
  TEST_ASSERT_FALSE(memberSees);
  TEST_ASSERT_TRUE(ownerSees);
}

// ---- 7. the tool's own flows --------------------------------------------------------
static void test_openq_edit_flows() {
  World w;
  w.q.begin(&w.io);
  ToolRegistry reg;
  registerOpenQTools(reg, w.ctx());
  const Principal owner = principalForRole("owner-chat", Role::Admin);
  const ToolResult add = callTool(reg, "add", nullptr, "Photograph the skyr pot label next time", owner);
  TEST_ASSERT_TRUE(add.success);
  TEST_ASSERT_TRUE(add.output.find("saved as open item q1") != std::string::npos);
  TEST_ASSERT_TRUE(add.output.find("at most once") != std::string::npos);
  const ToolResult list = callTool(reg, "list", nullptr, nullptr, owner);
  TEST_ASSERT_TRUE(list.success);
  TEST_ASSERT_TRUE(list.output.find("(q1) Photograph the skyr pot label") == 0);
  const ToolResult rm = callTool(reg, "remove", "q1", nullptr, owner);
  TEST_ASSERT_TRUE(rm.success);
  TEST_ASSERT_EQUAL(0, (int)w.q.count());
  TEST_ASSERT_FALSE(callTool(reg, "remove", "q1", nullptr, owner).success);   // gone
  TEST_ASSERT_FALSE(callTool(reg, "frobnicate", nullptr, nullptr, owner).success);
  TEST_ASSERT_FALSE(callTool(reg, "remove", "zzz", nullptr, owner).success);
  TEST_ASSERT_FALSE(callTool(reg, "remove", nullptr, nullptr, owner).success);
  // An unavailable store refuses without crashing.
  OpenQToolContext empty;
  ToolRegistry bare;
  registerOpenQTools(bare, empty);
  TEST_ASSERT_FALSE(callTool(bare, "list", nullptr, nullptr, owner).success);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_device_and_first_add);
  RUN_TEST(test_caps_refuse_with_plain_words);
  RUN_TEST(test_persist_failure_changes_nothing);
  RUN_TEST(test_damaged_files_parse_within_the_caps);
  RUN_TEST(test_a_reply_marks_an_item_raised_once);
  RUN_TEST(test_the_owner_returning_reopens_an_item);
  RUN_TEST(test_the_owner_withdrawing_removes_an_item);
  RUN_TEST(test_a_mention_needs_the_subject);
  RUN_TEST(test_openq_edit_rails_matrix);
  RUN_TEST(test_openq_edit_is_never_advertised_to_a_member);
  RUN_TEST(test_openq_edit_flows);
  return UNITY_END();
}
