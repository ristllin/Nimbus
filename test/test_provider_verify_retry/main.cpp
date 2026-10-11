#include <unity.h>

#include <cstdint>

// The pure policy behind CUM-447's low-memory verify deferral: the reason token +
// number decision, and the bounded self-retry backoff. Host-testable because the
// header carries no Arduino/hardware dependency (the device seam in
// provider_verify.cpp owns the per-provider clocks and the hardware calls).
#include "../../src/agent/provider_verify_retry.h"
// CUM-537 gate: the workspace probe's queue and its yield-to-a-turn fetch, driven
// through the same fake-exec seam the device wires to WiFiClientSecure.
#include "../../src/agent/workspace_probe.h"

#include <string>
#include <vector>

using agent::provider_verify::DeferralRetry;
using agent::provider_verify::deferReason;
using agent::provider_verify::WorkspaceProbeQueue;
using agent::provider_verify::ProbeExec;
using agent::provider_verify::FetchResult;
using agent::provider_verify::SyncOutcome;

void setUp() {}
void tearDown() {}

static const uint32_t kFloor = 8000;   // mirrors VERIFY_MIN_MAX8

// ---- the status token: below the gate -> "low-memory", the number rides along ----
// The class, not one instance: EVERY block strictly under the floor defers with the
// low-memory token; everything at/above it does not. A new gate value with no matching
// guard would make this fail.
static void test_defer_reason_is_the_class_not_a_point() {
  for (uint32_t max8 = 0; max8 < kFloor; max8 += 500) {
    TEST_ASSERT_EQUAL_STRING("low-memory", deferReason(max8, kFloor));
  }
  // The exact boundary and above are sufficient: no deferral, empty token.
  TEST_ASSERT_EQUAL_STRING("", deferReason(kFloor, kFloor));
  for (uint32_t max8 = kFloor; max8 < kFloor + 40000; max8 += 1000) {
    TEST_ASSERT_EQUAL_STRING("", deferReason(max8, kFloor));
  }
}

// ---- the backoff grows 2, 4, 8, 16 min then holds at the 16 min cap --------------
static void test_backoff_sequence_is_bounded_and_capped() {
  DeferralRetry r;
  const uint32_t kFirst = DeferralRetry::kFirstDelayMs;   // 2 min
  const uint32_t kMax   = DeferralRetry::kMaxDelayMs;     // 16 min
  TEST_ASSERT_EQUAL_UINT32(120000u, kFirst);
  TEST_ASSERT_EQUAL_UINT32(960000u, kMax);

  uint32_t now = 1000;
  r.onDeferred(now);
  TEST_ASSERT_EQUAL_UINT32(kFirst, r.delayMs);
  TEST_ASSERT_EQUAL_UINT32(now + kFirst, r.nextAtMs);

  uint32_t prev = r.delayMs;
  // Ten more consecutive deferrals: each step at most doubles, never shrinks, never
  // passes the cap, and once at the cap it stays there.
  for (int i = 0; i < 10; ++i) {
    now += r.delayMs;
    r.onDeferred(now);
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs >= prev, "backoff must never shrink");
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs <= 2u * prev, "backoff at most doubles");
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs <= kMax, "backoff never exceeds the cap");
    prev = r.delayMs;
  }
  TEST_ASSERT_EQUAL_UINT32(kMax, r.delayMs);   // saturated at the cap
}

// A fresh arm (after a clear) restarts at the first delay, not wherever it saturated.
static void test_clear_resets_the_streak() {
  DeferralRetry r;
  uint32_t now = 5000;
  for (int i = 0; i < 6; ++i) { r.onDeferred(now); now += r.delayMs; }
  TEST_ASSERT_EQUAL_UINT32(DeferralRetry::kMaxDelayMs, r.delayMs);
  r.clear();
  TEST_ASSERT_FALSE(r.armed);
  TEST_ASSERT_EQUAL_UINT32(0u, r.delayMs);
  r.onDeferred(now);
  TEST_ASSERT_EQUAL_UINT32(DeferralRetry::kFirstDelayMs, r.delayMs);   // back to 2 min
}

