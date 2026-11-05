#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/net/tg_receive.h"

// The mid-turn queue peek (lane PROG). The device's turnAckQueued peeks getUpdates at
// the drain cursor's offset while a turn runs, to acknowledge at once a message that
// arrived mid-turn. The drain cursor still points at the page being drained while its
// turns run (it advances only after the last one returns), so the peek RE-SERVES that
// page: without a filter the device acknowledged the very message the running turn was
// answering - "Got it, queued" about the message being replied to right now. These
// tests drive the real receive cycle (the same code src/agent/telegram.cpp runs) with
// a fake Telegram and peek inside Io::runTurn, the exact moment the device peeks:
//   - the peek at the cursor's offset re-serves the running turn's own page (the
//     mechanism of the bug, pinned so the filter can never be dropped as unneeded);
//   - net::peekQueuedToAck never names an update of the page being drained (the
//     turn's own message, a sibling of its page, a later bucket) and always names a
//     message that genuinely arrived mid-turn;
//   - the high-water dedupes across peeks (one ack, not one per boundary);
//   - the ack consumes nothing: the post-turn poll still runs the message's turn;
//   - the device seam: turnAckQueued routes through the filter (src grep, the
//     test_tg_hooks style).

using nimbus::core::TgOffsetCursor;
using nimbus::net::BootKind;
using nimbus::net::TgCycle;
using nimbus::net::TgFetch;
using nimbus::net::TgGuard;
using nimbus::net::TgGuardStore;
using nimbus::net::TgNotice;
using nimbus::net::TgReceiver;
using nimbus::orch::ClassifiedUpdate;
using nimbus::orch::MessageBatcher;
using nimbus::tg::Update;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kBuild = 0x1234ABCDu;
static constexpr int64_t kEpoch0 = 1792400000;   // the device clock at t = 0 (set)

// The fake Telegram: every update ever sent, ascending id. A getUpdates at an offset
// serves what it still holds from that offset (nothing below the confirmed point) and
// confirms everything below it - the real server's contract, which is what makes the
// unconfirmed page come back at the un-advanced offset.
struct Server {
  struct Msg {
    int32_t uid;
    std::string chat;
    std::string text;
    int64_t date;
    uint32_t arriveMs;
  };
  uint32_t now = 100000;
  std::vector<Msg> all;
  int nextUid = 5001;
  int32_t confirmed = 0;

  int64_t epoch() const { return kEpoch0 + now / 1000; }

  Msg& send(const std::string& chat, const std::string& text) {
    Msg m;
    m.uid = nextUid++;
    m.chat = chat;
    m.text = text;
    // The date predates the device clock by half a minute: the page is a backlog the
    // sender finished long ago, so the settle rule dispatches it at first sight.
    m.date = kEpoch0 - 30;
    m.arriveMs = now;
    all.push_back(m);
    return all.back();
  }

  void fetch(int32_t offset, std::vector<Update>& out) {
    confirmed = std::max(confirmed, offset);   // getUpdates confirms below its offset
    for (const Msg& m : all)
      if (m.uid >= offset && m.arriveMs <= now && out.size() < 10) {
        Update u;
        u.updateId = m.uid;
        u.chatId = m.chat;
        u.from = "u" + m.chat;
        u.messageId = m.uid * 3;
        u.date = m.date;
        u.text = m.text;
        out.push_back(u);
      }
  }
};

// The device: the RAM objects the poll task owns, and the Io the receiver calls. The
// turn body replays the device's turnAckQueued at a round boundary: peek at the
// cursor's offset with timeout 0, the offset NOT advanced, then the filter, then the
// ack. `injects` lands a message at the start of a turn: the moment a message can
// arrive while the turn runs, before the boundary's peek.
struct Device {
  Server& srv;
  TgOffsetCursor cursor;
  MessageBatcher batch;
  TgGuardStore rtc;
  TgGuard guard;
  TgReceiver rx;
  int32_t ackHighWater = 0;   // the device's s_peekAckedId
  std::vector<int32_t> acked;   // every "Got it, queued" the device would send
  std::vector<int32_t> peeked;  // every id the peeks saw (the raw re-serve)
  int turns = 0;
  std::vector<std::string> turnChats;
  std::vector<std::pair<int, std::pair<std::string, std::string>>> injects;

