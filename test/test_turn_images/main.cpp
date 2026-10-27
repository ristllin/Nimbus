#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/orch/caps.h"            // kImageRowMaxChars - the history's photo row
#include "nimbus/orch/model_catalog.h"   // mistralVisionByName, classifyModel: the one Mistral vision rule
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

// ---- lane IMG: the head's note, the looked photos, the settled row --------------------

// image.note: one line, trimmed, capped at kImageNoteMax on a character boundary, and
// only for this turn's own photos (an earlier photo's row is never rewritten).
static void test_note_is_one_capped_line_for_this_turns_photo_only() {
  auto set = twoPhotos();
  TEST_ASSERT_TRUE(set->setNote("0123456789abcdef", "  pasta,\nabout 600 kcal\t "));
  TEST_ASSERT_EQUAL_STRING("pasta, about 600 kcal", set->items[0].note.c_str());
  TEST_ASSERT_FALSE(set->setNote("aaaaaaaaaaaaaaaa", "not this turn's photo"));
  TEST_ASSERT_FALSE(set->setNote("fedcba9876543210", " \n\t "));
  TEST_ASSERT_TRUE(set->items[1].note.empty());
  TEST_ASSERT_FALSE(set->setNote("", "no ref"));
  // A long note of 3-byte characters is cut on a character boundary, never mid-sequence.
  std::string longNote;
  for (int i = 0; i < 200; ++i) longNote += "\xE2\x82\xAC";   // the euro sign, 600 bytes
  TEST_ASSERT_TRUE(set->setNote("fedcba9876543210", longNote));
  const std::string& n = set->items[1].note;
  TEST_ASSERT_TRUE(n.size() <= kImageNoteMax);
  TEST_ASSERT_EQUAL(0, (int)(n.size() % 3));
  TEST_ASSERT_TRUE(n.size() >= kImageNoteMax - 2);
}

// The looked photos: counted in the set's bytes, admitted under the same per-photo and
// per-turn limits plus their own count, found by ref, with their own markers.
static void test_looked_photos_limits_markers_and_bytes() {
  auto set = twoPhotos();
  const size_t base = set->bytes();
  static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xDB, 1, 2, 3, 4};
  const char* why = "";
  TEST_ASSERT_EQUAL_STRING("image/jpeg", admitLook(*set, jpeg, sizeof jpeg, why));
  for (size_t j = 0; j < kTurnLookMax; ++j) {
    LookedImage l;
    l.img.ref = std::string("00000000000000") + char('a' + j) + "0";
    l.img.bytes = jpeg;
    l.img.len = sizeof jpeg;
    set->looked.push_back(l);
  }
  TEST_ASSERT_EQUAL(base + kTurnLookMax * sizeof jpeg, set->bytes());
  TEST_ASSERT_EQUAL_STRING("", admitLook(*set, jpeg, sizeof jpeg, why));
  TEST_ASSERT_EQUAL_STRING("the turn's limit of photos looked at again", why);
  set->looked.pop_back();
  static const uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_EQUAL_STRING("", admitLook(*set, text, sizeof text, why));
  TEST_ASSERT_EQUAL_STRING("not an image by its bytes", why);
  std::vector<uint8_t> big(kImagePartBytes + 1, 0);
  big[0] = 0xFF; big[1] = 0xD8; big[2] = 0xFF;
  TEST_ASSERT_EQUAL_STRING("", admitLook(*set, big.data(), big.size(), why));
  TEST_ASSERT_EQUAL_STRING("too large", why);
  TEST_ASSERT_EQUAL(1, set->lookedIndex("00000000000000b0"));
  TEST_ASSERT_EQUAL(-1, set->lookedIndex("0123456789abcdef"));   // a photo of the turn is not a look
  TEST_ASSERT_EQUAL(-1, set->lookedIndex(""));
  TEST_ASSERT_EQUAL_STRING("nlook:n0nce:1:", set->lookMarker(1).c_str());
  TEST_ASSERT_TRUE(set->lookMarker(1).find(set->marker(1)) == std::string::npos);
}

