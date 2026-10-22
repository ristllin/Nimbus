#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "nimbus/net/tg_receive.h"
#include "nimbus/orch/media_label.h"

// The Telegram receive cycle (nimbus/net/tg_receive.h, the code src/agent/telegram.cpp
// runs) against a fake Telegram, a fake card and a millisecond clock, over random
// bursts: 1-3 chats, albums of 2-10 photos with the caption on a random item, voice
// notes, commands and texts, gaps of 0-10 s, turns of 5-120 s, restarts of every kind
// at a random step, and a full settings store. The MASTER properties:
//   - each update lands in exactly one turn, or twice only if it was in the page in
//     flight at a restart (at most one replayed page);
//   - arrival order per chat is kept;
//   - no item is acked (the cursor past it, or Telegram told) before its turn returns;
//   - at most one TLS session is open at a time;
//   - the modelled PSRAM stays within budget;
//   - no settings (NVS) write happens during a hold;
//   - a held page re-polls at most ceil(quiet / 1000) + 1 times while it does not grow,
//     and no hold outlives the maximum;
//   - the poison guard acks a page after 2 crash resets on it;
//   - the 15 Oct sequence: the turn that contains the follow-up text also contains all
//     three photo segments, and no turn with that text runs before photo 3 is rendered.

using nimbus::core::TgOffsetCursor;
using nimbus::net::BootKind;
using nimbus::net::TgCycle;
using nimbus::net::TgFetch;
using nimbus::net::TgGuard;
using nimbus::net::TgGuardState;
using nimbus::net::TgNotice;
using nimbus::net::TgReceiver;
using nimbus::orch::ClassifiedUpdate;
using nimbus::orch::MessageBatcher;
using nimbus::tg::Attachment;
using nimbus::tg::Update;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kBuild = 0x1234ABCDu;
static constexpr int64_t kEpoch0 = 1792400000;   // the device clock at t = 0 (set)
static constexpr size_t kSlots = nimbus::orch::kBatchMaxMediaPerDrain;
// PSRAM model (wf2 section 1.7): the 128 KB poll body, the slot array, one image's
// base64 while it is described, the vision reply, and the descriptions held.
static constexpr size_t kPollBody = 128 * 1024, kSlotBytes = 600, kB64 = 699056, kVisionReply = 16384;
static constexpr size_t kPsramBudget = 1024 * 1024;

struct Restart {
  BootKind kind;
};

// ---- the fake world ---------------------------------------------------------------

struct Msg {
  int32_t uid;
  std::string chat;
  int64_t mid;
  int64_t date;
  Attachment::Kind kind = Attachment::Kind::None;
  std::string text;          // text or caption
  std::string group;         // media_group_id
  uint32_t arriveMs = 0;
  bool poison = false;       // processing it crashes the device (panic)
  bool turnPoison = false;   // a turn carrying it crashes the device
  bool classifyPoison = false;   // classifying it crashes the device
  bool fetchFails = false;   // its download fails (the label says why)
};

struct World {
  uint32_t now = 0;
  std::vector<Msg> all;      // every update ever sent, ascending uid
  int32_t confirmed = 0;     // Telegram has deleted every uid below this
  int nextUid = 5000;
  // Device-side facts.
  bool pollOpen = false;
  int workSessions = 0;      // open non-poll TLS sessions (must stay 0 or 1, never with the poll socket)
  size_t psram = kPollBody, psramPeak = kPollBody;
  // The settings store: the offset key.
  bool storeFull = false;
  int32_t stored = 0;
  int writes = 0;
  bool holding = false;
  int writesDuringHold = 0;
  // RTC no-init memory: survives every restart but a power-on.
  TgGuardState rtc{};

  int64_t epoch() const { return kEpoch0 + now / 1000; }
  void psramAdd(size_t n) {
    psram += n;
    psramPeak = std::max(psramPeak, psram);
  }
  void psramSub(size_t n) { psram -= n; }
  bool writeOffset(int32_t v) {
    ++writes;
    if (holding) ++writesDuringHold;
    if (v != 0 && storeFull) return false;
    stored = v;
    return true;
  }
  Msg& send(const std::string& chat, Attachment::Kind kind, const std::string& text, uint32_t at,
            const std::string& group = "") {
    Msg m;
    m.uid = nextUid++;
    m.chat = chat;
    m.mid = m.uid * 3;
    m.date = kEpoch0 + at / 1000;
    m.kind = kind;
    m.text = text;
    m.group = group;
    m.arriveMs = at;
    all.push_back(m);
    return all.back();
  }
  const Msg* byUid(int32_t uid) const {
    for (const Msg& m : all)
      if (m.uid == uid) return &m;
    return nullptr;
  }
};

// What the turns carried, for the properties.
struct Turn {
  std::string chat;
  std::string text;
  bool command = false;
  uint32_t startMs = 0;
  std::vector<int32_t> uids;   // every "#<uid>#" in the text, in order
};

static std::vector<int32_t> uidsIn(const std::string& t) {
  std::vector<int32_t> out;
  for (size_t p = t.find('#'); p != std::string::npos; p = t.find('#', p + 1)) {
    const size_t e = t.find('#', p + 1);
    if (e == std::string::npos) break;
    const std::string mid = t.substr(p + 1, e - p - 1);
    if (!mid.empty() && mid.find_first_not_of("0123456789") == std::string::npos) {
      out.push_back(std::stoi(mid));
      p = e;
    }
  }
  return out;
}

