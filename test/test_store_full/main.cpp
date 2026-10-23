#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/file_store.h"
#include "nimbus/orch/store_full.h"

// A full file store is said once (storm leg g, 22 Oct 2026): five owner photos into a
// store at its limit gave ten owner messages and no device word. These pin the class:
// which refusals are "the store is full", one owner notice per episode however many
// items the full store refuses, the episode's end, and the words.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

FileEntry mk(const std::string& proj, const std::string& name, uint32_t bytes) {
  FileEntry e;
  e.project = proj;
  e.name = name;
  e.bytes = bytes;
  e.kind = fileKindForName(name);
  return e;
}

FileStore::Limits small(size_t entries, uint64_t total) {
  FileStore::Limits l;
  l.maxEntries = entries;
  l.maxTotalBytes = total;
  l.maxFileBytes = 1000;
  return l;
}

// Fill `s` with new files of `bytes` each until it refuses one; returns that refusal.
std::string fillUntilRefused(FileStore& s, uint32_t bytes) {
  std::string err;
  for (int k = 0; k < 10000; ++k)
    if (!s.add(mk("fill", "f" + std::to_string(k) + ".txt", bytes), err)) return err;
  return "";
}

bool hasEmDash(const std::string& s) { return s.find("\xE2\x80\x94") != std::string::npos; }

bool printableAscii(const std::string& s) {
  for (unsigned char c : s)
    if (c != '\n' && (c < 0x20 || c > 0x7E)) return false;
  return true;
}

}  // namespace

// ---- which refusals mean "the store is full" ---------------------------------

// Every way the store can be full refuses with a full refusal, from add() and from the
// streaming pre-check alike, and the store reports no room.
static void test_every_full_state_is_a_full_refusal() {
  {   // out of entries
    FileStore s(small(4, 100000));
    const std::string err = fillUntilRefused(s, 10);
    TEST_ASSERT_EQUAL_STRING(FileStore::kErrTooManyFiles, err.c_str());
    TEST_ASSERT_TRUE(s.fullRefusal(err));
    std::string pre;
    TEST_ASSERT_TRUE(s.wouldExceed("chat-1", "photo-9.jpg", 10, pre));
    TEST_ASSERT_TRUE(s.fullRefusal(pre));
    TEST_ASSERT_FALSE(s.hasRoom());
  }
  {   // out of bytes, to the byte
    FileStore s(small(100, 50));
    const std::string err = fillUntilRefused(s, 10);
    TEST_ASSERT_EQUAL_STRING(FileStore::kErrStoreFull, err.c_str());
    TEST_ASSERT_TRUE(s.fullRefusal(err));
    TEST_ASSERT_EQUAL_UINT64(50, s.totalBytes());
    TEST_ASSERT_FALSE(s.hasRoom());
  }
  {   // a photo bigger than the bytes left: full for that photo, though a note fits
    FileStore s(small(100, 50));
    std::string err;
    TEST_ASSERT_TRUE(s.add(mk("p", "a.txt", 45), err));
    TEST_ASSERT_TRUE(s.wouldExceed("chat-1", "photo-1.jpg", 20, err));
    TEST_ASSERT_TRUE(s.fullRefusal(err));
    TEST_ASSERT_TRUE(s.hasRoom());
  }
}

// A request the store turns down is not a full store: removing files would not help.
static void test_a_refused_request_is_not_a_full_store() {
  FileStore s(small(4, 100000));
  std::string err;
  TEST_ASSERT_TRUE(s.wouldExceed("../x", "a.txt", 1, err));
  TEST_ASSERT_FALSE(s.fullRefusal(err));
  TEST_ASSERT_TRUE(s.wouldExceed("p", "a b.txt", 1, err));
  TEST_ASSERT_FALSE(s.fullRefusal(err));
  TEST_ASSERT_TRUE(s.wouldExceed("p", "big.bin", 1001, err));   // one file over its limit
  TEST_ASSERT_FALSE(s.fullRefusal(err));
  TEST_ASSERT_FALSE(s.fullRefusal(""));
  TEST_ASSERT_FALSE(s.fullRefusal("SD short write"));
  TEST_ASSERT_FALSE(s.fullRefusal("an upload is in progress - try again"));
  // A card below the supported size takes nothing (zero quota): its bytes refusal is
  // not about fullness, and deleting files cannot fix it.
  FileStore tiny(small(256, 0));
  TEST_ASSERT_TRUE(tiny.wouldExceed("p", "a.txt", 1, err));
  TEST_ASSERT_EQUAL_STRING(FileStore::kErrStoreFull, err.c_str());
  TEST_ASSERT_FALSE(tiny.fullRefusal(err));
  TEST_ASSERT_FALSE(tiny.hasRoom());
}