// The record a look leaves in the transcript names only its ref, and only that exact
// text reads back as a look (a tool's prose quoting it does not).
static void test_look_result_text_round_trips_only_exactly() {
  const std::string t = lookResultText("0123456789abcdef");
  TEST_ASSERT_EQUAL_STRING("[image ref=0123456789abcdef] shown to you again", t.c_str());
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef", lookResultRef(t).c_str());
  TEST_ASSERT_EQUAL_STRING("", lookResultRef("x" + t).c_str());
  TEST_ASSERT_EQUAL_STRING("", lookResultRef(t + " and more").c_str());
  TEST_ASSERT_EQUAL_STRING("", lookResultRef("[image ref=0123456789ABCDEF] shown to you again").c_str());
  TEST_ASSERT_EQUAL_STRING("", lookResultRef("[image ref=0123456789abcde] shown to you again").c_str());
  TEST_ASSERT_EQUAL_STRING("", lookResultRef("").c_str());
}

// A blind host's reading of a looked photo is made once, on first use, and names why
// when nothing could read it.
static void test_look_answer_made_once() {
  auto set = twoPhotos();
  int asked = 0;
  set->answer = [&asked](const TurnImage& it, const std::string& q) {
    ++asked;
    return it.tag + " (transcribed by mistral) " + q + ": 73";
  };
  LookedImage l;
  l.img.ref = "00000000000000a0";
  l.img.tag = "[image ref=00000000000000a0]";
  l.question = "the number";
  set->looked.push_back(l);
  TEST_ASSERT_EQUAL_STRING("[image ref=00000000000000a0] (transcribed by mistral) the number: 73",
                           set->lookAnswer(0).c_str());
  set->lookAnswer(0);
  TEST_ASSERT_EQUAL(1, asked);
  auto bare = twoPhotos();
  bare->looked.push_back(l);
  TEST_ASSERT_EQUAL_STRING("[image ref=00000000000000a0] (not read: no provider could read it)",
                           bare->lookAnswer(0).c_str());
}

// One cell of the grid: the row, the re-read flag and the settle word the turn's
// state must produce, with no caption call anywhere.
static void checkSettledCase(int sent, int noted, int read, int failed) {
  int transcribed = 0;
  auto set = twoPhotos(nullptr, &transcribed);
  if (failed)
    set->transcribe = [](TurnImage& it) {
      it.retryRead = true;
      return it.tag + " (not read: mistral out of credit (HTTP 402))";
    };
  set->sent = sent;
  if (noted) TEST_ASSERT_TRUE(set->setNote("0123456789abcdef", "pasta, about 600 kcal"));
  if (read) set->fallbackOf(0);
  transcribed = 0;
  const std::string row = settledRow(*set, 0);
  char what[64];
  snprintf(what, sizeof what, "sent=%d noted=%d read=%d failed=%d", sent, noted, read, failed);
  TEST_ASSERT_EQUAL_MESSAGE(0, transcribed, what);
  std::string want;
  if (sent && noted) want = "[image 1/2 ref=0123456789abcdef] (seen; note: pasta, about 600 kcal)";
  else if (sent) want = "[image 1/2 ref=0123456789abcdef] (seen in its turn; image.look shows it again)";
  else if (read && failed) want = "[image 1/2 ref=0123456789abcdef] (not read: mistral out of credit (HTTP 402))";
  else if (read) want = "[image 1/2 ref=0123456789abcdef] (machine transcription by anthropic: photo 1)";
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), row.c_str(), what);
  TEST_ASSERT_EQUAL_MESSAGE(!sent && read && failed, settledNeedsReread(*set, 0), what);
  TEST_ASSERT_TRUE_MESSAGE(*settledHow(*set, 0), what);
}

// Property over every case of a photo's turn (an answered request showed it or not, a
// note or not, a blind host's reading or not, that reading failed for a passing reason
// or not): a photo the head saw keeps the head's own words (its note, or "seen"),
// whatever a blind host on another round read or failed to read; a blind host's reading
// is the row only when nothing showed the photo, and only a failed one of those is read
// again later; a photo the turn never showed or read keeps the intake row. No case asks
// for a caption: the settled row is built from what the turn already has.
static void test_settled_row_property() {
  for (int sent = 0; sent < 2; ++sent)
    for (int noted = 0; noted < 2; ++noted)
      for (int read = 0; read < 2; ++read)
        for (int failed = 0; failed < 2; ++failed) checkSettledCase(sent, noted, read, failed);
  auto set = twoPhotos();
  TEST_ASSERT_EQUAL_STRING("", settledRow(*set, 2).c_str());   // out of range
  TEST_ASSERT_FALSE(settledNeedsReread(*set, 2));
}

