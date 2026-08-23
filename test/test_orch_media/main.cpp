#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/media.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// ---- format sniff ----------------------------------------------------------

static void test_sniff_by_content() {
  const uint8_t wav[12] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E'};
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Wav, (int)sniffFormat(wav, 12, "x.bin"));
  const uint8_t id3[3] = {'I','D','3'};
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Mp3, (int)sniffFormat(id3, 3, "x.bin"));
  const uint8_t frame[2] = {0xFF, 0xFB};   // MPEG frame sync
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Mp3, (int)sniffFormat(frame, 2, "x.bin"));
  // RIFF but not WAVE (e.g. AVI) is not a WAV.
  const uint8_t riffavi[12] = {'R','I','F','F', 0,0,0,0, 'A','V','I',' '};
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Unknown, (int)sniffFormat(riffavi, 12, "x.bin"));
}

static void test_sniff_by_extension_fallback() {
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Wav, (int)sniffFormat(nullptr, 0, "song.WAV"));
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Mp3, (int)sniffFormat(nullptr, 0, "song.Mp3"));
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Unknown, (int)sniffFormat(nullptr, 0, "notes.txt"));
  // content beats a wrong extension
  const uint8_t id3[3] = {'I','D','3'};
  TEST_ASSERT_EQUAL_INT((int)MediaFormat::Mp3, (int)sniffFormat(id3, 3, "mislabeled.wav"));
}

static void test_valid_music_name() {
  TEST_ASSERT_TRUE(validMusicName("song.mp3"));
  TEST_ASSERT_TRUE(validMusicName("A Track 01.wav"));
  TEST_ASSERT_TRUE(validMusicName("track_02-final.MP3"));
  TEST_ASSERT_FALSE(validMusicName("song.txt"));          // wrong ext
  TEST_ASSERT_FALSE(validMusicName("../secret.mp3"));     // traversal
  TEST_ASSERT_FALSE(validMusicName("sub/song.mp3"));      // separator
  TEST_ASSERT_FALSE(validMusicName(".hidden.mp3"));       // leading dot
  TEST_ASSERT_FALSE(validMusicName("a.mp3\t"));           // control char (tab not allowed)
  TEST_ASSERT_FALSE(validMusicName(""));
  TEST_ASSERT_FALSE(validMusicName(nullptr));
}

// ---- queue state machine ---------------------------------------------------

static void test_queue_playnow_and_advance() {
  MediaQueue q;
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_FALSE(q.play());   // nothing to play

  int kept = q.playNow({"/music/a.mp3", "/music/b.wav", "/music/c.mp3"});
  TEST_ASSERT_EQUAL_INT(3, kept);
  TEST_ASSERT_TRUE(q.playing());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", q.current().c_str());
  // finish a -> b -> c -> end (stops, no wrap without repeat)
  TEST_ASSERT_TRUE(q.trackFinished());
  TEST_ASSERT_EQUAL_STRING("/music/b.wav", q.current().c_str());
  TEST_ASSERT_TRUE(q.trackFinished());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", q.current().c_str());
  TEST_ASSERT_FALSE(q.trackFinished());     // end of list
  TEST_ASSERT_EQUAL_INT((int)MediaState::Stopped, (int)q.state());
}

static void test_queue_pause_stop_resume() {
  MediaQueue q;
  q.playNow({"/music/a.mp3", "/music/b.mp3"});
  q.pause();
  TEST_ASSERT_EQUAL_INT((int)MediaState::Paused, (int)q.state());
  TEST_ASSERT_TRUE(q.play());               // resume
  TEST_ASSERT_TRUE(q.playing());
  q.stop();
  TEST_ASSERT_EQUAL_INT((int)MediaState::Stopped, (int)q.state());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", q.current().c_str());   // position kept
  TEST_ASSERT_TRUE(q.play());               // resumes at the kept position
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", q.current().c_str());
}

static void test_queue_repeat_wraps() {
  MediaQueue q;
  q.playNow({"/music/only.mp3"});
  q.setRepeat(true);
  TEST_ASSERT_TRUE(q.trackFinished());      // wraps to itself
  TEST_ASSERT_EQUAL_STRING("/music/only.mp3", q.current().c_str());
  TEST_ASSERT_TRUE(q.playing());
}

static void test_queue_enqueue_fifo_no_drop_and_bounds() {
  MediaQueue q;
  TEST_ASSERT_TRUE(q.enqueue("/music/1.mp3"));
  TEST_ASSERT_TRUE(q.enqueue("/music/2.mp3"));
  TEST_ASSERT_FALSE(q.enqueue(""));         // empty rejected
  TEST_ASSERT_EQUAL_INT(2, q.size());
  TEST_ASSERT_EQUAL_INT((int)MediaState::Stopped, (int)q.state());  // enqueue doesn't auto-play
  TEST_ASSERT_TRUE(q.play());
  TEST_ASSERT_EQUAL_STRING("/music/1.mp3", q.current().c_str());
  // fill to the cap; further enqueues are refused (never a silent drop of music)
  MediaQueue big;
  for (int i = 0; i < MediaQueue::kMaxTracks; i++)
    TEST_ASSERT_TRUE(big.enqueue("/music/x.mp3"));
  TEST_ASSERT_FALSE(big.enqueue("/music/over.mp3"));
  TEST_ASSERT_EQUAL_INT(MediaQueue::kMaxTracks, big.size());
}

static void test_queue_next_prev_skip() {
  MediaQueue q;
  q.playNow({"/music/a.mp3", "/music/b.mp3", "/music/c.mp3"});
  TEST_ASSERT_TRUE(q.next());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", q.current().c_str());
  TEST_ASSERT_TRUE(q.prev());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", q.current().c_str());
  TEST_ASSERT_TRUE(q.prev());               // clamp at start (no repeat)
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", q.current().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sniff_by_content);
  RUN_TEST(test_sniff_by_extension_fallback);
  RUN_TEST(test_valid_music_name);
  RUN_TEST(test_queue_playnow_and_advance);
  RUN_TEST(test_queue_pause_stop_resume);
  RUN_TEST(test_queue_repeat_wraps);
  RUN_TEST(test_queue_enqueue_fifo_no_drop_and_bounds);
  RUN_TEST(test_queue_next_prev_skip);
  return UNITY_END();
}
