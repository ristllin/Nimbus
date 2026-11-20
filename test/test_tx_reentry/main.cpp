// The re-entry rule (lane RB-MG1a, PRD section 9 "Going back and forward again"): a second
// migration over an existing /mem/tx imports only the rows the other engine wrote since, as text
// behind one device note; it never duplicates, drops or reorders an item, and it resumes after a
// power cut at any write. Properties over random sequences of engine-1 rows, engine-2 turns and
// interrupted migrations.
#include <unity.h>

#include <cstdlib>
#include <memory>
#include <random>

#include "../test_tx_migrate/world.h"

using namespace mg;

void setUp() {}
void tearDown() {}

namespace {

const char* kGood = "The person planned the mooring repair and asked for short answers; the walk is still open.";

struct World {
  txb::MemFiles files;
  OldStore old{files};
  StoreSource src{old, kChat};
  Month m;
  int asks = 0;
  uint64_t clock = kMonthStart + 20 * kDay;
  std::mt19937 rng{1};

  tx::MigrateDeps deps() {
    tx::MigrateDeps d;
    d.files = &files;
    d.chatKey = kChatKey;
    d.rows = &src;
    d.fold = old.oldFold();
    d.digest0 = [this](const tx::Digest0Ask& a, tx::Digest0Answer& out) {
      ++asks;
      out.summary = kGood;
      out.day = dayOf(a.at - 4 * kHour);
      return tx::digestText(out.day, out.summary, out.text);
    };
    d.exportText = [](const std::string&, const std::string&) { return true; };
    d.now = clock;
    d.head = tx::HeadKey{"anthropic", "claude-sonnet-5-5", false};   // the import is sized per head (RB-H81)
    return d;
  }
  tx::MigrationOutcome done(const char* when) {
    tx::MigrationOutcome out;
    const tx::Status s = tx::migrate(deps(), out);
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: %s at %s", when, tx::errSlug(s.err), s.where);
    TEST_ASSERT_TRUE_MESSAGE(s.ok(), msg);
    TEST_ASSERT_EQUAL_MESSAGE((int)tx::MigrationStep::Done, (int)out.step, when);
    return out;
  }
  // Through cuts: each call may be cut; the next one resumes.
  tx::MigrationOutcome resume(const char* when) {
    tx::MigrationOutcome out;
    for (int i = 0; i < 6; ++i) {
      bool fin = false;
      try {
        fin = tx::migrate(deps(), out).ok() && out.step == tx::MigrationStep::Done;
      } catch (const txb::Killed&) {
      }
      files.clearFaults();   // a cut that did not come (nothing was written) is not left armed
      if (fin) return out;
    }
    TEST_FAIL_MESSAGE(when);
    return out;
  }
  // The old engine answers `n` messages.
  void engine1(int n) {
    for (int i = 0; i < n; ++i) {
      clock += 600 + rng() % 4000;
      exchange(old, rng, clock);
    }
  }
  // Engine 2 starts (the migration first, as the binding does) and answers one message: its rows
  // carry "e2", its items go to the log. `store` false: the engine stores without migrating first
  // (a binding that broke the contract).
  tx::MigrationOutcome engine2(const std::string& text, bool migrateFirst = true) {
    tx::MigrationOutcome started;
    if (migrateFirst) started = resume("engine 2 start");
    clock += 900;
    const uint64_t row = old.add(kChat, clock, "user", "message", "E2 " + text, "e2");
    old.add(kChat, clock, "assistant", "message", "E2 reply to " + text, "e2");
    tx::TxLog log(files, kChatKey);
    tx::HeadState head;
    TEST_ASSERT_TRUE(log.open(head).ok());
    uint32_t turn = 0;
    for (const tx::Item& it : stored(files)) turn = std::max(turn, it.turn);
    ++turn;
    tx::StoreState st;
    tx::Item u;
    u.kind = tx::ItemKind::User, u.q = head.nextQ, u.turn = turn, u.t = clock, u.x = "E2 " + text, u.m = row;
    TEST_ASSERT_TRUE(log.append(u, st).ok());
    tx::Item a;
    a.kind = tx::ItemKind::Assistant, a.q = head.nextQ + 1, a.turn = turn, a.t = clock, a.x = "E2 reply to " + text, a.fin = true;
    TEST_ASSERT_TRUE(log.append(a, st).ok());
    tx::Item e;
    e.kind = tx::ItemKind::TurnEnd, e.q = head.nextQ + 2, e.turn = turn, e.t = clock, e.end.cause = "reply";
    TEST_ASSERT_TRUE(log.append(e, st).ok());
    head.nextQ += 3;
    head.mHi = std::max(head.mHi, row);
    TEST_ASSERT_TRUE(log.writeHead(head).ok());
    return started;
  }
  uint32_t notes() {
    uint32_t n = 0;
    for (const tx::Item& it : stored(files)) n += importNoteText(it);
    return n;
  }
};

void firstMigration(World& w, uint32_t seed) {
  w.rng.seed(seed);
  w.m = month(w.old, {seed, 400, 6, 3, 5, 800});
  w.clock = w.m.lastT + kHour;
  w.done("first");
}

void test_a_second_migration_is_a_reentry_that_imports_only_the_other_engines_rows() {
  World w;
  firstMigration(w, 3);
  bool re = false;
  TEST_ASSERT_TRUE(tx::isReentry(w.files, kChatKey, re).ok());
  TEST_ASSERT_TRUE(re);
  const uint32_t notes = w.notes();
  w.engine2("how is the boat");
  w.engine1(5);
  const tx::MigrationOutcome out = w.engine2("and the garden");   // its start imports the five
  TEST_ASSERT_TRUE(out.reentry);
  TEST_ASSERT_TRUE(out.digest0);   // the first migration's, kept
  TEST_ASSERT_EQUAL(1, w.asks);    // never asked again
  TEST_ASSERT_EQUAL(notes + 1, w.notes());   // one note for the batch
  TEST_ASSERT_TRUE(out.turns >= 1 && out.turns <= 5);
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "reentry");
  // the batch sits after engine 2's items, behind its note, and names the earlier engine
  const std::vector<tx::Item> items = stored(w.files);
  size_t note = 0;
  for (size_t i = 0; i < items.size(); ++i)
    if (importNoteText(items[i])) note = i;
  TEST_ASSERT_TRUE(items[note].x.find("answered by the earlier engine; their tool work is not shown.") != std::string::npos);
  TEST_ASSERT_TRUE(note > 0 && items[note - 1].kind == tx::ItemKind::TurnEnd && items[note - 1].end.cause == "reply");
  TEST_ASSERT_TRUE(startsWith(items.back().x, "E2 reply to and the garden") || items.back().kind == tx::ItemKind::TurnEnd);
  TEST_ASSERT_EQUAL(0, w.done("nothing new").items);
}