// ---- due(): fires only when armed, past the backoff, and the device is free ------
static void test_due_respects_backoff_and_gates() {
  DeferralRetry r;
  const uint32_t t0 = 100000;
  // Not armed -> never due.
  TEST_ASSERT_FALSE(r.due(t0, /*inTurn=*/false, /*online=*/true, /*busy=*/false));

  r.onDeferred(t0);
  const uint32_t fire = r.nextAtMs;
  // Before the backoff elapses: not due even when the device is idle and online.
  TEST_ASSERT_FALSE(r.due(fire - 1, false, true, false));
  // At/after the backoff, device free: due.
  TEST_ASSERT_TRUE(r.due(fire, false, true, false));
  TEST_ASSERT_TRUE(r.due(fire + 60000, false, true, false));
}

// The three gates that must hold the retry back even when the backoff is due.
static void test_due_is_skipped_during_a_turn_offline_or_busy() {
  DeferralRetry r;
  const uint32_t t0 = 200000;
  r.onDeferred(t0);
  const uint32_t fire = r.nextAtMs;
  TEST_ASSERT_TRUE(r.due(fire, false, true, false));         // baseline: would fire
  TEST_ASSERT_FALSE(r.due(fire, /*inTurn=*/true,  true,  false));  // a turn holds the TLS slot
  TEST_ASSERT_FALSE(r.due(fire, false, /*online=*/false, false));  // no network
  TEST_ASSERT_FALSE(r.due(fire, false, true, /*busy=*/true));      // a verify already queued
}

// ---- stuckFor(): the Memory health line only after the window ---------------------
static void test_stuck_window_tracks_the_first_defer_not_the_last() {
  DeferralRetry r;
  const uint32_t t0 = 300000;
  const uint32_t win = 1200000;   // 20 min, mirrors kStuckWindowMs
  r.onDeferred(t0);
  TEST_ASSERT_FALSE(r.stuckFor(t0, win));
  TEST_ASSERT_FALSE(r.stuckFor(t0 + win - 1, win));
  // Later deferrals in the same streak must NOT reset the "since first" clock.
  r.onDeferred(t0 + 130000);
  r.onDeferred(t0 + 400000);
  TEST_ASSERT_TRUE(r.stuckFor(t0 + win, win));
  // Once cleared, it is no longer stuck.
  r.clear();
  TEST_ASSERT_FALSE(r.stuckFor(t0 + win + 1000000, win));
}

// ---- CUM-537 gate: the probe queue never loses a request ---------------------------
// (a) The pump used to clear the queued flag AFTER creating the worker; a worker that
// failed fast (low memory) and re-armed in between had its retry wiped. With the
// generation queue the pump writes nothing after the start: the worker's re-arm
// stands, two minutes out, with one try spent.
static void test_probe_rearm_before_the_starter_returns_is_kept() {
  WorkspaceProbeQueue q;
  q.request(1000, 0);
  TEST_ASSERT_TRUE(q.due(1000));
  const uint32_t gen = q.generation();   // the pump starts the worker...
  // ...which fails fast on its own task before the pump's next statement.
  TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Retry, (int)q.rearm(gen, 1001, true));
  TEST_ASSERT_TRUE(q.queued());
  TEST_ASSERT_FALSE(q.due(1002));
  TEST_ASSERT_EQUAL_UINT32(1001 + WorkspaceProbeQueue::kRetryMs, q.nextAt());
  TEST_ASSERT_EQUAL_UINT8(1, q.tries());
}

// (b) A save made while a fetch runs (the listing predates it) stays queued after
// that fetch lands; the fetch settles only the generation it read.
static void test_probe_save_during_a_fetch_stays_queued() {
  WorkspaceProbeQueue q;
  q.request(0, 0);
  const uint32_t gen = q.generation();   // the fetch begins
  q.request(5000, 0);                    // the owner saves a connector meanwhile
  q.served(gen);                         // the earlier fetch lands
  TEST_ASSERT_TRUE(q.queued());
  TEST_ASSERT_TRUE(q.due(5000));
  q.served(q.generation());              // the next fetch answers the save
  TEST_ASSERT_FALSE(q.queued());
}

