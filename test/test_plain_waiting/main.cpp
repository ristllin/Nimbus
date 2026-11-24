// test_plain_waiting - the messages kept while a chat's earlier conversation is read in (lane
// E2-FIX13, nimbus/harness/plain/waiting.h), answered through a real ChatBinding over a fake card
// and a scripted Claude head on the real Anthropic wire:
//   - two messages that arrive during the read-in are answered after it, in arrival order, each
//     once, and the list leaves the card;
//   - a restart between arrival and the end of the read-in loses nothing (the list is on the card,
//     a re-served update is not kept twice), and a restart after a turn but before its removal
//     answers nothing twice (the transcript holds it);
//   - a read-in that failed still answers (the chat runs unloaded, with its line);
//   - a photo kept with its message reaches the turn from the card's copy;
//   - a list the card cannot read is never taken as empty, and one that does not decode is set aside.
#include <unity.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/chat_photos.h"
#include "nimbus/harness/plain/waiting.h"
#include "nimbus/orch/tx/card_files.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC
constexpr char kChat[] = "1001";

// A card that keeps files and folders (test_plain_chat_binding's), with a listing that can fail.
class Card : public tx::CardOps {
 public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs{"/"};
  bool failList = false;
  bool refuseWrites = false;
  bool present() override { return true; }
  bool mayWrite() override { return !refuseWrites; }
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
    if (failList) return false;
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (failList || !dirs.count(dir)) return false;
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

// The earlier engine's store that never answers: the import fails every time.
class DeadRows : public tx::RowSource {
 public:
  plain::Status read(uint64_t, uint64_t, size_t, std::vector<tx::DayRow>&, bool&) override {
    return plain::Status::fail(tx::Err::Io, "test.rows");
  }
  plain::Status newest(uint64_t&) override { return plain::Status::fail(tx::Err::Io, "test.rows"); }
};

struct Device {
  Device() : files(card) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
  }
  // A boot of the chat: the restart of the device when called again (RAM gone, the card stays).
  plain::Status boot() {
    binding.reset();
    plain::ChatBindingConfig c;
    c.chatKey = kChat;
    c.model = "claude-haiku-5-5";
    c.summarizerModel = "claude-haiku-5-5";
    c.timeZone = "Europe/London";
    plain::ChatBindingDeps d;
    d.platform = &platform;
    d.transport = &model;
    d.files = &files;
    d.registry = &reg;
    d.sendPart = [this](const std::string& chat, const std::string& part) {
      sent.push_back(chat + "|" + part);
      return true;
    };
    d.key = [] { return std::string("sk-fake-waiting"); };
    d.photo = [this](const std::string& ref, plain::PhotoBytes& o) { return plain::photoBytesFrom(stored, ref, o); };
    if (importFails)
      d.oldEngine = [this](tx::MigrateDeps& m) {
        m.rows = &deadRows;
        return true;
      };
    binding.reset(new plain::ChatBinding(c, d));
    return binding->boot();
  }
  // The device's answer to one kept message: one turn of the chat with what was kept.
  plain::WaitingDrainFns drain() {
    plain::WaitingDrainFns f;
    f.taken = [this](const plain::WaitingMessage& w) { return binding->holdsMessage(w.src, w.m); };
    f.answer = [this](const plain::WaitingMessage& w) {
      plain::ChatMessage msg;
      msg.text = w.text;
      msg.ch = w.ch;
      msg.src = w.src;
      msg.m = w.m;
      msg.parts = w.parts;
      msg.stored = w.stored;
      plain::TurnOutcome out;
      turns.push_back(binding->turn(msg, out));
      return plain::WaitingAnswer::Done;
    };
    return f;
  }

  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  Card card;
  tx::CardTxFiles files;
  nimbus::orch::ToolRegistry reg;
  DeadRows deadRows;
  bool importFails = false;
  nimbus::orch::TurnImages* stored = nullptr;   // the photos the card keeps
  std::vector<std::string> sent;
  std::vector<plain::Status> turns;
  std::unique_ptr<plain::ChatBinding> binding;
};

