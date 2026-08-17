#include <unity.h>

#include <initializer_list>

#include "nimbus/notifier_map.h"

using namespace nimbus;
using namespace nimbus::notifier;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

// ---- helpers ----------------------------------------------------------------

static nsn::Frame frame(uint8_t brightness,
                        std::initializer_list<nsn::Segment> segs) {
  nsn::Frame f;
  f.brightness = brightness;
  f.count = 0;
  for (const nsn::Segment& s : segs) f.segs[f.count++] = s;
  return f;
}

static nsn::Segment seg(Status st, uint8_t hue) {
  return nsn::Segment{uint8_t(st), hue, 0, 0};
}

static solide::ring::Slot slotForKey(const attn::Router& r, uint32_t key) {
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  const int n = r.jobs().snapshot(snap, RING_MAX_SEGMENTS);
  for (int i = 0; i < n; ++i)
    if (snap[i].key == key) return snap[i];
  return {};
}

// ---- tests ------------------------------------------------------------------

static void test_frame_populates_jobs_by_index() {
  attn::Router r;
  Mapper m;
  FrameResult res = m.apply(
      frame(200, {seg(Status::Running, 170), seg(Status::Done, 85)}), r, 100);

  TEST_ASSERT_EQUAL(2, res.eventsRouted);
  TEST_ASSERT_TRUE(res.ringDirty);
  TEST_ASSERT_FALSE(res.anyAttention);           // Running/Done aren't attention
  TEST_ASSERT_EQUAL(int(attn::ScreenId::StatusIdle), int(res.epd.screen));
  TEST_ASSERT_FALSE(res.epd.attention);

  TEST_ASSERT_EQUAL(2, r.jobs().count());
  TEST_ASSERT_EQUAL(int(Status::Running), int(slotForKey(r, 0).status));
  TEST_ASSERT_EQUAL(int(Status::Done), int(slotForKey(r, 1).status));
  TEST_ASSERT_EQUAL(200, m.brightness());
}

// The wire hue becomes the segment accent - including 255 (white).
static void test_wire_hue_becomes_accent_including_white() {
  attn::Router r;
  Mapper m;
  m.apply(frame(255, {seg(Status::Running, 42), seg(Status::Running, 255)}), r,
          0);
  TEST_ASSERT_TRUE(slotForKey(r, 0).hasAccent);
  TEST_ASSERT_EQUAL(42, slotForKey(r, 0).accentHue);
  TEST_ASSERT_TRUE(slotForKey(r, 1).hasAccent);
  TEST_ASSERT_EQUAL(255, slotForKey(r, 1).accentHue);
}

// An attention status in the frame flags the aggregate as an immediate badge.
static void test_attention_segment_flags_badge() {
  attn::Router r;
  Mapper m;
  FrameResult res = m.apply(
      frame(128, {seg(Status::Running, 170),
                  seg(Status::AwaitingApproval, 32)}),
      r, 0);
  TEST_ASSERT_TRUE(res.anyAttention);
  TEST_ASSERT_EQUAL(int(attn::ScreenId::StatusIdle), int(res.epd.screen));
  TEST_ASSERT_TRUE(res.epd.attention);
  // topAttention resolves to the approval job (positional key 1).
  attn::Router::Attention top = r.topAttention();
  TEST_ASSERT_TRUE(top.active);
  TEST_ASSERT_EQUAL(int(Status::AwaitingApproval), int(top.status));
}

// A shrinking snapshot Offlines the vanished tail; a growing one re-adds it.
static void test_tail_truncation_and_regrowth() {
  attn::Router r;
  Mapper m;
  m.apply(frame(100, {seg(Status::Running, 1), seg(Status::Running, 2),
                      seg(Status::Running, 3)}),
          r, 0);
  TEST_ASSERT_EQUAL(3, r.jobs().count());

  // Snapshot shrinks to 2: key 2 is freed.
  FrameResult res = m.apply(
      frame(100, {seg(Status::Running, 1), seg(Status::Running, 2)}), r, 10);
  TEST_ASSERT_EQUAL(3, res.eventsRouted);        // 2 upserts + 1 offline
  TEST_ASSERT_EQUAL(2, r.jobs().count());
  TEST_ASSERT_FALSE(slotForKey(r, 2).used);

  // Grows back to 3: key 2 returns.
  m.apply(frame(100, {seg(Status::Running, 1), seg(Status::Running, 2),
                      seg(Status::Running, 9)}),
          r, 20);
  TEST_ASSERT_EQUAL(3, r.jobs().count());
  TEST_ASSERT_EQUAL(9, slotForKey(r, 2).accentHue);
}

