#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "nimbus/cloud/loopback_target.h"
#include "nimbus/cloud/relay_codec.h"
#include "nimbus/cloud/relay_slot.h"
#include "nimbus/cloud/relay_ws.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// ---- the res frame a body travels in (emitResFrame) ------------------------------
//
// The slot rule is stated on the body, but the bytes on the wire (and the time a write
// takes) are the frame's: the WebSocket header, the JSON head, then the body in base64.
// Every test below names a BODY and derives its frame the way the device builds it, so
// a rule applied to the wrong unit shows up here. kHead is the head of a dashboard
// poll's answer in the shape emitResFrame serializes (src/net/relay_client.cpp: t, id,
// status, the kept headers, reopened for bodyB64): a cloud request id and the two
// headers /api/state carries.
static const std::string kHead =
    "{\"t\":\"res\",\"id\":\"3f0c2a9e-5b7d-4e21-9a8f-6c1d2e3b4a5f\",\"status\":200,"
    "\"headers\":{\"Content-Type\":\"application/json\",\"Cache-Control\":\"no-store\"}";
static constexpr size_t kMaxBody = kLoopbackMaxRespBody;   // the 383 KiB tunnel cap
// The dashboard's 5 s /api/state polls as measured: 6,384 B and 7,074 B bodies.
static constexpr size_t kPollBodies[] = {6384, 7074};

static size_t resFrameBytes(size_t bodyBytes) {
  static const char kMid[] = ",\"bodyB64\":\"";
  static const char kSuf[] = "\"}";
  const size_t payload =
      kHead.size() + (bodyBytes ? (sizeof(kMid) - 1) + b64EncodedLen(bodyBytes) + (sizeof(kSuf) - 1) : 1);
  const uint8_t mask[4] = {1, 2, 3, 4};
  uint8_t hdr[14];
  return ws::writeFrameHeader(ws::Opcode::Text, payload, mask, hdr) + payload;
}

// ---- a simulated device: one work slot, a turn task, the relay ----------------------
//
// Single-threaded and deterministic on a fake clock. The turn task wants the slot at
// scheduled times and holds it for an exchange; it outranks the relay (tg_poll runs at
// a higher priority on the same core), so whenever the slot is released while a turn
// waits, the turn gets it. The relay drives the real policy templates against it.
struct Sim {
  struct TurnReq { uint32_t wantAt, holdMs; uint32_t gotAt = UINT32_MAX; };
  struct Span { uint32_t a, b; };
  enum Holder { None, Relay, Turn };

  uint32_t now = 0;
  Holder holder = None;
  uint32_t turnEnd = 0;
  std::vector<TurnReq> turns;   // sorted by wantAt
  size_t next = 0;
  std::vector<Span> turnHolds, bulkTx;
  uint32_t bytesPerSec = 200000;   // the relay's link speed
  uint32_t maxRelayHold = 0, holdStart = 0;
  std::vector<uint32_t> pumps;
  int writesWithoutSlot = 0;
  uint32_t lastTx = 0;   // start time of the most recent write (any frame)