static void test_room_follows_entries_and_bytes() {
  FileStore s(small(2, 100));
  TEST_ASSERT_TRUE(s.hasRoom());   // a fresh store
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("p", "a.txt", 10), err));
  TEST_ASSERT_TRUE(s.hasRoom());
  TEST_ASSERT_TRUE(s.add(mk("p", "b.txt", 10), err));
  TEST_ASSERT_FALSE(s.hasRoom());   // out of entries
  TEST_ASSERT_TRUE(s.remove("p", "a.txt"));
  TEST_ASSERT_TRUE(s.hasRoom());
}

// ---- one notice per episode ---------------------------------------------------

// The storm shape: the store at its limit, then many inbound photos. However many the
// full store refuses, the owner is told once, and the episode stays open throughout.
static void test_one_notice_however_many_photos() {
  FileStore s(small(8, 100000));
  fillUntilRefused(s, 10);
  StoreFullEpisode ep;
  int notices = 0;
  for (int k = 0; k < 50; ++k) {
    std::string err;
    const bool refused = !s.add(mk("chat-1", "photo-" + std::to_string(k) + ".jpg", 10), err);
    TEST_ASSERT_TRUE(refused);
    TEST_ASSERT_TRUE(ep.refused(s.fullRefusal(err)));
    TEST_ASSERT_TRUE(ep.active());
    notices += ep.takeNotice();
  }
  TEST_ASSERT_EQUAL_INT(1, notices);
}

// A save that lands mid-episode (a store a little under its byte quota) does not end
// it: a note between two refused photos never earns the owner a second notice.
static void test_a_save_between_refusals_does_not_tell_again() {
  FileStore s(small(100, 100));
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("p", "a.txt", 85), err));
  StoreFullEpisode ep;
  int notices = 0;
  for (int k = 0; k < 4; ++k) {
    TEST_ASSERT_FALSE(s.add(mk("chat-1", "photo-" + std::to_string(k) + ".jpg", 20), err));
    ep.refused(s.fullRefusal(err));
    notices += ep.takeNotice();
    TEST_ASSERT_TRUE(s.add(mk("p", "n" + std::to_string(k) + ".txt", 1), err));   // a small note lands
  }
  TEST_ASSERT_EQUAL_INT(1, notices);
  TEST_ASSERT_TRUE(ep.active());
}

// The owner makes room: the episode ends; the store fills again: a new episode and
// exactly one more notice. Notices equal episodes, never more (no thrash).
static void test_making_room_ends_the_episode() {
  FileStore s(small(3, 100000));
  fillUntilRefused(s, 10);
  StoreFullEpisode ep;
  int notices = 0, episodes = 0;
  for (int round = 0; round < 3; ++round) {
    for (int k = 0; k < 5; ++k) {
      std::string err;
      const bool wasActive = ep.active();
      TEST_ASSERT_FALSE(s.add(mk("chat-1", "p" + std::to_string(round) + std::to_string(k) + ".jpg", 10), err));
      ep.refused(s.fullRefusal(err));
      episodes += !wasActive && ep.active();
      notices += ep.takeNotice();
    }
    TEST_ASSERT_TRUE(s.remove("fill", "f" + std::to_string(round) + ".txt"));   // the owner deletes one
    if (s.hasRoom()) ep.room();
    TEST_ASSERT_FALSE(ep.active());
    std::string err;
    TEST_ASSERT_TRUE(s.add(mk("chat-1", "kept" + std::to_string(round) + ".jpg", 10), err));   // fills it again
  }
  TEST_ASSERT_EQUAL_INT(3, episodes);
  TEST_ASSERT_EQUAL_INT(3, notices);
}

// Room made before the notice went out: the notice is dropped, not sent stale.
static void test_room_before_the_notice_drops_it() {
  StoreFullEpisode ep;
  ep.refused(true);
  ep.room();
  TEST_ASSERT_FALSE(ep.active());
  TEST_ASSERT_FALSE(ep.takeNotice());
}

// Refusals that are not a full store never open an episode or raise a notice.
static void test_other_refusals_never_tell() {
  StoreFullEpisode ep;
  for (int k = 0; k < 10; ++k) TEST_ASSERT_FALSE(ep.refused(false));
  TEST_ASSERT_FALSE(ep.active());
  TEST_ASSERT_FALSE(ep.takeNotice());
}

// ---- the words ----------------------------------------------------------------

