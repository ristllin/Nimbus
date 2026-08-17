#include <unity.h>

#include <cstring>
#include <string>

#include "../test_proto/nsn_vectors.h"  // real broker-encoded packets
#include "nimbus/attention.h"
#include "nimbus/notifier_map.h"
#include "nimbus/nsn_proto.h"
#include "nimbus/profile.h"
#include "nimbus/ring_plan.h"

// End-to-end for Notifier mode over the PORTABLE stack, driven by bytes the
// real Python broker (frame.py) produced: feed each vector packet through the
// incremental decoder exactly as the serial loop would, map it into the
// attention Router, and compose the ring Plan. This closes the loop that P2
// proved on hardware (Plan -> solide::leds/display): broker bytes in, correct
// ring/e-ink decisions out.

using namespace nimbus;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

static const NsnVector& vec(const char* name) {
  for (size_t i = 0; i < kNsnVectorCount; ++i)
    if (std::string(kNsnVectors[i].name) == name) return kNsnVectors[i];
  TEST_FAIL_MESSAGE("vector not found");
  return kNsnVectors[0];
}

// Stream a packet byte-by-byte through the decoder (as the UART/USB loop does)
// and apply the first complete frame via the Mapper.
static notifier::FrameResult streamApply(const NsnVector& v, nsn::Decoder& dec,
                                         notifier::Mapper& map,
                                         attn::Router& r, uint32_t now) {
  nsn::Frame f;
  bool got = false;
  for (size_t i = 0; i < v.packetLen; ++i)
    if (dec.feed(v.packet[i], f)) got = true;
  TEST_ASSERT_TRUE_MESSAGE(got, v.name);
  return map.apply(f, r, now);
}

// default_styles_all_states carries all seven statuses; after decode+map the
// ring Plan (Active) must show them all, and topAttention must surface the
// highest-priority attention state (AwaitingApproval).
static void test_all_states_vector_drives_active_ring() {
  nsn::Decoder dec;
  notifier::Mapper map;
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);  // Active

  const NsnVector& v = vec("default_styles_all_states");
  notifier::FrameResult res = streamApply(v, dec, map, r, 100);
  TEST_ASSERT_TRUE(res.anyAttention);  // WaitingInput/AwaitingApproval/Error present

  ring::Cursor cur;
  ring::Plan p = ring::compose(r, cfg, cur, /*panelBusy=*/false, 200);
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  // 7 states, ring capacity 8 -> all fit.
  TEST_ASSERT_EQUAL(r.jobs().count(), p.segCount);

  attn::Router::Attention top = r.topAttention();
  TEST_ASSERT_TRUE(top.active);
  TEST_ASSERT_EQUAL(int(Status::AwaitingApproval), int(top.status));
}

// Same vector, Passive posture: ring dark except the single attention LED,
// whose hue follows the top attention state (AwaitingApproval style = 32).
static void test_all_states_vector_passive_single_led() {
  nsn::Decoder dec;
  notifier::Mapper map;
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Balanced);  // Passive

  streamApply(vec("default_styles_all_states"), dec, map, r, 0);
  ring::Cursor cur;
  ring::Plan p = ring::compose(r, cfg, cur, false, 10);
  TEST_ASSERT_EQUAL(0, p.segCount);  // ring dark
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(32, p.single.hue);  // AwaitingApproval hue
}

// A sequence of real frames: single_running -> empty_ring. The empty snapshot
// must clear the ring (Offline the vanished segment).
static void test_frame_sequence_clears_on_empty() {
  nsn::Decoder dec;
  notifier::Mapper map;
  attn::Router r;

  streamApply(vec("single_running"), dec, map, r, 0);
  TEST_ASSERT_EQUAL(1, r.jobs().count());

  notifier::FrameResult res = streamApply(vec("empty_ring"), dec, map, r, 10);
  TEST_ASSERT_EQUAL(0, r.jobs().count());
  TEST_ASSERT_TRUE(res.ringDirty);
  TEST_ASSERT_FALSE(res.anyAttention);
}

// Brightness from the wire is carried through to the Mapper (the device applies
// it as the ring cap).
static void test_wire_brightness_propagates() {
  nsn::Decoder dec;
  notifier::Mapper map;
  attn::Router r;
  streamApply(vec("single_running"), dec, map, r, 0);  // brightness 60
  TEST_ASSERT_EQUAL(60, map.brightness());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_all_states_vector_drives_active_ring);
  RUN_TEST(test_all_states_vector_passive_single_led);
  RUN_TEST(test_frame_sequence_clears_on_empty);
  RUN_TEST(test_wire_brightness_propagates);
  return UNITY_END();
}