// (c) The last try's drop never drops a save made after that try began, and a
// newer save keeps its own fresh clock instead of inheriting the retry spacing.
static void test_probe_drop_never_drops_a_newer_save() {
  WorkspaceProbeQueue q;
  q.request(0, 0);
  uint32_t gen = q.generation();
  for (int i = 0; i < WorkspaceProbeQueue::kMaxTries - 1; i++)
    TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Retry, (int)q.rearm(gen, 10, true));
  const uint32_t lastTry = q.generation();
  q.request(20, 0);   // a save lands while the last try runs
  TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Newer, (int)q.rearm(lastTry, 30, true));
  TEST_ASSERT_TRUE(q.queued());
  TEST_ASSERT_TRUE(q.due(30));            // the save's clock, not +2 min
  TEST_ASSERT_EQUAL_UINT8(0, q.tries());
  // Exhausting the save's own tries drops it, and only then.
  gen = q.generation();
  for (int i = 0; i < WorkspaceProbeQueue::kMaxTries - 1; i++) q.rearm(gen, 40, true);
  TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Dropped, (int)q.rearm(gen, 50, true));
  TEST_ASSERT_FALSE(q.queued());
  // A running turn defers without spending a try, however often it happens.
  q.request(60, 0);
  gen = q.generation();
  for (int i = 0; i < 10; i++)
    TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Retry, (int)q.rearm(gen, 70, false));
  TEST_ASSERT_EQUAL_UINT8(0, q.tries());
  TEST_ASSERT_TRUE(q.queued());
}

// ---- CUM-537 gate: the probe gives the work slot back to a turn ---------------------
// The probe used to check for a turn only before taking the single TLS work slot,
// then hold it through the whole ~300 KB body; a turn that started meanwhile failed
// with "tls work-slot busy" and failed over to another provider. Now a turn seen
// after the connect or before any read aborts the fetch, the slot is released, the
// attempt reads "a turn is running" and re-arms two minutes out without a try spent.
struct FakeExec {
  int turnAtRead = -1;           // the read call index from which a turn is in flight
  bool turnBeforeConnect = false;
  bool turnAfterConnect = false;
  int reads = 0, closes = 0, acquires = 0, releases = 0;
  bool connected = false;
  std::vector<std::string> chunks;   // what successive reads return; then closed
  uint32_t ms = 0;
  ProbeExec exec() {
    ProbeExec io;
    io.turnInFlight = [this] {
      if (turnBeforeConnect) return true;
      if (connected && turnAfterConnect) return true;
      return turnAtRead >= 0 && reads >= turnAtRead;
    };
    io.lowMemory = [] { return false; };
    io.acquireSlot = [this] { acquires++; return true; };
    io.releaseSlot = [this] { releases++; };
    io.connect = [this] { connected = true; return true; };
    io.read = [this](char* dst, size_t want) -> int {
      const int i = reads++;
      if (i >= (int)chunks.size()) return -1;
      const std::string& c = chunks[(size_t)i];
      const size_t n = c.size() < want ? c.size() : want;
      memcpy(dst, c.data(), n);
      return (int)n;
    };
    io.close = [this] { closes++; connected = false; };
    io.nowMs = [this] { return ms; };
    io.idle = [this] { ms += 2; };
    return io;
  }
};

static void test_probe_fetch_yields_to_a_turn_mid_body() {
  FakeExec f;
  f.chunks = {"HTTP/1.0 200 OK\r\n\r\n{\"items\":[", "{\"name\":\"gcal\"}", "]}"};
  f.turnAtRead = 1;   // the owner's message arrives after the first chunk
  ProbeExec io = f.exec();
  char buf[256];
  size_t len = 0;
  TEST_ASSERT_EQUAL((int)FetchResult::TurnStarted, (int)fetchBody(io, buf, sizeof buf, 25000, true, len));
  TEST_ASSERT_EQUAL(1, f.reads);    // stopped before the second read
  TEST_ASSERT_EQUAL(1, f.closes);
  // The whole attempt: the slot is released once, and the reason defers without a try.
  FakeExec g;
  g.chunks = f.chunks;
  g.turnAtRead = 1;
  ProbeExec gio = g.exec();
  const char* why = attemptProbe(gio, [&] {
    char b[256];
    size_t n = 0;
    return fetchBody(gio, b, sizeof b, 25000, true, n) == FetchResult::TurnStarted
               ? SyncOutcome::TurnStarted : SyncOutcome::Landed;
  });
  TEST_ASSERT_EQUAL_STRING("a turn is running", why);
  TEST_ASSERT_FALSE(agent::provider_verify::countsAsTry(why));
  TEST_ASSERT_EQUAL(1, g.acquires);
  TEST_ASSERT_EQUAL(1, g.releases);
  WorkspaceProbeQueue q;
  q.request(0, 0);
  TEST_ASSERT_EQUAL((int)WorkspaceProbeQueue::Rearm::Retry,
                    (int)q.rearm(q.generation(), 100, agent::provider_verify::countsAsTry(why)));
  TEST_ASSERT_EQUAL_UINT32(100 + WorkspaceProbeQueue::kRetryMs, q.nextAt());
  TEST_ASSERT_EQUAL_UINT8(0, q.tries());
}