void test_migrating_twice_in_a_row_changes_nothing() {
  World w;
  firstMigration(w, 4);
  w.engine1(3);
  w.done("reentry");
  for (int i = 0; i < 3; ++i) {
    const auto before = w.files.files;
    const size_t appends = w.files.appends, renames = w.files.renames, whole = w.files.wholeWrites;
    const tx::MigrationOutcome out = w.done("again");
    TEST_ASSERT_TRUE(before == w.files.files);
    TEST_ASSERT_EQUAL(appends, w.files.appends);
    TEST_ASSERT_EQUAL(renames, w.files.renames);
    TEST_ASSERT_EQUAL(0, out.items);
    (void)whole;
  }
  // engine 2's own rows alone move the marker on, and add no item
  w.engine2("only mine");
  const size_t count = stored(w.files).size();
  const tx::MigrationOutcome out = w.done("own rows");
  TEST_ASSERT_EQUAL(0, out.items);
  TEST_ASSERT_EQUAL(count, stored(w.files).size());
  const auto before = w.files.files;
  w.done("again after own rows");
  TEST_ASSERT_TRUE(before == w.files.files);
}

void test_a_cut_at_every_write_of_a_reentry_resumes_exactly_once() {
  World ref;
  firstMigration(ref, 5);
  ref.engine2("first");
  ref.engine1(9);
  const size_t a0 = ref.files.appends, r0 = ref.files.renames;
  const tx::MigrationOutcome rout = ref.done("clean reentry");
  const std::vector<tx::Item> want = stored(ref.files);
  const size_t appends = ref.files.appends - a0, renames = ref.files.renames - r0;
  TEST_ASSERT_TRUE(appends > 5);
  char msg[160];
  for (int torn = 0; torn < 2; ++torn)
    for (size_t k = 0; k <= appends + renames; ++k) {
      World w;
      firstMigration(w, 5);
      w.engine2("first");
      w.engine1(9);
      const auto old = w.old.snapshot();
      if (k < appends) {
        w.files.killAtAppend = (long)(w.files.appends + k);
        w.files.tornKill = torn;
      } else {
        w.files.killAtRename = (long)(w.files.renames + (k - appends));
      }
      std::snprintf(msg, sizeof msg, "reentry cut %u torn %d", (unsigned)k, torn);
      const tx::MigrationOutcome out = w.resume(msg);
      expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
      TEST_ASSERT_EQUAL_UINT64_MESSAGE(rout.hi, out.hi, msg);
      const std::vector<tx::Item> got = stored(w.files);
      TEST_ASSERT_EQUAL_MESSAGE(want.size(), got.size(), msg);
      for (size_t i = 0; i < got.size(); ++i) {
        std::string x, y;
        tx::Item p = want[i], q = got[i];
        TEST_ASSERT_TRUE(tx::encodeLine(p, x).ok() && tx::encodeLine(q, y).ok());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(x.c_str(), y.c_str(), msg);
      }
      TEST_ASSERT_TRUE_MESSAGE(old == w.old.snapshot(), msg);
    }
}