  bool turnWaiting() const { return next < turns.size() && turns[next].wantAt <= now; }
  // At `now`: end a finished turn hold, then hand a free slot to a waiting turn.
  void settle() {
    for (;;) {
      if (holder == Turn && turnEnd <= now) holder = None;
      if (holder != None || !turnWaiting()) return;
      TurnReq& t = turns[next++];
      t.gotAt = now;
      holder = Turn;
      turnEnd = now + t.holdMs;
      turnHolds.push_back({now, turnEnd});
    }
  }
  // The next moment the slot can change hands (UINT32_MAX: none pending).
  uint32_t nextEvent() const {
    if (holder == Turn) return turnEnd;
    if (holder == None && next < turns.size() && turns[next].wantAt > now) return turns[next].wantAt;
    return UINT32_MAX;
  }
  void advanceTo(uint32_t t) {
    settle();
    for (uint32_t e = nextEvent(); e != UINT32_MAX && e <= t; e = nextEvent()) {
      now = e;
      settle();
    }
    if (t > now) now = t;
    settle();
  }
  bool grab() {
    settle();
    if (holder != None || turnWaiting()) return false;
    holder = Relay;
    holdStart = now;
    return true;
  }
  // The relay's acquireWork(ms).
  bool take(uint32_t ms) {
    const uint32_t until = now + ms;
    for (;;) {
      if (grab()) return true;
      const uint32_t e = nextEvent();
      if (e == UINT32_MAX || e > until) {
        advanceTo(until);
        return grab();
      }
      advanceTo(e);
    }
  }
  void release() {
    if (holder != Relay) return;
    maxRelayHold = std::max(maxRelayHold, now - holdStart);
    holder = None;
    settle();
  }
  // Time passes while the relay writes; a turn wanting the slot meanwhile waits.
  bool write(size_t n, bool bulk) {
    if (bulk && holder != Relay) ++writesWithoutSlot;
    const uint32_t dur = (uint32_t)((uint64_t)n * 1000 / bytesPerSec) + 1;
    lastTx = now;
    if (bulk) bulkTx.push_back({now, now + dur});
    advanceTo(now + dur);
    return true;
  }
};

struct Outcome {
  bool small = false;
  SlotWait wait = SlotWait::Busy;
  BulkResult bulk = BulkResult::Failed;
  uint32_t answeredAt = 0;   // when the res frame (or the 503) finished
};

// One tunneled request through the real templates: loopback (no slot), then the
// frame for a `bodyBytes` answer. The decision is on the body (as sendFrame makes it);
// a small answer's frame goes straight out, a big one waits (pumping) and is written
// per chunk.
static Outcome serve(Sim& s, uint32_t reqAt, uint32_t loopbackMs, size_t bodyBytes) {
  Outcome o;
  const size_t frameBytes = resFrameBytes(bodyBytes);
  s.advanceTo(reqAt + loopbackMs);   // the slot is never held across the loopback
  if (!frameNeedsSlot(bodyBytes)) {
    o.small = true;
    s.write(frameBytes, false);
    o.answeredAt = s.now;
    return o;
  }
  s.pumps.clear();
  s.pumps.push_back(s.now);
  o.wait = waitForSlot(
      reqAt + kRelayBusyAfterMs, [&] { return s.now; }, [&](uint32_t ms) { return s.take(ms); },
      [&] {
        s.pumps.push_back(s.now);
        return true;
      });
  if (o.wait == SlotWait::Busy) {
    s.write(64, false);   // the small 503 answer, no slot
    o.answeredAt = s.now;
    return o;
  }
  o.bulk = writeBulk(
      frameBytes, reqAt + kRelayResBudgetMs,
      [&](uint32_t deadline) {
        while ((int32_t)(deadline - s.now) > 0)
          if (s.take(std::min<uint32_t>(kRelaySlotTakeMs, deadline - s.now))) return true;
        return false;
      },
      [&] { s.release(); }, [&](size_t, size_t n) { return s.write(n, true); }, [&] {});
  o.answeredAt = s.now;
  return o;
}

static bool overlaps(const Sim::Span& x, const Sim::Span& y) { return x.a < y.b && y.a < x.b; }

// ---- policy ---------------------------------------------------------------------