// The device: every RAM object a restart loses, and the Io the receiver calls.
struct Device {
  World& w;
  std::mt19937& rng;
  TgOffsetCursor cursor;
  MessageBatcher batch;
  TgGuard guard;
  TgReceiver rx;
  std::vector<Turn>& turns;
  std::map<int, int32_t> slots;   // slot -> uid
  std::vector<int32_t> inFlight;  // uids of the page being processed
  int restartPct = 0;             // chance (per mille) of a restart at each step
  uint32_t turnMinMs = 5000, turnMaxMs = 120000;
  int dupNotices = 0, ceilingNotices = 0, poisonNotices = 0, failedReplies = 0;
  std::vector<int32_t> lastPage;   // uids of the page fetched last
  std::set<int32_t>* poisoned = nullptr;   // uids acked unprocessed by the poison guard

  Device(World& world, std::mt19937& r, std::vector<Turn>& t, BootKind boot)
      : w(world), rng(r), guard(world.rtc), rx(cursor, batch, guard), turns(t) {
    guard.boot(boot, kBuild);
    cursor.seed(w.stored);
  }

  void maybeRestart() {
    if (restartPct && (int)(rng() % 1000) < restartPct) throw Restart{(BootKind)(rng() % 3)};
  }
  void closePoll() { w.pollOpen = false; }
  void work(uint32_t ms) {   // one non-poll TLS session: the poll socket closes first
    closePoll();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.workSessions, "two TLS sessions at once");
    ++w.workSessions;
    w.now += ms;
    --w.workSessions;
  }

  static Update toUpdate(const Msg& m) {
    Update u;
    u.updateId = m.uid;
    u.chatId = m.chat;
    u.from = "u" + m.chat;
    u.messageId = m.mid;
    u.date = m.date;
    u.mediaGroupId = m.group;
    u.text = m.text;
    u.attachment.kind = m.kind;
    if (m.kind != Attachment::Kind::None) {
      u.attachment.fileId = "f" + std::to_string(m.uid);
      u.attachment.fileUniqueId = "U" + std::to_string(m.uid);
      u.textIsCaption = !m.text.empty();
    }
    return u;
  }

  // ---- Io ----
  TgFetch fetch(int32_t offset, int timeoutS, std::vector<Update>& out) {
    maybeRestart();
    TgFetch f;
    f.rp.limit = 10;
    if (!w.pollOpen) w.now += 1200;   // handshake
    w.pollOpen = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, w.workSessions, "poll while a work session is open");
    w.confirmed = std::max(w.confirmed, offset);   // getUpdates confirms below its offset
    // Telegram serves what it still holds: nothing below the confirmed point.
    const int32_t from = std::max(offset, w.confirmed);
    auto ready = [&] {
      for (const Msg& m : w.all)
        if (m.uid >= from && m.arriveMs <= w.now) return true;
      return false;
    };
    if (!ready() && timeoutS > 0) {
      uint32_t next = w.now + (uint32_t)timeoutS * 1000;
      for (const Msg& m : w.all)
        if (m.uid >= from && m.arriveMs > w.now) next = std::min(next, m.arriveMs);
      w.now = next;
    }
    w.now += 150;   // round trip
    for (const Msg& m : w.all)
      if (m.uid >= from && m.arriveMs <= w.now && (int)out.size() < f.rp.limit) out.push_back(toUpdate(m));
    f.rp.count = (int)out.size();
    lastPage.clear();
    for (const Update& u : out) lastPage.push_back(u.updateId);
    return f;
  }
  uint32_t nowMs() const { return w.now; }
  int64_t nowEpoch() const { return w.epoch(); }
  uint32_t bestEpoch() const { return (uint32_t)w.epoch(); }
  bool allowed(const std::string& chat) const { return chat != "666"; }   // 666: a stranger
  bool isOwnerChat(const std::string& chat) const { return chat != "300"; }   // 300: a member
  ClassifiedUpdate classify(const Update& u) {
    const Msg* src = w.byUid(u.updateId);
    if (src && src->classifyPoison) throw Restart{BootKind::Crash};
    ClassifiedUpdate cu;
    cu.updateId = u.updateId;
    cu.action = ClassifiedUpdate::Action::AckOnly;
    if (!allowed(u.chatId)) return cu;
    cu.chatId = u.chatId;
    cu.from = u.from;
    cu.text = u.text;
    const Attachment::Kind k = u.attachment.kind;
    if (k == Attachment::Kind::Photo || k == Attachment::Kind::Voice) {
      if (slots.size() >= kSlots) {
        cu.action = ClassifiedUpdate::Action::StopNoAck;
        return cu;
      }
      int s = 0;
      while (slots.count(s)) ++s;
      slots[s] = u.updateId;
      if (slots.size() == 1) w.psramAdd(kSlots * kSlotBytes);   // the lazy slot array
      cu.slot = s;
      cu.action = k == Attachment::Kind::Photo ? ClassifiedUpdate::Action::BatchMedia
                                               : ClassifiedUpdate::Action::BatchVoice;
      return cu;
    }
    cu.action = ClassifiedUpdate::Action::BatchText;
    return cu;
  }
  bool processMedia(size_t, size_t k, size_t n, int slot, std::string& seg) {
    const int32_t uid = slots.at(slot);
    const Msg* m = w.byUid(uid);
    maybeRestart();
    work(1500 + rng() % 2000);   // getFile + download
    if (m && m->poison) throw Restart{BootKind::Crash};
    if (m && m->fetchFails) {
      nimbus::orch::MediaLabel f;
      f.k = (int)k + 1;
      f.n = (int)n;
      f.cls = nimbus::orch::MediaClass::FetchFailed;
      f.detail = "download failed #" + std::to_string(uid) + "#";
      seg = nimbus::orch::mediaSegment(f);
      return false;
    }
    w.psramAdd(kB64 + kVisionReply);
    work(4000 + rng() % 4000);   // vision
    w.psramSub(kB64 + kVisionReply);
    w.psramAdd(nimbus::orch::kMediaTextMax);   // the description, held until the page ends
    maybeRestart();
    nimbus::orch::MediaLabel l;
    l.k = (int)k + 1;
    l.n = (int)n;
    char ref[17];
    std::snprintf(ref, sizeof ref, "%016x", (unsigned)uid);
    l.ref = ref;
    l.detail = "photo #" + std::to_string(uid) + "#";
    seg = nimbus::orch::mediaSegment(l);
    return true;
  }
  void mediaFailed(const std::string&, size_t) { ++failedReplies; }
  void skipMedia(size_t, size_t k, size_t n, int slot, std::string& seg) {
    nimbus::orch::MediaLabel l;
    l.k = (int)k + 1;
    l.n = (int)n;
    l.cls = nimbus::orch::MediaClass::FetchFailed;
    l.detail = "skipped #" + std::to_string(slots.at(slot)) + "#";
    seg = nimbus::orch::mediaSegment(l);
  }
  bool processVoice(int slot, std::string& transcript) {
    const int32_t uid = slots.at(slot);
    maybeRestart();
    work(3000);
    transcript = "voice #" + std::to_string(uid) + "#";
    return true;
  }
  void runTurn(const std::string&, const std::string& chat, const nimbus::orch::PsString& text, bool command) {
    Turn t;
    t.chat = chat;
    t.text = text.c_str();
    t.command = command;
    t.startMs = w.now;
    t.uids = uidsIn(t.text);
    // Nothing in this turn is acked yet: the cursor is not past it, and Telegram has
    // not been told (it confirms only what a later poll's offset passes).
    for (int32_t id : t.uids) {
      TEST_ASSERT_TRUE_MESSAGE(id >= cursor.offset(), "an item was acked before its turn ran");
      TEST_ASSERT_TRUE_MESSAGE(id >= w.confirmed, "Telegram was told before the turn ran");
    }
    turns.push_back(t);
    maybeRestart();
    for (int32_t id : t.uids)
      if (const Msg* m = w.byUid(id))
        if (m->turnPoison) throw Restart{BootKind::Crash};
    work(turnMinMs + rng() % (turnMaxMs - turnMinMs + 1));
    maybeRestart();
  }
  void notice(const TgNotice& n) {
    if (n.kind == TgNotice::Kind::Duplicate) ++dupNotices;
    if (n.kind == TgNotice::Kind::Ceiling) ++ceilingNotices;
    if (n.kind == TgNotice::Kind::Poison) {
      ++poisonNotices;
      TEST_ASSERT_TRUE(n.items >= 1 && n.items <= (int)lastPage.size());
      if (poisoned) poisoned->insert(lastPage.begin(), lastPage.end());
    }
  }
  void checkpoint() {
    cursor.checkpoint(w.now, [&](int32_t v) { return w.writeOffset(v); });
  }
  void endPage() {
    if (!slots.empty()) w.psramSub(kSlots * kSlotBytes);
    slots.clear();
    w.psram = kPollBody;   // descriptions and slots freed with the page
  }

  // One poll-task cycle's Telegram part, as pollTask runs it.
  TgCycle cycleOnce() {
    w.holding = false;   // a dispatch may checkpoint a command page: not a hold
    const TgCycle c = rx.cycle(*this, 30);
    w.holding = rx.holding();
    if (!rx.holding()) cursor.persist(w.now, [&](int32_t v) { return w.writeOffset(v); });
    w.now += nimbus::net::kSettleRepollMs;   // the inter-cycle pause
    return c;
  }
};