  explicit Device(Server& s) : srv(s), guard(rtc), rx(cursor, batch, guard) {
    guard.boot(BootKind::PowerOn, kBuild);
    cursor.seed(0);
  }

  // ---- Io ----
  TgFetch fetch(int32_t offset, int, std::vector<Update>& out) {
    TgFetch f;
    srv.fetch(offset, out);
    f.rp.count = (int)out.size();
    f.rp.limit = 10;
    return f;
  }
  uint32_t nowMs() const { return srv.now; }
  int64_t nowEpoch() const { return srv.epoch(); }
  uint32_t bestEpoch() const { return (uint32_t)srv.epoch(); }
  bool allowed(const std::string& chat) const { return chat != "666"; }
  bool isOwnerChat(const std::string&) const { return true; }
  ClassifiedUpdate classify(const Update& u) {
    ClassifiedUpdate cu;
    cu.updateId = u.updateId;
    cu.action = ClassifiedUpdate::Action::AckOnly;
    if (!allowed(u.chatId)) return cu;
    cu.chatId = u.chatId;
    cu.from = u.from;
    cu.text = u.text;
    cu.action = ClassifiedUpdate::Action::BatchText;
    return cu;
  }
  bool processMedia(size_t, size_t, size_t, int, std::string&) { return false; }
  void skipMedia(size_t, size_t, size_t, int, std::string&) {}
  bool processVoice(int, std::string&) { return false; }
  void mediaFailed(const std::string&, size_t) {}
  void runTurn(const std::string&, const std::string& chat, const nimbus::orch::PsString&, bool) {
    for (const auto& inj : injects)
      if (inj.first == turns) srv.send(inj.second.first, inj.second.second);
    ++turns;
    turnChats.push_back(chat);
    // The device's turnAckQueued (rate limit aside): the peek at the cursor's
    // offset, timeout 0, the offset NOT advanced.
    std::vector<Update> page;
    srv.fetch(cursor.offset(), page);
    for (const Update& u : page) peeked.push_back(u.updateId);
    const auto toAck = nimbus::net::peekQueuedToAck(
        page, ackHighWater, [&](int32_t id) { return rx.updateOf(id) != nullptr; });
    for (const Update* u : toAck) {
      ackHighWater = u->updateId;
      if (u->edited || u->chatId.empty() || !allowed(u->chatId)) continue;
      acked.push_back(u->updateId);
    }
  }
  void notice(const TgNotice&) {}
  void typing(const std::string&) {}
  void checkpoint() {}
  void endPage() {}

  void runUntilIdle(int maxCycles = 20) {
    for (int i = 0; i < maxCycles; ++i) {
      const TgCycle c = rx.cycle(*this, 0);
      if (c.kind == TgCycle::Kind::Idle) return;
    }
    TEST_FAIL_MESSAGE("the world never went idle");
  }
};

static bool has(const std::vector<int32_t>& v, int32_t id) {
  for (int32_t x : v)
    if (x == id) return true;
  return false;
}

// The owner's 4 Nov case, one message at a time: the peek re-serves the running
// turn's own message, and the filter keeps the ack off it - only the message that
// arrived mid-turn is acknowledged, and the ack consumes nothing (its own turn
// still runs on the post-turn poll).
static void test_own_message_never_acked_mid_turn() {
  Server srv;
  Device d(srv);
  const int32_t own = srv.send("100", "what changed in the update?").uid;
  d.injects.push_back({0, {"100", "and the skyr figures?"}});
  d.runUntilIdle();
  const int32_t mid = srv.all[1].uid;
  // The mechanism: the peek at the un-advanced offset saw the page being drained,
  // the turn's own message included.
  TEST_ASSERT_TRUE_MESSAGE(has(d.peeked, own), "the peek did not re-serve the drained page");
  // The fix: the ack names only the mid-turn message, never the turn's own input.
  TEST_ASSERT_EQUAL_INT(1, (int)d.acked.size());
  TEST_ASSERT_TRUE(has(d.acked, mid));
  TEST_ASSERT_FALSE(has(d.acked, own));
  // The ack consumed nothing: the mid-turn message still got its own turn.
  TEST_ASSERT_EQUAL_INT(2, d.turns);
  TEST_ASSERT_EQUAL_STRING("100", d.turnChats[1].c_str());
}

