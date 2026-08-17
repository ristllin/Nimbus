// Host regression tests for the STT response path (lib/core/audio_req).
//
// The audio-input regression these lock down: raising the hold-to-talk recording
// cap to 60 s made transcription responses longer, but the multipart RESPONSE read
// still had a fixed 2048-byte cap + a 20 s deadline. A long transcript's
// {"text":"..."} JSON exceeded 2048 -> the body was truncated mid-string ->
// deserializeJson failed -> the transcript came back EMPTY ("Didn't catch that").
//
// `readHttpBody` (the extracted, size-transparent read) + `parseTranscription` are
// now the ONE code path the device uses too (http_multipart.cpp / audio_stt.cpp),
// so these asserts guard the real firmware behavior, not a parallel copy.

#include <unity.h>

#include <string>

#include "nimbus/audio_req.h"

using core::ByteReader;
using core::parseTranscription;
using core::readHttpBody;

// A canned response source: hands out `data` in small chunks (simulating partial
// socket reads), then reports disconnected - the HTTP/1.0 `Connection: close` shape.
struct FakeReader : ByteReader {
  std::string data;
  size_t pos = 0;
  int chunk;
  explicit FakeReader(std::string d, int ch = 64) : data(std::move(d)), chunk(ch) {}
  int available() override { return static_cast<int>(data.size() - pos); }
  int read(uint8_t* b, int n) override {
    int avail = static_cast<int>(data.size() - pos);
    int take = n < avail ? n : avail;
    if (take > chunk) take = chunk;  // never hand back more than a chunk at once
    for (int i = 0; i < take; i++) b[i] = static_cast<uint8_t>(data[pos++]);
    return take;
  }
  bool connected() override { return pos < data.size(); }
};

void setUp() {}
void tearDown() {}

