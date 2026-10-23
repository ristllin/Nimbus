#include <unity.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/orch/turn_images.h"

// R1 (photos to the head as images): the portable pieces under every head renderer.
//  - the spliced request body (http.h): a photo's bytes are written into the body at
//    send time, base64-encoded slice by slice, never assembled and never a JSON string;
//  - the image rule (headTakesImages), the formats a head takes, HEIC and animated GIF;
//  - the turn's image set: markers, the blind text (each attached label replaced by its
//    machine transcription, made once), the release hook;
//  - the channel -> engine slot: bound to the chat and the routed text, consumed once,
//    a stale offer freed.

using namespace nimbus::orch;

void setUp() {}
void tearDown() { dropTurnImages(); }

// ---- the spliced body --------------------------------------------------------------

static std::string b64(const std::vector<uint8_t>& v) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < v.size(); i += 3) {
    const uint32_t n = (uint32_t)v[i] << 16 | (i + 1 < v.size() ? (uint32_t)v[i + 1] << 8 : 0) |
                       (i + 2 < v.size() ? v[i + 2] : 0);
    out += k[(n >> 18) & 63];
    out += k[(n >> 12) & 63];
    out += i + 1 < v.size() ? k[(n >> 6) & 63] : '=';
    out += i + 2 < v.size() ? k[n & 63] : '=';
  }
  return out;
}

static std::vector<uint8_t> bytesOf(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1103515245u + 12345u;
    v[i] = (uint8_t)(seed >> 16);
  }
  return v;
}

// Every length class of the base64 tail (0, 1 and 2 bytes over a group), raw and
// base64 splices, at the start, middle and end of the body: the materialized body is
// the reference string, and reading it back in slices of every size from 1 to past
// the end gives the same bytes at every offset.
static void test_spliced_body_reads_back_at_every_offset() {
  for (size_t a = 0; a < 3; ++a) {
    const std::vector<uint8_t> p1 = bytesOf(30 + a, 7), p2 = bytesOf(4 + a, 9), p3 = bytesOf(2 + a, 11);
    agent::HttpRequest r;
    r.body = "{\"a\":\"\",\"b\":\"\"}";
    r.splices = {agent::BodySplice{0, p3.data(), p3.size(), false},
                 agent::BodySplice{6, p1.data(), p1.size(), true},
                 agent::BodySplice{r.body.size() - 2, p2.data(), p2.size(), true}};
    const std::string want = std::string(p3.begin(), p3.end()) + "{\"a\":\"" + b64(p1) + "\",\"b\":\"" + b64(p2) +
                             "\"}";
    TEST_ASSERT_EQUAL(want.size(), agent::bodyLength(r));
    TEST_ASSERT_TRUE(agent::materializeBody(r) == want);
    for (size_t slice = 1; slice <= want.size() + 3; ++slice) {
      std::string got;
      std::vector<uint8_t> buf(slice);
      for (size_t off = 0;;) {
        const size_t n = agent::bodyRead(r, off, buf.data(), slice);
        if (!n) break;
        got.append((const char*)buf.data(), n);
        off += n;
      }
      TEST_ASSERT_TRUE_MESSAGE(got == want, ("slice " + std::to_string(slice)).c_str());
    }
    uint8_t one;
    TEST_ASSERT_EQUAL(0u, agent::bodyRead(r, want.size(), &one, 1));   // past the end
  }
  agent::HttpRequest plain;
  plain.body = "abc";
  TEST_ASSERT_EQUAL(3u, agent::bodyLength(plain));
  TEST_ASSERT_TRUE(agent::materializeBody(plain) == "abc");
}

// A 600 KB photo in 4,096 B slices (the transport's slice size): what goes out equals
// the reference encoding, and no slice needs more than the slice buffer.
static void test_large_photo_in_transport_slices() {
  const std::vector<uint8_t> photo = bytesOf(600 * 1024 + 1, 3);
  agent::HttpRequest r;
  r.body = "{\"data\":\"\"}";
  r.splices = {agent::BodySplice{9, photo.data(), photo.size(), true}};
  const std::string want = "{\"data\":\"" + b64(photo) + "\"}";
  std::string got;
  uint8_t slice[4096];
  for (size_t off = 0, n; (n = agent::bodyRead(r, off, slice, sizeof slice)) > 0; off += n) got.append((char*)slice, n);
  TEST_ASSERT_TRUE(got == want);
  TEST_ASSERT_TRUE(want.size() > 65535);   // far past what one JSON string may hold
}