// Empty frame clears everything.
static void test_empty_frame_clears() {
  attn::Router r;
  Mapper m;
  m.apply(frame(100, {seg(Status::Running, 1), seg(Status::Running, 2)}), r, 0);
  TEST_ASSERT_EQUAL(2, r.jobs().count());
  FrameResult res = m.apply(frame(100, {}), r, 10);
  TEST_ASSERT_EQUAL(2, res.eventsRouted);        // both offlined
  TEST_ASSERT_EQUAL(0, r.jobs().count());
}

// Re-applying the identical frame doesn't reset the Fade/Breathe clock
// (enteredAt), so animations don't stutter on a resend.
static void test_identical_frame_preserves_entered_at() {
  attn::Router r;
  Mapper m;
  m.apply(frame(100, {seg(Status::Done, 85)}), r, 1000);
  const uint32_t entered = slotForKey(r, 0).enteredAt;
  m.apply(frame(100, {seg(Status::Done, 85)}), r, 5000);
  TEST_ASSERT_EQUAL_UINT32(entered, slotForKey(r, 0).enteredAt);
}

static void test_timeout_offlines_stale_ring() {
  attn::Router r;
  Mapper m;
  m.apply(frame(100, {seg(Status::Running, 1), seg(Status::Running, 2)}), r,
          1000);

  // Within the window: no-op.
  FrameResult res = m.timeout(r, 3000, /*ambientMs=*/5000, /*attnMs=*/5000);
  TEST_ASSERT_EQUAL(0, res.eventsRouted);
  TEST_ASSERT_FALSE(res.ringDirty);
  TEST_ASSERT_EQUAL(2, r.jobs().count());

  // Past the window: everything Offlined (these are ambient Running segments).
  res = m.timeout(r, 6001, 5000, 5000);
  TEST_ASSERT_TRUE(res.ringDirty);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
  TEST_ASSERT_EQUAL(int(attn::ScreenId::StatusIdle), int(res.epd.screen));

  // Idempotent: a second timeout does nothing.
  res = m.timeout(r, 20000, 5000, 5000);
  TEST_ASSERT_EQUAL(0, res.eventsRouted);
}

// timeout() before any frame is a no-op (no phantom offlines at boot).
// A frame stamped slightly AFTER the nowMs a later timeout() is called with is
// normal - loop() caches now=millis() at the top of the iteration, so anything
// applying a frame later in that same iteration stamps a larger value. The age
// delta must therefore be signed: unsigned, the tiny negative wrapped to ~4.29e9
// and every job (calls-to-action included) expired on the very next tick.
// Hardware-measured on Nimbus-4: a 3-segment frame went 3 -> 0 jobs instantly.
static void test_timeout_ignores_a_frame_stamped_in_the_future() {
  attn::Router r;
  Mapper m;
  m.apply(frame(120, {seg(Status::Running, 10),
                      seg(Status::WaitingInput, 20)}), r, 100050);
  TEST_ASSERT_EQUAL(2, r.jobs().count());

  // 50 ms "in the past" relative to the frame - must read as fresh, not stale.
  FrameResult res = m.timeout(r, 100000, 5000, 300000);
  TEST_ASSERT_EQUAL(0, res.eventsRouted);
  TEST_ASSERT_EQUAL(2, r.jobs().count());

  // ...and a genuinely stale ring still expires, so the guard is not a mute.
  res = m.timeout(r, 100050 + 300001, 5000, 300000);
  TEST_ASSERT_EQUAL(2, res.eventsRouted);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
}