// Run the world until every message has arrived and the device is idle, restarting
// the device whenever a step throws.
struct Run {
  World w;
  std::mt19937 rng;
  std::vector<Turn> turns;
  int restarts = 0, powerOns = 0;
  int restartPct = 0;
  int maxHeldRun = 0;
  std::set<int32_t> replayable;   // uids of pages in flight at a restart
  std::set<int32_t> poisoned;     // uids of pages the poison guard acked unprocessed
  int dupNotices = 0, ceilingNotices = 0, poisonNotices = 0;

  explicit Run(uint32_t seed) : rng(seed) {}

  void go(uint32_t untilMs, BootKind firstBoot = BootKind::PowerOn) {
    BootKind boot = firstBoot;
    for (int lives = 0; lives < 200 && (w.now < untilMs || pending()); ++lives) {
      Device d(w, rng, turns, boot);
      d.restartPct = restartPct;
      d.poisoned = &poisoned;
      try {
        int held = 0;
        for (int guard = 0; guard < 20000 && (w.now < untilMs || pending()); ++guard) {
          const int32_t first = d.cursor.offset();
          const TgCycle c = d.cycleOnce();
          held = c.kind == TgCycle::Kind::Held ? held + 1 : 0;
          maxHeldRun = std::max(maxHeldRun, held);
          (void)first;
        }
        tally(d);
        return;
      } catch (const Restart& r) {
        ++restarts;
        if (r.kind == BootKind::PowerOn) ++powerOns;
        for (const Msg& m : w.all)
          if (m.uid >= w.confirmed) replayable.insert(m.uid);   // unconfirmed: may run again
        tally(d);
        w.pollOpen = false;
        w.workSessions = 0;
        w.psram = kPollBody;
        boot = r.kind;
      }
    }
  }
  bool pending() const {
    for (const Msg& m : w.all)
      if (m.uid >= w.confirmed) return true;
    return false;
  }
  void tally(const Device& d) {
    dupNotices += d.dupNotices;
    ceilingNotices += d.ceilingNotices;
    poisonNotices += d.poisonNotices;
  }
};

