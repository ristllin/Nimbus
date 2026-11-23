// test_e2_migration - the earlier engine's conversation and rules on engine 2 (lane E2-MIG): the
// one-chat binding (plain/chat_binding.h) with its oldEngine hook over an episodic store and a fold
// record, as the device binds it (src/agent/migrate_device.cpp), a scripted Claude head on the real
// Anthropic wire and the card store over a fake card. Pins:
//   - a history of a few hundred rows with an old summary imports once into an empty engine-2 chat,
//     and the first request carries an early row and the summary's fact;
//   - a second boot imports only the rows the earlier engine wrote since (not engine 2's own "e2"
//     rows), and a third imports nothing;
//   - the earlier engine's stores are byte-identical after every import (the episodic blob, the
//     fold record, no fold save);
//   - the row source pages forward once, oldest first, inside its bound;
//   - an engine-2 image reports v5.9.0 and the updater leaves it there when the stable manifest
//     names an engine-1 release (v5.2.0); only a strictly newer manifest is offered;
//   - the standing rules arrive once as pinned rule notes and reach the request; an edit made on
//     engine 2 is kept, a rule the earlier engine changed is followed when the note is untouched.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/chat_commands.h"   // /clear's number (E2-FIX3)
#include "nimbus/orch/compact.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/tx/card_files.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/old_engine.h"
#include "nimbus/ota/ota_logic.h"

// The version an engine-2 image reports (include/version.h under -DNIMBUS_ENGINE=2).
#define NIMBUS_ENGINE 2
#include "../../include/version.h"
#undef NIMBUS_ENGINE

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC
constexpr uint32_t kHour0 = (uint32_t)(kEpoch0 / 3600) - 30;   // the history starts 30 h before
const char kChat[] = "1001";
const char kHead[] = "claude-sonnet-5-5";
const char kSummarizer[] = "claude-haiku-5-5";

const char kDigest[] =
    "The person set up the week, noted that the bike is blue and asked for short answers; the assistant "
    "answered and kept the plan. Nothing is left open.";