// A full-length note keeps its row inside the history's photo row (the label plus
// kImageRowMaxChars), so the history never cuts the head's own note.
static void test_a_full_note_fits_the_history_row() {
  MediaLabel l;
  l.cls = MediaClass::Noted;
  l.detail = std::string(kImageNoteMax, 'a');
  TEST_ASSERT_TRUE(mediaSuffix(l).size() <= kImageRowMaxChars);
}

// A turn that brought no photo still gets a set (image.look can show it an earlier
// photo); it is freed, its release hook run once, when the turn ends.
static void test_turn_without_photos_gets_an_empty_set() {
  int released = 0;
  {
    ActiveTurnImages none(nullptr);
    TurnImages* set = activeTurnImages();
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_TRUE(set->empty());
    set->release = [&released](TurnImages&) { ++released; };
  }
  TEST_ASSERT_NULL(activeTurnImages());
  TEST_ASSERT_EQUAL(1, released);
}

// The class rule behind native-per-host (lane IMG): intake never decides by the
// configured head. The Telegram intake that keeps a photo for the turn must not ask
// which head is configured or whether it reads images; only the renderers' per-request
// rule (shown/sees) and the engine's per-host rule do. A source guard over the device
// file, so a later edit that brings the configured head back into intake fails here.
static void test_intake_never_decides_by_the_configured_head() {
  FILE* f = std::fopen("src/agent/telegram.cpp", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "run from the project root");
  std::string src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.append(buf, n);
  std::fclose(f);
  // Every intake function a photo passes through, from the fetch to the stored row.
  std::string intake;
  for (const char* fn : {"static bool attachNative(", "static void describeStaged(", "static bool processMedia("}) {
    const size_t at = src.find(fn);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, fn);
    const size_t end = src.find("\n}\n", at);
    TEST_ASSERT_TRUE(end != std::string::npos);
    intake += src.substr(at, end - at);
  }
  for (const char* banned : {"nativeHead(", "headTakesImages(", "resolvedOrchHost(", "orchModel("})
    TEST_ASSERT_TRUE_MESSAGE(intake.find(banned) == std::string::npos, banned);
  TEST_ASSERT_TRUE(intake.find("admitImage(") != std::string::npos);   // the guard reads the real bodies
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

// The provider's own list decides when it says (HFM, 26 Oct 2026): a model its list
// says takes no images gets the transcription, one it says takes them gets the parts.
// A model the list has no row for (-1: an alias such as mistral-large-latest, a dated id
// the list folded into its alias, a model newer than the fetch) falls back to the name
// rule on every host, Mistral included (RCA 27 Oct, A2: a fresh Mistral list without the
// head's exact id turned a seeing head caption-only). A router selector is judged by its
// upstream, by the name rule when no list says.
static int g_knownVision = -1;
static std::string g_askedHost;
static int lookupStub(const char* host, const char* model) {
  g_askedHost = host;
  return std::string(model) == "listed-blind" ? 0 : std::string(model) == "listed-seeing" ? 1 : g_knownVision;
}
static void test_the_providers_list_decides_image_input() {
  installImageInputLookup(&lookupStub);
  TEST_ASSERT_FALSE(headTakesImages("mistral", "listed-blind"));
  TEST_ASSERT_TRUE(headTakesImages("zai", "listed-seeing"));
  TEST_ASSERT_TRUE(headTakesImages("mistral", "mistral-large-latest"));   // -1: the name rule, native photos
  TEST_ASSERT_TRUE(headTakesImages("mistral", "mistral-small-latest"));
  TEST_ASSERT_TRUE(headTakesImages("mistral", "mistral-large-2512"));     // a dated id folded into its alias
  TEST_ASSERT_TRUE(headTakesImages("anthropic", "claude-opus-5-5"));      // -1 elsewhere: the name rule
  TEST_ASSERT_TRUE(headTakesImages("openai", "gpt-5.6-sol"));
  TEST_ASSERT_TRUE(headTakesImages("cumulo", "mistral/mistral-large-latest"));   // a router upstream: the name rule
  TEST_ASSERT_FALSE(headTakesImages("mistral", "codestral-latest"));
  TEST_ASSERT_FALSE(headTakesImages("cumulo", "mistral/listed-blind"));
  TEST_ASSERT_EQUAL_STRING("mistral", g_askedHost.c_str());               // asked of the upstream
  g_knownVision = 0;   // the API said no: no image, whatever the name says
  TEST_ASSERT_FALSE(headTakesImages("anthropic", "claude-haiku-5-5"));
  TEST_ASSERT_FALSE(headTakesImages("mistral", "mistral-large-latest"));
  TEST_ASSERT_FALSE(headTakesImages("mistral", "mistral-small-latest"));
  g_knownVision = 1;   // the API said yes: images, whatever the name says
  TEST_ASSERT_TRUE(headTakesImages("mistral", "codestral-latest"));
  g_knownVision = -1;
  installImageInputLookup(nullptr);
  TEST_ASSERT_TRUE(headTakesImages("anthropic", "claude-haiku-5-5"));
}