// ---- properties ------------------------------------------------------------------

static bool turnCandidate(const Msg& m) { return m.chat != "666"; }

static void checkExactlyOnce(const Run& r, bool restartsOk) {
  std::map<int32_t, int> seen;
  for (const Turn& t : r.turns)
    for (int32_t id : t.uids) ++seen[id];
  for (const Msg& m : r.w.all) {
    if (!turnCandidate(m)) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, seen[m.uid], "a stranger's message ran a turn");
      continue;
    }
    if (m.poison) continue;
    const int n = seen[m.uid];
    if (r.poisoned.count(m.uid)) {   // two crash resets on its page: acked, owner told
      TEST_ASSERT_TRUE(r.poisonNotices > 0);
      TEST_ASSERT_TRUE(n <= 2 + r.restarts);
      continue;
    }
    char msg[96];
    std::snprintf(msg, sizeof msg, "uid %d ran %d times (restarts %d)", m.uid, n, r.restarts);
    if (!restartsOk || !r.replayable.count(m.uid)) TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, msg);
    else TEST_ASSERT_TRUE_MESSAGE(n >= 1 && n <= 2 + r.restarts, msg);
  }
}

static void checkOrderPerChat(const Run& r) {
  std::map<std::string, int32_t> last;
  std::set<int32_t> done;
  for (const Turn& t : r.turns)
    for (int32_t id : t.uids) {
      if (done.count(id)) continue;   // a replay after a restart
      done.insert(id);
      TEST_ASSERT_TRUE_MESSAGE(id > last[t.chat], "arrival order lost within a chat");
      last[t.chat] = id;
    }
}

static void sendBurst(World& w, std::mt19937& rng, uint32_t& at, const std::string& chat) {
  const int shape = (int)(rng() % 5);
  if (shape <= 1) {   // an album of 2-10 with the caption on a random item
    const int n = 2 + (int)(rng() % 9);
    const int capAt = (int)(rng() % n);
    const std::string g = "G" + std::to_string(at);
    for (int i = 0; i < n; ++i) {
      w.send(chat, Attachment::Kind::Photo, i == capAt ? "dinner, all of it" : "", at, g);
      at += rng() % 700;   // the phone uploads the items one by one
    }
    if (rng() % 2) w.send(chat, Attachment::Kind::None, "did you see them all?", at + 1500);
  } else if (shape == 2) {
    w.send(chat, Attachment::Kind::Voice, "", at);
  } else if (shape == 3) {
    w.send(chat, Attachment::Kind::None, "/status", at);
  } else {
    for (int i = 0, n = 1 + (int)(rng() % 4); i < n; ++i) {
      Msg& m = w.send(chat, Attachment::Kind::None, "", at);
      m.text = "text #" + std::to_string(m.uid) + "# " + std::to_string(rng() % 1000);
      at += rng() % 1200;
    }
  }
  at += rng() % 10000;
}

// Text messages and commands carry their uid marker in the text; photos and voice
// notes get it in their segment text (the fake processMedia / processVoice).
static void markTexts(World& w) {
  for (Msg& m : w.all)
    if (m.kind == Attachment::Kind::None && m.text.find('#') == std::string::npos)
      m.text += " #" + std::to_string(m.uid) + "#";
}