static void test_timeout_before_any_frame_is_noop() {
  attn::Router r;
  Mapper m;
  FrameResult res = m.timeout(r, 100000, 5000, 5000);
  TEST_ASSERT_EQUAL(0, res.eventsRouted);
  TEST_ASSERT_FALSE(m.seenFrame());
}

// A call-to-action (needs-you / error) must OUTLIVE the ambient link-timeout:
// the broker sends one HITL frame then goes quiet, and the job - still waiting on
// the human - must not vanish with the ambient ring 5 s later.
static void test_timeout_holds_attention_calls_to_action() {
  attn::Router r;
  Mapper m;
  m.apply(frame(100, {seg(Status::Running, 1),
                      seg(Status::AwaitingApproval, 2)}), r, 1000);
  TEST_ASSERT_EQUAL(2, r.jobs().count());

  // Past the ambient window (6 s) but well within the attention hold (5 min):
  // the Running clears, the needs-approval CTA HOLDS.
  FrameResult res = m.timeout(r, 1000 + 6000, /*ambientMs=*/5000, /*attnMs=*/300000);
  TEST_ASSERT_TRUE(res.ringDirty);            // the Running expired
  TEST_ASSERT_EQUAL(1, r.jobs().count());     // the call-to-action survived
  TEST_ASSERT_TRUE(r.topAttention().active);  // and it's the attention source

  // Past the attention hold too: the CTA finally expires.
  res = m.timeout(r, 1000 + 300001, 5000, 300000);
  TEST_ASSERT_TRUE(res.ringDirty);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
}

// The ambient hold scales with the ring level: Full lingers like a desk display,
// Dark clears fast. (Calls-to-action hold on their own clock - see the CTA test.)
static void test_ambient_hold_scales_with_posture() {
  TEST_ASSERT_EQUAL_UINT32(300000, ambientHoldFor(Posture::Full));  // 5 min desk
  TEST_ASSERT_EQUAL_UINT32(30000,  ambientHoldFor(Posture::Calm));  // 30 s glance
  TEST_ASSERT_EQUAL_UINT32(5000,   ambientHoldFor(Posture::Dark));  // 5 s frugal
}

// Overflow: a frame with more than the ring capacity of attention jobs - the
// mapper still routes every event and count saturates at capacity (documented
// P3 overflow limitation; this pins the current behavior).
static void test_overflow_saturates_at_capacity() {
  attn::Router r;
  Mapper m;
  nsn::Frame f;
  f.brightness = 100;
  f.count = nsn::kMaxSegs;  // 16 > RING_MAX_SEGMENTS (8)
  for (int i = 0; i < nsn::kMaxSegs; ++i)
    f.segs[i] = nsn::Segment{uint8_t(Status::AwaitingApproval), uint8_t(i), 0, 0};
  FrameResult res = m.apply(f, r, 0);
  TEST_ASSERT_EQUAL(nsn::kMaxSegs, res.eventsRouted);
  TEST_ASSERT_TRUE(res.anyAttention);
  TEST_ASSERT_EQUAL(RING_MAX_SEGMENTS, r.jobs().count());  // saturated, not crashed
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_frame_populates_jobs_by_index);
  RUN_TEST(test_wire_hue_becomes_accent_including_white);
  RUN_TEST(test_attention_segment_flags_badge);
  RUN_TEST(test_tail_truncation_and_regrowth);
  RUN_TEST(test_empty_frame_clears);
  RUN_TEST(test_identical_frame_preserves_entered_at);
  RUN_TEST(test_timeout_offlines_stale_ring);
  RUN_TEST(test_timeout_holds_attention_calls_to_action);
  RUN_TEST(test_timeout_ignores_a_frame_stamped_in_the_future);
  RUN_TEST(test_timeout_before_any_frame_is_noop);
  RUN_TEST(test_ambient_hold_scales_with_posture);
  RUN_TEST(test_overflow_saturates_at_capacity);
  return UNITY_END();
}