// HFM review, LOW: a device that holds NO Mistral list at all (a Cumulo-only device whose head
// routes to Mistral, or Lumi before its first Mistral fetch) keeps the name rule (the lookup
// says -2: no list), so intake and the renderer agree - exactly as a list with no row does.
static int lookupNoList(const char* host, const char* model) {
  (void)model;
  return std::string(host) == "mistral" ? -2 : -1;
}
static void test_no_list_at_all_keeps_the_name_rule() {
  installImageInputLookup(&lookupNoList);
  TEST_ASSERT_TRUE(headTakesImages("mistral", "mistral-small-latest"));
  TEST_ASSERT_TRUE(headTakesImages("cumulo", "mistral/mistral-small-latest"));
  TEST_ASSERT_FALSE(headTakesImages("mistral", "codestral-latest"));
  installImageInputLookup(nullptr);
}

// One Mistral vision rule (RCA 27 Oct): the catalog's vision role and the head's native
// photos read the same name rule, so they agree on every Mistral id - the API reads a
// photo's pixels on both mistral-small-latest and mistral-large-latest (HFM's
// mistral_vision proof, 26 Oct), which the catalog used to deny for small.
static void test_the_catalog_and_the_head_agree_on_mistral_vision() {
  installImageInputLookup(nullptr);
  for (const char* id : {"mistral-small-latest", "mistral-small-2603", "mistral-medium-latest", "mistral-large-latest",
                         "mistral-large-2512", "ministral-8b-latest", "magistral-medium-latest",
                         "pixtral-large-latest", "codestral-latest", "devstral-small-latest", "open-mistral-nemo",
                         "mistral-ocr-latest", "voxtral-small-latest", "mistral-embed", "mistral-moderation-latest"}) {
    const bool head = headTakesImages("mistral", id);
    TEST_ASSERT_EQUAL_MESSAGE(nimbus::orch::mistralVisionByName(id), head, id);
    TEST_ASSERT_EQUAL_MESSAGE(head, nimbus::orch::classifyModel("mistral", id).hasRole(nimbus::orch::RoleVision), id);
  }
  TEST_ASSERT_TRUE(nimbus::orch::classifyModel("mistral", "mistral-small-latest").hasRole(nimbus::orch::RoleVision));
}

// The set knows its own photos (image.look on one the head already sees makes no call).
static void test_the_set_holds_its_refs() {
  TurnImages set;
  TurnImage a;
  a.ref = "0123456789abcdef";
  set.items.push_back(a);
  TEST_ASSERT_TRUE(set.holds("0123456789abcdef"));
  TEST_ASSERT_FALSE(set.holds("fedcba9876543210"));
  TEST_ASSERT_FALSE(set.holds(""));
  TEST_ASSERT_FALSE(set.shown);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_spliced_body_reads_back_at_every_offset);
  RUN_TEST(test_large_photo_in_transport_slices);
  RUN_TEST(test_head_takes_images_table);
  RUN_TEST(test_the_providers_list_decides_image_input);
  RUN_TEST(test_the_set_holds_its_refs);
  RUN_TEST(test_no_list_at_all_keeps_the_name_rule);
  RUN_TEST(test_the_catalog_and_the_head_agree_on_mistral_vision);
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
  RUN_TEST(test_note_is_one_capped_line_for_this_turns_photo_only);
  RUN_TEST(test_looked_photos_limits_markers_and_bytes);
  RUN_TEST(test_look_result_text_round_trips_only_exactly);
  RUN_TEST(test_look_answer_made_once);
  RUN_TEST(test_settled_row_property);
  RUN_TEST(test_a_full_note_fits_the_history_row);
  RUN_TEST(test_turn_without_photos_gets_an_empty_set);
  RUN_TEST(test_intake_never_decides_by_the_configured_head);
  return UNITY_END();
}