void test_a_long_absence_is_imported_within_the_budget_and_the_rest_named() {
  World w;
  firstMigration(w, 6);
  // Weeks back on the earlier engine: past the head's due mark (RB-H81: the import is sized per
  // head; Sonnet's H, not a fixed 60,000 B), so the oldest are named.
  w.engine1(4000);
  const tx::MigrationOutcome out = w.done("absence");
  TEST_ASSERT_TRUE(out.named > 0 && out.imported > 0);
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "absence");
  const std::vector<tx::Item> items = stored(w.files);
  const tx::Item* note = nullptr;
  for (const tx::Item& it : items)
    if (importNoteText(it)) note = &it;
  TEST_ASSERT_NOT_NULL(note);
  TEST_ASSERT_EQUAL((int)tx::NoteClass::Omit, (int)note->note);
  TEST_ASSERT_EQUAL(out.named, namedIn(note->x));
}

// A plan left pending (cut before its first item) while the binding broke the contract and let
// the engine store: nothing is lost, doubled or left open; the rest comes after what is stored.
void test_a_batch_overtaken_by_the_engine_loses_and_doubles_nothing() {
  for (int cutAt = 0; cutAt < 4; ++cutAt) {
    World w;
    firstMigration(w, 7);
    w.engine1(6);
    const size_t a0 = w.files.appends;
    w.files.killAtAppend = (long)(a0 + cutAt);   // the batch's note, or a row of it
    try {
      tx::MigrationOutcome o;
      (void)tx::migrate(w.deps(), o);
    } catch (const txb::Killed&) {
    }
    w.files.clearFaults();
    w.engine2("stored before the migration finished", false);
    const tx::MigrationOutcome out = w.done("overtaken");
    TEST_ASSERT_TRUE(out.overtaken);
    expectAccounted(w.old, w.files, w.m.thru, out.hi, "overtaken", true);
    TEST_ASSERT_EQUAL(0, w.done("after overtaken").items);
    expectAccounted(w.old, w.files, w.m.thru, out.hi, "overtaken again", true);
  }
}