static void test_policy_which_answers_take_the_slot() {
  // Heartbeat, hello, pong, acks and the dashboard's polls never do, though a poll's
  // frame is over kRelayBulkBytes (8.6 and 9.5 KB): the rule is on the body.
  for (size_t n : {(size_t)0, (size_t)2, (size_t)20, (size_t)180, kPollBodies[0], kPollBodies[1], kRelayBulkBytes})
    TEST_ASSERT_FALSE(frameNeedsSlot(n));
  for (size_t poll : kPollBodies) TEST_ASSERT_TRUE(resFrameBytes(poll) > kRelayBulkBytes);
  for (size_t n : {kRelayBulkBytes + 1, (size_t)65536, kMaxBody}) TEST_ASSERT_TRUE(frameNeedsSlot(n));
  // Counter-test in the dimension the body rule stops checking: the frame. The biggest
  // frame that skips the slot (an 8 KB body) stays under 12 KB, one and a half chunks,
  // and every frame past it takes the slot.
  const size_t unslottedMax = resFrameBytes(kRelayBulkBytes);
  TEST_ASSERT_TRUE(unslottedMax < 12 * 1024);
  for (size_t n = 0; n <= 4 * kRelayBulkBytes; ++n)
    if (resFrameBytes(n) > unslottedMax) TEST_ASSERT_TRUE(frameNeedsSlot(n));
  TEST_ASSERT_TRUE(kRelayBusyAfterMs < kRelayResBudgetMs);
  TEST_ASSERT_TRUE(kRelayResBudgetMs < kRelayCloudTimeoutMs);
  TEST_ASSERT_FALSE(firstDialReady(false, 0));
  TEST_ASSERT_FALSE(firstDialReady(false, kRelayFirstDialWaitMs - 1));
  TEST_ASSERT_TRUE(firstDialReady(false, kRelayFirstDialWaitMs));
  TEST_ASSERT_TRUE(firstDialReady(true, 0));
}