// The owner notice: plain device copy (no I/me/my, no em dash, ASCII), says what
// happened and the one next step, names no fixed count (the limit can change).
static void test_owner_notice_words() {
  const std::string n = kStoreFullNotice;
  TEST_ASSERT_FALSE(hasEmDash(n));
  TEST_ASSERT_TRUE(printableAscii(n));
  TEST_ASSERT_TRUE(n.find("file store is full") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("To make room") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("web page") != std::string::npos);
  TEST_ASSERT_TRUE(n.find("256") == std::string::npos);
  for (const char* first : {" I ", " me ", " my ", "I'm", "!"})
    TEST_ASSERT_TRUE_MESSAGE(n.find(first) == std::string::npos, first);
  TEST_ASSERT_EQUAL_CHAR('.', n.back());
}

// The head's fact: one bracketed line after the item's label, the item named, the
// history sentence only when the item itself was kept there.
static void test_head_fact_words() {
  for (bool photo : {true, false}) {
    for (bool kept : {true, false}) {
      const std::string f = storeFullFact(photo, kept);
      TEST_ASSERT_FALSE(hasEmDash(f));
      TEST_ASSERT_TRUE(printableAscii(f));
      TEST_ASSERT_EQUAL_STRING("\n[", f.substr(0, 2).c_str());
      TEST_ASSERT_EQUAL_CHAR(']', f.back());
      TEST_ASSERT_TRUE(f.find("already sent the owner a message") != std::string::npos);
      TEST_ASSERT_TRUE(f.find("do not ask which files to delete") != std::string::npos);
      const std::string item = std::string("The ") + (photo ? "photo" : "file") + " itself stays";
      TEST_ASSERT_EQUAL(kept, f.find(item) != std::string::npos);
      TEST_ASSERT_TRUE(f.find(photo ? "The file itself" : "The photo itself") == std::string::npos);
    }
  }
}

static void test_tool_note_words() {
  const std::string t = kStoreFullToolNote;
  TEST_ASSERT_FALSE(hasEmDash(t));
  TEST_ASSERT_TRUE(printableAscii(t));
  TEST_ASSERT_EQUAL_CHAR(' ', t.front());   // appended to the store's own refusal
  TEST_ASSERT_TRUE(t.find("file store is full") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("do not explain the store") != std::string::npos);
}

// ---- the device wiring (a source scan: these paths cannot run on the host) ----

static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// An inbound refusal notes the episode, sends its notice before the turn's reply and
// puts the fact in the turn; every assistant save hands the head refusalForHead; only
// a removal ends an episode; a web upload never opens one (the page shows its refusal).
static void test_device_wiring() {
  const std::string tg = readFile("src/agent/telegram.cpp");
  TEST_ASSERT_EQUAL_size_t(1, count(tg, "agent::files::noteRefusal(ferr)"));
  TEST_ASSERT_EQUAL_size_t(1, count(tg, "if (full) seg += nimbus::orch::storeFullFact(photo, g_slots[slot].media.kept);"));
  TEST_ASSERT_EQUAL_size_t(3, count(tg, "sendFileStoreNotice()"));   // the sender, the photo path, the poll loop
  const std::string fs = readFile("src/agent/files_subsystem.cpp");
  // artifact.save, files.append, image.generate, a helper's saved report (+ the definition).
  TEST_ASSERT_EQUAL_size_t(5, count(fs, "refusalForHead("));
  TEST_ASSERT_EQUAL_size_t(2, count(fs, "if (g_store.hasRoom()) g_full.room();"));   // removeFile, removeProject
  TEST_ASSERT_EQUAL_size_t(2, count(fs, "g_full.room()"));
  TEST_ASSERT_EQUAL_size_t(2, count(readFile("src/agent/pdf_tool.cpp"), "files::refusalForHead(err)"));
  for (const char* web : {"src/net/web_files.cpp", "src/net/webui.cpp"}) {
    const std::string w = readFile(web);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, count(w, "noteRefusal"), web);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, count(w, "refusalForHead"), web);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_full_state_is_a_full_refusal);
  RUN_TEST(test_a_refused_request_is_not_a_full_store);
  RUN_TEST(test_room_follows_entries_and_bytes);
  RUN_TEST(test_one_notice_however_many_photos);
  RUN_TEST(test_a_save_between_refusals_does_not_tell_again);
  RUN_TEST(test_making_room_ends_the_episode);
  RUN_TEST(test_room_before_the_notice_drops_it);
  RUN_TEST(test_other_refusals_never_tell);
  RUN_TEST(test_owner_notice_words);
  RUN_TEST(test_head_fact_words);
  RUN_TEST(test_tool_note_words);
  RUN_TEST(test_device_wiring);
  return UNITY_END();
}