// A card that keeps files and folders (test_plain_chat_binding's).
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool down = false;            // the card is fitted but does not answer (lane E2-FIX2B, P3)
  bool refuseMarker = false;    // the migration marker's save is refused (P3, sequence B)
  bool present() override { return !down; }
  bool mayWrite() override { return true; }
  uint32_t largestInternal() override { return 64 * 1024; }
  uint32_t nowMs() override { return 0; }
  void lock() override {}
  void unlock() override {}
  bool exists(const std::string& p) override { return files.count(p) || dirs.count(p); }
  bool isDir(const std::string& p) override { return dirs.count(p) > 0; }
  bool mkdir(const std::string& p) override { return dirs.insert(p).second; }
  size_t append(const std::string& p, const char* d, size_t n) override {
    files[p].append(d, n);
    return n;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    got = 0;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off < it->second.size()) got = it->second.copy(buf, want, off);
    return true;
  }
  bool fileSize(const std::string& p, size_t& out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  size_t writeWhole(const std::string& p, const char* d, size_t n) override {
    if (refuseMarker && p.find("migrated.json") != std::string::npos) return 0;
    files[p].assign(d, n);
    return n;
  }
  bool rename(const std::string& a, const std::string& b) override {
    if (!files.count(a) || files.count(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {   // E2-STORE's CardOps
    names.clear();
    if (!dirs.count(dir)) return false;
    const std::string pre = dir == "/" ? std::string("/") : dir + "/";
    auto child = [&](const std::string& p) {
      if (p.size() > pre.size() && p.rfind(pre, 0) == 0 && p.find('/', pre.size()) == std::string::npos)
        names.push_back(p.substr(pre.size()));
    };
    for (const auto& kv : files) child(kv.first);
    for (const std::string& d : dirs) child(d);
    return true;
  }
};

// The fold record as the earlier engine left it; a save would be a write to the old store.
class FoldFile : public orch::FoldStoreIO {
 public:
  std::string blob;
  int saves = 0;
  std::string load() override { return blob; }
  void save(const std::string&) override { ++saves; }
};

// The owner's notes in RAM.
class MemNotes : public orch::notes::NotesIo {
 public:
  std::map<int, std::string> shelf;
  bool available(const std::string& person) override { return person == "owner"; }
  bool read(const std::string&, orch::notes::Shelf s, std::string& out) override {
    out = shelf[(int)s];
    return true;
  }
  bool write(const std::string&, orch::notes::Shelf s, const std::string& bytes) override {
    shelf[(int)s] = bytes;
    return true;
  }
  orch::notes::Caps caps(const std::string&) override { return orch::notes::kOwnerCaps; }
};

// The earlier engine's day streams: `n` rows of one chat, owner and assistant in turn, with a tool
// row and a photo among them. Row i has id 0x100 + i.
struct OldEngine {
  orch::InMemoryEpisodicStore store{5000};
  FoldFile fold;
  uint32_t next = 0x100;

  void row(const char* role, orch::MsgKind kind, const std::string& text, uint32_t hours,
           const std::string& tags = std::string(), const char* chat = kChat) {
    orch::EpisodicMessage m;
    m.id = tx::rowIdString(next++);
    m.sessionId = chat;
    m.tsHours = hours;
    m.role = role;
    m.kind = kind;
    m.text = text;
    m.tags = tags;
    store.addMessage(m);
  }
  void history(int pairs) {
    for (int i = 0; i < pairs; ++i) {
      const uint32_t h = kHour0 + (uint32_t)(i * 27 / pairs);
      std::string owner = "owner line " + std::to_string(i) + " about the day";
      if (i == 30) owner = "my locker code is 4417, keep it in mind";
      if (i == 60) {
        row("user", orch::MsgKind::Image, "a photo of a red door with a brass knocker", h, "from:Owner");
        row("assistant", orch::MsgKind::Message, "That is a red door.", h);
        continue;
      }
      row("user", orch::MsgKind::Message, owner, h, "from:Owner");
      if (i % 10 == 5) row("tool", orch::MsgKind::ToolOutput, "{\"result\":\"tool row " + std::to_string(i) + "\"}", h);
      row("assistant", orch::MsgKind::Message, "reply " + std::to_string(i) + ", noted.", h);
    }
    row("user", orch::MsgKind::Message, "a row of another chat", kHour0, "", "2002");
  }
};

// A fold record written by the earlier engine's own FoldStore.
struct SavingFold : public orch::FoldStoreIO {
  std::string* into;
  explicit SavingFold(std::string* p) : into(p) {}
  std::string load() override { return std::string(); }
  void save(const std::string& b) override { *into = b; }
};

std::string foldBlob(uint64_t thruId) {
  std::string blob;
  SavingFold io(&blob);
  orch::FoldStore fs;
  fs.begin(&io);
  fs.applyFold(kChat, "Earlier: the owner said the bike is blue and asked for short answers.",
               (uint32_t)(kEpoch0 - 20 * 3600), tx::rowIdString(thruId));
  return blob;
}

struct Device {
  Device() : files(card), notebook(notesIo) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    // The summarizer (digest 0) and the head answer by model.
    model.respondWith([this](const sp::Recorded& r, size_t) {
      if (r.body.find(kSummarizer) != std::string::npos) {
        ++digestAsks;
        return sp::say(summarized);
      }
      return sp::say(answer);
    });
  }
  // The device's fill (migrate_device.cpp) over the host fakes.
  bool fill(tx::MigrateDeps& m) {
    rows.reset(new tx::EpisodicRowSource(m.chatKey, [this](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
      return old.store.query(q, &info);
    }));
    orch::FoldStore folds;
    folds.begin(&old.fold);
    m.fold = tx::oldFoldOf(folds.get(m.chatKey));
    m.rows = rows.get();
    bool mirrored = false;
    (void)tx::dropMirroredPointer(*rows, m.fold, mirrored);
    m.exportText = [this](const std::string& path, const std::string& text) {
      exported[path] = text;
      return true;
    };
    return true;
  }
  plain::Status boot(const std::string& key = kChat) {
    plain::ChatBindingConfig c;
    c.chatKey = key;
    c.chatId = kChat;
    c.model = kHead;
    c.summarizerModel = kSummarizer;
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string&, const std::string& part) {
      sent.push_back(part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-migration"); };
    d.pinnedNotes = [this](const orch::Principal& who) { return notebook.pinnedBlock(who); };
    d.oldEngine = [this](tx::MigrateDeps& m) { return fill(m); };
    if (noCard) d.noCard = [] { return true; };
    binding.reset();
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }
  plain::Status say(const std::string& text) {
    plain::ChatMessage m;
    m.text = text;
    m.ch = "web";
    out = plain::TurnOutcome{};
    return binding->turn(m, out);
  }
  std::string lastBody() const { return model.requests().back().body; }

  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  std::string answer = "Your locker code is 4417.";
  int digestAsks = 0;
  std::string summarized = kDigest;   // the summarizer's answer (digest 0, then compaction's changes)
  bool noCard = false;
  Card card;
  tx::CardTxFiles files;
  orch::ToolRegistry reg;
  OldEngine old;
  MemNotes notesIo;
  orch::notes::Notebook notebook;
  std::unique_ptr<tx::EpisodicRowSource> rows;
  std::map<std::string, std::string> exported;
  std::vector<std::string> sent;
  std::unique_ptr<plain::ChatBinding> binding;
  plain::TurnOutcome out;
};

// The card under the device's episodic store (test_episodic_after's fake).
struct EpiCard : orch::EpiFs {
  std::map<std::string, std::string> files;
  long size(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  long append(const std::string& path, const std::string& bytes) override {
    std::string& f = files[path];
    const long off = (long)f.size();
    f += bytes;
    return off;
  }
  std::string readAll(const std::string& path) const override {
    auto it = files.find(path);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& path, long offset, long len) const override {
    auto it = files.find(path);
    if (it == files.end() || offset < 0 || len < 0 || (size_t)(offset + len) > it->second.size()) return std::string();
    return it->second.substr((size_t)offset, (size_t)len);
  }
  void forEachName(const std::string& dir, const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
         ++it) {
      const char* rest = it->first.c_str() + prefix.size();
      if (std::strchr(rest, '/') == nullptr) fn(rest);
    }
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.push_back(n); });
    return out;
  }
  bool remove(const std::string& path) override { return files.erase(path) > 0; }
  bool exists(const std::string& path) const override { return files.count(path) > 0; }
};

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
  return n;
}

constexpr int kPairs = 180;   // about 370 rows over 27 hours

void seed(Device& dev) {
  dev.old.history(kPairs);
  dev.old.fold.blob = foldBlob(0x100 + 41);   // the summary covers the first 20 pairs
}

}  // namespace

void setUp() {}
void tearDown() {}