// A page with more of its own input (a sibling the batcher may fold into the same
// turn or run as the next bucket): none of the page is ever "queued", only what
// arrived after it was fetched.
static void test_no_update_of_the_drained_page_is_acked() {
  Server srv;
  Device d(srv);
  const int32_t a = srv.send("100", "first").uid;
  const int32_t c = srv.send("200", "second").uid;
  d.injects.push_back({0, {"100", "arrived while it runs"}});
  d.runUntilIdle();
  const int32_t mid = srv.all[2].uid;
  TEST_ASSERT_TRUE(has(d.peeked, a));
  TEST_ASSERT_TRUE(has(d.peeked, c));
  TEST_ASSERT_EQUAL_INT(1, (int)d.acked.size());
  TEST_ASSERT_TRUE(has(d.acked, mid));
  TEST_ASSERT_FALSE(has(d.acked, a));
  TEST_ASSERT_FALSE(has(d.acked, c));
  // The high-water dedupes across the page's turns: the mid-turn message is
  // acknowledged once, not once per turn that peeks.
  TEST_ASSERT_EQUAL_INT(1, (int)d.acked.size());
  // Both page messages and the mid-turn one all ran their turn.
  TEST_ASSERT_EQUAL_INT(3, d.turns);
}

// The filter's contract on its own terms: the high-water drops earlier acks, page
// membership drops the drained page, everything past it passes.
static void test_filter_drops_high_water_and_page_keeps_the_rest() {
  const std::vector<Update> peeked = [] {
    std::vector<Update> v(4);
    v[0].updateId = 10;
    v[1].updateId = 20;
    v[2].updateId = 30;
    v[3].updateId = 40;
    return v;
  }();
  const auto inPage = [](int32_t id) { return id == 20; };   // 20: the drained page
  const auto out = nimbus::net::peekQueuedToAck(peeked, 10, inPage);
  TEST_ASSERT_EQUAL_INT(2, (int)out.size());
  TEST_ASSERT_EQUAL_INT(30, out[0]->updateId);
  TEST_ASSERT_EQUAL_INT(40, out[1]->updateId);
  // Nothing left once the whole peek is page or high-water.
  const auto none = nimbus::net::peekQueuedToAck(peeked, 40, inPage);
  TEST_ASSERT_EQUAL_INT(0, (int)none.size());
}

// ---- the device seam (the test_tg_hooks style) ------------------------------------

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

static size_t copyLiteral(const std::string& s, size_t i, std::string& out) {
  const char q = s[i];
  out += q;
  for (++i; i < s.size(); ++i) {
    out += s[i];
    if (s[i] == '\\' && i + 1 < s.size()) {
      out += s[++i];
      continue;
    }
    if (s[i] == q) break;
  }
  return i;
}

static std::string stripComments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    const char n = i + 1 < s.size() ? s[i + 1] : 0;
    if (c == '"' || c == '\'') {
      i = copyLiteral(s, i, out);
    } else if (c == '/' && n == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      out += '\n';
    } else if (c == '/' && n == '*') {
      i = s.find("*/", i + 2);
      if (i == std::string::npos) break;
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

// The device's turnAckQueued must route through the portable filter, with page
// membership from the receiver (pageUpdate), never a raw loop over the peek.
static void test_device_peek_routes_through_the_filter() {
  const std::string tg = stripComments(readFile("src/agent/telegram.cpp"));
  const size_t fn = tg.find("int turnAckQueued()");
  TEST_ASSERT_TRUE_MESSAGE(fn != std::string::npos, "turnAckQueued not found");
  const size_t end = tg.find("\n}\n", fn);
  TEST_ASSERT_TRUE_MESSAGE(end != std::string::npos, "turnAckQueued body not bounded");
  const std::string body = tg.substr(fn, end - fn);
  TEST_ASSERT_TRUE_MESSAGE(body.find("nimbus::net::peekQueuedToAck(") != std::string::npos,
                           "the peek does not route through the host-tested filter");
  TEST_ASSERT_TRUE_MESSAGE(body.find("pageUpdate(") != std::string::npos,
                           "page membership does not come from the receiver's drained page");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_own_message_never_acked_mid_turn);
  RUN_TEST(test_no_update_of_the_drained_page_is_acked);
  RUN_TEST(test_filter_drops_high_water_and_page_keeps_the_rest);
  RUN_TEST(test_device_peek_routes_through_the_filter);
  return UNITY_END();
}
