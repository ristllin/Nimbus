#pragma once
// Lane E2-FIX4: a photo the head was shown stays in view the way the rest of the conversation does
// (lanes/E2-BENCH/RESULT.md part 2: seven minutes after a photo was described, the head said it could
// not see it and disowned its own description). Through the plain engine and the Anthropic wire, with
// a scripted provider and the binding's photo hook (txb::Battery::photoStore, the card's copy).
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/props.h"
#include "engine_tests.h"
#include "nimbus/orch/tx/photo_text.h"

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

// The other bound of the rule: compaction. A photo, then long turns until the head's due mark makes
// the engine compact at a turn open: while no digest covers the photo's turn every request carries
// its pixels; once a digest's thru passes it, the request carries neither the pixels nor a line (the
// summary stands for that part of the conversation, as for any other turn).
inline void test_a_photo_stays_until_compaction_passes_its_turn() {
  txb::Battery b;
  tx::HeadBudget hb;
  TEST_ASSERT_TRUE(tx::budgetFor(b.headKey(), tx::Profile::Full, hb).ok());
  b.photoStore["pa1b2c3d4"] = jpeg('j', 120000);
  b.model.push(sp::say("A red car."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("look", out, {photoPart("pa1b2c3d4", 120000)}));
  uint64_t photoQ = 0;
  for (const tx::Item& it : engine_tests::storedOf(b))
    if (it.kind == tx::ItemKind::User && !it.parts.empty()) photoQ = it.q;
  TEST_ASSERT_TRUE(photoQ > 0);
  bool covered = false;
  for (int k = 0; k < 120 && !covered; ++k) {
    b.advance(300);
    sp::Step s = sp::say("reply " + std::to_string(k) + " " + std::string(3000, 'r'));
    s.usage.input = 100;
    s.usage.cacheRead = k % 10 == 9 ? hb.tokens.due : 1000;   // every tenth answer reaches the due mark
    b.model.push(s);
    TXB_OK(b.owner("turn " + std::to_string(k) + " " + std::string(1500, 'q'), out));
    uint64_t thru = 0;
    for (const tx::Item& it : engine_tests::storedOf(b))
      if (it.kind == tx::ItemKind::Digest) thru = std::max(thru, it.thru);
    covered = thru >= photoQ;
    char msg[96];
    std::snprintf(msg, sizeof msg, "turn %d, thru %llu, photo at q%llu", k, (unsigned long long)thru, (unsigned long long)photoQ);
    // The turn's head request is the last one the head got (the summarizer has its own provider).
    TEST_ASSERT_EQUAL_MESSAGE(!covered, has(lastBody(b), pixelsOf('j')), msg);
    if (covered) TEST_ASSERT_FALSE_MESSAGE(has(lastBody(b), "[photo pa1b2c3d4"), msg);
  }
  TEST_ASSERT_TRUE_MESSAGE(covered, "compaction never passed the photo's turn: the case proves nothing");
}

// A photo whose stored copy cannot be read back (gone from the card) renders as its true line, never
// as an empty image or nothing.
inline void test_a_photo_the_card_cannot_give_back_renders_as_its_line() {
  txb::Battery b;
  b.photoStore["pa1b2c3d4"] = jpeg('j', 120000);
  b.model.push(sp::say("A red car."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("look", out, {photoPart("pa1b2c3d4", 120000)}));
  b.photoStore.erase("pa1b2c3d4");
  b.photoStore["pother000"] = jpeg('k', 1000);   // the hook stays installed
  b.model.push(sp::say("ok"));
  TXB_OK(b.owner("next", out));
  TEST_ASSERT_FALSE(has(lastBody(b), pixelsOf('j')));
  TEST_ASSERT_TRUE(has(lastBody(b), "[photo pa1b2c3d4") && has(lastBody(b), "not attached to this request"));
  TEST_ASSERT_FALSE(has(lastBody(b), "\"type\":\"image\""));
}

// With lane E2-CACHE's byte-stable prefix (rule A): the bytes of a request ahead of its last closed
// turn (cache markers left out) open the next request. A photo in view sits inside that cached part
// as the same base64 on every request, so it keeps rule A turn after turn; the only change it makes
// is the one turn the cap pushes it out (its pixels become its line), and that change starts at its
// own message: everything before that message stays byte for byte.
inline std::string stripCacheMarks(std::string s) {
  for (const char* m : {",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}", ",\"cache_control\":{\"type\":\"ephemeral\"}"}) {
    const std::string mark = m;
    for (size_t at = s.find(mark); at != std::string::npos; at = s.find(mark, at)) s.erase(at, mark.size());
  }
  return s;
}
// As test_e2_cache_prefix reads it, from "system" on: max_tokens (the load-aware output cap, which a
// photo's upload time lowers) is not part of the provider's cached prefix (tools, system, messages).
inline std::string fromSystem(const std::string& body) {
  const size_t sys = body.find("\"system\":[");
  TEST_ASSERT_TRUE(sys != std::string::npos);
  return body.substr(sys);
}
inline std::string aheadOfClosed(const std::string& whole) {
  const std::string body = fromSystem(whole);
  const size_t msgs = body.find("\"messages\":[");
  TEST_ASSERT_TRUE(msgs != std::string::npos);
  std::vector<size_t> marks;
  for (size_t at = body.find(",\"cache_control\":", msgs); at != std::string::npos; at = body.find(",\"cache_control\":", at + 1))
    marks.push_back(at);
  return stripCacheMarks(body.substr(0, marks.size() >= 2 ? marks.front() : msgs));
}

inline void test_a_photo_in_view_keeps_the_cached_prefix_byte_for_byte() {
  txb::Battery b;
  const char fills[] = {'a', 'b', 'c', 'd', 'e'};
  plain::TurnOutcome out;
  std::string prev;
  int photos = 0, pairs = 0;
  for (int turn = 0; turn < 14; ++turn) {
    b.advance(120);
    const bool photoTurn = turn % 3 == 0 && photos < 5;
    std::vector<tx::Part> parts;
    if (photoTurn) {
      const std::string ref = "p" + std::string(7, '0') + std::to_string(photos + 1);
      b.photoStore[ref] = jpeg(fills[photos], 60000);
      parts.push_back(photoPart(ref, 60000));
      ++photos;
    }
    b.model.push(sp::say("ok " + std::to_string(turn)));
    TXB_OK(b.owner("message " + std::to_string(turn), out, parts));
    const std::string body = lastBody(b);
    if (!prev.empty()) {
      const std::string ahead = aheadOfClosed(prev), next = stripCacheMarks(fromSystem(body));
      char msg[96];
      if (photos <= (int)tx::kInlinePhotosInViewMax || !photoTurn) {   // nothing fell out this turn
        std::snprintf(msg, sizeof msg, "turn %d: a photo in view rewrote the bytes ahead of the closed turns", turn);
        TEST_ASSERT_TRUE_MESSAGE(next.compare(0, ahead.size(), ahead) == 0, msg);
        ++pairs;
      } else {   // the fifth photo pushed the first out: the change starts at the first photo's message
        const size_t at = ahead.find(pixelsOf('a'));
        TEST_ASSERT_TRUE(at != std::string::npos);
        const size_t open = ahead.rfind("{\"role\":\"user\"", at);
        TEST_ASSERT_TRUE(open != std::string::npos);
        std::snprintf(msg, sizeof msg, "turn %d: bytes before the photo that fell out changed", turn);
        TEST_ASSERT_TRUE_MESSAGE(next.compare(0, open, ahead, 0, open) == 0, msg);
        TEST_ASSERT_FALSE(has(body, pixelsOf('a')));
      }
      if (photos >= 2 && photos <= (int)tx::kInlinePhotosInViewMax)   // the first photo's turn is closed and in view
        TEST_ASSERT_TRUE_MESSAGE(has(aheadOfClosed(body), pixelsOf('a')), "the photo is not inside the cached part");
    }
    prev = body;
  }
  TEST_ASSERT_EQUAL(5, photos);
  TEST_ASSERT_TRUE(pairs >= 12);
}

inline size_t imagesIn(const std::string& body) {
  size_t n = 0;
  for (size_t at = body.find("\"type\":\"image\""); at != std::string::npos; at = body.find("\"type\":\"image\"", at + 1)) ++n;
  return n;
}

// Lane E2-P-PHOTO D1: an album of ten 200 KB photos is seen whole in the turn it arrives (engine 1
// shows ten; this engine showed four and stubbed six), inside the body cap; one turn later the
// four-photo and 600,000 B caps hold again (three of 200 KB). The worst-case request is printed.
inline void test_an_album_of_ten_is_seen_whole_when_it_arrives() {
  txb::Battery b;
  const char fills[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'k'};
  std::vector<tx::Part> parts;
  for (size_t i = 0; i < 10; ++i) {
    const std::string ref = "p" + std::string(7, '0') + std::to_string(i);
    b.photoStore[ref] = jpeg(fills[i], 200000);
    parts.push_back(photoPart(ref, 200000));
  }
  b.model.push(sp::say("Ten meter readings."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("Here are ten meter photos: what is the difference between the first and the last?", out, parts));
  TEST_ASSERT_TRUE_MESSAGE(out.delivered, "the album turn ended without the model's reply");
  const std::string first = lastBody(b);
  TEST_ASSERT_EQUAL_MESSAGE(10, (int)imagesIn(first), "the arrival turn did not show the whole album");
  for (char f : fills) TEST_ASSERT_TRUE_MESSAGE(has(first, pixelsOf(f)), "a photo of the album is missing its pixels");
  TEST_ASSERT_FALSE_MESSAGE(has(first, "[photo p"), "a photo of the arriving album rendered as its line");
  TEST_ASSERT_EQUAL(1, (int)b.model.requests().size());   // no ladder climb, no refusal: inside the body cap
  char msg[160];
  std::snprintf(msg, sizeof msg, "E2-P-PHOTO worst case: ten 200,000 B photos, request body %u B (text %u B)",
                (unsigned)first.size(), (unsigned)(first.size() - 10 * ((200000 + 2) / 3 * 4)));
  TEST_MESSAGE(msg);
  b.model.push(sp::say("The second read 18,251."));
  TXB_OK(b.owner("what did the second one read?", out));
  const std::string next = lastBody(b);
  TEST_ASSERT_TRUE_MESSAGE(imagesIn(next) <= tx::kInlinePhotosInViewMax, "a later turn carried more than four photos");
  TEST_ASSERT_EQUAL_MESSAGE(3, (int)imagesIn(next), "a later turn did not hold 600,000 B of the album");
  TEST_ASSERT_TRUE(has(next, "[photo p00000009"));
}

// Lane E2-P-PHOTO D3: a provider that refuses the photo for its format (a 400 naming the image) never
// ends the turn in silence or an error line alone: the round goes once more with the photo as its true
// line, the model answers, and the photo is not sent as pixels again in this conversation.
inline void test_a_photo_the_provider_refuses_ends_in_a_reply_with_a_true_line() {
  txb::Battery b;
  b.photoStore["pab12cd34"] = jpeg('j', 60000);
  b.model.push(sp::httpError(400, R"({"type":"error","error":{"type":"invalid_request_error","message":)"
                                  R"("messages.0.content.1.image.source.base64: The image format is not supported."}})"));
  b.model.push(sp::say("I could not open that photo: the provider refused its format. Could you send it as a JPEG?"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("What is on this label?", out, {photoPart("pab12cd34", 60000)}));
  TEST_ASSERT_TRUE_MESSAGE(out.delivered, "the refused photo ended the turn without a reply");
  TEST_ASSERT_TRUE(has(out.text, "could not open that photo"));
  TEST_ASSERT_EQUAL(2, (int)b.model.requests().size());
  TEST_ASSERT_TRUE(has(b.model.requests()[0].body, pixelsOf('j')));
  const std::string resent = lastBody(b);
  TEST_ASSERT_FALSE_MESSAGE(has(resent, pixelsOf('j')), "the re-sent round carried the refused photo again");
  TEST_ASSERT_TRUE_MESSAGE(has(resent, "[photo pab12cd34") && has(resent, tx::kPhotoRefused),
                           "the re-sent round does not say the provider refused the photo");
  TEST_ASSERT_FALSE(has(resent, "shown earlier"));
  b.model.push(sp::say("Noted."));
  TXB_OK(b.owner("ok, never mind", out));
  TEST_ASSERT_EQUAL(3, (int)b.model.requests().size());
  TEST_ASSERT_FALSE_MESSAGE(has(lastBody(b), pixelsOf('j')), "the refused photo was sent again a turn later");
  TEST_ASSERT_TRUE(has(lastBody(b), tx::kPhotoRefused));
}

inline void run() {
  RUN_TEST(test_a_photo_stays_in_view_across_turns_time_and_a_restart);
  RUN_TEST(test_a_photo_past_the_caps_gets_a_true_line_and_names_no_unbound_tool);
  RUN_TEST(test_no_gap_and_no_restart_takes_a_photo_out_of_view);
  RUN_TEST(test_a_photo_stays_until_compaction_passes_its_turn);
  RUN_TEST(test_a_photo_the_card_cannot_give_back_renders_as_its_line);
  RUN_TEST(test_a_photo_in_view_keeps_the_cached_prefix_byte_for_byte);
  RUN_TEST(test_an_album_of_ten_is_seen_whole_when_it_arrives);
  RUN_TEST(test_a_photo_the_provider_refuses_ends_in_a_reply_with_a_true_line);
}

}  // namespace photo_tests