// THE regression: a transcript whose JSON is far larger than the old 2048-byte cap
// must be read IN FULL (not truncated), and then parse cleanly.
static void test_body_over_2048_read_in_full() {
  std::string big(5000, 'x');
  std::string json = "{\"text\":\"" + big + "\"}";
  FakeReader r(json);
  std::string out;
  size_t n = readHttpBody(r, /*contentLen=*/-1, /*cap=*/16384, out);  // read to close
  TEST_ASSERT_EQUAL_UINT(json.size(), n);
  TEST_ASSERT_EQUAL_STRING(json.c_str(), out.c_str());

  bool ok = false;
  std::string text = parseTranscription(out.c_str(), &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT(5000, text.size());  // the full transcript survives
}

// Content-Length is honored: stops exactly at the body, ignores anything after it.
static void test_content_length_honored() {
  std::string json = "{\"text\":\"hi\"}";
  FakeReader r(json + "GARBAGE-AFTER-BODY");
  std::string out;
  readHttpBody(r, static_cast<long>(json.size()), 16384, out);
  TEST_ASSERT_EQUAL_STRING(json.c_str(), out.c_str());
}

// The 16 KB ceiling bounds a runaway/error page (and can never hang).
static void test_cap_bounds_runaway() {
  std::string huge(40000, 'y');
  FakeReader r(huge);
  std::string out;
  size_t n = readHttpBody(r, -1, 16384, out);
  TEST_ASSERT_EQUAL_UINT(16384, n);
}

// A body arriving in 1-byte dribbles (worst-case partial reads) is still assembled
// whole - the read loop must not give up early on a slow trickle.
static void test_tiny_chunks_assemble_full_body() {
  std::string json = "{\"text\":\"the quick brown fox jumps over the lazy dog\"}";
  FakeReader r(json, /*chunk=*/1);
  std::string out;
  readHttpBody(r, -1, 16384, out);
  TEST_ASSERT_EQUAL_STRING(json.c_str(), out.c_str());
}

// \uXXXX escapes (accents/emoji) decode to real UTF-8 - the reason the parse uses a
// JSON decoder, not a hand-rolled scanner.
static void test_unicode_escapes_decode() {
  bool ok = false;
  std::string text = parseTranscription("{\"text\":\"caf\\u00e9\"}", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", text.c_str());  // é == UTF-8 0xC3 0xA9
}

// An error object (valid JSON, no "text") -> ok=true, empty transcript, no crash.
static void test_error_object_empty_text() {
  bool ok = false;
  std::string text = parseTranscription("{\"error\":{\"message\":\"nope\"}}", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT(0, text.size());
}

// Truncated JSON (what the 2048 cap produced) -> ok=false, empty, fail-closed.
static void test_truncated_json_fails_closed() {
  bool ok = true;
  std::string text = parseTranscription("{\"text\":\"abcdefg", &ok);  // cut mid-string
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_UINT(0, text.size());
}

// Leading/trailing whitespace is trimmed (mirrors the old Arduino String::trim).
// ---- F26: Transfer-Encoding chunked decode ---------------------------------
static void test_dechunk_basic_two_chunks() {
  std::string b = "7\r\n{\"text\"\r\nA\r\n:\"hi roy\"}\r\n0\r\n\r\n";
  TEST_ASSERT_TRUE(core::dechunkHttpBody(b));
  TEST_ASSERT_EQUAL_STRING("{\"text\":\"hi roy\"}", b.c_str());
  bool ok = false;
  TEST_ASSERT_EQUAL_STRING("hi roy", core::parseTranscription(b.c_str(), &ok).c_str());
  TEST_ASSERT_TRUE(ok);
}

static void test_dechunk_extensions_and_uppercase_hex() {
  std::string b = "A;ext=1\r\n0123456789\r\n0\r\n\r\n";
  TEST_ASSERT_TRUE(core::dechunkHttpBody(b));
  TEST_ASSERT_EQUAL_STRING("0123456789", b.c_str());
}

// Connection-close truncation mid-chunk keeps everything decoded so far.
static void test_dechunk_truncated_keeps_partial() {
  std::string b = "5\r\nhello\r\n5\r\nwo";
  TEST_ASSERT_TRUE(core::dechunkHttpBody(b));
  TEST_ASSERT_EQUAL_STRING("hellowo", b.c_str());
}

// Truncation landing EXACTLY on the lone '\r' of a chunk-terminating CRLF must
// still keep the fully-decoded payload (prism riders finding - it returned false).
static void test_dechunk_truncated_at_lone_cr_keeps_decode() {
  std::string b = "5\r\nhello\r";   // full chunk decoded; tail cut between CR and LF
  TEST_ASSERT_TRUE(core::dechunkHttpBody(b));
  TEST_ASSERT_EQUAL_STRING("hello", b.c_str());
}

// A plain (non-chunked) JSON body must be left untouched - the detector can
// misfire only if the body STARTS with hex + CRLF, and '{' is not hex.
static void test_dechunk_plain_body_unchanged() {
  std::string b = "{\"text\":\"plain\"}";
  std::string orig = b;
  TEST_ASSERT_FALSE(core::dechunkHttpBody(b));
  TEST_ASSERT_EQUAL_STRING(orig.c_str(), b.c_str());
}

static void test_dechunk_garbage_unchanged() {
  std::string a = "";
  TEST_ASSERT_FALSE(core::dechunkHttpBody(a));
  std::string c = "5\r\nhelloXX";   // data not followed by CRLF/close -> not chunked
  std::string origC = c;
  TEST_ASSERT_FALSE(core::dechunkHttpBody(c));
  TEST_ASSERT_EQUAL_STRING(origC.c_str(), c.c_str());
}

static void test_trims_whitespace() {
  bool ok = false;
  std::string text = parseTranscription("{\"text\":\"  hello \\n\"}", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("hello", text.c_str());
}

// Empty and non-JSON inputs fail closed with ok=false (never a crash).
static void test_empty_and_garbage() {
  bool ok = true;
  TEST_ASSERT_EQUAL_UINT(0, parseTranscription("", &ok).size());
  TEST_ASSERT_FALSE(ok);
  ok = true;
  TEST_ASSERT_EQUAL_UINT(0, parseTranscription("not json at all", &ok).size());
  TEST_ASSERT_FALSE(ok);
  // nullptr is tolerated.
  TEST_ASSERT_EQUAL_UINT(0, parseTranscription(nullptr).size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_body_over_2048_read_in_full);
  RUN_TEST(test_content_length_honored);
  RUN_TEST(test_cap_bounds_runaway);
  RUN_TEST(test_tiny_chunks_assemble_full_body);
  RUN_TEST(test_unicode_escapes_decode);
  RUN_TEST(test_error_object_empty_text);
  RUN_TEST(test_truncated_json_fails_closed);
  RUN_TEST(test_dechunk_basic_two_chunks);
  RUN_TEST(test_dechunk_extensions_and_uppercase_hex);
  RUN_TEST(test_dechunk_truncated_keeps_partial);
  RUN_TEST(test_dechunk_truncated_at_lone_cr_keeps_decode);
  RUN_TEST(test_dechunk_plain_body_unchanged);
  RUN_TEST(test_dechunk_garbage_unchanged);
  RUN_TEST(test_trims_whitespace);
  RUN_TEST(test_empty_and_garbage);
  return UNITY_END();
}