static void test_an_old_history_imports_once_and_an_early_row_is_answerable() {
  Device dev;
  seed(dev);
  const std::string storeBefore = dev.old.store.serialize();
  const std::string foldBefore = dev.old.fold.blob;
  const plain::Status st = dev.boot();
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
  TEST_ASSERT_TRUE(dev.binding->migrationRan());
  TEST_ASSERT_TRUE_MESSAGE(dev.binding->migrationStatus().ok(), dev.binding->migrationStatus().where);
  const tx::MigrationOutcome& o = dev.binding->migration();
  TEST_ASSERT_EQUAL((int)tx::MigrationStep::Done, (int)o.step);
  TEST_ASSERT_FALSE(o.reentry);
  TEST_ASSERT_TRUE(o.digest0);
  TEST_ASSERT_EQUAL(0u, o.named);                  // the whole history after the summary fits
  TEST_ASSERT_EQUAL((uint32_t)(kPairs - 20), o.turns);   // one turn per owner row after the pointer (20 pairs under it)
  TEST_ASSERT_TRUE(dev.digestAsks >= 1 && dev.digestAsks <= (int)tx::kDigest0Tries);

  TEST_ASSERT_TRUE(dev.say("what is my locker code?").ok());
  const std::string body = dev.lastBody();
  TEST_ASSERT_TRUE_MESSAGE(has(body, "my locker code is 4417"), "the early row is in the request");
  TEST_ASSERT_TRUE_MESSAGE(has(body, "bike is blue"), "the old summary's fact is in the request");
  TEST_ASSERT_TRUE_MESSAGE(has(body, "a photo of a red door"), "a photo comes as its description");
  TEST_ASSERT_TRUE(has(body, "owner line " + std::to_string(kPairs - 1) + " about the day"));
  TEST_ASSERT_FALSE_MESSAGE(has(body, "a row of another chat"), "only this chat's rows");
  TEST_ASSERT_FALSE_MESSAGE(has(body, "tool row 25"), "tool rows are not replayed");
  TEST_ASSERT_EQUAL(1u, count(body, "my locker code is 4417"));
  TEST_ASSERT_EQUAL(1, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING("Your locker code is 4417.", dev.sent[0].c_str());

  // The earlier engine's stores: byte-identical, never saved.
  TEST_ASSERT_TRUE(storeBefore == dev.old.store.serialize());
  TEST_ASSERT_TRUE(foldBefore == dev.old.fold.blob);
  TEST_ASSERT_EQUAL(0, dev.old.fold.saves);
}


// Lane E2-FIX2B, finding P3 (sequence A): the card does not answer at the first engine-2 boot. Before:
// the owner's turns of the outage were stored when the card came back, and the earlier conversation
// never arrived. Now nothing is stored before the import answered Done: the chat runs in RAM with
// the owner told, the import runs before the next turn, and the RAM turn follows the imported
// history once, in order, also after a restart.
static void test_a_card_out_at_the_first_boot_imports_before_anything_is_stored() {
  Device dev;
  seed(dev);
  dev.card.down = true;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->migrationRan());
  TEST_ASSERT_FALSE(dev.binding->migrationStatus().ok());
  TEST_ASSERT_TRUE(dev.binding->importPending());
  TEST_ASSERT_TRUE(dev.binding->unloaded());
  dev.answer = "Noted: 9911.";
  TEST_ASSERT_TRUE(dev.say("my bike lock code is 9911").ok());
  TEST_ASSERT_EQUAL_STRING(plain::kNotLoadedLine, dev.sent.back().c_str());
  TEST_ASSERT_FALSE(has(dev.lastBody(), "my locker code is 4417"));

  dev.card.down = false;
  dev.answer = "4417 and 9911.";
  TEST_ASSERT_TRUE(dev.say("what are my locker and bike lock codes?").ok());
  TEST_ASSERT_FALSE(dev.binding->importPending());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  std::string body = dev.lastBody();
  TEST_ASSERT_EQUAL_MESSAGE(1u, count(body, "my locker code is 4417"), "the earlier conversation arrived");
  TEST_ASSERT_TRUE(has(body, "bike is blue"));
  TEST_ASSERT_EQUAL(1u, count(body, "my bike lock code is 9911"));
  TEST_ASSERT_TRUE(body.find("my locker code is 4417") < body.find("my bike lock code is 9911"));

  dev.ms += 3600 * 1000;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->migration().reentry);
  TEST_ASSERT_EQUAL(0u, dev.binding->migration().imported);
  TEST_ASSERT_TRUE(dev.say("and again?").ok());
  body = dev.lastBody();
  TEST_ASSERT_EQUAL(1u, count(body, "my locker code is 4417"));
  TEST_ASSERT_EQUAL(1u, count(body, "my bike lock code is 9911"));
  TEST_ASSERT_EQUAL(1u, count(body, "what are my locker and bike lock codes?"));
}

// P3 sequence B: the card answers but the import fails before its first marker (the marker save is
// refused). Nothing of the chat is stored meanwhile; the import is tried again before the next turn.
static void test_an_import_refused_before_its_marker_stores_nothing_until_done() {
  Device dev;
  seed(dev);
  dev.card.refuseMarker = true;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->importPending());
  TEST_ASSERT_TRUE(dev.say("my bike lock code is 9911").ok());
  TEST_ASSERT_TRUE(dev.say("and the shed key is in the red tin").ok());
  for (const auto& kv : dev.card.files)
    TEST_ASSERT_TRUE_MESSAGE(kv.first.find("/" + std::string(kChat) + "/seg-") == std::string::npos, kv.first.c_str());
  dev.card.refuseMarker = false;
  TEST_ASSERT_TRUE(dev.say("what do you know?").ok());
  TEST_ASSERT_FALSE(dev.binding->importPending());
  const std::string body = dev.lastBody();
  TEST_ASSERT_EQUAL(1u, count(body, "my locker code is 4417"));
  TEST_ASSERT_EQUAL(1u, count(body, "my bike lock code is 9911"));
  TEST_ASSERT_EQUAL(1u, count(body, "and the shed key is in the red tin"));
  TEST_ASSERT_TRUE(body.find("my bike lock code is 9911") < body.find("and the shed key is in the red tin"));
}

