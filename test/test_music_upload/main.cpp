#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/media.h"

// CUM-40: the music UPLOAD path. The device write seam (src/sfx/music_store.cpp)
// and the web route are Arduino/SD glue - not host-buildable - but the DECISION
// that guards them, nimbus::orch::musicUploadAllowed, is portable and is the same
// gate the player + /play read back through. These tests pin that gate as a CLASS
// (every reject reason, not one instance) and prove the round-trip invariant: a
// name the upload accepts is exactly a name the listing keeps and the player will
// play - the two can never drift.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- the gate, as a class --------------------------------------------------

static void test_upload_refused_without_sd() {
  std::string err;
  TEST_ASSERT_FALSE(musicUploadAllowed("song.mp3", /*sdPresent=*/false, err));
  TEST_ASSERT_FALSE(err.empty());                    // honest, user-facing reason
  TEST_ASSERT_NOT_NULL(strstr(err.c_str(), "SD"));   // names the actual cause
}

static void test_upload_accepts_valid_audio() {
  const char* ok[] = {"song.mp3", "A Track 01.wav", "track_02-final.MP3", "x.WAV"};
  for (const char* n : ok) {
    std::string err = "sentinel";
    TEST_ASSERT_TRUE_MESSAGE(musicUploadAllowed(n, true, err), n);
    TEST_ASSERT_EQUAL_STRING("sentinel", err.c_str());   // success leaves err untouched
  }
}

static void test_upload_refuses_bad_names_as_a_class() {
  // One entry per reject REASON, so a new hole in the gate fails here instead of
  // shipping a way to smuggle a non-track (or a traversal) into /music.
  const char* bad[] = {
    "notes.txt",          // wrong extension
    "song",               // no extension
    "song.mp3.exe",       // real extension is not audio
    "../evil.mp3",        // path traversal
    "a/b.mp3",            // path separator
    ".hidden.mp3",        // leading dot
    "song.wav ",          // trailing char after the extension (fails endsWith)
    "",                   // empty
  };
  for (const char* n : bad) {
    std::string err;
    TEST_ASSERT_FALSE_MESSAGE(musicUploadAllowed(n, true, err), n);
    TEST_ASSERT_FALSE_MESSAGE(err.empty(), n);   // always a reason to show the user
  }
}

// ---- the round-trip invariant: accept <=> list <=> play --------------------

// The device listMusicDir() keeps exactly validMusicName() entries; model that here
// so the test drives the same filter the firmware does.
static std::vector<std::string> listingFilter(const std::vector<std::string>& raw) {
  std::vector<std::string> out;
  for (const auto& n : raw) if (validMusicName(n.c_str())) out.push_back(n);
  return out;
}

static void test_accept_equals_playable_for_all_names() {
  // For EVERY candidate the invariant must hold: the upload gate agrees exactly
  // with what the player will accept. If either side ever changes alone, this
  // fails - the whole point (upload can't add a track the player then rejects,
  // and vice versa).
  const char* names[] = {
    "song.mp3", "A Track.wav", "notes.txt", "../evil.mp3", "song", ".x.mp3", "y.WAV",
  };
  std::string err;
  for (const char* n : names) {
    const bool gate = musicUploadAllowed(n, true, err);
    const bool play = validMusicName(n);
    TEST_ASSERT_EQUAL_MESSAGE(play, gate, n);
  }
}

static void test_uploaded_track_lists_and_plays() {
  // Simulate: two good uploads + one junk file already on the card. Only the
  // accepted tracks survive the listing, and the player queues exactly those.
  std::vector<std::string> onCard;
  std::string err;
  for (const char* n : {"first.mp3", "second.wav", "README.txt"}) {
    if (musicUploadAllowed(n, true, err)) onCard.push_back(n);   // web route would write it
    // README.txt is refused: it never reaches the card, so don't add it.
  }
  onCard.push_back("README.txt");   // but a hand-copied junk file could still be present

  const std::vector<std::string> listed = listingFilter(onCard);
  TEST_ASSERT_EQUAL_INT(2, (int)listed.size());          // junk filtered out of the listing
  TEST_ASSERT_EQUAL_STRING("first.mp3", listed[0].c_str());
  TEST_ASSERT_EQUAL_STRING("second.wav", listed[1].c_str());

  MediaQueue q;
  const int n = q.playNow(listed);
  TEST_ASSERT_EQUAL_INT(2, n);                            // both queued
  TEST_ASSERT_TRUE(q.playing());
  TEST_ASSERT_EQUAL_STRING("first.mp3", q.current().c_str());
  TEST_ASSERT_TRUE(q.trackFinished());                   // advance
  TEST_ASSERT_EQUAL_STRING("second.wav", q.current().c_str());
  TEST_ASSERT_FALSE(q.trackFinished());                  // end of queue, no repeat
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_upload_refused_without_sd);
  RUN_TEST(test_upload_accepts_valid_audio);
  RUN_TEST(test_upload_refuses_bad_names_as_a_class);
  RUN_TEST(test_accept_equals_playable_for_all_names);
  RUN_TEST(test_uploaded_track_lists_and_plays);
  return UNITY_END();
}