plain::WaitingMessage tgMessage(const std::string& text, uint64_t updateId, uint64_t row) {
  plain::WaitingMessage m;
  m.text = text;
  m.from = "bench";
  m.src = {updateId};
  m.m = row;
  return m;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

size_t count(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) n++;
  return n;
}

// Every byte the chat's transcript stored, in segment order.
std::string transcript(const Card& card) {
  std::string all;
  for (const auto& kv : card.files)
    if (kv.first.rfind(std::string("/mem/tx/") + kChat + "/seg-", 0) == 0) all += kv.second;
  return all;
}

const std::string kWaitPath = std::string("/mem/tx/") + kChat + ".wait.json";

}  // namespace

void setUp() {}
void tearDown() {}

// D2 (1): two messages during the read-in, answered after it in arrival order, each once.
static void test_two_messages_kept_during_a_read_in_are_answered_in_order_after_it() {
  Device dev;
  plain::WaitingInbox in(&dev.files, kChat);
  TEST_ASSERT_TRUE(in.load().ok());
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Kept, (int)in.hold(tgMessage("first: the plumber comes at nine", 501, 0x10)));
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Kept, (int)in.hold(tgMessage("second: what time is it in Lisbon", 502, 0x11)));
  TEST_ASSERT_TRUE_MESSAGE(dev.card.files.count(kWaitPath) == 1, "the list is on the card before anyone is told");
  TEST_ASSERT_EQUAL(0, (int)dev.model.requests().size());   // nothing is asked while the read-in runs

  TEST_ASSERT_TRUE(dev.boot().ok());   // the read-in ended: the chat starts
  dev.model.push(sp::say("Noted: the plumber at nine."));
  dev.model.push(sp::say("It is one hour behind London."));
  TEST_ASSERT_EQUAL(2, (int)plain::answerWaiting(in, dev.drain()));

  TEST_ASSERT_EQUAL(2, (int)dev.model.requests().size());
  TEST_ASSERT_TRUE(has(dev.model.requests()[0].body, "the plumber comes at nine"));
  TEST_ASSERT_FALSE(has(dev.model.requests()[0].body, "Lisbon"));
  TEST_ASSERT_TRUE(has(dev.model.requests()[1].body, "Lisbon"));
  TEST_ASSERT_EQUAL(2, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING("1001|Noted: the plumber at nine.", dev.sent[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1001|It is one hour behind London.", dev.sent[1].c_str());
  const std::string t = transcript(dev.card);
  TEST_ASSERT_EQUAL(1, (int)count(t, "the plumber comes at nine"));
  TEST_ASSERT_EQUAL(1, (int)count(t, "what time is it in Lisbon"));
  TEST_ASSERT_TRUE(t.find("the plumber comes at nine") < t.find("what time is it in Lisbon"));
  TEST_ASSERT_TRUE(in.empty());
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)dev.card.files.count(kWaitPath), "the list leaves the card when it is done");
  // A second pass finds nothing to do.
  TEST_ASSERT_EQUAL(0, (int)plain::answerWaiting(in, dev.drain()));
  TEST_ASSERT_EQUAL(2, (int)dev.model.requests().size());
}