// The MASTER property over random bursts with no restarts: exactly once, in order, no
// early ack, one TLS session, PSRAM within budget, no write during a hold, bounded holds.
static void test_random_bursts_exactly_once_in_order() {
  size_t turnsTotal = 0, multiTurns = 0;
  int heldMax = 0;
  for (uint32_t seed = 1; seed <= 60; ++seed) {
    Run r(seed);
    uint32_t at = 2000;
    const int chats = 1 + (int)(r.rng() % 3);
    for (int b = 0; b < 8; ++b) sendBurst(r.w, r.rng, at, std::to_string(100 + (int)(r.rng() % chats)));
    markTexts(r.w);
    r.w.storeFull = seed % 3 == 0;   // a full settings store on every third run
    r.go(at + 5000);
    checkExactlyOnce(r, false);
    checkOrderPerChat(r);
    TEST_ASSERT_TRUE_MESSAGE(r.w.psramPeak <= kPsramBudget, "PSRAM over budget");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.w.writesDuringHold, "a settings write during a hold");
    const int maxHolds = (int)(nimbus::net::kSettleMaxHoldMs / nimbus::net::kSettleRepollMs) + 1;
    TEST_ASSERT_TRUE_MESSAGE(r.maxHeldRun <= maxHolds, "a hold outlived the maximum");
    TEST_ASSERT_EQUAL_INT(0, r.dupNotices);
    turnsTotal += r.turns.size();
    for (const Turn& t : r.turns) multiTurns += t.uids.size() > 1;
    heldMax = std::max(heldMax, r.maxHeldRun);
  }
  // Not vacuous: the runs batched real bursts and held pages along the way.
  TEST_ASSERT_TRUE(turnsTotal > 200);
  TEST_ASSERT_TRUE(multiTurns > 50);
  TEST_ASSERT_TRUE(heldMax >= 2);
}

// With restarts of every kind at random steps: still every update runs, at most the
// pages in flight at a restart run twice, order holds, and the device never wedges.
static void test_random_bursts_with_restarts() {
  int restarts = 0, replayedSeeds = 0, poisonedSeeds = 0;
  for (uint32_t seed = 100; seed <= 160; ++seed) {
    Run r(seed);
    r.restartPct = 15;   // 1.5% of steps
    uint32_t at = 2000;
    for (int b = 0; b < 6; ++b) sendBurst(r.w, r.rng, at, std::to_string(200 + (int)(r.rng() % 2)));
    markTexts(r.w);
    r.w.storeFull = seed % 2 == 0;
    r.go(at + 5000);
    checkExactlyOnce(r, true);
    checkOrderPerChat(r);
    TEST_ASSERT_TRUE(r.w.psramPeak <= kPsramBudget);
    TEST_ASSERT_EQUAL_INT(0, r.w.writesDuringHold);
    restarts += r.restarts;
    replayedSeeds += !r.replayable.empty();
    poisonedSeeds += !r.poisoned.empty();
  }
  TEST_ASSERT_TRUE(restarts > 30);   // the restarts really happened, at many points
  TEST_ASSERT_TRUE(replayedSeeds > 10);
  (void)poisonedSeeds;
}

// An album that fully arrived before the dispatch decision lands in one turn (unless
// the page is full), with every photo numbered k/N.
static void test_album_lands_in_one_turn() {
  for (int n = 1; n <= 10; ++n) {
    Run r(7);
    uint32_t at = 1000;
    for (int i = 0; i < n; ++i) r.w.send("77", Attachment::Kind::Photo, i == 0 ? "lunch" : "", at += 400, "G1");
    r.go(at + 30000);
    TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
    TEST_ASSERT_EQUAL_UINT((size_t)n, r.turns[0].uids.size());
    char want[32];
    std::snprintf(want, sizeof want, "[image %d/%d ref=", n, n);
    TEST_ASSERT_TRUE(r.turns[0].text.find(want) != std::string::npos);
    TEST_ASSERT_TRUE(r.turns[0].text.find("They wrote: lunch") != std::string::npos);
  }
  // A 10-photo album plus one more photo: two turns, nothing lost (the 10/11 cap).
  Run r(9);
  uint32_t at = 1000;
  for (int i = 0; i < 11; ++i) r.w.send("77", Attachment::Kind::Photo, "", at += 300, i < 10 ? "G2" : "");
  r.go(at + 60000);
  TEST_ASSERT_EQUAL_UINT(2, r.turns.size());
  TEST_ASSERT_EQUAL_UINT(10, r.turns[0].uids.size());
  TEST_ASSERT_EQUAL_UINT(1, r.turns[1].uids.size());
}

// The 15 Oct sequence: photo 1 with the caption, photos 2 and 3, then "did you see all
// three?" 2 s after photo 3 (inside the quiet window): ONE turn holds the text and all
// three photo segments, the text after photo 3. Sent after the page was dispatched
// (during processing), the text is a later turn: never before photo 3 is rendered.
static void test_fifteen_october_sequence() {
  for (uint32_t textGap : {2000u, 40000u}) {
    Run r(15);
    r.w.send("77", Attachment::Kind::Photo, "All of this for dinner", 1000, "G9");
    r.w.send("77", Attachment::Kind::Photo, "", 1600, "G9");
    Msg& p3 = r.w.send("77", Attachment::Kind::Photo, "", 2300, "G9");
    const int32_t p3uid = p3.uid;
    Msg& q = r.w.send("77", Attachment::Kind::None, "", 2300 + textGap);
    q.text = "I sent 3 images. Did you see all? #" + std::to_string(q.uid) + "#";
    const int32_t quid = q.uid;
    r.go(200000);
    const Turn* withText = nullptr;
    size_t at = 0;
    for (size_t i = 0; i < r.turns.size(); ++i)
      if (std::count(r.turns[i].uids.begin(), r.turns[i].uids.end(), quid)) {
        withText = &r.turns[i];
        at = i;
        break;
      }
    TEST_ASSERT_NOT_NULL(withText);
    bool p3Rendered = false;
    for (size_t i = 0; i <= at; ++i)
      p3Rendered = p3Rendered || std::count(r.turns[i].uids.begin(), r.turns[i].uids.end(), p3uid);
    TEST_ASSERT_TRUE_MESSAGE(p3Rendered, "the text ran before photo 3 was rendered");
    if (textGap == 2000u) {
      TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
      for (const char* k : {"[image 1/3", "[image 2/3", "[image 3/3"})
        TEST_ASSERT_TRUE(withText->text.find(k) != std::string::npos);
      TEST_ASSERT_TRUE(withText->text.find("[image 3/3") < withText->text.find("I sent 3 images"));
    } else {
      TEST_ASSERT_EQUAL_UINT(2, r.turns.size());
    }
  }
}