// ---- the image rule and formats ----------------------------------------------------

static void test_head_takes_images_table() {
  struct Row {
    const char* host;
    const char* model;
    bool yes;
  } rows[] = {
      {"anthropic", "claude-haiku-5-5", true},   {"anthropic", "claude-sonnet-5-5", true},
      {"anthropic", "", true},                   {"openai", "gpt-5.6-luna", true},
      {"openai", "gpt-5.4-mini", true},          {"openai", "gpt-4o", true},
      {"openai", "gpt-4.1-mini", true},          {"openai", "o4-mini", true},
      {"openai", "o3-mini", false},              {"openai", "gpt-realtime", false},
      {"openai", "gpt-4o-mini-transcribe", false}, {"openai", "gpt-image-2", false},
      {"mistral", "mistral-small-latest", true}, {"mistral", "mistral-medium-latest", true},
      {"mistral", "mistral-large-latest", true}, {"mistral", "ministral-8b-latest", true},
      {"mistral", "codestral-latest", false},    {"mistral", "devstral-small-latest", false},
      {"mistral", "mistral-ocr-latest", false},  {"mistral", "voxtral-small-latest", false},
      {"zai", "glm-5.3-flash", false},           {"zai", "glm-5.3", false},
      {"custom", "anything", false},             {"cumulo", "anthropic/claude-haiku-5-5", true},
      {"cumulo", "openai/gpt-5.6-luna", true},   {"cumulo", "zai/glm-5.3", false},
      {"cumulo", "claude-haiku-5-5", false},     {"cumulo", "cumulo/x", false},
  };
  for (const Row& r : rows)
    TEST_ASSERT_EQUAL_MESSAGE(r.yes, headTakesImages(r.host, r.model), (std::string(r.host) + "/" + r.model).c_str());
}

static void test_formats() {
  TEST_ASSERT_EQUAL_STRING("image/jpeg", imagePartMime("image/jpeg"));
  TEST_ASSERT_EQUAL_STRING("image/jpeg", imagePartMime("IMAGE/JPG"));
  TEST_ASSERT_EQUAL_STRING("image/png", imagePartMime("image/png"));
  TEST_ASSERT_EQUAL_STRING("image/webp", imagePartMime("image/webp"));
  TEST_ASSERT_EQUAL_STRING("image/gif", imagePartMime("image/gif"));
  TEST_ASSERT_EQUAL_STRING("", imagePartMime("image/heic"));
  TEST_ASSERT_EQUAL_STRING("", imagePartMime("application/pdf"));
  TEST_ASSERT_EQUAL_STRING("", imagePartMime("image/jpeg\"},{\"x"));   // a sender-supplied type
  TEST_ASSERT_TRUE(isHeic("image/heic"));
  TEST_ASSERT_TRUE(isHeic("image/HEIF"));
  TEST_ASSERT_FALSE(isHeic("image/jpeg"));
  const uint8_t still[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x21, 0xF9, 0x04, 0, 0, 0, 0, 0, 0x2C, 0x3B};
  const uint8_t twoFrames[] = {'G', 'I', 'F', '8', '9', 'a', 0x21, 0xF9, 0x04, 0, 0, 0, 0, 0,
                               0x2C, 0x21, 0xF9, 0x04, 0, 0, 0, 0, 0, 0x2C, 0x3B};
  const char loop[] = "GIF89a....NETSCAPE2.0....";
  TEST_ASSERT_FALSE(animatedGif(still, sizeof still));
  TEST_ASSERT_TRUE(animatedGif(twoFrames, sizeof twoFrames));
  TEST_ASSERT_TRUE(animatedGif((const uint8_t*)loop, sizeof loop - 1));
  TEST_ASSERT_FALSE(animatedGif((const uint8_t*)"\xFF\xD8\xFF", 3));   // a JPEG
}