// D2 (2a): a restart between arrival and the end of the read-in: the list comes back from the card,
// and Telegram's re-served update is not kept a second time.
static void test_a_restart_during_the_read_in_loses_nothing() {
  Device dev;
  {
    plain::WaitingInbox before(&dev.files, kChat);
    TEST_ASSERT_TRUE(before.load().ok());
    (void)before.hold(tgMessage("first after the update", 601, 0x20));
    (void)before.hold(tgMessage("second after the update", 602, 0x21));
  }   // the restart: RAM gone
  plain::WaitingInbox after(&dev.files, kChat);
  TEST_ASSERT_FALSE(after.loaded());
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Unread, (int)after.hold(tgMessage("x", 603, 0x22)));
  TEST_ASSERT_TRUE(after.load().ok());
  TEST_ASSERT_EQUAL(2, (int)after.messages().size());
  TEST_ASSERT_EQUAL_STRING("first after the update", after.messages()[0].text.c_str());
  TEST_ASSERT_EQUAL(0x20, (int)after.messages()[0].m);
  TEST_ASSERT_EQUAL(601, (int)after.messages()[0].src[0]);
  // The stored offset was behind: Telegram serves update 601 again.
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Duplicate, (int)after.hold(tgMessage("first after the update", 601, 0x30)));
  TEST_ASSERT_EQUAL(2, (int)after.messages().size());

  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("One."));
  dev.model.push(sp::say("Two."));
  TEST_ASSERT_EQUAL(2, (int)plain::answerWaiting(after, dev.drain()));
  TEST_ASSERT_EQUAL(2, (int)dev.sent.size());
  TEST_ASSERT_EQUAL_STRING("1001|One.", dev.sent[0].c_str());
  TEST_ASSERT_EQUAL_STRING("1001|Two.", dev.sent[1].c_str());
}

// D2 (2b): a restart after the first message's turn and before its removal: the transcript holds
// it, so it is not answered again; the second is answered once. Also for a message with no update
// id (the web chat): its day-stream row finds it.
static void test_a_restart_after_a_turn_answers_nothing_twice() {
  for (bool web : {false, true}) {
    Device dev;
    plain::WaitingInbox in(&dev.files, kChat);
    TEST_ASSERT_TRUE(in.load().ok());
    plain::WaitingMessage a = tgMessage("remember the blue drawer", 701, 0x40);
    plain::WaitingMessage b = tgMessage("where is the spare key?", 702, 0x41);
    if (web) {
      a.src.clear(), a.ch = "web";
      b.src.clear(), b.ch = "web";
    }
    (void)in.hold(a);
    (void)in.hold(b);
    TEST_ASSERT_TRUE(dev.boot().ok());
    dev.model.push(sp::say("Noted, the blue drawer."));
    // The first turn runs; the device restarts before the list is written again.
    plain::WaitingDrainFns crashAfterTurn = dev.drain();
    TEST_ASSERT_EQUAL((int)plain::WaitingAnswer::Done, (int)crashAfterTurn.answer(in.messages().front()));
    TEST_ASSERT_EQUAL(1, (int)dev.sent.size());

    TEST_ASSERT_TRUE(dev.boot().ok());   // the restart
    plain::WaitingInbox again(&dev.files, kChat);
    TEST_ASSERT_TRUE(again.load().ok());
    TEST_ASSERT_EQUAL(2, (int)again.messages().size());   // both still listed
    dev.model.push(sp::say("In the blue drawer."));
    TEST_ASSERT_EQUAL(1, (int)plain::answerWaiting(again, dev.drain()));
    TEST_ASSERT_EQUAL(2, (int)dev.model.requests().size());
    TEST_ASSERT_EQUAL(2, (int)dev.sent.size());
    TEST_ASSERT_EQUAL_STRING("1001|In the blue drawer.", dev.sent[1].c_str());
    TEST_ASSERT_EQUAL(1, (int)count(transcript(dev.card), "remember the blue drawer"));
    TEST_ASSERT_TRUE(again.empty());
  }
}