static void test_probe_fetch_checks_right_after_the_connect() {
  FakeExec f;
  f.chunks = {"HTTP/1.0 200 OK\r\n\r\n{}"};
  f.turnAfterConnect = true;
  ProbeExec io = f.exec();
  char buf[64];
  size_t len = 0;
  TEST_ASSERT_EQUAL((int)FetchResult::TurnStarted, (int)fetchBody(io, buf, sizeof buf, 25000, true, len));
  TEST_ASSERT_EQUAL(0, f.reads);    // not one body byte read
  TEST_ASSERT_EQUAL(1, f.closes);
  // A turn already running never takes the slot at all.
  FakeExec b;
  b.turnBeforeConnect = true;
  ProbeExec bio = b.exec();
  bool synced = false;
  TEST_ASSERT_EQUAL_STRING("a turn is running",
                           attemptProbe(bio, [&] { synced = true; return SyncOutcome::Landed; }));
  TEST_ASSERT_FALSE(synced);
  TEST_ASSERT_EQUAL(0, b.acquires);
}

// The owner-initiated verify (yieldToTurn off) and an undisturbed probe read the
// whole body; the byte cap and the deadline still bound it.
static void test_probe_fetch_reads_the_body_and_stays_bounded() {
  FakeExec f;
  f.chunks = {"HTTP/1.0 200 OK\r\n\r\n", "{\"items\":[]}"};
  f.turnAtRead = 0;   // a turn runs, but this is the manual verify: no yield
  ProbeExec io = f.exec();
  char buf[64];
  size_t len = 0;
  TEST_ASSERT_EQUAL((int)FetchResult::Ok, (int)fetchBody(io, buf, sizeof buf, 25000, false, len));
  TEST_ASSERT_EQUAL_STRING("HTTP/1.0 200 OK\r\n\r\n{\"items\":[]}", buf);
  TEST_ASSERT_EQUAL(1, f.closes);
  FakeExec big;
  big.chunks = {std::string(100, 'x')};
  ProbeExec bio = big.exec();
  char small[16];
  TEST_ASSERT_EQUAL((int)FetchResult::Truncated, (int)fetchBody(bio, small, sizeof small, 25000, true, len));
  TEST_ASSERT_EQUAL(15, (int)len);
  TEST_ASSERT_EQUAL(0, (int)small[15]);
  FakeExec slow;   // the peer sends nothing: the deadline ends it
  slow.chunks = {};
  ProbeExec sio = slow.exec();
  sio.read = [](char*, size_t) { return 0; };
  TEST_ASSERT_EQUAL((int)FetchResult::Ok, (int)fetchBody(sio, buf, sizeof buf, 100, true, len));
  TEST_ASSERT_TRUE(slow.ms >= 100);
  TEST_ASSERT_EQUAL(0, (int)len);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defer_reason_is_the_class_not_a_point);
  RUN_TEST(test_backoff_sequence_is_bounded_and_capped);
  RUN_TEST(test_clear_resets_the_streak);
  RUN_TEST(test_due_respects_backoff_and_gates);
  RUN_TEST(test_due_is_skipped_during_a_turn_offline_or_busy);
  RUN_TEST(test_stuck_window_tracks_the_first_defer_not_the_last);
  RUN_TEST(test_probe_rearm_before_the_starter_returns_is_kept);
  RUN_TEST(test_probe_save_during_a_fetch_stays_queued);
  RUN_TEST(test_probe_drop_never_drops_a_newer_save);
  RUN_TEST(test_probe_fetch_yields_to_a_turn_mid_body);
  RUN_TEST(test_probe_fetch_checks_right_after_the_connect);
  RUN_TEST(test_probe_fetch_reads_the_body_and_stays_bounded);
  return UNITY_END();
}