// Lane E2-FIX3, finding R1 (ruling 116 item 1): an import that fails the same way every time (here
// its marker is always refused while the card answers). Before: the owner was told every turn to wait
// for the card (which was in), /clear was refused, and every held turn was lost at each restart.
// Now: the owner's line names the import and offers /clear; /clear stores the held turns in the new
// conversation, in order, once; a restart keeps them; the earlier history is left as it was.
static void test_an_import_that_never_finishes_offers_clear_and_clear_keeps_the_held_turns() {
  Device dev;
  seed(dev);
  const std::string storeBefore = dev.old.store.serialize();
  dev.card.refuseMarker = true;   // forever
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->importPending());
  TEST_ASSERT_TRUE(dev.say("my bike lock code is 9911").ok());
  TEST_ASSERT_TRUE(dev.binding->importStuck());
  TEST_ASSERT_EQUAL_STRING(plain::kImportStuckLine, dev.sent.back().c_str());
  TEST_ASSERT_TRUE(dev.say("and the shed key is in the red tin").ok());
  TEST_ASSERT_EQUAL_STRING(plain::kImportStuckLine, dev.sent.back().c_str());
  TEST_ASSERT_FALSE(dev.binding->reload());   // what the device's /clear asks first: still held

  // /clear: the number first, then the held turns into its transcript.
  uint32_t next = 0;
  TEST_ASSERT_TRUE(plain::freeGeneration(dev.files, kChat, 0, next));
  TEST_ASSERT_EQUAL(1u, next);
  TEST_ASSERT_TRUE(plain::storeGeneration(dev.files, kChat, next));
  const std::string key = std::string(kChat) + "_c1";
  const plain::Status st = dev.binding->storeHeldAs(key);
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
  TEST_ASSERT_TRUE(dev.binding->moved());
  TEST_ASSERT_TRUE(dev.binding->reload());   // done: the caller lets it go
  for (const auto& kv : dev.card.files)       // nothing under the chat's own key
    TEST_ASSERT_TRUE_MESSAGE(kv.first.find("/" + std::string(kChat) + "/seg-") == std::string::npos, kv.first.c_str());

  for (int restart = 0; restart < 2; ++restart) {
    uint32_t g = 0;
    TEST_ASSERT_EQUAL((int)plain::GenRead::Value, (int)plain::readGeneration(dev.files, kChat, g));
    TEST_ASSERT_EQUAL(1u, g);
    TEST_ASSERT_TRUE(dev.boot(key).ok());
    TEST_ASSERT_FALSE(dev.binding->unloaded());
    TEST_ASSERT_FALSE(dev.binding->migrationRan());
    dev.sent.clear();
    TEST_ASSERT_TRUE(dev.say(restart ? "and again?" : "what do you know?").ok());
    for (const std::string& part : dev.sent) TEST_ASSERT_TRUE(part != plain::kImportStuckLine && part != plain::kNotLoadedLine);
    const std::string body = dev.lastBody();
    TEST_ASSERT_EQUAL(1u, count(body, "my bike lock code is 9911"));
    TEST_ASSERT_EQUAL(1u, count(body, "and the shed key is in the red tin"));
    TEST_ASSERT_TRUE(body.find("my bike lock code is 9911") < body.find("and the shed key is in the red tin"));
    TEST_ASSERT_FALSE_MESSAGE(has(body, "my locker code is 4417"), "a new conversation starts empty");
    dev.ms += 3600 * 1000;
  }
  TEST_ASSERT_TRUE(storeBefore == dev.old.store.serialize());   // the earlier history, as it was
}

// The neighbor: the card answers but the import fails only once: the owner is told it is not
// imported yet (not the card), and the next turn's import ends it.
static void test_an_import_that_fails_once_is_not_stuck() {
  Device dev;
  seed(dev);
  dev.card.refuseMarker = true;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_FALSE(dev.binding->importStuck());   // one failure
  dev.card.refuseMarker = false;
  TEST_ASSERT_TRUE(dev.say("my bike lock code is 9911").ok());
  TEST_ASSERT_FALSE(dev.binding->importPending());
  for (const std::string& part : dev.sent) TEST_ASSERT_TRUE(part != plain::kImportStuckLine);
}

// The neighbor: no card fitted at all (noCard) runs from RAM by design; a refused import is not held for.
static void test_with_no_card_fitted_the_import_holds_nothing() {
  Device dev;
  seed(dev);
  dev.card.down = true;
  dev.noCard = true;
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_FALSE(dev.binding->importPending());
  TEST_ASSERT_FALSE(dev.binding->unloaded());
  TEST_ASSERT_TRUE(dev.say("hello").ok());
  for (const std::string& part : dev.sent) TEST_ASSERT_TRUE(part != plain::kNotLoadedLine);
}

// Lane E2-FIX2B, finding P5 (ruling 115 item 3), the way back: engine-2 turns and a compaction leave
// the earlier engine's fold record and its pointer byte-identical, never saved (the binding has no
// path to it any more; the device binds none).
static void test_turns_and_a_compaction_leave_the_earlier_fold_record_as_it_was() {
  Device dev;
  seed(dev);
  for (int i = 0; i < 60; ++i) {   // enough conversation past the recent part for a compaction
    const std::string pad(1500, (char)('a' + i % 26));
    dev.old.row("user", orch::MsgKind::Message, "long owner line " + std::to_string(i) + " " + pad, kHour0 + 27, "from:Owner");
    dev.old.row("assistant", orch::MsgKind::Message, "long reply " + std::to_string(i) + " " + pad, kHour0 + 27);
  }
  const std::string foldBefore = dev.old.fold.blob;
  TEST_ASSERT_TRUE(dev.boot().ok());
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(dev.say("turn " + std::to_string(i) + " on engine 2").ok());
  const uint64_t thru0 = dev.binding->engine()->head()->thru;
  dev.summarized = "+F The person keeps long owner lines.\n+D The assistant answered each one.";
  TEST_ASSERT_TRUE(dev.binding->compactNow().ok());
  TEST_ASSERT_TRUE_MESSAGE(dev.binding->engine()->head()->thru > thru0, dev.sent.back().c_str());
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(foldBefore == dev.old.fold.blob);
  TEST_ASSERT_EQUAL(0, dev.old.fold.saves);
}