// The pre-frame wait pumps at least every 200 ms and answers busy before 30 s when a
// turn holds the slot for the whole window (a 120 s exchange).
static void test_wait_pumps_every_200ms_and_answers_busy_before_30s() {
  Sim s;
  s.turns.push_back({0, 120000});
  s.advanceTo(1);
  const Outcome o = serve(s, 1000, 300, 375598);   // the config page
  TEST_ASSERT_EQUAL((int)SlotWait::Busy, (int)o.wait);
  TEST_ASSERT_TRUE(o.answeredAt < 1000 + kRelayCloudTimeoutMs);
  TEST_ASSERT_TRUE(o.answeredAt >= 1000 + kRelayBusyAfterMs);
  for (size_t i = 1; i < s.pumps.size(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(s.pumps[i] - s.pumps[i - 1] <= kRelaySlotTakeMs, "a pump gap over 200 ms");
  TEST_ASSERT_TRUE(s.pumps.size() >= kRelayBusyAfterMs / kRelaySlotTakeMs - 3);
  TEST_ASSERT_TRUE(s.bulkTx.empty());   // only the small 503 went out
}

// The dashboard's polls are never delayed, even while a turn holds the slot for a
// minute: every body up to kRelayBulkBytes goes straight out after the loopback,
// whatever base64 makes of its frame. Deciding on the frame stalled every poll over
// about 6 KB for 20 s and then answered it busy, so the page went dead mid-turn.
static void test_dashboard_polls_never_wait_for_the_slot() {
  std::vector<size_t> bodies = {20, kPollBodies[0], kPollBodies[1], kRelayBulkBytes};
  for (size_t n = 0; n <= kRelayBulkBytes; n += 97) bodies.push_back(n);
  for (size_t n : bodies) {
    Sim s;
    s.turns.push_back({0, 60000});
    s.advanceTo(10);
    const Outcome o = serve(s, 5000, 5, n);
    TEST_ASSERT_TRUE(o.small);
    TEST_ASSERT_EQUAL_UINT32(5005, s.lastTx);   // straight out after the loopback
    TEST_ASSERT_TRUE(o.answeredAt < 5005 + kRelaySlotTakeMs);
    TEST_ASSERT_TRUE(s.bulkTx.empty());
  }
}

// Counter-test: the longest single hold of a maximal answer (the 383 KiB cap, a
// 523 KB frame) stays under the turn's 10 s acquire, even on a link far slower than
// the field's (the 375 KB page took 2.7 s uncontended): one chunk at 1 KB/s is 8.2 s.
static void test_maximal_frame_hold_stays_under_the_turn_acquire() {
  TEST_ASSERT_TRUE(resFrameBytes(kMaxBody) > 520000);
  for (uint32_t bps : {1000u, 40000u, 200000u}) {
    Sim s;
    s.bytesPerSec = bps;
    const Outcome o = serve(s, 0, 100, kMaxBody);
    TEST_ASSERT_EQUAL((int)SlotWait::Acquired, (int)o.wait);
    TEST_ASSERT_TRUE(s.maxRelayHold < kTurnSlotAcquireMs);
  }
  TEST_ASSERT_TRUE((uint64_t)kRelayBulkBytes * 1000 / 1000 < kTurnSlotAcquireMs);
}

// Property over random interleavings (fake clock): turns wanting the slot at random
// times for holds of 0-120 s, relay answers of random body sizes. The relay never writes a
// big frame without the slot, its bulk writes never overlap a turn's hold, no turn
// ever waits as long as its 10 s acquire, every busy answer lands before 30 s, and a
// completed page lands inside the 28 s budget (a stall past it is reported, never
// silently over time).
static void test_property_relay_bulk_never_overlaps_a_turn() {
  std::mt19937 rng(20261018);
  int busy = 0, sent = 0, stalled = 0;
  for (int run = 0; run < 300; ++run) {
    Sim s;
    s.bytesPerSec = 20000 + rng() % 400000;
    // Turn exchanges come from one task (tg_poll), so they never overlap each other:
    // each wants the slot some time after the previous one ended.
    uint32_t t = 0;
    const int nTurns = 1 + (int)(rng() % 6);
    for (int i = 0; i < nTurns; ++i) {
      t += rng() % 40000;
      const uint32_t hold = (uint32_t)(rng() % 120001);
      s.turns.push_back({t, hold});
      t += hold + 11000;   // after the previous exchange plus its worst slot wait
    }
    std::vector<Outcome> outs;
    uint32_t reqAt = rng() % 30000;
    for (int i = 0; i < 4; ++i) {
      const size_t body = (rng() % 3 == 0) ? (size_t)(rng() % 8000) : (size_t)(9000 + rng() % (kMaxBody - 9000));
      const uint32_t at = std::max(reqAt, s.now);
      const Outcome o = serve(s, at, rng() % 3000, body);
      if (!o.small) {
        if (o.wait == SlotWait::Busy) {
          ++busy;
          TEST_ASSERT_TRUE(o.answeredAt < at + kRelayCloudTimeoutMs);
        } else if (o.bulk == BulkResult::Sent) {
          ++sent;
          TEST_ASSERT_TRUE(o.answeredAt <= at + kRelayResBudgetMs + 600);   // + one chunk past the check
        } else {
          ++stalled;
        }
      }
      reqAt = s.now + rng() % 20000;
    }
    s.advanceTo(std::max(s.now, s.turns.back().wantAt + s.turns.back().holdMs) + 15000);   // all turns done
    TEST_ASSERT_EQUAL_MESSAGE(0, s.writesWithoutSlot, "a big frame went out without the slot");
    for (const Sim::Span& tx : s.bulkTx)
      for (const Sim::Span& h : s.turnHolds)
        TEST_ASSERT_FALSE_MESSAGE(overlaps(tx, h), "relay bulk TX overlapped a turn exchange");
    for (const Sim::TurnReq& tr : s.turns) {
      TEST_ASSERT_TRUE(tr.gotAt != UINT32_MAX);
      TEST_ASSERT_TRUE_MESSAGE(tr.gotAt - tr.wantAt < kTurnSlotAcquireMs, "a turn waited 10 s for the slot");
    }
  }
  TEST_ASSERT_TRUE(busy > 0);    // the property exercised every branch
  TEST_ASSERT_TRUE(sent > 0);
  (void)stalled;
}

// Overlap check proper: the turn holds and the relay's big-frame writes, as spans.
static void test_property_spans_are_disjoint() {
  std::mt19937 rng(7);
  for (int run = 0; run < 200; ++run) {
    Sim s;
    s.bytesPerSec = 30000 + rng() % 300000;
    s.turns.push_back({rng() % 5000, rng() % 30000});
    s.turns.push_back({10000 + rng() % 20000, rng() % 60000});
    serve(s, rng() % 8000, 50, 150000 + rng() % 225000);
    s.advanceTo(s.now + 100000);
    for (const Sim::Span& tx : s.bulkTx)
      for (const Sim::Span& h : s.turnHolds)
        TEST_ASSERT_FALSE_MESSAGE(overlaps(tx, h), "relay bulk TX overlapped a turn exchange");
  }
}

// writeBulk returns the slot on every path: a write failure, a stall, an empty frame.
static void test_bulk_always_returns_the_slot() {
  int held = 1, releases = 0, chunks = 0;
  auto release = [&] { --held; ++releases; };
  auto reacquire = [&](uint32_t) { ++held; return true; };
  BulkResult r = writeBulk(
      3 * kRelayBulkBytes + 5, 0, reacquire, release,
      [&](size_t, size_t) { return ++chunks < 2; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Failed, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  held = 1;
  r = writeBulk(3 * kRelayBulkBytes, 0, [&](uint32_t) { return false; }, release,
                [&](size_t, size_t) { return true; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Stalled, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  held = 1;
  r = writeBulk(0, 0, reacquire, release, [&](size_t, size_t) { return true; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Sent, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  // A whole frame: one release per chunk, never a write past the length.
  held = 1;
  size_t last = 0, total = 0;
  r = writeBulk(5 * kRelayBulkBytes + 1, 0, reacquire, release,
                [&](size_t off, size_t n) { TEST_ASSERT_EQUAL(total, off); last = n; total += n; return true; },
                [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Sent, (int)r);
  TEST_ASSERT_EQUAL((size_t)(5 * kRelayBulkBytes + 1), total);
  TEST_ASSERT_EQUAL(1, (int)last);
  TEST_ASSERT_EQUAL(0, held);
}

// ---- the device side: sendFrame decides on the body ------------------------------
//
// Static check over src/net/relay_client.cpp (comments stripped): emitResFrame hands
// sendFrame the body length, and sendFrame's one slot decision uses it. The templates
// above cannot see the call site; this fails on the tree that passed the frame length.
static std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string all;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) all.append(buf, n);
  std::fclose(f);
  return all;
}
static std::string stripLineComments(const std::string& src) {
  std::string out;
  for (size_t a = 0; a < src.size();) {
    size_t e = src.find('\n', a);
    if (e == std::string::npos) e = src.size();
    std::string line = src.substr(a, e - a);
    const size_t c = line.find("//");
    if (c != std::string::npos) line.erase(c);
    out += line + "\n";
    a = e + 1;
  }
  return out;
}
static int countOf(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

static void test_send_frame_decides_on_the_body() {
  const std::string src = stripLineComments(readFile("src/net/relay_client.cpp"));
  const size_t at = src.find("\nBigFrame sendFrame(const uint8_t* frame, size_t len, size_t bodyLen,");
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, "sendFrame moved: update this guard");
  const size_t end = src.find("\n}\n", at);
  TEST_ASSERT_TRUE(end != std::string::npos);
  const std::string fn = src.substr(at, end - at);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOf(fn, "frameNeedsSlot("), "sendFrame makes one slot decision");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOf(fn, "frameNeedsSlot(bodyLen)"),
                                "sendFrame must decide the slot on the body length, never the frame's");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOf(src, "sendFrame(frame, off, obodyLen, reqAtMs)"),
                                "emitResFrame must hand sendFrame the body length");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOf(src, "frameNeedsSlot("), "a second slot decision in the relay");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_policy_which_answers_take_the_slot);
  RUN_TEST(test_wait_pumps_every_200ms_and_answers_busy_before_30s);
  RUN_TEST(test_dashboard_polls_never_wait_for_the_slot);
  RUN_TEST(test_maximal_frame_hold_stays_under_the_turn_acquire);
  RUN_TEST(test_property_relay_bulk_never_overlaps_a_turn);
  RUN_TEST(test_property_spans_are_disjoint);
  RUN_TEST(test_bulk_always_returns_the_slot);
  RUN_TEST(test_send_frame_decides_on_the_body);
  return UNITY_END();
}