void test_items_without_a_marker_are_a_reentry_from_the_newest_mirrored_row() {
  World w;
  w.engine1(0);
  w.old.add(kChat, w.clock, "user", "message", "an old message");
  w.engine2("engine 2 ran here without a migration", false);
  bool re = false;
  TEST_ASSERT_TRUE(tx::isReentry(w.files, kChatKey, re).ok());
  TEST_ASSERT_TRUE(re);
  w.engine1(3);
  const tx::MigrationOutcome out = w.done("no marker");
  TEST_ASSERT_TRUE(out.reentry);
  TEST_ASSERT_EQUAL(0, w.asks);
  // the message before engine 2's own row is not imported (it is older than what engine 2 mirrored)
  for (const tx::Item& it : stored(w.files)) TEST_ASSERT_TRUE(it.x != "an old message");
  TEST_ASSERT_TRUE(out.imported >= 3);
  // isReentry on an empty chat folder
  txb::MemFiles empty;
  TEST_ASSERT_TRUE(tx::isReentry(empty, kChatKey, re).ok());
  TEST_ASSERT_FALSE(re);
  TEST_ASSERT_EQUAL((int)tx::Err::InvalidArgument, (int)tx::isReentry(empty, "Bad/Key", re).err);
}

// The marker lost after a finished migration (card damage, a manual delete): the log says how far
// the rows go, so nothing is imported twice, the assistant replies after the newest owner row and
// the rows under the old pointer included.
void test_a_lost_marker_never_imports_a_row_twice() {
  for (int lose = 0; lose < 3; ++lose) {
    World w;
    w.rng.seed(8);
    w.m = month(w.old, {8, 400, 6, lose == 2 ? 6 : 3, 0, 800});   // 2: every row under the old pointer
    w.clock = w.m.lastT + kHour;
    if (lose != 2) {
      w.old.add(kChat, w.clock, "user", "message", "the last question");
      w.old.add(kChat, w.clock, "assistant", "message", "the first reply");
      w.old.add(kChat, w.clock, "assistant", "message", "the second reply");
    }
    w.resume("first");
    std::string path;
    TEST_ASSERT_TRUE(tx::migratedPath(kChatKey, path).ok());
    w.files.files.erase(path);
    w.files.files.erase(path + ".tmp");
    if (lose == 1) {
      std::string head;
      TEST_ASSERT_TRUE(tx::headPath(kChatKey, head).ok());
      w.files.files.erase(head);
    }
    w.engine1(3);
    const tx::MigrationOutcome out = w.done("marker lost");
    TEST_ASSERT_TRUE(out.reentry);
    expectAccounted(w.old, w.files, w.m.thru, out.hi, "marker lost");
    TEST_ASSERT_EQUAL(1, w.asks);
  }
}

// Digest 0's anchor stored, its s item not, and the engine stored before the migration went on:
// digest 0 follows the engine's items covering the anchor alone; one anchor, nothing hidden.
void test_an_overtaken_digest0_keeps_one_anchor_and_hides_nothing() {
  World w;
  w.rng.seed(9);
  w.m = month(w.old, {9, 300, 5, 3, 4, 700});
  w.clock = w.m.lastT + kHour;
  w.files.killAtAppend = 1;   // digest 0's s item
  try {
    tx::MigrationOutcome o;
    (void)tx::migrate(w.deps(), o);
    TEST_FAIL_MESSAGE("not cut");
  } catch (const txb::Killed&) {
  }
  w.files.clearFaults();
  w.engine2("before the digest", false);
  const tx::MigrationOutcome out = w.done("overtaken digest 0");
  TEST_ASSERT_TRUE(out.overtaken);
  TEST_ASSERT_TRUE(out.digest0);
  TEST_ASSERT_EQUAL(1, w.asks);
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "overtaken digest 0", true);
  const std::vector<tx::Item> items = stored(w.files);
  uint64_t anchorQ = 0, e2Q = 0;
  for (const tx::Item& it : items) {
    if (it.kind == tx::ItemKind::DeviceNote && it.x.find("was kept") != std::string::npos) anchorQ = it.q;
    if (it.kind == tx::ItemKind::User && startsWith(it.x, "E2 ")) e2Q = it.q;
    if (it.kind == tx::ItemKind::Digest) {
      TEST_ASSERT_EQUAL_UINT64(anchorQ, it.thru);
      TEST_ASSERT_TRUE(it.q > e2Q && e2Q > anchorQ);
    }
  }
  tx::TxLog log(w.files, kChatKey);
  tx::HeadState head;
  TEST_ASSERT_TRUE(log.open(head).ok());
  TEST_ASSERT_EQUAL_UINT64(anchorQ, head.thru);   // engine 2's turn stays shown
}