// The poison guard: a photo whose processing crashes the device every time. After two
// crash resets on that page it is acked unprocessed, the owner is told once, and the
// messages after it run normally.
static void test_poison_page_acked_after_two_crash_resets() {
  Run r(21);
  Msg& bad = r.w.send("77", Attachment::Kind::Photo, "this one crashes", 1000);
  bad.poison = true;
  Msg& later = r.w.send("77", Attachment::Kind::None, "", 60000);
  later.text = "still there? #" + std::to_string(later.uid) + "#";
  const int32_t laterUid = later.uid;
  r.go(120000);
  TEST_ASSERT_EQUAL_INT(2, r.restarts);
  TEST_ASSERT_EQUAL_INT(1, r.poisonNotices);
  // The photo's turn still runs, its label saying it could not be processed; the later
  // message runs too.
  TEST_ASSERT_EQUAL_UINT(2, r.turns.size());
  TEST_ASSERT_TRUE(r.turns[0].text.find("(could not be fetched: skipped") != std::string::npos);
  TEST_ASSERT_TRUE(r.turns[0].text.find("this one crashes") != std::string::npos);
  TEST_ASSERT_EQUAL_INT32(laterUid, r.turns[1].uids[0]);
  TEST_ASSERT_FALSE(r.pending());
}

// The 19 Oct storm shape on this cycle: the same page served again and again (a full
// settings store and a restart after every turn, so the stored offset never moves and
// every boot re-serves it). One paid turn, one owner notice.
static void test_reserved_page_after_restarts_costs_one_turn() {
  Run r(31);
  r.w.storeFull = true;
  Msg& m = r.w.send("77", Attachment::Kind::None, "", 1000);
  m.text = "what's the weather? #" + std::to_string(m.uid) + "#";
  // Restart right after the turn returned, before the next poll could confirm it.
  for (int k = 0; k < 12; ++k) {
    Device d(r.w, r.rng, r.turns, k == 0 ? BootKind::PowerOn : BootKind::Crash);
    d.turnMinMs = d.turnMaxMs = 6000;
    for (int c = 0; c < 6; ++c) d.cycleOnce();
    r.tally(d);
    r.w.confirmed = 0;   // the stale stored offset: Telegram serves the page again
    r.w.pollOpen = false;
  }
  TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
  TEST_ASSERT_EQUAL_INT(1, r.dupNotices);
}

// A fresh device (no stored offset, garbage RTC bytes) and a single photo with no
// group id: one turn, "[image 1/1 ref=...]".
static void test_fresh_device_single_photo() {
  Run r(41);
  std::memset(&r.w.rtc, 0xA5, sizeof r.w.rtc);
  r.w.stored = 0;
  r.w.send("5", Attachment::Kind::Photo, "", 1500);
  r.go(40000, BootKind::Other);
  TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
  TEST_ASSERT_TRUE(r.turns[0].text.rfind("[image 1/1 ref=", 0) == 0);
}

// A stranger's album never reaches a turn, takes no media slot and needs no notice.
static void test_stranger_media_never_runs() {
  Run r(51);
  for (int i = 0; i < 3; ++i) r.w.send("666", Attachment::Kind::Photo, "", 1000 + 300 * i, "GX");
  Msg& t = r.w.send("77", Attachment::Kind::None, "", 2500);
  t.text = "hi #" + std::to_string(t.uid) + "#";
  r.go(40000);
  checkExactlyOnce(r, false);
  TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
}

// The hourly ceiling through the receive cycle: a member chat's flood stops at the
// ceiling minus the owner's reserve, and the owner's own message after it still runs.
static void test_member_flood_leaves_the_owner_replies() {
  Run r(61);
  const int members = 80;
  for (int i = 0; i < members; ++i) {
    Msg& m = r.w.send("300", Attachment::Kind::None, "", 2000 + 30000u * (uint32_t)i);
    m.text = "member #" + std::to_string(m.uid) + "#";
  }
  Msg& own = r.w.send("77", Attachment::Kind::None, "", 2000 + 30000u * members + 5000);
  own.text = "owner #" + std::to_string(own.uid) + "#";
  const int32_t ownUid = own.uid;
  // Fast turns, so the whole flood lands inside one hour.
  for (int lives = 0; lives < 3 && r.pending(); ++lives) {
    Device d(r.w, r.rng, r.turns, BootKind::PowerOn);
    d.turnMinMs = d.turnMaxMs = 1000;
    for (int c = 0; c < 2000 && r.pending(); ++c) d.cycleOnce();
    r.tally(d);
  }
  const int memberTurns = (int)std::count_if(r.turns.begin(), r.turns.end(), [](const Turn& t) { return t.chat == "300"; });
  TEST_ASSERT_EQUAL_INT((int)(nimbus::net::kInboundTurnsPerHour - nimbus::net::kOwnerReserveTurns), memberTurns);
  TEST_ASSERT_TRUE_MESSAGE(std::any_of(r.turns.begin(), r.turns.end(),
                                       [&](const Turn& t) { return !t.uids.empty() && t.uids[0] == ownUid; }),
                           "the owner's message did not run");
  TEST_ASSERT_EQUAL_INT(1, r.ceilingNotices);
}