// D2 (3): the read-in failed (the earlier engine's store did not answer): the kept messages are
// still answered, on what the chat holds, and the person is told the import is not done.
static void test_a_failed_read_in_still_answers() {
  Device dev;
  dev.importFails = true;
  plain::WaitingInbox in(&dev.files, kChat);
  TEST_ASSERT_TRUE(in.load().ok());
  (void)in.hold(tgMessage("are you there?", 801, 0x50));
  (void)in.hold(tgMessage("hello again", 802, 0x51));
  TEST_ASSERT_TRUE(dev.boot().ok());
  TEST_ASSERT_TRUE(dev.binding->migrationRan());
  TEST_ASSERT_TRUE_MESSAGE(dev.binding->importPending(), (std::string(tx::errSlug(dev.binding->migrationStatus().err)) + "@" +
                                                          (dev.binding->migrationStatus().where ? dev.binding->migrationStatus().where : "")).c_str());
  dev.model.push(sp::say("Yes, here."));
  dev.model.push(sp::say("Hello."));
  TEST_ASSERT_EQUAL(2, (int)plain::answerWaiting(in, dev.drain()));
  int answers = 0, lines = 0;
  for (const std::string& s : dev.sent) {
    answers += s == "1001|Yes, here." || s == "1001|Hello.";
    lines += has(s, plain::kImportPendingLine) || has(s, plain::kImportStuckLine);   // the true cause, each turn
  }
  TEST_ASSERT_EQUAL(2, answers);
  std::string all;
  for (const std::string& s : dev.sent) all += s + "\n";
  TEST_ASSERT_TRUE_MESSAGE(lines >= 1, all.c_str());
  TEST_ASSERT_TRUE(in.empty());
}

// D1: a photo kept with its message reaches its turn as an image, read from the card's copy.
static void test_a_kept_photo_reaches_its_turn_from_the_card() {
  static const uint8_t kJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x02, 0x03};
  nimbus::orch::TurnImages card;
  nimbus::orch::TurnImage img;
  img.ref = "0123456789abcdef";
  img.mime = "image/jpeg";
  img.bytes = kJpeg;
  img.len = sizeof(kJpeg);
  card.items.push_back(img);
  Device dev;
  dev.stored = &card;
  plain::WaitingInbox in(&dev.files, kChat);
  TEST_ASSERT_TRUE(in.load().ok());
  plain::WaitingMessage m = tgMessage("[image 1/1 ref=0123456789abcdef] what is this?", 901, 0x60);
  m.parts = plain::photoParts(&card, "anthropic", "claude-haiku-5-5");
  m.stored = {"inbox/receipt.pdf"};
  TEST_ASSERT_EQUAL(1, (int)m.parts.size());
  (void)in.hold(m);
  plain::WaitingInbox reread(&dev.files, kChat);   // as a restart reads it
  TEST_ASSERT_TRUE(reread.load().ok());
  TEST_ASSERT_EQUAL(1, (int)reread.messages()[0].parts.size());
  TEST_ASSERT_EQUAL_STRING("p0123456789abcdef", reread.messages()[0].parts[0].ref.c_str());
  TEST_ASSERT_EQUAL_STRING("inbox/receipt.pdf", reread.messages()[0].stored[0].c_str());
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("A small test picture."));
  TEST_ASSERT_EQUAL(1, (int)plain::answerWaiting(reread, dev.drain()));
  TEST_ASSERT_EQUAL(1, (int)count(dev.model.last().body, "\"type\":\"image\""));
}