// The overtaken batch closes the turn it left open; a refused marker save after that never makes
// a second closing item.
void test_an_overtaken_batch_closes_its_open_turn_once() {
  World w;
  firstMigration(w, 12);
  w.engine1(4);
  const size_t a0 = w.files.appends;
  w.files.killAtAppend = (long)(a0 + 2);   // note and the first owner row stored, its reply not
  try {
    tx::MigrationOutcome o;
    (void)tx::migrate(w.deps(), o);
  } catch (const txb::Killed&) {
  }
  w.files.clearFaults();
  w.engine2("stored early", false);
  w.files.refuseRename = true;
  tx::MigrationOutcome o;
  const tx::Status st = tx::migrate(w.deps(), o);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_TRUE(o.overtaken);
  w.files.clearFaults();
  std::string path;
  TEST_ASSERT_TRUE(tx::migratedPath(kChatKey, path).ok());
  w.files.files.erase(path + ".tmp");   // its temp was not written whole either: the old plan is read again
  const tx::MigrationOutcome out = w.done("closed once");
  TEST_ASSERT_TRUE(out.overtaken);
  std::map<uint32_t, int> ends;
  for (const tx::Item& it : stored(w.files))
    if (it.kind == tx::ItemKind::TurnEnd) ++ends[it.turn];
  for (const auto& kv : ends) TEST_ASSERT_EQUAL_MESSAGE(1, kv.second, "a turn closed twice");
  expectAccounted(w.old, w.files, w.m.thru, out.hi, "closed once", true);
}

void test_engines_taking_turns_never_lose_double_or_reorder_a_row() {
  for (uint32_t seed = 1; seed <= 12; ++seed) {
    World w;
    firstMigration(w, 100 + seed);
    std::mt19937 r(seed);
    char msg[160];
    for (int round = 0; round < 10; ++round) {
      const int what = (int)(r() % 4);
      if (what == 0) w.engine1(1 + (int)(r() % 8));
      if (what == 1) w.engine2("round " + std::to_string(round));
      if (what == 2) {
        w.engine1(1 + (int)(r() % 4));
        w.engine2("after " + std::to_string(round));
      }
      // a start of engine 2: the migration runs, maybe cut somewhere
      if (r() % 2) {
        if (r() % 2) w.files.killAtAppend = (long)(w.files.appends + r() % 12), w.files.tornKill = r() % 2;
        else w.files.killAtRename = (long)(w.files.renames + r() % 3);
      }
      std::snprintf(msg, sizeof msg, "seed %u round %d", seed, round);
      const tx::MigrationOutcome out = w.resume(msg);
      expectAccounted(w.old, w.files, w.m.thru, out.hi, msg);
    }
    TEST_ASSERT_EQUAL(1, w.asks);
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_second_migration_is_a_reentry_that_imports_only_the_other_engines_rows);
  RUN_TEST(test_migrating_twice_in_a_row_changes_nothing);
  RUN_TEST(test_a_cut_at_every_write_of_a_reentry_resumes_exactly_once);
  RUN_TEST(test_a_long_absence_is_imported_within_the_budget_and_the_rest_named);
  RUN_TEST(test_a_batch_overtaken_by_the_engine_loses_and_doubles_nothing);
  RUN_TEST(test_items_without_a_marker_are_a_reentry_from_the_newest_mirrored_row);
  RUN_TEST(test_a_lost_marker_never_imports_a_row_twice);
  RUN_TEST(test_an_overtaken_digest0_keeps_one_anchor_and_hides_nothing);
  RUN_TEST(test_an_overtaken_batch_closes_its_open_turn_once);
  RUN_TEST(test_engines_taking_turns_never_lose_double_or_reorder_a_row);
  return UNITY_END();
}