// The poison guard across a command page's checkpoint: "/status" then a photo whose
// processing crashes the device. The command runs once; the photo's bucket is skipped
// after exactly two crash restarts (whether or not the command's checkpoint landed);
// the text after it runs.
static void test_poison_after_a_command_needs_two_crashes() {
  for (bool full : {false, true}) {
    Run r(71);
    r.w.storeFull = full;
    Msg& cmd = r.w.send("77", Attachment::Kind::None, "", 1000);
    cmd.text = "/status #" + std::to_string(cmd.uid) + "#";
    const int32_t cmdUid = cmd.uid;
    Msg& bad = r.w.send("77", Attachment::Kind::Photo, "", 1100);
    bad.poison = true;
    Msg& after = r.w.send("77", Attachment::Kind::None, "", 1200);
    after.text = "and this #" + std::to_string(after.uid) + "#";
    const int32_t afterUid = after.uid;
    r.go(200000);
    TEST_ASSERT_EQUAL_INT(2, r.restarts);
    TEST_ASSERT_EQUAL_INT(1, r.poisonNotices);
    int cmdRuns = 0, afterRuns = 0;
    const Turn* skipped = nullptr;
    for (const Turn& t : r.turns) {
      cmdRuns += (int)std::count(t.uids.begin(), t.uids.end(), cmdUid);
      afterRuns += (int)std::count(t.uids.begin(), t.uids.end(), afterUid);
      if (t.text.find("(could not be fetched: skipped") != std::string::npos) skipped = &t;
    }
    TEST_ASSERT_EQUAL_INT(1, cmdRuns);
    TEST_ASSERT_EQUAL_INT(1, afterRuns);   // the text with the skipped photo still ran
    TEST_ASSERT_NOT_NULL(skipped);
    TEST_ASSERT_TRUE(skipped->text.find("and this") != std::string::npos);
    TEST_ASSERT_FALSE(r.pending());
  }
}

// Every phase of the poison guard through the receive cycle, two crashes each:
// - photo 2 of an album crashes while it is processed: only that photo is skipped
//   (its label says so), photos 1 and 3 and the text run in the same turn;
// - a turn crashes while it runs: that bucket is acked without its turn, the next
//   message runs;
// - classifying an update crashes: the whole page is acked unprocessed.
// One owner notice each, and nothing is left pending.
static void test_poison_every_phase() {
  {
    Run r(81);
    r.w.send("77", Attachment::Kind::Photo, "lunch", 1000, "GP");
    Msg& p2 = r.w.send("77", Attachment::Kind::Photo, "", 1300, "GP");
    p2.poison = true;
    const int32_t p2uid = p2.uid;
    r.w.send("77", Attachment::Kind::Photo, "", 1600, "GP");
    Msg& t = r.w.send("77", Attachment::Kind::None, "", 2000);
    t.text = "which is healthiest? #" + std::to_string(t.uid) + "#";
    r.go(300000);
    TEST_ASSERT_EQUAL_INT(2, r.restarts);
    TEST_ASSERT_EQUAL_INT(1, r.poisonNotices);
    const Turn& last = r.turns.back();
    TEST_ASSERT_TRUE(last.text.find("[image 1/3 ref=") != std::string::npos);
    TEST_ASSERT_TRUE(last.text.find("[image 2/3] (could not be fetched: skipped #" + std::to_string(p2uid)) !=
                     std::string::npos);
    TEST_ASSERT_TRUE(last.text.find("[image 3/3 ref=") != std::string::npos);
    TEST_ASSERT_TRUE(last.text.find("which is healthiest?") != std::string::npos);
    TEST_ASSERT_FALSE(r.pending());
  }
  {
    Run r(82);
    Msg& bad = r.w.send("77", Attachment::Kind::None, "", 1000);
    bad.text = "this turn crashes #" + std::to_string(bad.uid) + "#";
    bad.turnPoison = true;
    Msg& next = r.w.send("77", Attachment::Kind::None, "", 90000);
    next.text = "next #" + std::to_string(next.uid) + "#";
    r.go(300000);
    TEST_ASSERT_EQUAL_INT(2, r.restarts);
    TEST_ASSERT_EQUAL_INT(1, r.poisonNotices);
    TEST_ASSERT_TRUE(r.turns.back().text.find("next #") != std::string::npos);
    TEST_ASSERT_FALSE(r.pending());
  }
  {
    Run r(83);
    Msg& bad = r.w.send("77", Attachment::Kind::None, "", 1000);
    bad.text = "classify crashes #" + std::to_string(bad.uid) + "#";
    bad.classifyPoison = true;
    Msg& next = r.w.send("77", Attachment::Kind::None, "", 90000);
    next.text = "after #" + std::to_string(next.uid) + "#";
    r.go(300000);
    TEST_ASSERT_EQUAL_INT(2, r.restarts);
    TEST_ASSERT_EQUAL_INT(1, r.poisonNotices);
    TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
    TEST_ASSERT_TRUE(r.turns[0].text.find("after #") != std::string::npos);
    TEST_ASSERT_FALSE(r.pending());
  }
}