// P5: a record an earlier engine-2 build replaced with its mirror (its cumulative summary as the
// text) is put back from the kept copy; every other chat's record stays byte for byte. Counter
// (seen on the bench unit): the earlier engine's own record right after its fold has the same
// counters and a pointer that may name an engine-2 row; its prose text is never taken for a
// mirror, and a record the copy does not hold is never removed.
static void test_a_mirrored_fold_record_is_put_back_from_the_kept_copy() {
  const std::string original = foldBlob(0x100 + 41);
  std::string other;
  SavingFold io(&other);
  orch::FoldStore fs;
  fs.begin(&io);
  fs.applyFold("2002", "Another chat's summary.", (uint32_t)(kEpoch0 - 3600), tx::rowIdString(0x105));
  tx::Summary sum;
  sum.lines.push_back(tx::SummaryLine{tx::SummarySection::Facts, 1, "The person keeps a blue bike."});
  std::string sumText;
  TEST_ASSERT_TRUE(tx::encodeSummary(sum, sumText).ok());
  tx::MirrorFacts f{kChat, 40, 0x100 + 300, (uint32_t)kEpoch0};
  std::vector<tx::DigestEntry> live{tx::DigestEntry{"2026-11-17", sumText, 41, 40}};
  std::string mirrored;
  TEST_ASSERT_TRUE(tx::mirrorChatsum(other, f, live, mirrored).ok());   // what the earlier build wrote
  std::string restored;
  TEST_ASSERT_TRUE(tx::restoreFoldRecord(mirrored, original, kChat, restored));
  TEST_ASSERT_TRUE(restored.find(other) == 0);                    // 2002's record, byte for byte
  TEST_ASSERT_TRUE(restored.substr(other.size()) == original);   // 1001's, as the earlier engine left it
  std::string again;
  TEST_ASSERT_FALSE(tx::restoreFoldRecord(restored, original, kChat, again));   // once: prose now
  TEST_ASSERT_TRUE(again.empty());
  // The earlier engine's own record after its fold: never taken for a mirror.
  TEST_ASSERT_FALSE(tx::restoreFoldRecord(original, std::string(), kChat, again));
  TEST_ASSERT_FALSE(tx::restoreFoldRecord(original, other, kChat, again));
  // A mirror the copy holds no record of stays as it is: nothing is removed.
  TEST_ASSERT_FALSE(tx::restoreFoldRecord(mirrored, other, kChat, again));
  TEST_ASSERT_TRUE(again.empty());
}

// Lane E2-FIX2B, finding P10: a message answered with a device line (the history wait, a refusal)
// was tagged e2, so neither the import nor engine 2 ever had it. Now only a message an engine-2 turn
// stores is tagged; the others come in through the import once.
static void test_a_message_answered_with_a_device_line_joins_the_conversation_once() {
  Device dev;
  seed(dev);
  const uint32_t h = kHour0 + 28;
  // During the history wait (the first start waits for the index): answered with the reading line.
  dev.old.row("user", orch::MsgKind::Message, "log 2 coffees today", h, plain::ownerRowTags("Owner", false));
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.answer = "Two coffees, noted.";
  // A turn: its row is engine 2's (the turn stores the message itself).
  dev.old.row("user", orch::MsgKind::Message, "how many coffees today?", h, plain::ownerRowTags("Owner", true));
  TEST_ASSERT_TRUE(dev.say("how many coffees today?").ok());
  std::string body = dev.lastBody();
  TEST_ASSERT_EQUAL(1u, count(body, "log 2 coffees today"));
  TEST_ASSERT_EQUAL(1u, count(body, "how many coffees today?"));
  // Refused (no key, the monthly limit): the next boot of the chat imports it, once.
  dev.old.row("user", orch::MsgKind::Message, "and one tea", h + 1, plain::ownerRowTags("Owner", false));
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_EQUAL(1u, dev.binding->migration().imported);
  TEST_ASSERT_TRUE(dev.say("what did I drink?").ok());
  body = dev.lastBody();
  TEST_ASSERT_EQUAL(1u, count(body, "and one tea"));
  TEST_ASSERT_EQUAL(1u, count(body, "log 2 coffees today"));
  TEST_ASSERT_EQUAL(1u, count(body, "how many coffees today?"));
  TEST_ASSERT_EQUAL_STRING("from:Owner,e2", plain::ownerRowTags("Owner", true).c_str());
  TEST_ASSERT_EQUAL_STRING("e2", plain::ownerRowTags("", true).c_str());
  TEST_ASSERT_EQUAL_STRING("from:Owner", plain::ownerRowTags("Owner", false).c_str());
}

static void test_a_second_boot_imports_only_what_the_earlier_engine_wrote_since() {
  Device dev;
  seed(dev);
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.say("what is my locker code?").ok());
  // Engine 2's own rows of that turn reach the day streams tagged e2 (orchestrator_plain.cpp).
  const uint32_t h = kHour0 + 28;
  dev.old.row("user", orch::MsgKind::Message, "what is my locker code?", h, "from:Owner,e2");
  dev.old.row("assistant", orch::MsgKind::Message, "Your locker code is 4417.", h, "e2");
  // Back on the earlier engine for one turn (the way back), then engine 2 again.
  dev.old.row("user", orch::MsgKind::Message, "the spare key sits under the green pot", h + 1, "from:Owner");
  dev.old.row("assistant", orch::MsgKind::Message, "Noted, under the green pot.", h + 1);
  const std::string storeBefore = dev.old.store.serialize();
  dev.ms += 3600 * 1000;
  TEST_ASSERT_TRUE(dev.boot().ok());
  const tx::MigrationOutcome& o = dev.binding->migration();
  TEST_ASSERT_TRUE_MESSAGE(dev.binding->migrationStatus().ok(), dev.binding->migrationStatus().where);
  TEST_ASSERT_TRUE(o.reentry);
  TEST_ASSERT_EQUAL(2u, o.imported);
  TEST_ASSERT_EQUAL(1u, o.turns);
  TEST_ASSERT_TRUE(storeBefore == dev.old.store.serialize());

  dev.answer = "Under the green pot.";
  TEST_ASSERT_TRUE(dev.say("where is the spare key?").ok());
  const std::string body = dev.lastBody();
  TEST_ASSERT_EQUAL(1u, count(body, "the spare key sits under the green pot"));
  TEST_ASSERT_EQUAL(1u, count(body, "Noted, under the green pot."));
  TEST_ASSERT_EQUAL(1u, count(body, "my locker code is 4417"));   // nothing imported twice
  TEST_ASSERT_EQUAL(1u, count(body, "Your locker code is 4417."));   // engine 2's own reply once

  // A third boot with nothing new writes nothing.
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_EQUAL(0u, dev.binding->migration().imported);
  TEST_ASSERT_EQUAL(0u, dev.binding->migration().items);
}

