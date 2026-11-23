#pragma once
// Lane E2-FIX4: a photo the head was shown stays in view the way the rest of the conversation does
// (lanes/E2-BENCH/RESULT.md part 2: seven minutes after a photo was described, the head said it could
// not see it and disowned its own description). Through the plain engine and the Anthropic wire, with
// a scripted provider and the binding's photo hook (txb::Battery::photoStore, the card's copy).
#include <unity.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/props.h"
#include "engine_tests.h"

namespace photo_tests {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

// A synthetic JPEG of `size` bytes: a 4-byte JPEG header, then `fill`. From byte 6 on every base64
// group is three fill bytes ('j' encodes as "ampq"), so a body holds a photo's pixels exactly when it
// holds pixelsOf(fill).
inline std::string jpeg(char fill, size_t size) {
  std::string s("\xFF\xD8\xFF\xE0", 4);
  s.resize(size, fill);
  return s;
}
inline std::string pixelsOf(char fill) {
  const std::string three(3, fill);
  static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned v = ((unsigned char)three[0] << 16) | ((unsigned char)three[1] << 8) | (unsigned char)three[2];
  std::string g;
  for (int k = 18; k >= 0; k -= 6) g += kB64[(v >> k) & 63];
  return g + g + g + g;   // 12 characters: never in a body by chance
}
inline tx::Part photoPart(const std::string& ref, size_t size) {
  return tx::Part{tx::PartKind::Image, ref, "image/jpeg", (uint32_t)size};
}
inline bool has(const std::string& body, const std::string& what) { return body.find(what) != std::string::npos; }
inline const std::string& lastBody(txb::Battery& b) { return b.model.requests().back().body; }

// D2 (a): a photo, then twelve unrelated turns over thirteen minutes, then a question about it: the
// request for that question still carries the pixels. After a restart, the same.
inline void test_a_photo_stays_in_view_across_turns_time_and_a_restart() {
  txb::Battery b;
  b.photoStore["pa1b2c3d4"] = jpeg('j', 120000);
  b.model.push(sp::say("A red car on green grass, a tree on the left, a yellow sun."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("What is in this photo? One sentence.", out, {photoPart("pa1b2c3d4", 120000)}));
  TEST_ASSERT_TRUE_MESSAGE(has(lastBody(b), pixelsOf('j')), "the arrival turn did not show the photo");
  for (int k = 0; k < 12; ++k) {
    b.advance(60);
    b.model.push(sp::say("ok " + std::to_string(k)));
    TXB_OK(b.owner("an unrelated message " + std::to_string(k), out));
  }
  b.advance(60);
  b.model.push(sp::say("On the left."));
  TXB_OK(b.owner("which side is the tree on?", out));
  TEST_ASSERT_TRUE_MESSAGE(has(lastBody(b), pixelsOf('j')), "13 minutes and 13 turns later the photo left the request");
  TEST_ASSERT_FALSE_MESSAGE(has(lastBody(b), "[photo pa1b2c3d4"), "a photo in view also rendered as its line");
  TXB_OK(b.powerCycle());
  b.advance(600);
  b.model.push(sp::say("Still on the left."));
  TXB_OK(b.owner("and after the restart, which side?", out));
  TEST_ASSERT_TRUE_MESSAGE(has(lastBody(b), pixelsOf('j')), "a restart dropped a photo still stored on the card");
}

// D2 (b): more photos than the caps hold, one per turn: the oldest falls out first and renders as a
// true line (shown earlier in this conversation, not attached to this request), naming no tool the
// engine has not bound; the newest stay as pixels.
inline void test_a_photo_past_the_caps_gets_a_true_line_and_names_no_unbound_tool() {
  txb::Battery b;
  const char fills[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'};
  const size_t n = sizeof fills;
  plain::TurnOutcome out;
  for (size_t i = 0; i < n; ++i) {
    const std::string ref = "p" + std::string(7, '0') + std::to_string(i + 1);
    b.photoStore[ref] = jpeg(fills[i], 60000);
    b.advance(30);
    b.model.push(sp::say("Photo " + std::to_string(i + 1) + " seen."));
    TXB_OK(b.owner("photo " + std::to_string(i + 1), out, {photoPart(ref, 60000)}));
  }
  b.advance(30);
  b.model.push(sp::say("The first one showed a ..."));
  TXB_OK(b.owner("what was in the first photo?", out));
  const std::string& body = lastBody(b);
  TEST_ASSERT_TRUE_MESSAGE(has(body, pixelsOf(fills[n - 1])), "the newest photo left the request");
  TEST_ASSERT_FALSE_MESSAGE(has(body, pixelsOf(fills[0])), "the oldest photo stayed past the caps");
  TEST_ASSERT_TRUE_MESSAGE(has(body, "[photo p00000001"), "the photo out of view has no line");
  TEST_ASSERT_TRUE_MESSAGE(has(body, "earlier in this conversation") && has(body, "not attached to this request"),
                           "the line does not say the photo was shown earlier and is not attached now");
  TEST_ASSERT_FALSE_MESSAGE(has(body, "view id=p"), "the line names the view tool, which this engine has not bound");
}

// Counter-test: a time or restart rule cannot come back. One photo, then three person turns at each
// gap around the old marks (270 s inline, 3,600 s by file reference) and far past them, with and
// without a restart in between: every later request carries the pixels.
inline void test_no_gap_and_no_restart_takes_a_photo_out_of_view() {
  for (uint32_t gap : {0u, 269u, 271u, 3599u, 3601u, 3u * 86400u})
    for (bool restart : {false, true}) {
      txb::Battery b;
      b.photoStore["pa1b2c3d4"] = jpeg('j', 120000);
      b.model.push(sp::say("A red car."));
      plain::TurnOutcome out;
      TXB_OK(b.owner("look", out, {photoPart("pa1b2c3d4", 120000)}));
      for (int k = 0; k < 3; ++k) {
        b.advance(gap);
        if (restart) TXB_OK(b.powerCycle());
        b.model.push(sp::say("ok"));
        TXB_OK(b.owner("next " + std::to_string(k), out));
        char msg[80];
        std::snprintf(msg, sizeof msg, "gap %u s, restart %d, turn %d: the photo left view", gap, (int)restart, k);
        TEST_ASSERT_TRUE_MESSAGE(has(lastBody(b), pixelsOf('j')), msg);
      }
    }
}

inline void run() {
  RUN_TEST(test_a_photo_stays_in_view_across_turns_time_and_a_restart);
  RUN_TEST(test_a_photo_past_the_caps_gets_a_true_line_and_names_no_unbound_tool);
  RUN_TEST(test_no_gap_and_no_restart_takes_a_photo_out_of_view);
}

}  // namespace photo_tests