// A turn with nothing to answer but failures (a lone photo whose download failed, no
// caption, no text) gets one plain reply, not a paid turn; with a caption, a text or
// another photo that was fetched, the failure is a label in the turn that runs.
static void test_failed_media_alone_gets_a_plain_reply() {
  Run r(91);
  Msg& lone = r.w.send("77", Attachment::Kind::Photo, "", 1000);
  lone.fetchFails = true;
  Msg& capt = r.w.send("77", Attachment::Kind::Photo, "what is it?", 40000);
  capt.fetchFails = true;
  Msg& a1 = r.w.send("77", Attachment::Kind::Photo, "", 80000, "GF");
  a1.fetchFails = true;
  r.w.send("77", Attachment::Kind::Photo, "", 80300, "GF");
  int failedReplies = 0;
  for (int lives = 0; lives < 3 && (r.w.now < 150000 || r.pending()); ++lives) {
    Device d(r.w, r.rng, r.turns, BootKind::PowerOn);
    for (int c = 0; c < 400 && (r.w.now < 150000 || r.pending()); ++c) d.cycleOnce();
    failedReplies += d.failedReplies;
  }
  TEST_ASSERT_EQUAL_INT(1, failedReplies);
  TEST_ASSERT_EQUAL_UINT(2, r.turns.size());   // the captioned photo, the album
  TEST_ASSERT_TRUE(r.turns[0].text.find("(could not be fetched: download failed") != std::string::npos);
  TEST_ASSERT_TRUE(r.turns[0].text.find("what is it?") != std::string::npos);
  TEST_ASSERT_TRUE(r.turns[1].text.find("[image 2/2 ref=") != std::string::npos);
  TEST_ASSERT_FALSE(r.pending());
}

// The injected test page (TGINJECT, the bench's no-phone path) runs the same cycle
// without settle, cursor or poison marks: an 11-photo page runs 10 in one turn and the
// 11th on the next call; the same page injected again and again (the bench storm leg)
// costs one turn and one notice, with a duplicate per repeat.
static void test_injected_pages() {
  Run r(101);
  std::vector<Update> page;
  for (int i = 0; i < 11; ++i) page.push_back(Device::toUpdate(r.w.send("77", Attachment::Kind::Photo, "", 1000, "GI")));
  Device d(r.w, r.rng, r.turns, BootKind::PowerOn);
  TgCycle c;
  const size_t next = d.rx.injected(d, page, 0, c);
  TEST_ASSERT_EQUAL_size_t(10, next);
  TEST_ASSERT_EQUAL_INT(10, c.messages);
  TEST_ASSERT_EQUAL_UINT(1, r.turns.size());
  TgCycle c2;
  TEST_ASSERT_EQUAL_size_t(page.size(), d.rx.injected(d, page, next, c2));
  TEST_ASSERT_EQUAL_UINT(2, r.turns.size());
  TEST_ASSERT_TRUE(r.turns[1].text.find("[image 1/1 ref=") != std::string::npos);
  TEST_ASSERT_EQUAL_INT32(0, d.cursor.offset());   // never moves the cursor
  // The storm leg: one text page injected 50 times, a restart after the 25th.
  Msg& m = r.w.send("77", Attachment::Kind::None, "", 2000);
  m.text = "storm #" + std::to_string(m.uid) + "#";
  const std::vector<Update> storm{Device::toUpdate(m)};
  int dups = 0, notices = 0;
  for (int life = 0; life < 2; ++life) {
    Device dev(r.w, r.rng, r.turns, life ? BootKind::Other : BootKind::PowerOn);
    for (int k = 0; k < 25; ++k) {
      TgCycle cy;
      dev.rx.injected(dev, storm, 0, cy);
      dups += cy.duplicates;
    }
    notices += dev.dupNotices;
  }
  int stormTurns = 0;
  for (const Turn& t : r.turns) stormTurns += t.text.find("storm #") != std::string::npos;
  TEST_ASSERT_EQUAL_INT(1, stormTurns);
  TEST_ASSERT_EQUAL_INT(49, dups);
  TEST_ASSERT_EQUAL_INT(1, notices);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_random_bursts_exactly_once_in_order);
  RUN_TEST(test_random_bursts_with_restarts);
  RUN_TEST(test_album_lands_in_one_turn);
  RUN_TEST(test_fifteen_october_sequence);
  RUN_TEST(test_poison_page_acked_after_two_crash_resets);
  RUN_TEST(test_reserved_page_after_restarts_costs_one_turn);
  RUN_TEST(test_fresh_device_single_photo);
  RUN_TEST(test_stranger_media_never_runs);
  RUN_TEST(test_member_flood_leaves_the_owner_replies);
  RUN_TEST(test_poison_after_a_command_needs_two_crashes);
  RUN_TEST(test_poison_every_phase);
  RUN_TEST(test_failed_media_alone_gets_a_plain_reply);
  RUN_TEST(test_injected_pages);
  return UNITY_END();
}