// The fold record names a row engine 2 wrote (its compaction's mirror): the pointer is not the
// earlier engine's, so a first start imports the rows under it too.
// A conversation /clear started (E2-CHAT: the key <chat>_c<n>) begins empty: the import fills only
// the chat's own key, and a later boot of the own key still imports everything.
static void test_the_import_fills_only_the_chats_own_key() {
  Device dev;
  seed(dev);
  const std::string cleared = std::string(kChat) + "_c1";
  TEST_ASSERT_TRUE(dev.boot(cleared).ok());
  TEST_ASSERT_FALSE(dev.binding->migrationRan());
  TEST_ASSERT_EQUAL(0, dev.digestAsks);
  for (const auto& kv : dev.card.files) TEST_ASSERT_TRUE_MESSAGE(kv.first.find("/" + std::string(kChat) + "/") == std::string::npos, kv.first.c_str());
  TEST_ASSERT_TRUE(dev.say("what is my locker code?").ok());
  TEST_ASSERT_FALSE_MESSAGE(has(dev.lastBody(), "my locker code is 4417"), "the cleared conversation starts empty");
  bool marked = false;
  for (const auto& kv : dev.card.files) marked = marked || kv.first.find(cleared + "/migrated.json") != std::string::npos;
  TEST_ASSERT_FALSE(marked);
  // The own key is still imported whole.
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->migrationRan());
  TEST_ASSERT_EQUAL((uint32_t)(kPairs - 20), dev.binding->migration().turns);
}

static void test_a_mirrored_fold_pointer_is_not_taken_as_the_earlier_summary() {
  Device dev;
  dev.old.history(40);
  dev.old.row("user", orch::MsgKind::Message, "an engine-2 message", kHour0 + 28, "from:Owner,e2");
  const uint64_t mirrorRow = dev.old.next - 1;
  dev.old.row("assistant", orch::MsgKind::Message, "an engine-2 reply", kHour0 + 28, "e2");
  dev.old.fold.blob = foldBlob(mirrorRow);
  TEST_ASSERT_TRUE(dev.boot().ok());
  const tx::MigrationOutcome& o = dev.binding->migration();
  TEST_ASSERT_TRUE_MESSAGE(dev.binding->migrationStatus().ok(), dev.binding->migrationStatus().where);
  TEST_ASSERT_EQUAL(40u, o.turns);   // every earlier-engine owner row, though all sit under the pointer
  TEST_ASSERT_TRUE(dev.say("what is my locker code?").ok());
  TEST_ASSERT_TRUE(has(dev.lastBody(), "my locker code is 4417"));
  TEST_ASSERT_FALSE(has(dev.lastBody(), "an engine-2 message"));
  // A pointer at an earlier-engine row is kept.
  tx::EpisodicRowSource src(kChat, [&dev](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
    return dev.old.store.query(q, &info);
  });
  tx::OldFold f;
  f.thru = 0x100 + 10;
  bool dropped = true;
  TEST_ASSERT_TRUE(tx::dropMirroredPointer(src, f, dropped).ok());
  TEST_ASSERT_FALSE(dropped);
  TEST_ASSERT_EQUAL((uint64_t)(0x100 + 10), f.thru);
}

static void test_the_row_source_pages_forward_once_inside_its_bound() {
  OldEngine old;
  old.history(100);
  tx::EpisodicRowSource src(kChat, [&old](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
    return old.store.query(q, &info);
  });
  uint64_t newest = 0;
  TEST_ASSERT_TRUE(src.newest(newest).ok());
  std::vector<tx::DayRow> all, page;
  bool end = false;
  uint64_t after = 0;
  while (!end) {
    TEST_ASSERT_TRUE(src.read(after, 0, tx::kRowPage, page, end).ok());
    TEST_ASSERT_TRUE(page.size() <= tx::kRowPage);
    for (const tx::DayRow& r : page) all.push_back(r);
    if (!page.empty()) after = page.back().id;
  }
  TEST_ASSERT_TRUE(all.size() > 200);
  for (size_t i = 1; i < all.size(); ++i) TEST_ASSERT_TRUE(all[i - 1].id < all[i].id);
  TEST_ASSERT_EQUAL((uint64_t)all.back().id, newest);
  // Roles as the migration reads them.
  size_t owner = 0, photo = 0, tool = 0;
  for (const tx::DayRow& r : all) {
    owner += r.role == tx::RowRole::Owner;
    photo += r.photo;
    tool += r.role == tx::RowRole::Tool;
    TEST_ASSERT_TRUE(r.t > 0);
  }
  TEST_ASSERT_EQUAL(100u, owner);
  TEST_ASSERT_EQUAL(1u, photo);
  TEST_ASSERT_TRUE(tool > 0);
  // A newest-first page that is out of id order (the device's RAM ring after a restart) still
  // gives the real newest: the forward query walks from it.
  tx::EpisodicRowSource skewed(kChat, [&old](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
    if (!q.after.empty()) return old.store.query(q, &info);
    orch::MsgQuery first = q;
    first.after = tx::rowIdString(0);
    first.limit = 1;
    return old.store.query(first, &info);   // the oldest row, as if it came back first
  });
  uint64_t skewedNewest = 0;
  TEST_ASSERT_TRUE(skewed.newest(skewedNewest).ok());
  TEST_ASSERT_EQUAL((uint64_t)newest, skewedNewest);
  // The upper bound ends the read.
  TEST_ASSERT_TRUE(src.read(0, all[9].id, tx::kRowPage, page, end).ok());
  TEST_ASSERT_TRUE(end);
  TEST_ASSERT_EQUAL(10u, page.size());
  TEST_ASSERT_TRUE(tx::hasTag("from:Owner,e2", "e2"));
  TEST_ASSERT_FALSE(tx::hasTag("from:e2x", "e2"));
}