// The bytes decide the type (a sender's declared mime is not trusted).
static void test_sniff_by_magic() {
  const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0};
  const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0};
  const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a', 0};
  const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  const uint8_t pdf[] = {'%', 'P', 'D', 'F', '-', '1'};
  TEST_ASSERT_EQUAL_STRING("image/jpeg", sniffImageMime(jpg, sizeof jpg));
  TEST_ASSERT_EQUAL_STRING("image/png", sniffImageMime(png, sizeof png));
  TEST_ASSERT_EQUAL_STRING("image/gif", sniffImageMime(gif, sizeof gif));
  TEST_ASSERT_EQUAL_STRING("image/webp", sniffImageMime(webp, sizeof webp));
  TEST_ASSERT_EQUAL_STRING("", sniffImageMime(pdf, sizeof pdf));
  TEST_ASSERT_EQUAL_STRING("", sniffImageMime(jpg, 2));   // too short to tell
  TEST_ASSERT_EQUAL_STRING("", sniffImageMime(nullptr, 0));
}

// Admission (the turn's photo budget): an over-budget rendition, a file that is not an
// image by its bytes, an animated GIF or an eleventh photo falls back to the caption
// step; the boundaries hold exactly.
static void test_admission_and_fallbacks() {
  TurnImages set;
  std::vector<std::vector<uint8_t>> keep;
  auto jpeg = [&](size_t n) {
    keep.emplace_back(n, 0x11);
    keep.back()[0] = 0xFF, keep.back()[1] = 0xD8, keep.back()[2] = 0xFF;
    return keep.back().data();
  };
  const char* why = "";
  TEST_ASSERT_EQUAL_STRING("image/jpeg", admitImage(set, jpeg(kImagePartBytes), kImagePartBytes, why));
  TEST_ASSERT_EQUAL_STRING("", admitImage(set, jpeg(kImagePartBytes + 1), kImagePartBytes + 1, why));
  TEST_ASSERT_EQUAL_STRING("too large", why);
  const uint8_t pdf[] = {'%', 'P', 'D', 'F', '-', '1', '.', '7'};
  TEST_ASSERT_EQUAL_STRING("", admitImage(set, pdf, sizeof pdf, why));
  TEST_ASSERT_EQUAL_STRING("not an image by its bytes", why);
  const uint8_t anim[] = {'G', 'I', 'F', '8', '9', 'a', 0x21, 0xF9, 0x04, 0, 0, 0, 0, 0, 0x21, 0xF9, 0x04};
  TEST_ASSERT_EQUAL_STRING("", admitImage(set, anim, sizeof anim, why));
  TEST_ASSERT_EQUAL_STRING("an animated GIF", why);
  // The turn budget: photos that fit are admitted until the next would pass it.
  size_t total = 0;
  while (total + kImagePartBytes <= kTurnImageBytes) {
    TurnImage it;
    it.bytes = jpeg(kImagePartBytes);
    it.len = kImagePartBytes;
    TEST_ASSERT_EQUAL_STRING("image/jpeg", admitImage(set, it.bytes, it.len, why));
    set.items.push_back(it);
    total += kImagePartBytes;
  }
  TEST_ASSERT_EQUAL_STRING("", admitImage(set, jpeg(kImagePartBytes), kImagePartBytes, why));
  TEST_ASSERT_EQUAL_STRING("over the turn's photo budget", why);
  // The count: ten small photos, then an eleventh falls back.
  TurnImages many;
  for (size_t i = 0; i < kTurnImageMax; ++i) {
    TurnImage it;
    it.bytes = jpeg(16);
    it.len = 16;
    many.items.push_back(it);
  }
  TEST_ASSERT_EQUAL_STRING("", admitImage(many, jpeg(16), 16, why));
  TEST_ASSERT_EQUAL_STRING("the turn's photo limit", why);
}

