#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Media (music) playback - the PURE, host-tested core behind the device player.
// Owns the two decidable-from-plain-data pieces so the device seam (src/sfx) only
// has to do I2S + the Helix/WAV decode loop:
//   1. format sniff (WAV vs MP3) from the file head + extension, and
//   2. the foreground playback QUEUE state machine.
//
// Music is FOREGROUND audio: unlike the SFX queue (depth 2, drop-when-full, because
// "sounds are decoration"), a music track is never silently dropped and always plays
// to completion. The queue is a single now-playing slot plus an ordered FIFO list.
// No Arduino, no I2S here - the device drives it (ask current(), decode + play,
// call trackFinished(), repeat).

namespace nimbus {
namespace orch {

// ---- format sniff ----------------------------------------------------------
enum class MediaFormat : uint8_t { Unknown = 0, Wav, Mp3 };
const char* mediaFormatName(MediaFormat f);

// Sniff from the first bytes of a file plus its name. Content wins (a RIFF/WAVE
// header, an "ID3" tag, or an MPEG-audio frame sync 0xFFEx); the extension is the
// fallback when the head is too short or ambiguous. `head` may be null/short.
MediaFormat sniffFormat(const uint8_t* head, size_t n, const char* filename);

// A valid /music track filename: 1..64 printable chars, ends in .wav or .mp3
// (case-insensitive), no path separators, no "..", no leading dot. This is the
// traversal gate for the media tools (mirrors the sfx/file path rules).
bool validMusicName(const char* name);

// ---- playback queue --------------------------------------------------------
enum class MediaState : uint8_t { Stopped = 0, Playing, Paused };
const char* mediaStateName(MediaState s);

class MediaQueue {
 public:
  static constexpr int kMaxTracks = 128;   // bounds RAM; a /music folder cap

  // Append one track to the FIFO (no drop; false only when full or the path is
  // empty). Does NOT change play state - the caller decides when to start.
  bool enqueue(const std::string& path);

  // Replace the whole queue with `paths` and start playing from the first
  // (the "play this now" default). Empty list == clear() + Stopped. Truncated to
  // kMaxTracks. Returns the number kept.
  int playNow(const std::vector<std::string>& paths);

  // Resume/START: Paused -> Playing; Stopped -> Playing from the current track
  // (or the first, if none selected yet). No-op (returns false) when the queue is
  // empty.
  bool play();
  void pause();   // Playing -> Paused (position kept)
  void stop();    // -> Stopped (position kept, so a later play() resumes here)
  void clear();   // stop + empty the list

  // Advance to the next track. Call when the current track finished playing OR to
  // skip. Honors repeat: at the end, wraps to 0 when repeat, else Stops. Returns
  // true if there is a track to play now (state stays Playing), false at the end.
  bool next();
  bool prev();

  // The device calls this when the current track's decode reached EOF. Same as
  // next() but semantically "it ended on its own".
  bool trackFinished() { return next(); }

  MediaState  state() const { return state_; }
  bool        playing() const { return state_ == MediaState::Playing; }
  const std::string& current() const;   // "" when nothing selected/empty
  int  index() const { return cur_; }   // -1 when none
  int  size() const { return (int)tracks_.size(); }
  bool empty() const { return tracks_.empty(); }
  const std::vector<std::string>& tracks() const { return tracks_; }

  void setRepeat(bool on) { repeat_ = on; }
  bool repeat() const { return repeat_; }

 private:
  std::vector<std::string> tracks_;
  int        cur_ = -1;                       // current index, -1 = none selected
  MediaState state_ = MediaState::Stopped;
  bool       repeat_ = false;
};

}  // namespace orch
}  // namespace nimbus