// The device's store after a restart: the boot scan indexes a suffix within its byte budget, the
// RAM ring holds the newest rows, the rest is the cold region. Every row of the chat after a
// mid-history cursor comes back once, oldest first, and newest is the real newest.
// 700 rounds of the chat (owner, assistant in turn) with a log row of another session between,
// 25 rounds an hour, written through the store; the chat's rows after round 200 are `want`.
static void writeHistory(EpiCard& card, std::vector<std::string>& want, uint64_t& cursorId, uint64_t& lastId) {
  orch::AppendLogEpisodicStore w(card, "/mem/episodic", 512);
  (void)w.hydrate(0, 0);
  uint32_t n = 0x100;
  for (int i = 0; i < 700; ++i) {
    for (const char* sess : {kChat, "system", kChat, kChat}) {
      const bool chat = std::string(sess) == kChat;
      orch::EpisodicMessage m;
      m.id = tx::rowIdString(n++);
      m.sessionId = sess;
      m.tsHours = kHour0 + (uint32_t)(i / 25);
      m.role = chat ? (i % 2 ? "assistant" : "user") : "system";
      m.kind = chat ? orch::MsgKind::Message : orch::MsgKind::Log;
      m.text = "row " + m.id + " " + std::string(300, 'x');
      w.addMessage(m);
      if (!chat) continue;
      if (i == 200 && !cursorId) cursorId = tx::rowIdNumber(m.id);
      if (cursorId && tx::rowIdNumber(m.id) > cursorId) want.push_back(m.id);
      lastId = tx::rowIdNumber(m.id);
    }
  }
}