// The send side's headroom rule: wait while the largest block is short, never longer
// than the per-slice bound, and never when there is room.
static void test_slice_headroom_rule() {
  TEST_ASSERT_FALSE(agent::sliceShouldWait(agent::kSliceHeadroom, 0));
  TEST_ASSERT_FALSE(agent::sliceShouldWait(40000, 0));
  TEST_ASSERT_TRUE(agent::sliceShouldWait(agent::kSliceHeadroom - 1, 0));
  TEST_ASSERT_TRUE(agent::sliceShouldWait(7156, agent::kSliceWaitMaxMs - agent::kSliceWaitStepMs));
  TEST_ASSERT_FALSE(agent::sliceShouldWait(7156, agent::kSliceWaitMaxMs));   // bounded: write anyway
  // The headroom covers one slice plus its TLS record overhead.
  TEST_ASSERT_TRUE(agent::kSliceHeadroom >= 4096 + 4096);
}

// ---- the image set -----------------------------------------------------------------

static std::unique_ptr<TurnImages> twoPhotos(int* released = nullptr, int* transcribed = nullptr) {
  static const uint8_t a[] = {1, 2, 3}, b[] = {4, 5, 6, 7};
  auto set = std::unique_ptr<TurnImages>(new TurnImages());
  set->nonce = "n0nce";
  for (int k = 1; k <= 2; ++k) {
    TurnImage it;
    it.ref = k == 1 ? "0123456789abcdef" : "fedcba9876543210";
    it.mime = "image/jpeg";
    it.bytes = k == 1 ? a : b;
    it.len = k == 1 ? sizeof a : sizeof b;
    it.k = k;
    it.n = 2;
    it.tag = "[image " + std::to_string(k) + "/2 ref=" + it.ref + "]";
    it.segment = it.tag + " (attached: the photo itself is in this message)";
    set->items.push_back(it);
  }
  if (released) set->release = [released](TurnImages&) { ++*released; };
  if (transcribed)
    set->transcribe = [transcribed](const TurnImage& it) {
      ++*transcribed;
      return it.tag + " (machine transcription by anthropic: photo " + std::to_string(it.k) + ")";
    };
  return set;
}

static void test_markers_and_bytes() {
  auto set = twoPhotos();
  TEST_ASSERT_EQUAL_STRING("nimg:n0nce:0:", set->marker(0).c_str());
  TEST_ASSERT_EQUAL_STRING("nimg:n0nce:1:", set->marker(1).c_str());
  // No marker is a prefix of another, however many photos a turn holds.
  for (size_t a = 0; a < 12; ++a)
    for (size_t b = 0; b < 12; ++b)
      if (a != b) TEST_ASSERT_TRUE(set->marker(b).rfind(set->marker(a), 0) != 0);
  TEST_ASSERT_EQUAL(7u, set->bytes());
  TEST_ASSERT_FALSE(set->empty());
}

// The blind text: each attached label replaced in place by its transcription, made
// once per photo however many requests read it; a label the text no longer holds is
// appended rather than lost.
static void test_blind_text_transcribes_once() {
  int transcribed = 0;
  auto set = twoPhotos(nullptr, &transcribed);
  const std::string text = "[USER]\n" + set->items[0].segment + "\n" + set->items[1].segment + "\n\nThey wrote: lunch";
  const std::string want = "[USER]\n[image 1/2 ref=0123456789abcdef] (machine transcription by anthropic: photo 1)\n"
                           "[image 2/2 ref=fedcba9876543210] (machine transcription by anthropic: photo 2)\n\n"
                           "They wrote: lunch";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), set->blindText(text).c_str());
  TEST_ASSERT_EQUAL_STRING(want.c_str(), set->blindText(text).c_str());
  TEST_ASSERT_EQUAL(2, transcribed);   // made once, read twice
  // Idempotent: the engine blinds a single-shot host's inputs and the provider path
  // blinds again; each transcription still appears exactly once.
  TEST_ASSERT_EQUAL_STRING(want.c_str(), set->blindText(set->blindText(text)).c_str());
  const std::string moved = set->blindText("only the caption");
  TEST_ASSERT_TRUE(moved.find("only the caption\n[image 1/2") == 0);
  TEST_ASSERT_TRUE(moved.find("photo 2)") != std::string::npos);
}