// Ruling 116: a list the card cannot read is not an empty one; a list that does not decode is set
// aside whole; a removal the card refused leaves the message first, found taken on the next pass.
static void test_an_unread_list_is_never_empty_and_a_refused_removal_is_not_lost() {
  Device dev;
  {
    plain::WaitingInbox in(&dev.files, kChat);
    TEST_ASSERT_TRUE(in.load().ok());
    (void)in.hold(tgMessage("kept", 1001, 0x70));
  }
  const std::string kept = dev.card.files[kWaitPath];
  dev.card.failList = true;
  plain::WaitingInbox unread(&dev.files, kChat);
  TEST_ASSERT_FALSE(unread.load().ok());
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Unread, (int)unread.hold(tgMessage("new", 1002, 0x71)));
  TEST_ASSERT_EQUAL(0, (int)plain::answerWaiting(unread, dev.drain()));
  TEST_ASSERT_EQUAL_STRING(kept.c_str(), dev.card.files[kWaitPath].c_str());   // nothing written over it
  dev.card.failList = false;

  // The removal after the turn is refused: still first, in RAM and on the card; the next pass pops it.
  plain::WaitingInbox in(&dev.files, kChat);
  TEST_ASSERT_TRUE(in.load().ok());
  TEST_ASSERT_TRUE(dev.boot().ok());
  dev.model.push(sp::say("Kept."));
  plain::WaitingDrainFns f = dev.drain();
  plain::WaitingDrainFns refuseAfter = f;
  refuseAfter.answer = [&](const plain::WaitingMessage& w) {
    const plain::WaitingAnswer a = f.answer(w);
    dev.card.refuseWrites = true;
    return a;
  };
  TEST_ASSERT_EQUAL(1, (int)plain::answerWaiting(in, refuseAfter));
  TEST_ASSERT_EQUAL(1, (int)in.messages().size());
  dev.card.refuseWrites = false;
  TEST_ASSERT_EQUAL(0, (int)plain::answerWaiting(in, f));   // taken: popped with no second turn
  TEST_ASSERT_TRUE(in.empty());
  TEST_ASSERT_EQUAL(1, (int)dev.model.requests().size());

  // A list that does not decode: set aside as it is, never removed; the chat starts empty.
  dev.card.files[kWaitPath] = "{\"v\":1,\"msgs\":[{\"text\":";
  plain::WaitingInbox torn(&dev.files, kChat);
  TEST_ASSERT_TRUE(torn.load().ok());
  TEST_ASSERT_TRUE(torn.empty());
  int aside = 0;
  for (const auto& kv : dev.card.files)
    aside += kv.first.rfind(kWaitPath + ".unread-", 0) == 0 && kv.second == "{\"v\":1,\"msgs\":[{\"text\":";
  TEST_ASSERT_EQUAL(1, aside);
}

// The list's own bounds: kWaitingMax messages, the next one is refused (none dropped); no card: RAM.
static void test_the_list_is_bounded_and_runs_in_ram_with_no_card() {
  Device dev;
  plain::WaitingInbox in(&dev.files, kChat);
  TEST_ASSERT_TRUE(in.load().ok());
  for (size_t i = 0; i < plain::kWaitingMax; ++i)
    TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Kept, (int)in.hold(tgMessage("m" + std::to_string(i), 2000 + i, 0x100 + i)));
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::Full, (int)in.hold(tgMessage("one too many", 3000, 0x200)));
  TEST_ASSERT_EQUAL((int)plain::kWaitingMax, (int)in.messages().size());
  plain::WaitingInbox ram(nullptr, "web");
  TEST_ASSERT_TRUE(ram.load().ok());
  plain::WaitingMessage w = tgMessage("from the web page", 0, 0x300);
  w.src.clear();
  TEST_ASSERT_EQUAL((int)plain::HoldVerdict::KeptInRam, (int)ram.hold(w));
  TEST_ASSERT_TRUE(ram.pop().ok());
  TEST_ASSERT_TRUE(ram.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_two_messages_kept_during_a_read_in_are_answered_in_order_after_it);
  RUN_TEST(test_a_restart_during_the_read_in_loses_nothing);
  RUN_TEST(test_a_restart_after_a_turn_answers_nothing_twice);
  RUN_TEST(test_a_failed_read_in_still_answers);
  RUN_TEST(test_a_kept_photo_reaches_its_turn_from_the_card);
  RUN_TEST(test_an_unread_list_is_never_empty_and_a_refused_removal_is_not_lost);
  RUN_TEST(test_the_list_is_bounded_and_runs_in_ram_with_no_card);
  return UNITY_END();
}