static void test_the_row_source_over_the_card_store_after_a_restart() {
  EpiCard card;
  std::vector<std::string> want;   // the chat's rows after the cursor
  uint64_t cursorId = 0, lastId = 0;
  writeHistory(card, want, cursorId, lastId);
  orch::AppendLogEpisodicStore store(card, "/mem/episodic", 512);
  (void)store.hydrate(orch::kHydrateMaxRows, orch::kHydrateMaxBytes);
  TEST_ASSERT_TRUE_MESSAGE(store.hydrateTruncated(), "the boot scan stopped at its budget");
  tx::EpisodicRowSource src(kChat, [&store](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
    return store.query(q, &info);
  });
  uint64_t newest = 0;
  TEST_ASSERT_TRUE(src.newest(newest).ok());
  TEST_ASSERT_EQUAL((uint64_t)lastId, newest);
  std::vector<std::string> got;
  std::vector<tx::DayRow> page;
  bool end = false;
  uint64_t after = cursorId;
  for (int guard = 0; guard < 1000 && !end; ++guard) {
    TEST_ASSERT_TRUE(src.read(after, newest, tx::kRowPage, page, end).ok());
    for (const tx::DayRow& r : page) got.push_back(tx::rowIdString(r.id));
    if (!page.empty()) after = page.back().id;
  }
  TEST_ASSERT_EQUAL((int)want.size(), (int)got.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}

static std::vector<tx::OldRule> fiveRules() {
  return {{1, "Answer in short sentences."}, {2, "Use metric units."}, {3, "Never use em dashes."},
          {5, "Ask before booking anything."}, {7, "Weekdays start at 7."}};
}

static void test_the_rules_arrive_once_as_pinned_notes_and_reach_the_request() {
  Device dev;
  const orch::Principal owner = orch::principalForRole("owner", orch::Role::Admin);
  std::string marker, next;
  tx::RulesImportOutcome o;
  tx::importRules(fiveRules(), marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(5u, o.added);
  TEST_ASSERT_EQUAL(0u, o.refused);
  const std::string block = dev.notebook.pinnedBlock(owner);
  TEST_ASSERT_TRUE(has(block, "rule-p1 [rule]: Answer in short sentences."));
  TEST_ASSERT_TRUE(has(block, "rule-p7 [rule]: Weekdays start at 7."));
  // A second run adds nothing.
  marker = next;
  tx::importRules(fiveRules(), marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(0u, o.added);
  TEST_ASSERT_EQUAL(5u, o.kept);
  TEST_ASSERT_TRUE(next == marker);
  TEST_ASSERT_TRUE(block == dev.notebook.pinnedBlock(owner));
  TEST_ASSERT_EQUAL(1u, count(block, "rule-p3"));
  // The rules reach the request as pinned notes.
  seed(dev);
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.say("hello").ok());
  TEST_ASSERT_TRUE(has(dev.lastBody(), "Use metric units."));
}

static void test_an_edit_on_engine_two_is_kept_and_an_untouched_rule_follows_the_earlier_engine() {
  Device dev;
  const orch::Principal owner = orch::principalForRole("owner", orch::Role::Admin);
  std::string marker, next;
  tx::RulesImportOutcome o;
  tx::importRules(fiveRules(), marker, dev.notebook, owner, marker, o);
  // The owner edits rule-p2 on engine 2 and removes rule-p5.
  JsonDocument edit;
  edit["op"] = "set";
  edit["key"] = "rule-p2";
  edit["value"] = "Use metric units, and Celsius.";
  TEST_ASSERT_TRUE(dev.notebook.call(edit.as<JsonObjectConst>(), owner, orch::notes::Origin::Attended).success);
  JsonDocument rm;
  rm["op"] = "remove";
  rm["key"] = "rule-p5";
  TEST_ASSERT_TRUE(dev.notebook.call(rm.as<JsonObjectConst>(), owner, orch::notes::Origin::Attended).success);
  // Back on the earlier engine the owner changed p2, p3 and p5 and added p8.
  std::vector<tx::OldRule> changed = {{1, "Answer in short sentences."}, {2, "Use imperial units."},
                                      {3, "Never use em dashes or semicolons."}, {5, "Ask before booking."},
                                      {7, "Weekdays start at 7."}, {8, "Call me by my first name."}};
  tx::importRules(changed, marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(1u, o.added);     // p8
  TEST_ASSERT_EQUAL(1u, o.updated);   // p3: untouched on engine 2
  TEST_ASSERT_EQUAL(4u, o.kept);      // p1, p7 as copied; p2 edited and p5 removed on engine 2
  const std::string block = dev.notebook.pinnedBlock(owner);
  TEST_ASSERT_TRUE(has(block, "rule-p2 [rule]: Use metric units, and Celsius."));
  TEST_ASSERT_FALSE(has(block, "imperial"));
  TEST_ASSERT_FALSE(has(block, "rule-p5"));
  TEST_ASSERT_TRUE(has(block, "rule-p3 [rule]: Never use em dashes or semicolons."));
  TEST_ASSERT_TRUE(has(block, "rule-p8 [rule]: Call me by my first name."));
  // And once more: nothing changes.
  marker = next;
  tx::importRules(changed, marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(0u, o.added + o.updated);
  TEST_ASSERT_TRUE(block == dev.notebook.pinnedBlock(owner));
}


// Lane E2-FIX2B, finding P6: rules the owner deleted or retired on the earlier engine leave engine 2
// too, unless he edited the note on engine 2 (then it is his and stays). Before: the note stayed
// pinned and engine 2 kept following a rule he had removed.
static void test_a_rule_removed_on_the_earlier_engine_leaves_unless_edited_on_engine_two() {
  Device dev;
  const orch::Principal owner = orch::principalForRole("owner", orch::Role::Admin);
  std::string marker, next;
  tx::RulesImportOutcome o;
  tx::importRules(fiveRules(), marker, dev.notebook, owner, marker, o);
  JsonDocument edit;   // the owner edits rule-p2 on engine 2
  edit["op"] = "set";
  edit["key"] = "rule-p2";
  edit["value"] = "Use metric units, and Celsius.";
  TEST_ASSERT_TRUE(dev.notebook.call(edit.as<JsonObjectConst>(), owner, orch::notes::Origin::Attended).success);
  // On the earlier engine he deletes p2 and p3 and retires p7 (no longer active); p1 and p5 stay.
  std::vector<tx::OldRule> left;
  for (const tx::OldRule& r : fiveRules())
    if (r.id != 2 && r.id != 3 && r.id != 7) left.push_back(r);
  tx::importRules(left, marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(2u, o.removed);   // p3, p7
  TEST_ASSERT_EQUAL(0u, o.refused);
  const std::string block = dev.notebook.pinnedBlock(owner);
  TEST_ASSERT_FALSE(has(block, "rule-p3"));
  TEST_ASSERT_FALSE(has(block, "rule-p7"));
  TEST_ASSERT_TRUE(has(block, "rule-p2 [rule]: Use metric units, and Celsius."));   // his edit stays
  TEST_ASSERT_TRUE(has(block, "rule-p1 [rule]"));
  TEST_ASSERT_TRUE(has(block, "rule-p5 [rule]"));
  TEST_ASSERT_FALSE(has(next, "p3 "));
  TEST_ASSERT_FALSE(has(next, "p7 "));
  // Once more: nothing changes.
  marker = next;
  tx::importRules(left, marker, dev.notebook, owner, next, o);
  TEST_ASSERT_EQUAL(0u, o.added + o.updated + o.removed);
  TEST_ASSERT_TRUE(block == dev.notebook.pinnedBlock(owner));
  TEST_ASSERT_TRUE(next == marker);
}

static void test_the_updater_leaves_an_engine_two_image_on_its_version() {
  TEST_ASSERT_EQUAL_STRING("v5.9.0", NIMBUS_FW_VERSION);
  TEST_ASSERT_EQUAL_STRING(NIMBUS_FW_VERSION_ENGINE2, NIMBUS_FW_VERSION);
  namespace ota = nimbus::ota;
  ota::ManifestInfo stable;
  std::snprintf(stable.version, sizeof(stable.version), "%s", "v5.2.0");
  const ota::Eligibility e = ota::eligibility(NIMBUS_FW_VERSION, stable);
  TEST_ASSERT_EQUAL((int)ota::Eligibility::Older, (int)e);
  TEST_ASSERT_FALSE(ota::autoEligible(e));
  TEST_ASSERT_FALSE(ota::autoMayInstall(e, /*held=*/false));
  // A candidate only reaches an engine-2 image when it is strictly newer than v5.9.0.
  ota::ManifestInfo cand;
  std::snprintf(cand.version, sizeof(cand.version), "%s", "v5.9.0");
  TEST_ASSERT_FALSE(ota::autoEligible(ota::eligibility(NIMBUS_FW_VERSION, cand)));
  std::snprintf(cand.version, sizeof(cand.version), "%s", "v5.9.1");
  TEST_ASSERT_TRUE(ota::autoEligible(ota::eligibility(NIMBUS_FW_VERSION, cand)));
  TEST_ASSERT_EQUAL((int)ota::Pick::Candidate, (int)ota::pickManifest(true, "v5.2.0", true, "v5.9.1"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_old_history_imports_once_and_an_early_row_is_answerable);
  RUN_TEST(test_a_card_out_at_the_first_boot_imports_before_anything_is_stored);
  RUN_TEST(test_an_import_refused_before_its_marker_stores_nothing_until_done);
  RUN_TEST(test_with_no_card_fitted_the_import_holds_nothing);
  RUN_TEST(test_an_import_that_never_finishes_offers_clear_and_clear_keeps_the_held_turns);
  RUN_TEST(test_an_import_that_fails_once_is_not_stuck);
  RUN_TEST(test_turns_and_a_compaction_leave_the_earlier_fold_record_as_it_was);
  RUN_TEST(test_a_mirrored_fold_record_is_put_back_from_the_kept_copy);
  RUN_TEST(test_a_message_answered_with_a_device_line_joins_the_conversation_once);
  RUN_TEST(test_a_second_boot_imports_only_what_the_earlier_engine_wrote_since);
  RUN_TEST(test_the_import_fills_only_the_chats_own_key);
  RUN_TEST(test_a_mirrored_fold_pointer_is_not_taken_as_the_earlier_summary);
  RUN_TEST(test_the_row_source_pages_forward_once_inside_its_bound);
  RUN_TEST(test_the_row_source_over_the_card_store_after_a_restart);
  RUN_TEST(test_the_rules_arrive_once_as_pinned_notes_and_reach_the_request);
  RUN_TEST(test_an_edit_on_engine_two_is_kept_and_an_untouched_rule_follows_the_earlier_engine);
  RUN_TEST(test_a_rule_removed_on_the_earlier_engine_leaves_unless_edited_on_engine_two);
  RUN_TEST(test_the_updater_leaves_an_engine_two_image_on_its_version);
  return UNITY_END();
}