// No transcriber (or an empty transcription): the label says plainly it was not read.
static void test_blind_text_without_a_transcriber() {
  auto set = twoPhotos();
  const std::string out = set->blindText(set->items[0].segment);
  TEST_ASSERT_EQUAL_STRING(
      "[image 1/2 ref=0123456789abcdef] (not read: no provider could read it)\n"
      "[image 2/2 ref=fedcba9876543210] (not read: no provider could read it)",
      out.c_str());
}

static void test_release_runs_once() {
  int released = 0;
  { auto set = twoPhotos(&released); }
  TEST_ASSERT_EQUAL(1, released);
}

// ---- the slot ----------------------------------------------------------------------

static void test_slot_binds_chat_and_text() {
  int released = 0;
  offerTurnImages("42", "turn text", twoPhotos(&released));
  TEST_ASSERT_NULL(takeTurnImages("43", "turn text").get());   // another chat: dropped
  TEST_ASSERT_EQUAL(1, released);
  TEST_ASSERT_NULL(takeTurnImages("42", "turn text").get());   // consumed: gone

  offerTurnImages("42", "turn text", twoPhotos(&released));
  TEST_ASSERT_NULL(takeTurnImages("42", "another turn").get());   // written for another turn
  TEST_ASSERT_EQUAL(2, released);

  offerTurnImages("42", "turn text", twoPhotos(&released));
  std::unique_ptr<TurnImages> got = takeTurnImages("42", "turn text");
  TEST_ASSERT_NOT_NULL(got.get());
  TEST_ASSERT_EQUAL(2u, got->items.size());
  TEST_ASSERT_EQUAL(2, released);   // the engine holds it now
  got.reset();
  TEST_ASSERT_EQUAL(3, released);
}

static void test_slot_replace_and_drop_free_the_old_set() {
  int released = 0;
  offerTurnImages("42", "a", twoPhotos(&released));
  offerTurnImages("42", "b", twoPhotos(&released));   // a newer offer replaces it
  TEST_ASSERT_EQUAL(1, released);
  dropTurnImages();
  TEST_ASSERT_EQUAL(2, released);
  offerTurnImages("42", "c", nullptr);   // nothing to offer: the slot stays empty
  TEST_ASSERT_NULL(takeTurnImages("42", "c").get());
  auto empty = std::unique_ptr<TurnImages>(new TurnImages());
  offerTurnImages("42", "d", std::move(empty));
  TEST_ASSERT_NULL(takeTurnImages("42", "d").get());   // an empty set is no offer
}

static void test_active_set_nests_and_restores() {
  TEST_ASSERT_NULL(activeTurnImages());
  auto a = twoPhotos();
  auto b = twoPhotos();
  {
    ActiveTurnImages outer(a.get());
    TEST_ASSERT_EQUAL_PTR(a.get(), activeTurnImages());
    {
      ActiveTurnImages inner(b.get());
      TEST_ASSERT_EQUAL_PTR(b.get(), activeTurnImages());
    }
    TEST_ASSERT_EQUAL_PTR(a.get(), activeTurnImages());
  }
  TEST_ASSERT_NULL(activeTurnImages());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_spliced_body_reads_back_at_every_offset);
  RUN_TEST(test_large_photo_in_transport_slices);
  RUN_TEST(test_head_takes_images_table);
  RUN_TEST(test_formats);
  RUN_TEST(test_sniff_by_magic);
  RUN_TEST(test_admission_and_fallbacks);
  RUN_TEST(test_slice_headroom_rule);
  RUN_TEST(test_markers_and_bytes);
  RUN_TEST(test_blind_text_transcribes_once);
  RUN_TEST(test_blind_text_without_a_transcriber);
  RUN_TEST(test_release_runs_once);
  RUN_TEST(test_slot_binds_chat_and_text);
  RUN_TEST(test_slot_replace_and_drop_free_the_old_set);
  RUN_TEST(test_active_set_nests_and_restores);
  return UNITY_END();
}
